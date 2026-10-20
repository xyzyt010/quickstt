//! Microphone input conditioning (Android).
//!
//! Port of the desktop `quickstt-core::audio::conditioner` — the same
//! protection, without the desktop's dependencies:
//!
//! * a one-pole 20 Hz high-pass removes the DC offset (a phone mic with
//!   a hot gain delivers a railed signal; without this the level meter
//!   reads "speech" forever and whisper transcribes a wall of noise),
//! * a soft limiter (`tanh`) keeps the signal inside the usable range
//!   instead of hard-clipping,
//! * the raw rail fraction and the input peak are tracked so the app can
//!   tell the user when the microphone is unusable ("clipped" / "no
//!   audio") instead of silently producing nothing.

/// Samples at or beyond this magnitude (i16 scale) count as "at the rail".
pub const RAIL: f32 = 32_000.0;

/// Soft-limiter ceiling (i16 scale).
pub const CEILING: f32 = 30_000.0;

/// High-pass corner: 20 Hz — removes the DC offset, keeps the speech band.
const HP_CUTOFF_HZ: f32 = 20.0;

/// Rolling window length (in chunks) for the saturation estimate.
const WINDOW: usize = 16;

/// Rail fraction of the rolling window above which the input counts as
/// saturated. A single loud clap clips a handful of samples (<1 %); a
/// railed mic clips nearly everything.
const SATURATED_FRACTION: f32 = 0.05;

/// Chunks to wait before the health verdict is meaningful.
const WARMUP_CHUNKS: u32 = 4;

/// Input health: the signal is usable.
pub const HEALTH_OK: u8 = 0;
/// Input health: the signal is railed (over-driven / clipped mic).
pub const HEALTH_SATURATED: u8 = 1;
/// Input health: the device delivers nothing at all (muted / dead mic).
pub const HEALTH_DEAD: u8 = 2;

/// Conditioner state for one microphone stream.
pub struct Conditioner {
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
    /// Write cursor into the windows.
    rail_idx: usize,
    /// Chunks processed so far.
    chunks: u32,
    /// Largest |raw| seen in the chunk being built.
    chunk_peak: f32,
}

impl Conditioner {
    pub fn new(sample_rate: u32) -> Self {
        let fs = sample_rate.max(1) as f32;
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

    /// Condition one raw sample in the i16 scale (−32768..32767).
    pub fn condition(&mut self, raw: f32) -> f32 {
        if raw.abs() >= RAIL {
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

    /// Call once per emitted chunk.
    pub fn end_chunk(&mut self, chunk_len: usize) {
        self.rail_hist[self.rail_idx] = (self.rail_pending, chunk_len);
        self.peak_hist[self.rail_idx] = self.chunk_peak;
        self.rail_idx = (self.rail_idx + 1) % WINDOW;
        self.rail_pending = 0;
        self.chunk_peak = 0.0;
        self.chunks += 1;
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

    /// True while the raw input is saturated — a clipped/over-driven mic.
    pub fn saturated(&self) -> bool {
        if self.chunks < WARMUP_CHUNKS {
            return false;
        }
        self.rail_fraction() >= SATURATED_FRACTION
    }

    /// True when the input device delivers nothing at all.
    pub fn is_dead(&self) -> bool {
        if self.chunks < WARMUP_CHUNKS {
            return false;
        }
        self.peak_hist.iter().all(|p| *p < 1.0)
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
}
