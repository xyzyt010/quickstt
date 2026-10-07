//! Clap transient detector.
//!
//! Port of `HybridAcousticDetector` from `Source/native/stt_service_native.cpp`:
//! real-time transient onset + spectral analysis with noise-floor adaptation.
//!
//! Clap-only by design: finger snaps were retired (too easy to confuse with
//! everyday clicks — the crisp short-burst band overlaps mouse/key/tap
//! sounds). A hand clap carries a broad 20ms+ palm burst + room body that
//! everyday clicks (<5ms) never have, so the width gate separates them.
//!
//! The ACTION (start/stop/disabled) is intentionally NOT in here — it lives in
//! `Settings::transient_action` and applies to the clap.

use std::time::{Duration, Instant};

/// Which transient fired. Clap-only (snap retired) — the enum stays so log
/// lines and the arbiter carry the kind explicitly.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TransientKind {
    Clap,
}

impl TransientKind {
    pub fn as_str(self) -> &'static str {
        "clap"
    }
}

/// Shared transient action values (mirror the dashboard dropdown order).
pub const TRANSIENT_START: u32 = 0;
pub const TRANSIENT_STOP: u32 = 1;
pub const TRANSIENT_DISABLED: u32 = 2;

/// Minimum burst width: samples above peak/4 within the chunk. 120 samples
/// ≈ 7.5ms — desk clicks/key taps (<5ms) and knocks fall below it, hand
/// claps (20ms+ of palm burst + room body) clear it with margin.
pub const MIN_BURST_WIDTH: usize = 120;

/// Peak absolute level of a chunk (for arbiter confirmation).
pub fn chunk_peak(samples: &[i16]) -> f64 {
    samples
        .iter()
        .map(|s| (*s as f64).abs())
        .fold(0.0f64, f64::max)
}

/// One-chunk confirmation arbiter: a detected onset is HELD, not acted on.
/// It only becomes actionable if the NEXT chunk is quiet (an isolated
/// impulse: a clap) — quiet in BOTH level and shape:
///   1. follow-up peak < 25% of the onset peak (loud syllable + pause veto:
///      a voice onset at 8000+ followed by a 1300-level pause passes a
///      40% test, but a real clap's tail drops far harder), AND
///   2. follow-up burst width <= HALF the onset width (the pause after a
///      voice onset still carries a wide energy body; a real clap's tail
///      has decayed well below half its burst width).
/// Costs one chunk (~80ms) of latency. Calibrated against live-room logs:
/// four consecutive phantoms (onset w134–521 / confirm w260–523) all veto
/// with margin; synthetic and measured real claps still confirm.
#[derive(Default)]
pub struct TransientArbiter {
    pending: Option<(TransientKind, f64, usize)>,
}

impl TransientArbiter {
    pub fn new() -> Self {
        Self { pending: None }
    }

    pub fn reset(&mut self) {
        self.pending = None;
    }

    /// Feed this chunk's detector result + peak + burst width. Returns the
    /// kind to ACT on (confirmed from the previous chunk), if any.
    pub fn update(
        &mut self,
        detected: Option<TransientKind>,
        chunk_peak: f64,
        chunk_width: usize,
    ) -> Option<TransientKind> {
        let mut fire = None;
        if let Some((kind, onset_peak, onset_width)) = self.pending.take() {
            let quiet_level = chunk_peak < onset_peak * 0.25;
            let decayed_shape = chunk_width * 2 <= onset_width;
            if quiet_level && decayed_shape {
                fire = Some(kind);
            } else {
                // Vetoed: sustained sound or a loud onset whose tail still
                // carries energy (speech pause) — not an isolated clap.
                tracing::info!(
                    "Clap onset vetoed (follow-up peak={:.0} vs onset={:.0}, width={} vs onset={})",
                    chunk_peak,
                    onset_peak,
                    chunk_width,
                    onset_width
                );
            }
        }
        if let Some(kind) = detected {
            self.pending = Some((kind, chunk_peak.max(1.0), chunk_width));
        }
        fire
    }
}

pub struct TransientDetector {
    noise_floor: f32,
    prev_rms: f32,
    last_trigger: Option<Instant>,
    warmup_frames: u32,
    /// Stats of the most recent processed chunk (for action-site logging).
    last_peak: f64,
    last_ratio: f64,
    last_jump: f64,
    last_zcr: f64,
    last_spectral: f64,
    last_floor: f64,
    last_width: usize,
    /// Stats of the most recent ONSET chunk (peak, ratio, jump, floor,
    /// width). `last_*` is overwritten by every chunk including the quiet
    /// arbiter-confirmation chunk, so fire lines read this instead — it
    /// survives until the next onset replaces it.
    last_onset: Option<(f64, f64, f64, f64, usize)>,
}

impl TransientDetector {
    pub fn new() -> Self {
        Self {
            noise_floor: 15.0,
            prev_rms: 0.0,
            last_trigger: None,
            warmup_frames: 0,
            last_peak: 0.0,
            last_ratio: 0.0,
            last_jump: 0.0,
            last_zcr: 0.0,
            last_spectral: 0.0,
            last_floor: 0.0,
            last_width: 0,
            last_onset: None,
        }
    }

    pub fn reset(&mut self) {
        self.noise_floor = 15.0;
        self.prev_rms = 0.0;
        self.last_trigger = None;
        self.warmup_frames = 0;
        self.last_onset = None;
    }

    /// Stats of the most recent processed chunk: (peak, peakRatio, rmsJump,
    /// zcr, spectral, floor, width). Logged at action sites so real-room
    /// calibration is driven by measured numbers, not guesses.
    pub fn last_stats(&self) -> (f64, f64, f64, f64, f64, f64, usize) {
        (
            self.last_peak,
            self.last_ratio,
            self.last_jump,
            self.last_zcr,
            self.last_spectral,
            self.last_floor,
            self.last_width,
        )
    }

    /// Onset stats of the most recent detection (survives the quiet
    /// confirmation chunk — see `last_onset`).
    pub fn last_onset_stats(&self) -> Option<(f64, f64, f64, f64, usize)> {
        self.last_onset
    }

    /// Feed one mic chunk (any length ≥ 16 samples). Returns the classified
    /// transient, or `None`. Warmup swallows the first frames so mic-open
    /// pops and AGC settles never fire.
    pub fn process(&mut self, samples: &[i16]) -> Option<TransientKind> {
        if samples.len() < 16 {
            return None;
        }

        let mut max_abs = 0.0f64;
        let mut sum_sq = 0.0f64;
        let mut diff_sum_sq = 0.0f64;
        let mut zero_crossings = 0usize;
        for (i, &s) in samples.iter().enumerate() {
            let v = (s as f64).abs();
            if v > max_abs {
                max_abs = v;
            }
            sum_sq += (s as f64) * (s as f64);
            if i > 0 {
                let d = (s as f64) - (samples[i - 1] as f64);
                diff_sum_sq += d * d;
                if (s >= 0) != (samples[i - 1] >= 0) {
                    zero_crossings += 1;
                }
            }
        }
        let n = samples.len() as f64;
        let rms = (sum_sq / n).sqrt();
        let diff_rms = (diff_sum_sq / (n - 1.0).max(1.0)).sqrt();
        let zcr = zero_crossings as f64 / n;
        let spectral_ratio = diff_rms / (rms + 0.1);
        // Burst width: samples carrying serious energy (≥ peak/4). Desk
        // clicks and key taps are ultra-short impulses (<5ms ≈ 80 samples)
        // with no burst body; hand claps carry 20ms+ of palm burst + room
        // body. Width separates deliberate claps from everyday clicks that
        // pass every level test on a hot mic.
        let width_thresh = max_abs / 4.0;
        let width = if max_abs > 0.0 {
            samples
                .iter()
                .filter(|s| (**s as f64).abs() >= width_thresh)
                .count()
        } else {
            0
        };

        // Warmup: let the noise floor settle (~4 × 80ms chunks).
        self.warmup_frames += 1;
        if self.warmup_frames < 5 {
            self.noise_floor = rms as f32;
            self.prev_rms = rms as f32;
            return None;
        }

        // Adapt the floor only on quiet frames.
        if rms < self.noise_floor as f64 * 2.0 + 20.0 {
            self.noise_floor = (self.noise_floor * 0.97 + rms as f32 * 0.03).clamp(3.0, 4000.0);
            if self.noise_floor < 3.0 {
                self.noise_floor = 3.0;
            }
        }

        let floor = (self.noise_floor as f64).max(10.0);
        let peak_over_floor = max_abs / floor;
        let rms_jump = rms / (self.prev_rms as f64 + 1.0);
        self.prev_rms = rms as f32;
        self.last_peak = max_abs;
        self.last_ratio = peak_over_floor;
        self.last_jump = rms_jump;
        self.last_zcr = zcr;
        self.last_spectral = spectral_ratio;
        self.last_floor = floor;
        self.last_width = width;

        // Fixed sensitivity 1.0 (legacy default), hardened for real rooms:
        // a trigger clap is a LOUD onset — it needs a peak spike (≥3×
        // floor), a sharp frame-to-frame jump (≥4×), AND a high absolute
        // peak (≥ 3000 ≈ −21dBFS). The absolute floor is the key
        // discriminator: household impulses (door thumps, coughs, desk
        // knocks land 2000–3000 near a laptop mic) pass every ratio test, so
        // ratios alone start transcription "on even a little sound". A
        // deliberate hand clap near the mic hits 5000+; everyday knocks stay
        // below 3000 and are blocked outright.
        // Escape hatch: an extremely loud onset (≥10× floor, abs ≥ 4000,
        // jump ≥ 2.5×, wide burst) fires on peak strength so close-range
        // claps over a loud background are never missed. The hatch
        // deliberately also requires a jump AND width — pure peak tests stay
        // true forever on loud sustained audio and chatter every chunk.
        let peak_triggered = peak_over_floor >= 3.0 && max_abs >= 3000.0;
        let jump_triggered = rms_jump >= 4.0 && max_abs >= 3000.0;
        let wide_triggered = width >= MIN_BURST_WIDTH;
        let bang_triggered =
            peak_over_floor >= 10.0 && max_abs >= 4000.0 && rms_jump >= 2.5;
        // Voiced veto (throat-clear / high-voice fix): a hand clap is a
        // broadband noise burst (high zero-crossing rate + high
        // sample-to-sample variation vs RMS). Voiced sounds — throat clears,
        // coughs, loud vowel onsets — are harmonic: low ZCR + low spectral
        // ratio. Veto onsets that are strongly voiced on BOTH axes; real
        // claps clear at least one with wide margin. Conservative by design:
        // only clearly-voiced onsets are blocked, so distant/soft claps
        // (noisy, high-ZCR even when quiet) still pass.
        let voiced = zcr < 0.10 && spectral_ratio < 5.0;
        if voiced {
            return None;
        }
        if !(((peak_triggered && jump_triggered) || bang_triggered) && wide_triggered)
        {
            return None;
        }

        // Debounce: one transient per 300ms max.
        let now = Instant::now();
        if let Some(t) = self.last_trigger {
            if now.duration_since(t) < Duration::from_millis(300) {
                return None;
            }
        }

        let kind = TransientKind::Clap;
        self.last_trigger = Some(now);
        self.last_onset = Some((max_abs, peak_over_floor, rms_jump, floor, width));
        // Debug level: room noise brushes these thresholds constantly on hot
        // mics — info here spams the log and looks like phantom triggers.
        // Audible actions log at info at their call sites instead.
        tracing::debug!(
            "Acoustic {}: peak={:.0} peakRatio={:.1} rmsJump={:.1} zcr={:.2} spectral={:.2} floor={:.1}",
            kind.as_str().to_uppercase(),
            max_abs,
            peak_over_floor,
            rms_jump,
            zcr,
            spectral_ratio,
            floor
        );
        Some(kind)
    }
}

impl Default for TransientDetector {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn warmup(d: &mut TransientDetector) {
        let silence = vec![0i16; 1280];
        for _ in 0..6 {
            assert_eq!(d.process(&silence), None);
        }
    }

    #[test]
    fn silence_never_fires() {
        let mut d = TransientDetector::new();
        let silence = vec![0i16; 1280];
        for _ in 0..20 {
            assert_eq!(d.process(&silence), None);
        }
    }

    #[test]
    fn sharp_burst_is_clap() {
        let mut d = TransientDetector::new();
        warmup(&mut d);
        // Sharp burst: alternating ±5000 for 200 samples. Clap-only: any
        // burst passing the onset + width gates is a clap.
        let mut chunk = vec![0i16; 1280];
        for (i, s) in chunk.iter_mut().enumerate().take(200) {
            *s = if i % 2 == 0 { 5000 } else { -5000 };
        }
        assert_eq!(d.process(&chunk), Some(TransientKind::Clap));
        // Debounce: immediate repeat is swallowed.
        assert_eq!(d.process(&chunk), None);
    }

    #[test]
    fn short_loud_click_never_fires() {
        let mut d = TransientDetector::new();
        warmup(&mut d);
        // Loud but ultra-short impulse (40 samples ≈ 2.5ms at ±5000, e.g. a
        // mouse click on a hot mic): passes every level test but carries no
        // burst body, so the width gate must block it.
        let mut click = vec![0i16; 1280];
        for (i, s) in click.iter_mut().enumerate().take(40) {
            *s = if i % 2 == 0 { 5000 } else { -5000 };
        }
        for _ in 0..4 {
            std::thread::sleep(std::time::Duration::from_millis(350));
            assert_eq!(d.process(&click), None, "short click must not fire");
        }
    }

    #[test]
    fn quiet_tap_never_fires() {        let mut d = TransientDetector::new();
        warmup(&mut d);
        // Soft everyday sounds (±300 ≈ −41dBFS) must NEVER fire, not even as
        // an onset: only deliberate loud claps (abs ≥ 3000) may trigger.
        let mut tap = vec![0i16; 1280];
        for (i, s) in tap.iter_mut().enumerate().take(100) {
            *s = if i % 2 == 0 { 300 } else { -300 };
        }
        for _ in 0..4 {
            std::thread::sleep(std::time::Duration::from_millis(350));
            assert_eq!(d.process(&tap), None, "quiet tap must not fire");
        }
    }

    #[test]
    fn broad_burst_is_clap() {
        let mut d = TransientDetector::new();
        warmup(&mut d);
        // Broad low-ZCR thump without snap is a door-thump, NOT a clap: the
        // voiced veto (zcr < 0.10 + spectral < 5.0) must block it even though
        // it passes every level/width gate. Real hand claps always carry a
        // broadband snap (see noisy_clap_passes_voiced_veto below).
        let mut chunk = vec![0i16; 1280];
        for s in chunk.iter_mut().take(120) {
            *s = 6000;
        }
        for _ in 0..4 {
            std::thread::sleep(std::time::Duration::from_millis(350));
            assert_eq!(d.process(&chunk), None, "door-thump must not fire");
        }
    }

    #[test]
    fn noisy_clap_passes_voiced_veto() {
        let mut d = TransientDetector::new();
        warmup(&mut d);
        // Hand clap: loud burst WITH broadband snap (alternating ±5500 for
        // 200 samples ≈ 12.5ms body): high ZCR + high spectral ratio, so the
        // voiced veto must let it through to the normal gates.
        let mut chunk = vec![0i16; 1280];
        for (i, s) in chunk.iter_mut().enumerate().take(200) {
            *s = if i % 2 == 0 { 5500 } else { -5500 };
        }
        assert_eq!(d.process(&chunk), Some(TransientKind::Clap));
    }

    #[test]
    fn voiced_throat_clear_never_fires() {
        let mut d = TransientDetector::new();
        warmup(&mut d);
        // Throat-clear model: loud (6000 peak) low-frequency harmonic burst
        // (110 Hz-ish square-ish wave, 300 samples ≈ 19ms body): passes peak,
        // jump and width gates, but zcr (~0.01) + spectral ratio (< 5) mark
        // it voiced, so the veto must block it.
        let mut chunk = vec![0i16; 1280];
        for (i, s) in chunk.iter_mut().enumerate().take(300) {
            let phase = (i as f32 * 110.0 * 2.0 * std::f32::consts::PI / 16000.0).sin();
            *s = (phase * 6000.0) as i16;
        }
        for _ in 0..4 {
            std::thread::sleep(std::time::Duration::from_millis(350));
            assert_eq!(d.process(&chunk), None, "voiced throat-clear must not fire");
        }
    }

    #[test]
    fn mid_level_knock_never_fires() {
        let mut d = TransientDetector::new();
        warmup(&mut d);
        // Household knock (±2500 ≈ −22dBFS with burst body): passes every
        // ratio test but sits below the deliberate-clap absolute floor
        // (3000), so it must never fire — not even the onset.
        let mut knock = vec![0i16; 1280];
        for (i, s) in knock.iter_mut().enumerate().take(150) {
            *s = if i % 2 == 0 { 2500 } else { -2500 };
        }
        for _ in 0..4 {
            std::thread::sleep(std::time::Duration::from_millis(350));
            assert_eq!(d.process(&knock), None, "sub-floor knock must not fire");
        }
    }

    #[test]
    fn sustained_tone_does_not_chatter() {
        let mut d = TransientDetector::new();
        warmup(&mut d);
        // Sustained 1500-peak tone: below the deliberate-clap absolute floor
        // (3000), so it must never fire — not even the onset. Steady-state
        // is doubly blocked by the jump gate.
        let tone: Vec<i16> = (0..1280)
            .map(|i| {
                ((2.0 * std::f32::consts::PI * 440.0 * i as f32 / 16000.0).sin() * 1500.0)
                    as i16
            })
            .collect();
        for _ in 0..8 {
            // Open a fresh debounce window each round so the gates — not the
            // timer — are what must hold.
            std::thread::sleep(std::time::Duration::from_millis(350));
            assert_eq!(d.process(&tone), None, "sub-threshold tone must not fire");
        }
    }

    #[test]
    fn arbiter_confirms_isolated_but_vetoes_sustained() {
        let mut a = TransientArbiter::new();
        // Isolated: loud wide onset then quiet narrow tail → fires.
        assert_eq!(a.update(Some(TransientKind::Clap), 5000.0, 300), None);
        assert_eq!(
            a.update(None, 800.0, 60),
            Some(TransientKind::Clap),
            "isolated impulse must confirm"
        );
        // Sustained: onset then still loud → vetoed.
        assert_eq!(a.update(Some(TransientKind::Clap), 5000.0, 300), None);
        assert_eq!(
            a.update(None, 4000.0, 250),
            None,
            "sustained sound must veto the onset"
        );
        // After a veto the arbiter is empty, not stuck.
        assert_eq!(a.update(None, 100.0, 10), None);
    }

    #[test]
    fn arbiter_vetoes_loud_onset_with_energetic_pause() {
        let mut a = TransientArbiter::new();
        // Voice-high phantom (from the live log): onset 8344/w192, pause
        // 1305/w523 — passes the 40% level test (1305 < 3337) but the pause
        // still carries a wider energy body than the snap: veto.
        assert_eq!(a.update(Some(TransientKind::Clap), 8344.0, 192), None);
        assert_eq!(
            a.update(None, 1305.0, 523),
            None,
            "loud onset + energetic pause must not fire"
        );
        // Real clap: wide burst, decayed narrow tail → fires.
        assert_eq!(a.update(Some(TransientKind::Clap), 6000.0, 400), None);
        assert_eq!(
            a.update(None, 900.0, 80),
            Some(TransientKind::Clap),
            "decayed tail must confirm"
        );
    }
}
