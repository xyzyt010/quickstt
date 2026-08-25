use crate::wakeword_loader::WakeWordModelInfo;
use anyhow::Result;
use livekit_wakeword::WakeWordModel;
use std::collections::HashMap;
use std::time::Instant;
use tracing::info;

const HIT_REQUIREMENT: u32 = 2;
const ACTIVATION_COOLDOWN_SECS: f64 = 1.0;
const PREDICTIVE_PRELOAD_THRESHOLD: f32 = 0.35;

pub struct WakeWordEngine {
    model: WakeWordModel,
    thresholds: HashMap<String, f32>,
    hit_counts: HashMap<String, u32>,
    last_activation: Option<Instant>,
    suppress_until: Option<Instant>,
    active_phrases: Vec<String>,
}

pub enum WakeWordEvent {
    Triggered(String, f32),
    PredictivePreload(String, f32),
    None,
}

impl WakeWordEngine {
    pub fn from_discovered_models(models: &[WakeWordModelInfo]) -> Result<Self> {
        let onnx_paths: Vec<String> = models
            .iter()
            .map(|m| m.onnx_path.to_string_lossy().into_owned())
            .collect();

        let path_refs: Vec<&str> = onnx_paths.iter().map(|s| s.as_str()).collect();
        let sample_rate = models
            .first()
            .map(|m| m.config.sample_rate)
            .unwrap_or(16000);

        let model = WakeWordModel::new(&path_refs, sample_rate)?;

        let thresholds: HashMap<String, f32> = models
            .iter()
            .map(|m| (m.config.name.clone(), m.config.threshold))
            .collect();

        let active_phrases: Vec<String> = models.iter().map(|m| m.config.phrase.clone()).collect();

        info!(
            "WakeWordEngine loaded {} models: {:?}",
            models.len(),
            active_phrases
        );

        Ok(Self {
            model,
            thresholds,
            hit_counts: HashMap::new(),
            last_activation: None,
            suppress_until: None,
            active_phrases,
        })
    }

    pub fn process_chunk(&mut self, pcm_chunk: &[i16]) -> Vec<WakeWordEvent> {
        if let Some(until) = self.suppress_until {
            if Instant::now() < until {
                return vec![WakeWordEvent::None];
            }
            self.suppress_until = None;
        }

        let scores = match self.model.predict(pcm_chunk) {
            Ok(s) => s,
            Err(e) => {
                tracing::warn!("Wakeword predict error: {}", e);
                return vec![WakeWordEvent::None];
            }
        };

        let mut events = Vec::new();

        for (name, score) in &scores {
            let threshold = self.thresholds.get(name).copied().unwrap_or(0.5);

            if *score >= threshold {
                let count = self.hit_counts.entry(name.clone()).or_insert(0);
                *count += 1;
                let hit_count = *count;

                if hit_count >= HIT_REQUIREMENT {
                    if self.can_activate() {
                        info!(
                            "WAKEWORD TRIGGERED: '{}' (score: {:.3}, threshold: {:.3}, hits: {})",
                            name, score, threshold, hit_count
                        );
                        self.last_activation = Some(Instant::now());
                        self.hit_counts.clear();
                        events.push(WakeWordEvent::Triggered(name.clone(), *score));
                    }
                }
            } else {
                self.hit_counts.insert(name.clone(), 0);
            }

            if *score >= PREDICTIVE_PRELOAD_THRESHOLD && *score < threshold {
                events.push(WakeWordEvent::PredictivePreload(name.clone(), *score));
            }
        }

        if events.is_empty() {
            events.push(WakeWordEvent::None);
        }

        events
    }

    pub fn suppress(&mut self, duration_secs: f32) {
        self.suppress_until =
            Some(Instant::now() + std::time::Duration::from_secs_f32(duration_secs));
        self.hit_counts.clear();
    }

    pub fn reset(&mut self) {
        self.hit_counts.clear();
        self.last_activation = None;
        self.suppress_until = None;
    }

    pub fn phrases(&self) -> &[String] {
        &self.active_phrases
    }

    fn can_activate(&self) -> bool {
        match self.last_activation {
            Some(t) => t.elapsed().as_secs_f64() > ACTIVATION_COOLDOWN_SECS,
            None => true,
        }
    }
}
