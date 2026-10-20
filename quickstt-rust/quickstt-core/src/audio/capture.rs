use crate::audio::conditioner::InputConditioner;
use crate::audio::normalize::InputNormalizer;
use anyhow::{Context, Result};
use std::sync::atomic::{AtomicU8, Ordering};
use std::sync::Arc;
use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use cpal::{SampleFormat, Stream, StreamConfig};
use tokio::sync::mpsc;

/// List OS input (microphone) device names in enumeration order.
pub fn list_input_devices() -> Vec<String> {
    let host = cpal::default_host();
    let mut names = Vec::new();
    if let Ok(devices) = host.input_devices() {
        for d in devices {
            if let Ok(n) = d.name() {
                let n = n.trim().to_string();
                if !n.is_empty() && !names.contains(&n) {
                    names.push(n);
                }
            }
        }
    }
    // OS default first (when present) so "System default" ~= names[0].
    if let Some(def) = host.default_input_device() {
        if let Ok(dn) = def.name() {
            if let Some(i) = names.iter().position(|n| n == &dn) {
                names.remove(i);
                names.insert(0, dn);
            }
        }
    }
    names
}

/// Resolve a preferred microphone: "" / "System default" (or unknown names)
/// fall back to the OS default input.
pub fn resolve_input_device(host: &cpal::Host, preferred: &str) -> Option<cpal::Device> {
    let preferred = preferred.trim();
    if !preferred.is_empty() && !preferred.eq_ignore_ascii_case("System default") {
        if let Ok(devices) = host.input_devices() {
            for d in devices {
                if d.name().map(|n| n == preferred).unwrap_or(false) {
                    return Some(d);
                }
            }
        }
        tracing::warn!(
            "Preferred microphone {:?} not found — falling back to system default",
            preferred
        );
    }
    host.default_input_device()
}
/// Rolling diagnostic for the foreground capture: how many chunks arrived
/// and how loud they were. A session that records nothing shows up here
/// as "0 chunks" instead of a silent mystery.
#[derive(Default)]
struct CaptureDiag {
    chunks: u64,
    peak: i16,
}

impl CaptureDiag {
    fn note(&mut self, chunk: &[i16]) {
        self.chunks += 1;
        for s in chunk {
            let a = s.abs();
            if a > self.peak {
                self.peak = a;
            }
        }
        if self.chunks % 50 == 0 {
            tracing::info!(
                "Foreground capture: {} chunks, peak={}",
                self.chunks,
                self.peak
            );
            self.peak = 0;
        }
    }
}

pub struct AudioCaptureManager {
    wakeword_stream: Option<Stream>,
    transcription_stream: Option<Stream>,
    host: cpal::Host,
}

impl AudioCaptureManager {
    pub fn new() -> Self {
        let host = cpal::default_host();
        Self {
            wakeword_stream: None,
            transcription_stream: None,
            host,
        }
    }

    /// Starts a lightweight microphone stream and normalizes it to 16 kHz mono i16.
    /// `preferred` is an OS device name ("" = system default).
    pub fn start_wakeword_stream(
        &mut self,
        wakeword_tx: mpsc::Sender<Vec<i16>>,
        preferred: &str,
    ) -> Result<()> {
        let device = resolve_input_device(&self.host, preferred)
            .context("No input device available")?;
        tracing::info!(
            "Capture device: {:?}",
            device.name().unwrap_or_else(|_| "<unnamed>".into())
        );

        let supported = device
            .default_input_config()
            .context("No default input config available")?;
        let sample_format = supported.sample_format();
        let config: cpal::StreamConfig = supported.into();
        let channels = config.channels;
        let sample_rate = config.sample_rate.0;

        let err_fn = |err| tracing::error!("Wakeword audio stream error: {}", err);
        // Input health of the foreground stream: a railed or dead mic must
        // be reported instead of silently producing an empty transcript.
        let health: Arc<AtomicU8> = Arc::new(AtomicU8::new(crate::audio::conditioner::HEALTH_OK));
        let stream = match sample_format {
            SampleFormat::F32 => {
                let mut normalizer = InputNormalizer::new(channels, sample_rate);
                let h = Arc::clone(&health);
                let mut diag = CaptureDiag::default();
                device.build_input_stream(
                    &config,
                    move |data: &[f32], _: &_| {
                        normalizer.process_f32(data, |chunk| {
                            diag.note(&chunk);
                            let _ = wakeword_tx.try_send(chunk);
                        });
                        let state = normalizer.health();
                        if state != h.load(Ordering::Relaxed) {
                            h.store(state, Ordering::Relaxed);
                            crate::audio::conditioner::warn_unhealthy(state);
                        }
                    },
                    err_fn,
                    None,
                )?
            }
            SampleFormat::I16 => {
                let mut normalizer = InputNormalizer::new(channels, sample_rate);
                let h = Arc::clone(&health);
                let mut diag = CaptureDiag::default();
                device.build_input_stream(
                    &config,
                    move |data: &[i16], _: &_| {
                        normalizer.process_i16(data, |chunk| {
                            diag.note(&chunk);
                            let _ = wakeword_tx.try_send(chunk);
                        });
                        let state = normalizer.health();
                        if state != h.load(Ordering::Relaxed) {
                            h.store(state, Ordering::Relaxed);
                            crate::audio::conditioner::warn_unhealthy(state);
                        }
                    },
                    err_fn,
                    None,
                )?
            }
            SampleFormat::U16 => {
                let mut normalizer = InputNormalizer::new(channels, sample_rate);
                let h = Arc::clone(&health);
                let mut diag = CaptureDiag::default();
                device.build_input_stream(
                    &config,
                    move |data: &[u16], _: &_| {
                        normalizer.process_u16(data, |chunk| {
                            diag.note(&chunk);
                            let _ = wakeword_tx.try_send(chunk);
                        });
                        let state = normalizer.health();
                        if state != h.load(Ordering::Relaxed) {
                            h.store(state, Ordering::Relaxed);
                            crate::audio::conditioner::warn_unhealthy(state);
                        }
                    },
                    err_fn,
                    None,
                )?
            }
            SampleFormat::I32 => {
                let mut normalizer = InputNormalizer::new(channels, sample_rate);
                let h = Arc::clone(&health);
                let mut diag = CaptureDiag::default();
                device.build_input_stream(
                    &config,
                    move |data: &[i32], _: &_| {
                        normalizer.process_i32(data, |chunk| {
                            diag.note(&chunk);
                            let _ = wakeword_tx.try_send(chunk);
                        });
                        let state = normalizer.health();
                        if state != h.load(Ordering::Relaxed) {
                            h.store(state, Ordering::Relaxed);
                            crate::audio::conditioner::warn_unhealthy(state);
                        }
                    },
                    err_fn,
                    None,
                )?
            }
            other => anyhow::bail!("Unsupported input sample format: {}", other),
        };

        stream.play()?;
        self.wakeword_stream = Some(stream);
        tracing::info!(
            "Input stream started: {:?}, {} channel(s), {} Hz -> 16 kHz mono",
            sample_format,
            channels,
            sample_rate
        );
        Ok(())
    }

    /// Stops the wakeword stream. Used to free resources when transcription takes over.
    pub fn stop_wakeword_stream(&mut self) {
        if let Some(stream) = self.wakeword_stream.take() {
            let _ = stream.pause();
        }
        tracing::info!("Wakeword audio stream stopped.");
    }

    /// Starts the full quality stream for whisper-rs transcription.
    /// This is strictly lazy-loaded ONLY after a wakeword fires.
    pub fn start_transcription_stream(
        &mut self,
        transcription_tx: mpsc::Sender<Vec<f32>>,
        preferred: &str,
    ) -> Result<()> {
        // Honor the microphone the user picked in the dashboard — the
        // whisper path used to open the OS default regardless, so the
        // selection only half-worked (the foreground stream used it, the
        // engine stream did not).
        let device = resolve_input_device(&self.host, preferred)
            .context("No input device available")?;
        tracing::info!(
            "Transcription capture device: {:?}",
            device.name().unwrap_or_else(|_| "<unnamed>".into())
        );

        let config = StreamConfig {
            channels: 1,
            sample_rate: cpal::SampleRate(16000), // whisper.cpp standard
            buffer_size: cpal::BufferSize::Default,
        };

        // Here dasp ring buffers accumulate exactly 2-4 seconds of audio to prevent RAM ballooning.
        // The samples are conditioned first (DC removal + soft limiting): a
        // railed mic otherwise hands whisper a wall of ±32767 and the
        // transcript is noise.
        //
        // Some devices refuse 16 kHz mono (a USB headset locked at 48 kHz
        // stereo, for example) — then fall back to the device's native
        // format and resample on the way in.
        let mut cond = InputConditioner::new(16_000);
        let tx = transcription_tx.clone();
        let stream = match device.build_input_stream(
            &config,
            move |data: &[f32], _: &_| {
                let conditioned: Vec<f32> = data
                    .iter()
                    .map(|s| cond.condition(s * i16::MAX as f32) / i16::MAX as f32)
                    .collect();
                cond.end_chunk(data.len());
                // Send chunks to transcription task
                let _ = tx.try_send(conditioned);
            },
            |err| tracing::error!("Transcription audio stream error: {}", err),
            None,
        ) {
            Ok(stream) => stream,
            Err(first_err) => {
                tracing::warn!(
                    "16 kHz mono capture refused by the device ({first_err}) — using its native format"
                );
                let native = device
                    .default_input_config()
                    .context("No default input config available")?;
                let sample_format = native.sample_format();
                let config: cpal::StreamConfig = native.into();
                let channels = config.channels;
                let rate = config.sample_rate.0;
                let mut normalizer = InputNormalizer::new(channels, rate);
                let mut cond = InputConditioner::new(16_000);
                let tx = transcription_tx.clone();
                match sample_format {
                    SampleFormat::F32 => device.build_input_stream(
                        &config,
                        move |data: &[f32], _: &_| {
                            let mut out: Vec<f32> = Vec::new();
                            normalizer.process_f32(data, |chunk| {
                                for s in chunk {
                                    out.push(s as f32 / i16::MAX as f32);
                                }
                            });
                            cond.end_chunk(out.len());
                            let _ = tx.try_send(out);
                        },
                        |err| tracing::error!("Transcription audio stream error: {}", err),
                        None,
                    )?,
                    SampleFormat::I16 => device.build_input_stream(
                        &config,
                        move |data: &[i16], _: &_| {
                            let mut out: Vec<f32> = Vec::new();
                            normalizer.process_i16(data, |chunk| {
                                for s in chunk {
                                    out.push(s as f32 / i16::MAX as f32);
                                }
                            });
                            cond.end_chunk(out.len());
                            let _ = tx.try_send(out);
                        },
                        |err| tracing::error!("Transcription audio stream error: {}", err),
                        None,
                    )?,
                    other => anyhow::bail!("Unsupported input sample format: {other}"),
                }
            }
        };

        stream.play()?;
        self.transcription_stream = Some(stream);
        tracing::info!("High-quality transcription audio stream started.");
        Ok(())
    }

    /// Stops transcription and drops the stream to return to idle RAM usage.
    pub fn stop_transcription_stream(&mut self) {
        if let Some(stream) = self.transcription_stream.take() {
            let _ = stream.pause();
        }
        tracing::info!("Transcription audio stream stopped.");
    }
}
