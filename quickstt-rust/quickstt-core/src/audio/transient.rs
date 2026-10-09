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

/// Two-chunk confirmation arbiter: a detected onset is HELD, not acted on.
/// It only becomes actionable if the next TWO chunks decay like an isolated
/// impulse (a clap) — fast collapse, not sustained voice energy:
///   1. chunk+1 peak < 50% of the onset peak AND burst width <= 3x the onset
///      width (lively-room reverb keeps tails wide and hot: 25%/1x vetoed
///      real claps all day — 34% ratios and 2x widths on genuine claps).
///      Clipped onsets (>= 32000 — the mic rail, ratios meaningless) use an
///      absolute < 20000 bar instead.
///   2. chunk+2 peak < 25% of the onset peak (the voice-pause phantom killer:
///      a plosive+vowel passes leg 1 on a narrow pause, but the vowel still
///      roars at +160ms while clap reverb has collapsed).
/// Costs two chunks (~160ms) of latency — imperceptible for a clap trigger.
#[derive(Default)]
pub struct TransientArbiter {
    pending: Option<(TransientKind, f64, usize)>,
    /// First confirmation chunk held while awaiting the second.
    confirm1: Option<(TransientKind, f64)>,
}

impl TransientArbiter {
    pub fn new() -> Self {
        Self {
            pending: None,
            confirm1: None,
        }
    }

    pub fn reset(&mut self) {
        self.pending = None;
        self.confirm1 = None;
    }

    /// Feed this chunk's detector result + peak + burst width. Returns the
    /// kind to ACT on (confirmed two chunks back), if any.
    pub fn update(
        &mut self,
        detected: Option<TransientKind>,
        chunk_peak: f64,
        chunk_width: usize,
    ) -> Option<TransientKind> {
        let mut fire = None;
        // Leg 2: the chunk after a passed leg 1 must have collapsed.
        if let Some((kind, onset_peak)) = self.confirm1.take() {
            if chunk_peak < onset_peak * 0.25 {
                fire = Some(kind);
            } else {
                // Vetoed: energy sustained into +160ms (vowel after a voice
                // onset) — not an isolated clap.
                tracing::info!(
                    "Clap onset vetoed at leg 2 (tail peak={:.0} vs onset={:.0})",
                    chunk_peak,
                    onset_peak,
                );
            }
        }
        // Leg 1: the chunk after the onset must decay fast in level and
        // stay compact in shape.
        if let Some((kind, onset_peak, onset_width)) = self.pending.take() {
            let clipped = onset_peak >= 32000.0;
            let quiet_level = if clipped {
                chunk_peak < 20000.0
            } else {
                chunk_peak < onset_peak * 0.5
            };
            let decayed_shape = chunk_width <= onset_width.saturating_mul(3).max(1);
            if quiet_level && decayed_shape {
                self.confirm1 = Some((kind, onset_peak));
            } else {
                // Vetoed: sustained sound — not an isolated clap.
                tracing::info!(
                    "Clap onset vetoed at leg 1 (follow-up peak={:.0} vs onset={:.0}, width={} vs onset={})",
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
        // Isolated: loud onset, decayed leg 1, collapsed leg 2 → fires on
        // the third chunk (two-chunk confirmation).
        assert_eq!(a.update(Some(TransientKind::Clap), 5000.0, 300), None);
        assert_eq!(a.update(None, 800.0, 60), None, "leg 1 only holds");
        assert_eq!(
            a.update(None, 100.0, 10),
            Some(TransientKind::Clap),
            "isolated impulse must confirm"
        );
        // Sustained: onset then still loud → vetoed at leg 1.
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
        // 1305/w523 — passes leg 1 (1305 < 4172, 523 <= 576) but the vowel
        // still roars at +160ms: vetoed at leg 2.
        assert_eq!(a.update(Some(TransientKind::Clap), 8344.0, 192), None);
        assert_eq!(a.update(None, 1305.0, 523), None, "leg 1 holds");
        assert_eq!(
            a.update(None, 8000.0, 500),
            None,
            "sustained vowel at +160ms must veto"
        );
        // Real clap: wide burst, decayed narrow tail → fires.
        assert_eq!(a.update(Some(TransientKind::Clap), 6000.0, 400), None);
        assert_eq!(a.update(None, 900.0, 80), None, "leg 1 holds");
        assert_eq!(
            a.update(None, 200.0, 20),
            Some(TransientKind::Clap),
            "decayed tail must confirm"
        );
    }

    #[test]
    fn arbiter_confirms_lively_room_claps() {
        // The three real claps vetoed by the old 25%/1x rule (live log):
        // reverb keeps leg-1 tails hot and wide — the two-chunk rule
        // must fire on all of them.
        let mut a = TransientArbiter::new();
        assert_eq!(a.update(Some(TransientKind::Clap), 3446.0, 186), None);
        assert_eq!(a.update(None, 465.0, 413), None);
        assert_eq!(
            a.update(None, 100.0, 50),
            Some(TransientKind::Clap),
            "reverb-wide tail must still confirm"
        );
        assert_eq!(a.update(Some(TransientKind::Clap), 4208.0, 122), None);
        assert_eq!(a.update(None, 1441.0, 69), None);
        assert_eq!(
            a.update(None, 300.0, 40),
            Some(TransientKind::Clap),
            "hot early reflection must still confirm"
        );
        // Clipped mic rail: ratios meaningless, absolute leg-1 bar applies.
        assert_eq!(a.update(Some(TransientKind::Clap), 32767.0, 211), None);
        assert_eq!(a.update(None, 17795.0, 155), None);
        assert_eq!(
            a.update(None, 3000.0, 100),
            Some(TransientKind::Clap),
            "clipped onset must confirm on collapsed leg 2"
        );
    }
}
