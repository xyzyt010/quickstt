//! Microphone input conditioning: DC removal, soft limiting, saturation watch.
//!
//! # Why this exists
//!
//! A microphone whose capture gain is cranked up — or whose input is left
//! unterminated — delivers a *railed* signal: the ADC sits at the digital
//! rail and the stream is mostly `+32767` with a little noise around it.
//! Such a stream is not speech, but every consumer of it misreads it:
//!
//! * the VAD reports continuous speech (`vad=true` forever),
//! * the wakeword head scores ~0.000 — the wake word never fires,
//! * the transient detector sees random onsets in the noise — the app
//!   starts dictating on nothing ("it keeps starting transcription
//!   unprovoked"),
//! * whisper transcribes the noise floor — "the STT does not work".
//!
//! The conditioner fixes what can be fixed and reports what cannot:
//!
//! * a one-pole 20 Hz high-pass removes the DC offset, so a railed input
//!   collapses back to its residual signal instead of a solid wall;
//! * a soft limiter (`tanh`) keeps the conditioned signal inside the
//!   usable range instead of hard-clipping;
//! * the raw rail fraction is tracked over a rolling window, so callers
//!   can *refuse to trigger* while the input is saturated — a broken mic
//!   must never start a dictation.
//!
//! The path is shared by the background wakeword stream and the foreground
//! transcription stream, so both see the same conditioned signal.

/// Samples at or beyond this magnitude count as "at the rail".
pub const RAIL: i32 = 32_000;

/// Soft-limiter ceiling (i16 scale). A little below full scale so the
/// limiter still has headroom to round the top off smoothly.
pub const CEILING: f32 = 30_000.0;

/// High-pass corner: 20 Hz — removes the DC offset, keeps the speech band.
const HP_CUTOFF_HZ: f32 = 20.0;

/// Rolling window length (in chunks) for the saturation estimate.
/// 16 chunks × 80 ms ≈ 1.3 s.
const WINDOW: usize = 16;

/// Rail fraction of the rolling window above which the input counts as
/// saturated. A single loud clap clips a handful of samples (<1 %); a
/// railed mic clips nearly everything.
const SATURATED_FRACTION: f32 = 0.05;

/// Chunks to wait before the saturation verdict is meaningful.
const WARMUP_CHUNKS: u32 = 4;

/// Input health: the signal is usable.
pub const HEALTH_OK: u8 = 0;
/// Input health: the signal is railed (over-driven / unterminated mic).
pub const HEALTH_SATURATED: u8 = 1;
/// Input health: the device delivers nothing at all (dead mic / muted).
pub const HEALTH_DEAD: u8 = 2;

/// Throttled warning for an unusable input (railed or dead). The user
/// needs the actionable hint — not a log flood.
pub fn warn_unhealthy(health: u8) {
    use std::sync::atomic::{AtomicU64, Ordering as AOrd};
    static LAST_SAT: AtomicU64 = AtomicU64::new(0);
    static LAST_DEAD: AtomicU64 = AtomicU64::new(0);
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0);
    match health {
        HEALTH_SATURATED => {
            let last = LAST_SAT.load(AOrd::Relaxed);
            if now.saturating_sub(last) < 5_000 || now < last {
                return;
            }
            LAST_SAT.store(now, AOrd::Relaxed);
            tracing::warn!(
                "Microphone input is CLIPPED (saturated): the level is pinned at the digital rail. \
                 Wakeword and clap detection are paused until the input is usable. \
                 Lower the input gain (Linux: 'pactl set-source-volume <source> 50%' / \
                 'amixer -c 0 set Capture 20%'; Windows: Settings > Sound > Input) and restart QuickSTT."
            );
        }
        HEALTH_DEAD => {
            let last = LAST_DEAD.load(AOrd::Relaxed);
            if now.saturating_sub(last) < 10_000 || now < last {
                return;
            }
            LAST_DEAD.store(now, AOrd::Relaxed);
            tracing::warn!(
                "No audio is arriving from the microphone (the input device delivers only silence). \
                 Wakeword and clap detection are paused. Check that the right input is selected \
                 (Dashboard > Microphone), that it is not muted, and that the device works \
                 (Linux: 'pactl list sources' / 'arecord -D default -d 3 /tmp/t.wav')."
            );
        }
        _ => {}
    }
}

/// Conditioner state for one microphone stream.
pub struct InputConditioner {
    /// One-pole high-pass state (input sample, output sample).
    hp_prev_x: f32,
    hp_prev_y: f32,
    /// High-pass pole for the stream's sample rate.
    hp_r: f32,
    /// Samples at the rail in the chunk being built.
    rail_pending: usize,
    /// Rolling window of per-chunk (clipped, total) sample counts.
    rail_hist: [(usize, usize); WINDOW],
    /// Rolling window of per-chunk raw peak.
    peak_hist: [f32; WINDOW],
    /// Write cursor into `rail_hist`.
    rail_idx: usize,
    /// Chunks processed so far.
    chunks: u32,
    /// Largest |raw| seen in the chunk being built.
    chunk_peak: f32,
}

impl InputConditioner {
    pub fn new(sample_rate: u32) -> Self {
        let fs = sample_rate.max(1) as f32;
        // One-pole high-pass: y[n] = x[n] - x[n-1] + R*y[n-1]
        let r = 1.0 - (2.0 * std::f32::consts::PI * HP_CUTOFF_HZ / fs).min(0.9);
        Self {
            hp_prev_x: 0.0,
            hp_prev_y: 0.0,
            hp_r: r,
            rail_pending: 0,
            rail_hist: [(0, 0); WINDOW],
            peak_hist: [0.0; WINDOW],
            rail_idx: 0,
            chunks: 0,
            chunk_peak: 0.0,
        }
    }

    /// Condition one raw sample.
    ///
    /// `raw` is in the i16 scale (−32768..32767). The returned value is
    /// conditioned the same way. Raw samples at the digital rail are
    /// counted for the saturation estimate — the count is what the caller
    /// uses to decide whether the input can be trusted at all.
    pub fn condition(&mut self, raw: f32) -> f32 {
        if raw.abs() >= RAIL as f32 {
            self.rail_pending += 1;
        }
        let a = raw.abs();
        if a > self.chunk_peak {
            self.chunk_peak = a;
        }
        // DC-blocking one-pole high-pass.
        let y = raw - self.hp_prev_x + self.hp_r * self.hp_prev_y;
        self.hp_prev_x = raw;
        self.hp_prev_y = y;
        // Soft limiter: smooth, never exceeds the ceiling.
        CEILING * (y / CEILING).tanh()
    }

    /// Call once per emitted chunk (the normalizer's 80 ms frames, or
    /// whatever the capture callback delivers).
    pub fn end_chunk(&mut self, chunk_len: usize) {
        self.rail_hist[self.rail_idx] = (self.rail_pending, chunk_len);
        self.peak_hist[self.rail_idx] = self.chunk_peak;
        self.rail_idx = (self.rail_idx + 1) % WINDOW;
        self.rail_pending = 0;
        self.chunk_peak = 0.0;
        self.chunks += 1;
    }

    /// True when the input device delivers nothing at all (a dead mic, a
    /// muted source, a broken driver). The app must say so instead of
    /// pretending to listen to silence.
    pub fn is_dead(&self) -> bool {
        if self.chunks < WARMUP_CHUNKS {
            return false;
        }
        self.peak_hist.iter().all(|p| *p < 1.0)
    }

    /// Fraction of the recent input that sat at the digital rail (0..1).
    pub fn rail_fraction(&self) -> f32 {
        let mut clipped = 0usize;
        let mut total = 0usize;
        for (c, t) in self.rail_hist.iter() {
            clipped += c;
            total += t;
        }
        if total == 0 {
            return 0.0;
        }
        clipped as f32 / total as f32
    }

    /// True while the raw input is saturated — a railed/over-driven mic.
    ///
    /// The verdict is deliberately sticky for a moment: the window is a
    /// second long, so a single clipped chunk does not flip it.
    pub fn saturated(&self) -> bool {
        if self.chunks < WARMUP_CHUNKS {
            return false;
        }
        self.rail_fraction() >= SATURATED_FRACTION
    }

    /// Overall input health (see `HEALTH_*`).
    pub fn health(&self) -> u8 {
        if self.saturated() {
            HEALTH_SATURATED
        } else if self.is_dead() {
            HEALTH_DEAD
        } else {
            HEALTH_OK
        }
    }

    /// Reset the state (new stream / device change).
    pub fn reset(&mut self) {
        self.hp_prev_x = 0.0;
        self.hp_prev_y = 0.0;
        self.rail_pending = 0;
        self.rail_hist = [(0, 0); WINDOW];
        self.peak_hist = [0.0; WINDOW];
        self.rail_idx = 0;
        self.chunks = 0;
        self.chunk_peak = 0.0;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dc_offset_is_removed() {
        let mut c = InputConditioner::new(16_000);
        // A constant +20000 offset (a railed input) must collapse to ~0.
        let mut last = 0.0;
        for _ in 0..400 {
            last = c.condition(20_000.0);
        }
        assert!(last.abs() < 100.0, "DC not removed: {last}");
    }

    #[test]
    fn railed_input_is_reported_as_saturated() {
        let mut c = InputConditioner::new(16_000);
        for _ in 0..8 {
            for _ in 0..1280 {
                c.condition(32_767.0);
            }
            c.end_chunk(1280);
        }
        assert!(c.saturated(), "railed input must be reported saturated");
    }

    #[test]
    fn clean_input_is_not_saturated() {
        let mut c = InputConditioner::new(16_000);
        for i in 0..8 {
            for j in 0..1280 {
                let s = ((i * 1280 + j) as f32 * 0.05).sin() * 3000.0;
                c.condition(s);
            }
            c.end_chunk(1280);
        }
        assert!(!c.saturated(), "clean input must not be reported saturated");
    }

    #[test]
    fn limiter_keeps_signal_inside_the_ceiling() {
        let mut c = InputConditioner::new(16_000);
        let y = c.condition(60_000.0);
        assert!(y <= CEILING + 1.0, "limiter ceiling exceeded: {y}");
        assert!(y > 20_000.0, "limiter crushed the signal: {y}");
    }
}
