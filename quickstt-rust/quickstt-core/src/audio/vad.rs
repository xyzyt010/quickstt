use std::collections::VecDeque;

const HOP_SAMPLES: usize = 256;
const SPEECH_HOLD_FRAMES: u32 = 12;
const DEFAULT_THRESHOLD: f32 = 0.50;

pub struct VadResult {
    pub speech_likely: bool,
    pub probability: f32,
}

pub struct EnergyVad {
    threshold: f32,
    speech_hold: u32,
    energy_history: VecDeque<f32>,
    noise_floor: f32,
}

impl EnergyVad {
    pub fn new(threshold: f32) -> Self {
        Self {
            threshold,
            speech_hold: 0,
            energy_history: VecDeque::with_capacity(50),
            noise_floor: 0.001,
        }
    }

    pub fn with_default_threshold() -> Self {
        Self::new(DEFAULT_THRESHOLD)
    }

    pub fn reset(&mut self) {
        self.speech_hold = 0;
        self.energy_history.clear();
        self.noise_floor = 0.001;
    }

    pub fn process(&mut self, samples: &[i16]) -> VadResult {
        let mut any_speech = false;
        let mut max_prob = 0.0f32;

        for hop in samples.chunks(HOP_SAMPLES) {
            if hop.len() < HOP_SAMPLES {
                break;
            }
            let result = self.process_hop(hop);
            if result.speech_likely {
                any_speech = true;
            }
            max_prob = max_prob.max(result.probability);
        }

        VadResult {
            speech_likely: any_speech || self.speech_hold > 0,
            probability: max_prob,
        }
    }

    fn process_hop(&mut self, hop: &[i16]) -> VadResult {
        let energy = rms_energy(hop);

        self.energy_history.push_back(energy);
        if self.energy_history.len() > 50 {
            self.energy_history.pop_front();
        }

        self.update_noise_floor(energy);

        let snr = if self.noise_floor > 0.0001 {
            energy / self.noise_floor
        } else {
            energy * 10000.0
        };

        let probability = sigmoid(snr - 3.0);

        if probability >= self.threshold {
            self.speech_hold = SPEECH_HOLD_FRAMES;
        } else if self.speech_hold > 0 {
            self.speech_hold -= 1;
        }

        VadResult {
            speech_likely: self.speech_hold > 0,
            probability,
        }
    }

    fn update_noise_floor(&mut self, energy: f32) {
        if self.speech_hold == 0 {
            self.noise_floor = self.noise_floor * 0.95 + energy * 0.05;
        }
    }
}

fn rms_energy(samples: &[i16]) -> f32 {
    if samples.is_empty() {
        return 0.0;
    }
    let sum: f64 = samples.iter().map(|&s| (s as f64) * (s as f64)).sum();
    ((sum / samples.len() as f64).sqrt() / 32768.0) as f32
}

fn sigmoid(x: f32) -> f32 {
    1.0 / (1.0 + (-x).exp())
}

pub fn audio_level_0_100(samples: &[i16]) -> u8 {
    let rms = rms_energy(samples);
    let db = 20.0 * (rms + 1e-10).log10();
    let normalized = ((db + 60.0) / 60.0).clamp(0.0, 1.0);
    (normalized * 100.0) as u8
}
