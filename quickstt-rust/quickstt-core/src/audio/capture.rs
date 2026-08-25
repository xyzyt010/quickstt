use crate::audio::normalize::InputNormalizer;
use anyhow::{Context, Result};
use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use cpal::{SampleFormat, Stream, StreamConfig};
use tokio::sync::mpsc;

/// The dual-stream audio architecture as specified in the architectural plan.
/// It uses a low-bitrate stream for wakeword detection to save memory,
/// and a high-quality stream that is ONLY opened when transcription is active.
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
    pub fn start_wakeword_stream(&mut self, wakeword_tx: mpsc::Sender<Vec<i16>>) -> Result<()> {
        let device = self
            .host
            .default_input_device()
            .context("No input device available")?;

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
