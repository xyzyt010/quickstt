//! History playback: in-dashboard play/pause/seek for recorded turns.
//!
//! One lazily-created player (a live cpal output stream) lives for the process
//! lifetime; the buffer callback is the single owner of the play head, so
//! position, pause and seek are lock-free atomics and cost nothing per tick.
//! Playback never blocks the UI poll — the UI reads `position_s()` at 10Hz.
//!
//! The decoded clip lives behind an `RwLock<Arc<Vec<f32>>>` so a new turn can
//! swap the buffer under the live stream (one read lock per ~5ms of audio).

use anyhow::{Context, Result};
use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use std::path::Path;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, OnceLock, RwLock};

const NO_SEEK: usize = usize::MAX;

struct Inner {
    samples: RwLock<Arc<Vec<f32>>>,
    rate: AtomicU32,
    pos: AtomicUsize,
    seek_to: AtomicUsize,
    playing: AtomicBool,
    ended: AtomicBool,
}

/// The output stream itself lives on in `build_stream` (intentionally leaked
/// for the process lifetime: one live stream, no re-creation, nothing to stop),
/// so this static only holds Send-able state.
static PLAYER: OnceLock<Mutex<Option<Arc<Inner>>>> = OnceLock::new();

fn cell() -> &'static Mutex<Option<Arc<Inner>>> {
    PLAYER.get_or_init(|| Mutex::new(None))
}

/// Decode a 16-bit (or 8/32-bit) PCM wav to mono f32.
fn decode_wav(path: &Path) -> Result<(Vec<f32>, u32)> {
    let mut reader = hound::WavReader::open(path)
        .with_context(|| format!("open wav {}", path.display()))?;
    let spec = reader.spec();
    let rate = spec.sample_rate.max(1);
    let out: Vec<f32> = match spec.sample_format {
        hound::SampleFormat::Float => reader.samples::<f32>().filter_map(|s| s.ok()).collect(),
        hound::SampleFormat::Int => {
            let scale = match spec.bits_per_sample {
                8 => 1.0 / 128.0,
                16 => 1.0 / 32768.0,
                _ => 1.0 / 2_147_483_648.0,
            };
            reader
                .samples::<i32>()
                .map(|s| s.unwrap_or(0) as f32 * scale)
                .collect()
        }
    };
    Ok((out, rate))
}

fn build_stream(inner: Arc<Inner>) -> Result<cpal::Stream> {
    let host = cpal::default_host();
    let device = host
        .default_output_device()
        .context("no output device for history playback")?;
    let supported = device
        .default_output_config()
        .context("no default output config")?;
    let cfg = supported.config();
    let cb_inner = inner.clone();
    let stream = device.build_output_stream(
        &cfg,
        move |data: &mut [f32], _: &cpal::OutputCallbackInfo| {
            // Apply any pending seek first (cheap atomic swap).
            let pending = cb_inner.seek_to.swap(NO_SEEK, Ordering::Relaxed);
            if pending != NO_SEEK {
                cb_inner.pos.store(pending, Ordering::Relaxed);
            }
            if !cb_inner.playing.load(Ordering::Relaxed) {
                for s in data.iter_mut() {
                    *s = 0.0;
                }
                return;
            }
            let buf = match cb_inner.samples.read() {
                Ok(g) => g.clone(),
                Err(_) => {
                    for s in data.iter_mut() {
                        *s = 0.0;
                    }
                    return;
                }
            };
            let total = buf.len();
            for s in data.iter_mut() {
                let p = cb_inner.pos.load(Ordering::Relaxed);
                if p < total {
                    *s = buf[p];
                    cb_inner.pos.store(p + 1, Ordering::Relaxed);
                } else {
                    cb_inner.playing.store(false, Ordering::Relaxed);
                    cb_inner.ended.store(true, Ordering::Relaxed);
                    *s = 0.0;
                }
            }
        },
        move |err| eprintln!("[history_player] output error: {err}"),
        None,
    )?;
    stream.play().context("start output stream")?;
    Ok(stream)
}

fn ensure_player() -> Result<Arc<Inner>> {
    let mut guard = cell().lock().map_err(|_| anyhow::anyhow!("player lock poisoned"))?;
    if let Some(inner) = guard.as_ref() {
        return Ok(inner.clone());
    }
    let inner = Arc::new(Inner {
        samples: RwLock::new(Arc::new(Vec::new())),
        rate: AtomicU32::new(16_000),
        pos: AtomicUsize::new(0),
        seek_to: AtomicUsize::new(NO_SEEK),
        playing: AtomicBool::new(false),
        ended: AtomicBool::new(true),
    });
    let stream = build_stream(inner.clone())?;
    // The stream stays alive for the process lifetime: playback is gated by
    // the `playing` flag, and nothing ever needs to stop or rebuild it.
    std::mem::forget(stream);
    *guard = Some(inner.clone());
    Ok(inner)
}

/// Play `path` from the start; re-playing the clip that is already loaded
/// restarts it (the UI's pause button uses [`pause`] instead).
pub fn play(path: &Path) -> Result<f32> {
    let (samples, rate) = decode_wav(path)?;
    let dur = samples.len() as f32 / rate as f32;
    let inner = ensure_player()?;
    if let Ok(mut buf) = inner.samples.write() {
        *buf = Arc::new(samples);
    }
    inner.rate.store(rate, Ordering::Relaxed);
    inner.pos.store(0, Ordering::Relaxed);
    inner.seek_to.store(NO_SEEK, Ordering::Relaxed);
    inner.ended.store(false, Ordering::Relaxed);
    inner.playing.store(true, Ordering::Relaxed);
    Ok(dur)
}

/// Pause, keeping the play head.
pub fn pause() {
    if let Ok(guard) = cell().lock() {
        if let Some(p) = guard.as_ref() {
            p.playing.store(false, Ordering::Relaxed);
        }
    }
}

/// Pause and rewind.
pub fn stop() {
    if let Ok(guard) = cell().lock() {
        if let Some(p) = guard.as_ref() {
            p.playing.store(false, Ordering::Relaxed);
            p.ended.store(true, Ordering::Relaxed);
            p.pos.store(0, Ordering::Relaxed);
        }
    }
}

/// Seek to a fraction (0..1) of the loaded clip.
pub fn seek(fraction: f32) {
    if let Ok(guard) = cell().lock() {
        if let Some(p) = guard.as_ref() {
            let total = p.samples.read().map(|g| g.len()).unwrap_or(0);
            if total > 0 {
                let target = (total.saturating_sub(1) as f32 * fraction.clamp(0.0, 1.0)) as usize;
                p.seek_to.store(target, Ordering::Relaxed);
                p.pos.store(target, Ordering::Relaxed);
                p.ended.store(false, Ordering::Relaxed);
            }
        }
    }
}

fn with_inner<F: FnOnce(&Inner) -> f32>(f: F) -> f32 {
    match cell().lock() {
        Ok(g) => match g.as_ref() {
            Some(p) => f(p),
            None => 0.0,
        },
        Err(_) => 0.0,
    }
}

/// Current play head in seconds (0 when idle).
pub fn position_s() -> f32 {
    with_inner(|i| {
        (i.pos.load(Ordering::Relaxed) as f32 / i.rate.load(Ordering::Relaxed).max(1) as f32)
            .min(duration_of(i))
    })
}

fn duration_of(i: &Inner) -> f32 {
    let len = i.samples.read().map(|g| g.len()).unwrap_or(0);
    len as f32 / i.rate.load(Ordering::Relaxed).max(1) as f32
}

/// Loaded clip length in seconds (0 when nothing is loaded).
#[allow(dead_code)]
pub fn duration_s() -> f32 {
    with_inner(duration_of)
}

/// True while audio is actually being produced.
pub fn is_playing() -> bool {
    match cell().lock() {
        Ok(g) => match g.as_ref() {
            Some(p) => p.playing.load(Ordering::Relaxed) && !p.ended.load(Ordering::Relaxed),
            None => false,
        },
        Err(_) => false,
    }
}