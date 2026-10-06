use crate::audio::normalize::InputNormalizer;
use anyhow::{Context, Result};
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
fn resolve_input_device(host: &cpal::Host, preferred: &str) -> Option<cpal::Device> {
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
        let stream = match sample_format {
            SampleFormat::F32 => {
                let mut normalizer = InputNormalizer::new(channels, sample_rate);
                device.build_input_stream(
                    &config,
                    move |data: &[f32], _: &_| {
                        normalizer.process_f32(data, |chunk| {
                            let _ = wakeword_tx.try_send(chunk);
                        })
                    },
                    err_fn,
                    None,
                )?
            }
            SampleFormat::I16 => {
                let mut normalizer = InputNormalizer::new(channels, sample_rate);
                device.build_input_stream(
                    &config,
                    move |data: &[i16], _: &_| {
                        normalizer.process_i16(data, |chunk| {
                            let _ = wakeword_tx.try_send(chunk);
                        })
                    },
                    err_fn,
                    None,
                )?
            }
            SampleFormat::U16 => {
                let mut normalizer = InputNormalizer::new(channels, sample_rate);
                device.build_input_stream(
                    &config,
                    move |data: &[u16], _: &_| {
                        normalizer.process_u16(data, |chunk| {
                            let _ = wakeword_tx.try_send(chunk);
                        })
                    },
                    err_fn,
                    None,
                )?
            }
            SampleFormat::I32 => {
                let mut normalizer = InputNormalizer::new(channels, sample_rate);
                device.build_input_stream(
                    &config,
                    move |data: &[i32], _: &_| {
                        normalizer.process_i32(data, |chunk| {
                            let _ = wakeword_tx.try_send(chunk);
                        })
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
    ) -> Result<()> {
        let device = self
            .host
            .default_input_device()
            .context("No input device available")?;

        let config = StreamConfig {
            channels: 1,
            sample_rate: cpal::SampleRate(16000), // whisper.cpp standard
            buffer_size: cpal::BufferSize::Default,
        };

        // Here dasp ring buffers accumulate exactly 2-4 seconds of audio to prevent RAM ballooning.
        let stream = device.build_input_stream(
            &config,
            move |data: &[f32], _: &_| {
                // Send chunks to transcription task
                let _ = transcription_tx.try_send(data.to_vec());
            },
            |err| tracing::error!("Transcription audio stream error: {}", err),
            None,
        )?;

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
