use std::collections::VecDeque;

const HOP_SAMPLES: usize = 256;
const SPEECH_HOLD_FRAMES: u32 = 10;
// Calibrated default: 0.68 ensures quiet ambient noise (fans, breathing, AC)
// never opens the speech gate, while direct speech clearly passes.
const DEFAULT_THRESHOLD: f32 = 0.68;
// Absolute RMS floor: silence and distant ambient murmurs (< 0.005) are rejected.
const DEFAULT_MIN_SPEECH_RMS: f32 = 0.0055;

#[derive(Debug, Clone, Copy)]
pub struct VadResult {
    pub speech_likely: bool,
    pub probability: f32,
}

pub struct EnergyVad {
    threshold: f32,
    min_rms: f32,
    speech_hold: u32,
    consecutive_speech_hops: u32,
    energy_history: VecDeque<f32>,
    noise_floor: f32,
}

impl EnergyVad {
    pub fn new(threshold: f32) -> Self {
        Self {
            threshold,
            min_rms: DEFAULT_MIN_SPEECH_RMS,
            speech_hold: 0,
            consecutive_speech_hops: 0,
            energy_history: VecDeque::with_capacity(50),
            noise_floor: 0.002,
        }
    }

    pub fn with_default_threshold() -> Self {
        Self::new(DEFAULT_THRESHOLD)
    }

    pub fn set_threshold(&mut self, threshold: f32) {
        self.threshold = threshold.clamp(0.20, 0.95);
    }

    pub fn set_min_rms(&mut self, min_rms: f32) {
        self.min_rms = min_rms.clamp(0.001, 0.05);
    }

    /// Map a 0..100 sensitivity value to threshold and min RMS.
    /// 0 = strictest (low sensitivity, high noise rejection)
    /// 50 = balanced default
    /// 100 = most sensitive (detects very quiet speech, requires quiet environment)
    pub fn set_sensitivity(&mut self, sensitivity: u32) {
        let s = (sensitivity.min(100) as f32) / 100.0;
        // At 0: threshold 0.85, min_rms 0.010
        // At 50: threshold 0.68, min_rms 0.0055
        // At 100: threshold 0.48, min_rms 0.0025
        self.threshold = 0.85 - s * 0.37;
        self.min_rms = 0.010 - s * 0.0075;
    }

    pub fn noise_floor(&self) -> f32 {
        self.noise_floor
    }

    pub fn reset(&mut self) {
        self.speech_hold = 0;
        self.consecutive_speech_hops = 0;
        self.energy_history.clear();
        self.noise_floor = 0.002;
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

        // Calculate Signal-to-Noise Ratio in dB
        let noise = self.noise_floor.max(0.0005);
        let snr = energy / noise;
        let snr_db = 20.0 * (snr.max(0.01)).log10();

        // Speech probability is a sigmoid over SNR centered at +8 dB
        let probability = sigmoid((snr_db - 8.0) * 0.35);

        // Genuine speech must exceed absolute RMS floor and relative SNR threshold
        let is_speech_hop = energy >= self.min_rms && probability >= self.threshold;

        if is_speech_hop {
            self.consecutive_speech_hops += 1;
            // Require at least 2 consecutive hops (~32ms) to open the hold gate
            if self.consecutive_speech_hops >= 2 {
                self.speech_hold = SPEECH_HOLD_FRAMES;
            }
        } else {
            self.consecutive_speech_hops = 0;
            if self.speech_hold > 0 {
                self.speech_hold -= 1;
            }
        }

        VadResult {
            speech_likely: self.speech_hold > 0,
            probability,
        }
    }

    fn update_noise_floor(&mut self, energy: f32) {
        // Only adapt noise floor when not in active speech
        if self.speech_hold == 0 && energy < self.min_rms * 1.5 {
            // Slow exponential moving average
            self.noise_floor = (self.noise_floor * 0.96 + energy * 0.04).clamp(0.0005, 0.04);
        }
    }
}

pub fn rms_energy(samples: &[i16]) -> f32 {
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
