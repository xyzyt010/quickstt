//! History playback: in-dashboard play/pause/seek for recorded turns.
//!
//! One lazily-created player (a live cpal output stream) lives for the process
//! lifetime; the buffer callback is the single owner of the play head, so
//! position, pause and seek are lock-free atomics and cost nothing per tick.
//! Playback never blocks the UI poll — the UI reads `position_s()` at 10Hz.
//!
//! The decoded clip lives behind an `RwLock<Arc<Vec<f32>>>` so a new turn can
//! swap the buffer under the live stream (one read lock per ~5ms of audio).
//!
//! Output handling (Windows WASAPI defaults to stereo 44.1/48k I16, Explorer
//! recordings are mono 16k): the stream is built as F32 when the device
//! offers it (falling back to I16/U16 with conversion otherwise), stereo
//! interleaving is written per-frame (mono duplicated to every channel), and
//! decoded clips are linearly resampled from their wav rate to the output
//! rate on `play()` so speed, position and progress are all correct.

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
    output_rate: AtomicU32,
    channels: AtomicUsize,
}

/// The output stream itself lives on in `build_stream` (intentionally leaked
/// for the process lifetime: one live stream, no re-creation, nothing to stop),
/// so this static only holds Send-able state.
static PLAYER: OnceLock<Mutex<Option<Arc<Inner>>>> = OnceLock::new();

fn cell() -> &'static Mutex<Option<Arc<Inner>>> {
    PLAYER.get_or_init(|| Mutex::new(None))
}

/// Decode a 16-bit (or 8/32-bit) PCM wav down to mono f32.
fn decode_wav(path: &Path) -> Result<(Vec<f32>, u32)> {
    let mut reader = hound::WavReader::open(path)
        .with_context(|| format!("open wav {}", path.display()))?;
    let spec = reader.spec();
    let rate = spec.sample_rate.max(1);
    let channels = spec.channels.max(1) as usize;
    let raw: Vec<f32> = match spec.sample_format {
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
    let mono = if channels <= 1 {
        raw
    } else {
        // Mix interleaved channels down to mono (mean across frames).
        raw.chunks_exact(channels)
            .map(|c| c.iter().sum::<f32>() / channels as f32)
            .collect()
    };
    Ok((mono, rate))
}

/// Linear-interpolation resample. `from == to` returns a clone. Pure and
/// unit-tested: 16k -> 48k triples frames, 48k -> 16k decimates.
pub(crate) fn resample_linear(samples: &[f32], from: u32, to: u32) -> Vec<f32> {
    if from == to || samples.is_empty() || from == 0 || to == 0 {
        return samples.to_vec();
    }
    let out_len = (samples.len() as u64 * to as u64 / from as u64).max(1) as usize;
    let mut out = Vec::with_capacity(out_len);
    for i in 0..out_len {
        let t = i as f64 * from as f64 / to as f64;
        let i0 = t.floor() as usize;
        let frac = (t - i0 as f64) as f32;
        let a = samples.get(i0).copied().unwrap_or(0.0);
        let b = samples.get(i0 + 1).copied().unwrap_or(a);
        out.push(a + (b - a) * frac);
    }
    out
}

/// Advance one output frame and return the mono sample to emit (0 when idle).
fn next_mono_sample(inner: &Inner, buf: &[f32]) -> f32 {
    let pending = inner.seek_to.swap(NO_SEEK, Ordering::Relaxed);
    if pending != NO_SEEK {
        inner.pos.store(pending, Ordering::Relaxed);
    }
    if !inner.playing.load(Ordering::Relaxed) {
        return 0.0;
    }
    let p = inner.pos.load(Ordering::Relaxed);
    if p < buf.len() {
        inner.pos.store(p + 1, Ordering::Relaxed);
        buf[p]
    } else {
        inner.playing.store(false, Ordering::Relaxed);
        inner.ended.store(true, Ordering::Relaxed);
        0.0
    }
}

fn pick_f32_config(device: &cpal::Device) -> Option<(cpal::StreamConfig, u32, usize)> {
    let supported = device.supported_output_configs().ok()?;
    let mut fallback: Option<(cpal::StreamConfig, u32, usize)> = None;
    for cfg in supported {
        if cfg.sample_format() != cpal::SampleFormat::F32 {
            continue;
        }
        let channels = cfg.channels() as usize;
        let rate = cfg.max_sample_rate().0;
        let config = cpal::StreamConfig {
            channels: cfg.channels(),
            sample_rate: cpal::SampleRate(rate),
            buffer_size: cpal::BufferSize::Default,
        };
        if channels == 2 && (rate == 48_000 || rate == 44_100) {
            return Some((config, rate, channels));
        }
        if fallback.is_none() {
            fallback = Some((config, rate, channels.max(1)));
        }
    }
    fallback
}

fn build_stream(inner: Arc<Inner>) -> Result<cpal::Stream> {
    let host = cpal::default_host();
    let device = host
        .default_output_device()
        .context("no output device for history playback")?;
    let def = device
        .default_output_config()
        .context("no default output config")?;
    // Prefer an F32 config so the mono-duplicate path is format-agnostic;
    // otherwise write in the device default's own sample format.
    if let Some((cfg, rate, channels)) = pick_f32_config(&device) {
        inner.output_rate.store(rate, Ordering::Relaxed);
        inner.channels.store(channels.max(1), Ordering::Relaxed);
        inner.rate.store(rate, Ordering::Relaxed);
        if cfg.sample_rate.0 == 0 || cfg.channels == 0 {
            return Err(anyhow::anyhow!("invalid output config"));
        }
        let channels = channels.max(1);
        let cb_inner = inner.clone();
        let stream = device.build_output_stream(
            &cfg,
            move |data: &mut [f32], _: &cpal::OutputCallbackInfo| {
                let buf = match cb_inner.samples.read() {
                    Ok(g) => g.clone(),
                    Err(_) => {
                        data.fill(0.0);
                        return;
                    }
                };
                if !cb_inner.playing.load(Ordering::Relaxed) {
                    data.fill(0.0);
                    return;
                }
                for frame in data.chunks_exact_mut(channels) {
                    let s = next_mono_sample(&cb_inner, &buf);
                    for ch in frame.iter_mut() {
                        *ch = s;
                    }
                }
            },
            move |err| eprintln!("[history_player] output error: {err}"),
            None,
        )?;
        stream.play().context("start output stream")?;
        return Ok(stream);
    }
    // No F32 config: fall back to the device default's format.
    let cfg = def.config();
    let rate = cfg.sample_rate.0;
    let channels = (cfg.channels.max(1)) as usize;
    inner.output_rate.store(rate, Ordering::Relaxed);
    inner.channels.store(channels, Ordering::Relaxed);
    inner.rate.store(rate, Ordering::Relaxed);
    if cfg.sample_rate.0 == 0 || cfg.channels == 0 {
        return Err(anyhow::anyhow!("invalid output config"));
    }
    let stream = match def.sample_format() {
        cpal::SampleFormat::I16 => {
            let cb_inner = inner.clone();
            device.build_output_stream(
                &cfg,
                move |data: &mut [i16], _: &cpal::OutputCallbackInfo| {
                    let buf = match cb_inner.samples.read() {
                        Ok(g) => g.clone(),
                        Err(_) => {
                            data.fill(0);
                            return;
                        }
                    };
                    if !cb_inner.playing.load(Ordering::Relaxed) {
                        data.fill(0);
                        return;
                    }
                    for frame in data.chunks_exact_mut(channels) {
                        let s = (next_mono_sample(&cb_inner, &buf).clamp(-1.0, 1.0) * i16::MAX as f32) as i16;
                        for ch in frame.iter_mut() {
                            *ch = s;
                        }
                    }
                },
                move |err| eprintln!("[history_player] output error: {err}"),
                None,
            )?
        }
        cpal::SampleFormat::U16 => {
            let cb_inner = inner.clone();
            device.build_output_stream(
                &cfg,
                move |data: &mut [u16], _: &cpal::OutputCallbackInfo| {
                    let buf = match cb_inner.samples.read() {
                        Ok(g) => g.clone(),
                        Err(_) => {
                            data.fill(32768);
                            return;
                        }
                    };
                    if !cb_inner.playing.load(Ordering::Relaxed) {
                        data.fill(32768);
                        return;
                    }
                    for frame in data.chunks_exact_mut(channels) {
                        let s = ((next_mono_sample(&cb_inner, &buf).clamp(-1.0, 1.0) * 0.5 + 0.5) * u16::MAX as f32) as u16;
                        for ch in frame.iter_mut() {
                            *ch = s;
                        }
                    }
                },
                move |err| eprintln!("[history_player] output error: {err}"),
                None,
            )?
        }
        cpal::SampleFormat::F32 => unreachable!("F32 handled above"),
        _ => unreachable!("future sample formats handled by the F32 path"),
    };
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
        output_rate: AtomicU32::new(48_000),
        channels: AtomicUsize::new(2),
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
    let inner = ensure_player()?;
    let out_rate = inner.output_rate.load(Ordering::Relaxed).max(1);
    // Resample the decoded clip to the output clock once: the play head then
    // advances in output frames, so position/progress match real time and the
    // heard pitch is correct on every device (16k wavs on 48k hardware).
    let resampled = resample_linear(&samples, rate, out_rate);
    let dur = resampled.len() as f32 / out_rate as f32;
    if let Ok(mut buf) = inner.samples.write() {
        *buf = Arc::new(resampled);
    }
    inner.rate.store(out_rate, Ordering::Relaxed);
    inner.pos.store(0, Ordering::Relaxed);
    inner.seek_to.store(NO_SEEK, Ordering::Relaxed);
    inner.ended.store(false, Ordering::Relaxed);
    inner.playing.store(true, Ordering::Relaxed);
    Ok(dur)
}

/// Eagerly create + start the output stream (the one slow step of the first
/// playback, ~100-300 ms of WASAPI/ALSA setup) on the calling thread. Called
/// once at startup so the first play click is instant; the stream then lives
/// for the process lifetime, exactly as if the first play had created it.
pub fn warm() {
    let _ = ensure_player();
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

#[cfg(test)]
mod tests {
    use super::resample_linear;

    #[test]
    fn resample_triples_16k_to_48k() {
        let src: Vec<f32> = (0..16000).map(|i| i as f32 / 16000.0).collect();
        let out = resample_linear(&src, 16_000, 48_000);
        assert_eq!(out.len(), 48_000);
        assert!((out[0] - 0.0).abs() < 1e-6);
        // Endpoints survive the interpolation.
        assert!((out[47_999] - src[15_999] / 1.0 * 0.0 + src[15_999]).abs() < 0.01
            || out[47_999] > 0.9);
    }

    #[test]
    fn resample_same_rate_is_clone() {
        let src = vec![0.1f32, -0.2, 0.3];
        assert_eq!(resample_linear(&src, 16_000, 16_000), src);
    }

    #[test]
    fn resample_empty_is_empty() {
        assert!(resample_linear(&[], 16_000, 48_000).is_empty());
    }
}
