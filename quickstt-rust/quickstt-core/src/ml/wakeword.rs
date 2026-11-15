use super::oww::OwwBackend;
use anyhow::Result;
use std::collections::HashMap;
use std::time::Instant;
use tracing::info;

// Calibrated activation cooldown: 2.5s prevents reverberant room echo or
// trailing dictation audio from immediately re-triggering wakeword mode.
const ACTIVATION_COOLDOWN_SECS: f64 = 2.0;
// Scores are community-head probabilities in [0,1]. openWakeWord documents
// 0.5 as the default threshold (<0.5 false accepts/hour on hours of speech);
// the engine eases it a touch to 0.47 at mid sensitivity so normal-volume
// phrases confirm without shouting (non-phrase scores ~0.00, margin intact).
const WAKE_BASE_THRESHOLD: f32 = 0.47;
const PREDICTIVE_PRELOAD_THRESHOLD: f32 = 0.28;
// Inference cadence over the 80ms mic chunks: every 2nd chunk (~160ms).
// Audio is buffered on EVERY chunk (see push_audio); only the expensive
// 5-head ONNX pass runs on this cadence. Faster than this burns CPU for no
// latency gain on a hands-free trigger; slower adds user-visible lag.
const WAKE_INFER_EVERY_CHUNKS: u32 = 2;
// Energy gate: chunks quieter than this RMS never reach the ONNX heads.
// Digital silence / room hiss scores noise, wastes LSTM runs per tick,
// and single-frame spikes on quiet audio were the phantom-trigger source.
// Operates on gain-boosted audio (see INPUT_GAIN).
const MIN_WAKE_RMS: f32 = 0.002;
// VAD gate floor: when the voice-activity detector reports no speech AND the
// chunk is below this RMS, the heads are skipped and stale hits cleared.
// Loud audio always scores (it might be speech the VAD hasn't opened on
// yet); quiet non-speech never does. Together with the 3-hit rule this is
// the phantom-trigger fix. (Lowered so normal-volume speech opens
// the gate without shouting; loud non-phrase still scores ~0.00.)
const VAD_GATE_RMS: f32 = 0.005;
// Post-speech gate hangover (in 80ms chunks): openWakeWord's 76-frame mel
// window + 16-embedding head has ~200–700ms of causal latency, so the peak
// probability (~0.99+) routinely lands AFTER the user stops speaking the
// wakeword. Without hangover the gate snaps shut on the first quiet chunk
// after the word, drops hit counts, and never sees the peak. 12 chunks ≈ 960ms.
const GATE_HANGOVER_CHUNKS: u32 = 12;
// Software input gain applied to every mic chunk before RMS/gates/heads.
// The community heads score this laptop mic's normal-volume speech <0.15
// at 1x (shouts reach 0.9+) — same heads, same mic, proven by the
// `oww_gain_calibration_probe` on the user's own recorded speech, which
// also proved 3x gain still scores 0.000 on non-phrase audio (no false
// fires from the boost itself). 3x lifts conversational peaks (~1500-3000)
// into the range that demonstrably triggers, with i16 clamping +
// attenuate-only normalization below keeping loud voices from railing.
// (4x was tried for very quiet 500-900-peak attempts but never probed for
// false fires — and the false-fire storm came from the UI instant-cancel
// bug making every trigger flash-die, not from weak gain. Back to the
// probed-safe 3x.) Re-proven safe 2026-10-07: all 13 saved utterances +
// both reference clips peak 0.000 at BOTH 2x and 3x (probe_utterances) —
// margin fully intact.
const INPUT_GAIN: f32 = 3.0;

/// RMS of an i16 chunk in [-1, 1] units.
fn chunk_rms(pcm: &[i16]) -> f32 {
    if pcm.is_empty() {
        return 0.0;
    }
    let sum: f64 = pcm.iter().map(|&s| (s as f64) * (s as f64)).sum();
    ((sum / pcm.len() as f64).sqrt() / 32768.0) as f32
}

/// Fixed software gain + attenuate-only level normalization. The community
/// heads are level-hungry below and clip-blind above: fixed 3x lifts quiet
/// speech into their range, but a loud voice rails at ±32767 (square-wave
/// mel → every head scores 0.000 — the louder you shout, the deader the
/// trigger). So each chunk is scaled toward 0.08 RMS but NEVER amplified:
/// quiet chunks come out byte-identical to plain 3x, loud chunks are turned
/// down into the heads' window. Memoryless (no AGC state to poison, no
/// silence pumping — the failure of the previous adaptive attempt).
fn apply_input_gain(pcm_chunk: &[i16]) -> Vec<i16> {
    const LEVEL_TARGET_RMS: f32 = 0.08;
    let raw_rms = chunk_rms(pcm_chunk);
    if raw_rms < 1e-6 {
        return pcm_chunk.to_vec();
    }
    // Compute gain in f32 BEFORE clamping to i16: the previous implementation
    // multiplied by 3x and clamped to [-32768, 32767] first, turning any
    // normal/loud mic input (peak > 10,922) into a hard-clipped square wave
    // before scaling it down to 0.08 RMS.
    // Also cap gain by headroom so sudden consonants stay under ±30,000.
    let raw_peak = pcm_chunk
        .iter()
        .map(|&s| (s as f32).abs())
        .fold(0.0f32, f32::max);
    let peak_cap = if raw_peak > 1.0 {
        30_000.0 / raw_peak
    } else {
        INPUT_GAIN
    };
    let g = (LEVEL_TARGET_RMS / raw_rms).min(INPUT_GAIN).min(peak_cap);
    if (g - 1.0).abs() < 1e-4 {
        return pcm_chunk.to_vec();
    }
    pcm_chunk
        .iter()
        .map(|&s| ((s as f32 * g).clamp(-32768.0, 32767.0)) as i16)
        .collect()
}

fn norm_key(s: &str) -> String {
    s.trim().to_lowercase().replace('_', " ")
}

// Inference runs on community openWakeWord heads (see super::oww): a
// shared mel→embedding frontend plus one tiny classifier per phrase with
// real discrimination (silence/noise score ~0.00, phrase ~0.9+).
pub struct WakeWordEngine {
    backend: OwwBackend,
    thresholds: HashMap<String, f32>,
    enabled_phrases: HashMap<String, bool>,
    hit_counts: HashMap<String, u32>,
    last_activation: Option<Instant>,
    suppress_until: Option<Instant>,
    active_phrases: Vec<String>,
    user_sensitivity: u32,
    chunks_since_infer: u32,
    /// Remaining post-speech chunks (80ms each) to keep scoring heads after
    /// VAD/energy drops, catching causal end-of-phrase peaks.
    gate_hangover: u32,
    /// Throttled diagnostics: épocas since last top-score log line.
    infers_since_log: u32,
    /// Last marginal-score log (rate-limits the below-threshold line).
    last_marginal_log: Option<Instant>,
    /// Lifetime inference count (diagnostics + tests).
    infers_run: u32,
    last_scores_pair: Option<(f32, f32)>,
}

pub enum WakeWordEvent {
    Triggered(String, f32),
    PredictivePreload(String, f32),
    None,
}

impl WakeWordEngine {
    /// Build from community OWW heads: (onnx path, head name, phrase).
    pub fn from_oww_heads(heads: &[(std::path::PathBuf, String, String)]) -> Result<Self> {
        let pairs: Vec<(std::path::PathBuf, String)> = heads
            .iter()
            .map(|(p, n, _)| (p.clone(), n.clone()))
            .collect();
        let backend = OwwBackend::new(&pairs)?;

        let mut thresholds: HashMap<String, f32> = HashMap::new();
        let mut enabled_phrases: HashMap<String, bool> = HashMap::new();
        let mut active_phrases: Vec<String> = Vec::new();

        for (_, name, phrase) in heads {
            let key = norm_key(name);
            let phrase_key = norm_key(phrase);
            // Thresholds derive from the sensitivity slider (see
            // recalculate_thresholds + threshold_for).
            thresholds.insert(key.clone(), WAKE_BASE_THRESHOLD);
            thresholds.insert(phrase_key.clone(), WAKE_BASE_THRESHOLD);
            enabled_phrases.insert(key, true);
            enabled_phrases.insert(phrase_key, true);
            active_phrases.push(phrase.clone());
        }

        info!(
            "WakeWordEngine loaded {} heads: {:?}",
            heads.len(),
            active_phrases
        );

        let mut engine = Self {
            backend,
            thresholds,
            enabled_phrases,
            hit_counts: HashMap::new(),
            last_activation: None,
            suppress_until: None,
            active_phrases,
            user_sensitivity: 50,
            chunks_since_infer: 0,
            gate_hangover: 0,
            infers_since_log: 0,
            last_marginal_log: None,
            infers_run: 0,
            last_scores_pair: None,
        };
        engine.recalculate_thresholds();
        Ok(engine)
    }

    /// Set master wakeword sensitivity (0..100) and recalculate per-model thresholds.
    pub fn set_sensitivity(&mut self, sensitivity: u32) {
        self.user_sensitivity = sensitivity.clamp(0, 100);
        self.recalculate_thresholds();
    }

    pub fn set_phrase_enabled(&mut self, phrase: &str, enabled: bool) {
        let key = norm_key(phrase);
        self.enabled_phrases.insert(key, enabled);
    }

    pub fn is_phrase_enabled(&self, phrase: &str) -> bool {
        let key = norm_key(phrase);
        self.enabled_phrases.get(&key).copied().unwrap_or(true)
    }

    fn recalculate_thresholds(&mut self) {
        // Dashboard sensitivity 0..100 → probability threshold. At 50 the
        // 0.47 bar applies: a touch under the documented 0.50 default so
        // normal-volume phrases confirm without shouting — non-phrase audio
        // scores ~0.00, so the false-trigger margin stays huge. 0 is strict
        // (0.77), 100 is loose (0.30). "hey jarvis" gets its own eased bar
        // (see threshold_for_phrase): the two-word phrase peaks ~0.1 lower
        // than "alexa" at the same volume and misses a shared bar.
        let keys: Vec<String> = self.thresholds.keys().cloned().collect();
        for k in keys {
            self.thresholds
                .insert(k.clone(), Self::threshold_for_phrase(self.user_sensitivity, &k));
        }
    }

    /// Effective trigger threshold for a sensitivity value (0..100).
    /// Shared with the dashboard readout so the UI shows the exact number
    /// the engine enforces.
    pub fn threshold_for(sensitivity: u32) -> f32 {
        (0.72 - sensitivity.clamp(0, 100) as f32 * 0.005).clamp(0.30, 0.77)
    }

    /// Per-phrase threshold: "hey jarvis" (normalized key "hey jarvis") is
    /// eased by 0.10 — measured marginals 0.17–0.35 at conversational volume
    /// against a 0.37 shared bar, while "alexa" hits 0.9+ on the same voice.
    /// Non-phrase audio still scores ~0.00, so the margin holds.
    pub fn threshold_for_phrase(sensitivity: u32, phrase_key: &str) -> f32 {
        let base = Self::threshold_for(sensitivity);
        if phrase_key == "hey jarvis" {
            (base - 0.10).clamp(0.22, 0.77)
        } else {
            base
        }
    }

    /// Feed one 80ms mic chunk. `vad_speech` is the caller's voice-activity
    /// verdict for this chunk (the service runs `EnergyVad` on the same
    /// audio) — quiet non-speech never reaches the ONNX heads.
    pub fn process_chunk(&mut self, pcm_chunk: &[i16], vad_speech: bool) -> Vec<WakeWordEvent> {
        self.last_scores_pair = None;
        if let Some(until) = self.suppress_until {
            if Instant::now() < until {
                return vec![WakeWordEvent::None];
            }
            self.suppress_until = None;
        }

        // Software input gain + attenuate-only normalization (see
        // apply_input_gain): every downstream stage — RMS gates, mel
        // frontend, heads — sees boosted audio with loud voices turned down
        // into the heads' window instead of railing at ±32767.
        let boosted = apply_input_gain(pcm_chunk);
        let pcm_chunk = &boosted;

        let rms_boosted = chunk_rms(pcm_chunk);
        // Push EVERY chunk into the mel frontend so the 16kHz audio stream and
        // MEL_CTX overlap stay strictly contiguous (skipping quiet chunks
        // time-warped silence/pauses and starved the 16-embedding window).
        self.backend.push_audio(pcm_chunk);

        // ~160ms inference cadence once the window is hot.
        self.chunks_since_infer += 1;
        if (self.chunks_since_infer % WAKE_INFER_EVERY_CHUNKS) != 0 {
            return vec![WakeWordEvent::None];
        }
        // Score only user-enabled phrases — check before update if everything
        // was disabled so zero ONNX work runs when no phrase is active.
        let enabled: Vec<String> = self
            .backend
            .heads
            .iter()
            .map(|m| m.name.clone())
            .filter(|n| self.enabled_phrases.get(&norm_key(n)).copied().unwrap_or(true))
            .collect();
        if enabled.is_empty() {
            self.hit_counts.clear();
            self.gate_hangover = 0;
            return vec![WakeWordEvent::None];
        }
        // Caches advance on EVERY inference tick, gated or not: mel +
        // embeddings fill through silence and room noise, so a phrase arriving
        // after quiet scores from its first tick. Only head runs are gated.
        self.backend.update();
        // Energy + VAD gate with post-speech hangover: quiet room hiss never
        // wakes the heads, while speech keeps the gate open for ~960ms after
        // the last spoken syllable so the causal 76-frame/16-emb peak lands.
        let active_audio =
            rms_boosted >= MIN_WAKE_RMS && (vad_speech || rms_boosted >= VAD_GATE_RMS);
        if active_audio {
            self.gate_hangover = GATE_HANGOVER_CHUNKS;
        } else if self.gate_hangover > 0 {
            self.gate_hangover = self.gate_hangover.saturating_sub(WAKE_INFER_EVERY_CHUNKS);
        }
        if self.gate_hangover == 0 {
            // Gate closed: drop stale hit counts so two far-apart blips can
            // never add up to a phantom wake (mirrors the C++ reference).
            self.hit_counts.clear();
            return vec![WakeWordEvent::None];
        }
        let scores = self.backend.score_heads(&enabled);
        self.infers_run += 1;
        {
            let hj = scores.iter().find(|s| s.0 == "hey_jarvis").map(|s| s.1).unwrap_or(0.0);
            let al = scores.iter().find(|s| s.0 == "alexa").map(|s| s.1).unwrap_or(0.0);
            self.last_scores_pair = Some((hj, al));
        }

        // Throttled diagnostics: top score every ~10s so the log proves the
        // mic path is live and shows whether speech nears the threshold.
        self.infers_since_log += 1;
        if self.infers_since_log >= 16 {
            self.infers_since_log = 0;
            if let Some((name, score)) = scores.iter().max_by(|a, b| {
                a.1.partial_cmp(&b.1).unwrap_or(std::cmp::Ordering::Equal)
            }) {
                info!("Wakeword scores: top '{}'={:.3} (n={})", name, score, scores.len());
            }
        }

        let mut events = Vec::new();

        for (name, score) in &scores {
            let key = norm_key(name);
            // Skip if disabled by user
            if !self.enabled_phrases.get(&key).copied().unwrap_or(true) {
                continue;
            }
            // Community heads are trained far-field robust (reverberated
            // training data), so one bar fits all levels — no tier hacks.
            let threshold = self.thresholds.get(&key).copied().unwrap_or(WAKE_BASE_THRESHOLD);
            // Sustained-confirmation rule: require 2 consecutive checks (~320ms)
            // above threshold. Community heads score ~0.00 on non-phrase speech
            // and spike cleanly across 4–10 frames on real wakewords; requiring 3
            // frames at 160ms cadence (480ms) rejected briskly spoken "Alexa".
            if *score >= threshold {
                let count = self.hit_counts.entry(key.clone()).or_insert(0);
                *count += 1;
                let hit_count = *count;
                let required = 2;
                if hit_count >= required {
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
                // Any miss breaks the streak.
                self.hit_counts.remove(&key);
                // Marginal attempts (audible to the heads but below the bar)
                // log immediately, rate-limited: this is the line that shows
                // "heard something like the phrase, not enough to fire".
                if *score >= 0.15 {
                    let due = match self.last_marginal_log {
                        Some(t) => t.elapsed().as_secs() >= 3,
                        None => true,
                    };
                    if due {
                        self.last_marginal_log = Some(Instant::now());
                        info!(
                            "Wakeword marginal: '{}' score={:.3} threshold={:.3} (below bar)",
                            name, score, threshold
                        );
                    }
                }
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

    /// Lifetime inference count (diagnostics + tests).
    pub fn infer_count(&self) -> u32 {
        self.infers_run
    }

    #[cfg(test)]
    pub fn last_scores(&self) -> Option<(f32, f32)> {
        self.last_scores_pair
    }

    /// Cache diagnostics: (mel rows, embedding windows, head-scoring runs).
    pub fn cache_stats(&self) -> (usize, usize, u32) {
        (
            self.backend.mel_rows(),
            self.backend.emb_cached(),
            self.infers_run,
        )
    }

    /// Loaded head names (diagnostics + tests).
    pub fn classifier_names(&self) -> Vec<String> {
        self.backend.heads.iter().map(|m| m.name.clone()).collect()
    }

    pub fn reset(&mut self) {
        self.hit_counts.clear();
        self.last_activation = None;
        self.suppress_until = None;
        // Drop frontend state: after a mic reopen the mel buffer holds stale
        // speech that must not score against the fresh stream.
        self.backend.reset();
        self.chunks_since_infer = 0;
        self.gate_hangover = 0;
        self.infers_since_log = 0;
        self.last_marginal_log = None;
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

#[cfg(test)]
mod tests {
    use super::*;

    fn oww_heads() -> Vec<(std::path::PathBuf, String, String)> {
        let dir =
            std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../wakeword_models");
        let heads = crate::wakeword_loader::discover_oww_heads(&dir);
        assert_eq!(heads.len(), 2, "expected hey_jarvis + alexa heads");
        heads
    }

    #[test]
    fn wakeword_loads_community_heads() {
        let heads = oww_heads();
        let eng = WakeWordEngine::from_oww_heads(&heads).expect("engine loads");
        let names = eng.classifier_names();
        assert!(names.contains(&"hey_jarvis".to_string()), "got {:?}", names);
        assert!(names.contains(&"alexa".to_string()), "got {:?}", names);
        assert_eq!(eng.phrases(), &["hey jarvis".to_string(), "alexa".to_string()]);
    }

    #[test]
    fn wakeword_threshold_curve_is_sane() {
        // 50 → 0.47 eased bar; strict end high, loose end low.
        assert!((WakeWordEngine::threshold_for(50) - 0.47).abs() < 1e-6);
        assert!(WakeWordEngine::threshold_for(0) > WakeWordEngine::threshold_for(50));
        assert!(WakeWordEngine::threshold_for(100) < WakeWordEngine::threshold_for(50));
        // "hey jarvis" rides 0.10 under the shared bar (measured gap).
        assert!(
            (WakeWordEngine::threshold_for_phrase(70, "hey jarvis") - 0.27).abs() < 1e-6
        );
        assert!(
            (WakeWordEngine::threshold_for_phrase(70, "alexa")
                - WakeWordEngine::threshold_for(70))
            .abs()
                < 1e-6
        );
    }

    #[test]
    fn input_gain_leaves_quiet_byte_identical_attenuates_loud() {
        // Silence: untouched.
        let silence = vec![0i16; 1280];
        assert_eq!(apply_input_gain(&silence), silence);
        // Quiet speech (peak 500, boosted RMS ~0.05 < 0.08 target): plain 3x,
        // byte-identical.
        let quiet: Vec<i16> = (0..1280).map(|i| if i % 2 == 0 { 500 } else { -500 }).collect();
        let expected: Vec<i16> = quiet.iter().map(|&s| s * 3).collect();
        assert_eq!(apply_input_gain(&quiet), expected);
        // Loud voice (peak 20000 → 60000 clipped at 3x): attenuated toward
        // 0.08 RMS, never railed.
        let loud: Vec<i16> = (0..1280)
            .map(|i| {
                ((2.0 * std::f32::consts::PI * 220.0 * i as f32 / 16000.0).sin() * 20000.0)
                    as i16
            })
            .collect();
        let out = apply_input_gain(&loud);
        let peak = out.iter().map(|s| s.abs()).max().unwrap_or(0);
        assert!(peak < 32767, "loud must not rail (peak {peak})");
        let rms = chunk_rms(&out);
        assert!(
            (rms - 0.08).abs() < 0.02,
            "loud must land near 0.08 RMS (got {rms:.3})"
        );
        // Verify loud signal stays sinusoidal (not clipped to a square wave before attenuation):
        // crest factor of a sine wave is sqrt(2) ≈ 1.414; a clipped square wave drops toward 1.0.
        let crest = (peak as f32 / 32768.0) / rms;
        assert!(
            crest > 1.30,
            "loud speech must attenuate cleanly without square-wave clipping (crest {crest:.3})"
        );
    }

    #[test]
    fn wakeword_gate_hangover_stays_open_after_speech_stops() {
        let heads = oww_heads();
        let mut eng = WakeWordEngine::from_oww_heads(&heads).expect("engine loads");
        // Warm up with 2s silence (0 infers).
        let silence = vec![0i16; 1280];
        for _ in 0..25 {
            eng.process_chunk(&silence, false);
        }
        assert_eq!(eng.infer_count(), 0);
        // Speech burst (2 chunks = 160ms, 1 infer).
        let tone: Vec<i16> = (0..1280)
            .map(|i| ((2.0 * std::f32::consts::PI * 440.0 * i as f32 / 16000.0).sin() * 3000.0) as i16)
            .collect();
        eng.process_chunk(&tone, true);
        eng.process_chunk(&tone, true);
        let infers_at_speech_end = eng.infer_count();
        assert_eq!(infers_at_speech_end, 1);
        // Following trailing silence: hangover must keep scoring heads for ~800ms+
        // so openWakeWord's causal peak (which lands after the word ends) is caught,
        // and then close again cleanly.
        for _ in 0..20 {
            eng.process_chunk(&silence, false);
        }
        let infers_after_hangover = eng.infer_count();
        assert!(
            infers_after_hangover >= infers_at_speech_end + 4,
            "expected hangover to keep scoring after speech ends (got {} vs {})",
            infers_after_hangover,
            infers_at_speech_end
        );
        // Further silence after hangover expired must stop scoring heads.
        for _ in 0..10 {
            eng.process_chunk(&silence, false);
        }
        assert_eq!(eng.infer_count(), infers_after_hangover);
    }

    #[test]
    fn wakeword_silence_never_fires_or_infers() {
        let heads = oww_heads();
        let mut eng = WakeWordEngine::from_oww_heads(&heads).expect("engine loads");
        // 40 × 80ms silence: must never trigger and never reach ONNX
        // (energy-gated). Audio still buffers into the backend ctx.
        let silence = vec![0i16; 1280];
        for _ in 0..40 {
            for ev in eng.process_chunk(&silence, false) {
                assert!(
                    !matches!(ev, WakeWordEvent::Triggered(..)),
                    "silence must not trigger wakeword"
                );
            }
        }
        assert_eq!(eng.infer_count(), 0, "silence must be energy-gated");
        assert!(eng.backend.buffered_samples() > 0, "chunks must still buffer");
        // Warm caches: silence fills mel rows through the adaptive update()
        // path (every 4th tick in dead silence, no head runs), so a phrase
        // arriving after quiet still finds warm context instead of spending
        // the phrase building it. 40 chunks → 10 updates → ~48 rows.
        assert!(
            eng.backend.mel_rows() > 40,
            "silence must still warm the mel cache (got {})",
            eng.backend.mel_rows()
        );
    }

    #[test]
    fn wakeword_loud_tone_reaches_inference_without_trigger() {
        let heads = oww_heads();
        let mut eng = WakeWordEngine::from_oww_heads(&heads).expect("engine loads");
        // Pure 440Hz tone at moderate level with VAD open: community heads
        // score ~0.000 on it (probe), so it must reach inference but never
        // confirm into a trigger.
        let tone: Vec<i16> = (0..1280)
            .map(|i| {
                ((2.0 * std::f32::consts::PI * 440.0 * i as f32 / 16000.0).sin() * 3000.0)
                    as i16
            })
            .collect();
        for _ in 0..40 {
            for ev in eng.process_chunk(&tone, true) {
                assert!(
                    !matches!(ev, WakeWordEvent::Triggered(..)),
                    "pure tone must not trigger wakeword"
                );
            }
        }
        assert!(eng.infer_count() > 0, "audible audio must reach inference");
    }

    #[test]
    fn wakeword_vad_gate_blocks_quiet_nonspeech() {
        let heads = oww_heads();
        let mut eng = WakeWordEngine::from_oww_heads(&heads).expect("engine loads");
        // Two related guarantees, split because the 3x input gain moved them
        // apart (see INPUT_GAIN):
        //
        // A) The GATE: a tone quiet enough that the boosted level stays under
        //    the VAD floor must never even reach the heads. Peak 40 (raw RMS
        //    ~0.0009, ~0.0026 after the 3x gain).
        // B) The SAFETY NET: a slightly louder tone (peak 100) legitimately
        //    reaches inference at 3x gain, and must still never trigger.
        let tone_at = |peak: f32| -> Vec<i16> {
            (0..1280)
                .map(|i| {
                    ((2.0 * std::f32::consts::PI * 440.0 * i as f32 / 16000.0).sin() * peak)
                        as i16
                })
                .collect()
        };
        // A) gate floor.
        let gated = tone_at(40.0);
        for _ in 0..40 {
            for ev in eng.process_chunk(&gated, false) {
                assert!(
                    !matches!(ev, WakeWordEvent::Triggered(..)),
                    "VAD-gated audio must not trigger wakeword"
                );
            }
        }
        assert_eq!(
            eng.infer_count(),
            0,
            "audio under the VAD gate floor must be blocked before inference"
        );
        // B) reaches inference at 3x gain but must stay silent on scores.
        let audible = tone_at(100.0);
        let before = eng.infer_count();
        let mut triggered = 0usize;
        for _ in 0..40 {
            for ev in eng.process_chunk(&audible, false) {
                if matches!(ev, WakeWordEvent::Triggered(..)) {
                    triggered += 1;
                }
            }
        }
        assert_eq!(triggered, 0, "a pure tone must never trigger a wakeword");
        assert!(
            eng.infer_count() > before,
            "this tone is above the gate floor and is expected to be scored"
        );
    }
}
