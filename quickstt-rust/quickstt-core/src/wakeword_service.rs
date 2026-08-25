//! Background wakeword detector.
//!
//! Opens a lightweight 16 kHz mono microphone stream only when the floating
//! widget is hidden, runs [`WakeWordEngine`] inference on every PCM chunk,
//! and — when a wakeword fires — emits an `OrchestratorCommand` so the
//! on-screen widget pops back open.
//!
//! ## Architecture
//!
//! `cpal::Stream` (Windows WASAPI) and the `livekit_wakeword` ONNX model are
//! both `!Send` / `!Sync`. We therefore do ALL of this work on a single
//! dedicated OS thread that runs the service event loop.
//!
//! ```text
//!  ┌────────────────────────────┐    mpsc::SyncChannel<Vec<i16>>    ┌────────────────────────┐
//!  │ cpal audio callback thread │ ───────────────────────────────▶ │  Service thread          │
//!  └────────────────────────────┘                                    │  - WakeWordEngine        │
//!                                                                    │  - inference             │
//!  ┌────────────────────────────┐   mpsc::Sender<ServiceCommand>    │  - classification        │
//!  │  GUI (tokio)               │ ───────────────────────────────▶ │  - emits orch commands   │
//!  └────────────────────────────┘                                    └────────────────────────┘
//! ```
//!
//! On the service thread we use `try_recv` on the audio chunk channel so we
//! can also drain user commands (Start/Stop) without blocking on either.
//! When Stop is called we drop the live stream → mic is released → engine is
//! parked on the service thread awaiting the next Start.

use crate::audio::normalize::{InputNormalizer, TARGET_CHUNK_FRAMES, TARGET_SAMPLE_RATE};
use crate::ml::wakeword::{WakeWordEngine, WakeWordEvent};
use crate::orchestration::OrchestratorCommand;
use crate::wakeword_loader;

use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use cpal::{SampleFormat, Stream};
use std::path::Path;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::sync::Arc;
use tokio::sync::mpsc as tmpsc;
use tracing::{info, warn};

/// Public commands for the wakeword service.
#[derive(Clone, Copy, Debug)]
pub enum WakewordServiceCommand {
    Start,
    Stop,
}

/// Opaque handle returned by [`spawn_background_service`].
#[derive(Clone)]
pub struct WakewordHandle {
    cmd_tx: mpsc::Sender<WakewordServiceCommand>,
    running: Arc<AtomicBool>,
}

impl WakewordHandle {
    pub fn start(&self) {
        let _ = self.cmd_tx.send(WakewordServiceCommand::Start);
    }
    pub fn stop(&self) {
        let _ = self.cmd_tx.send(WakewordServiceCommand::Stop);
    }
    pub fn is_active(&self) -> bool {
        self.running.load(Ordering::Acquire)
    }
}

/// Holds the live cpal stream while inference is running. When this is
/// dropped the OS releases the microphone.
struct LiveStream(#[allow(dead_code)] Stream);

/// Spawn the background wakeword service on its own dedicated thread.
/// Returns `None` if no models were found on disk or model loading failed.
pub fn spawn_background_service(
    models_dir: &Path,
    orchestrator_tx: tmpsc::Sender<OrchestratorCommand>,
) -> Option<WakewordHandle> {
    let discovered = wakeword_loader::discover_models(models_dir);
    if discovered.is_empty() {
        warn!(
            "Wakeword background: no models in {:?} - background detection disabled",
            models_dir
        );
        return None;
    }

    let engine = match WakeWordEngine::from_discovered_models(&discovered) {
        Ok(e) => {
            info!(
                "Wakeword background loaded {} models: {:?}",
                discovered.len(),
                e.phrases()
            );
            e
        }
        Err(e) => {
            warn!(
                "Wakeword background failed to load models: {} - detection disabled",
                e
            );
            return None;
        }
    };

    let (cmd_tx, cmd_rx) = mpsc::channel::<WakewordServiceCommand>();
    let running = Arc::new(AtomicBool::new(false));
    let running_for_thread = Arc::clone(&running);

    if std::thread::Builder::new()
        .name("wakeword-bg".to_string())
        .spawn(move || {
            run_service_thread(engine, cmd_rx, running_for_thread, orchestrator_tx);
        })
        .is_err()
    {
        warn!("Wakeword background: failed to spawn service thread");
        return None;
    }

    Some(WakewordHandle { cmd_tx, running })
}

/// Service thread main loop. Owns the engine and the (optional) live stream
/// for the lifetime of the thread.
fn run_service_thread(
    mut engine: WakeWordEngine,
    cmd_rx: mpsc::Receiver<WakewordServiceCommand>,
    running: Arc<AtomicBool>,
    orchestrator_tx: tmpsc::Sender<OrchestratorCommand>,
) {
    // Live stream is `Some` while running. The channel for audio chunks that
    // the cpal callback pushes into.
    let mut live: Option<(LiveStream, mpsc::Receiver<Vec<i16>>)> = None;

    loop {
        // First, drain any pending Stop events so toggling is responsive.
        // We process commands first if any are ready; otherwise we drain
        // one audio chunk and process it.

        // 1. Drain queued commands (non-blocking).
        match cmd_rx.try_recv() {
            Ok(WakewordServiceCommand::Start) => {
                if live.is_some() {
                    continue;
                }
                match try_open_stream() {
                    Ok((stream, rx)) => {
                        live = Some((stream, rx));
                        running.store(true, Ordering::Release);
                        info!("Wakeword background streaming");
                    }
                    Err(e) => {
                        warn!("Wakeword background failed to open stream: {e}");
                    }
                }
                continue;
            }
            Ok(WakewordServiceCommand::Stop) => {
                if live.take().is_some() {
                    running.store(false, Ordering::Release);
                    info!("Wakeword background stopped");
                }
                continue;
            }
            Err(mpsc::TryRecvError::Disconnected) => {
                // Handle dropped; tear down and exit.
                let _ = live.take();
                running.store(false, Ordering::Release);
                return;
            }
            Err(mpsc::TryRecvError::Empty) => {
                // No commands ready; fall through to audio processing.
            }
        }

        // 2. If streaming, process up to one audio chunk (non-blocking).
        if let Some((_, rx_chunks)) = live.as_ref() {
            match rx_chunks.try_recv() {
                Ok(chunk) => {
                    if process_audio_chunk(&mut engine, &chunk, &orchestrator_tx, &running) {
                        let _ = live.take();
                    }
                }
                Err(mpsc::TryRecvError::Empty) => {
                    // Nothing to do this tick; sleep briefly to spin lightly.
                    std::thread::sleep(std::time::Duration::from_millis(2));
                }
                Err(mpsc::TryRecvError::Disconnected) => {
                    // Stream was dropped; this shouldn't happen unless
                    // somebody reach in and took the live stream out from
                    // under us. Just stop.
                    let _ = live.take();
                    running.store(false, Ordering::Release);
                }
            }
        } else {
            // Idle: block waiting for a command so we don't burn CPU when the
            // service isn't in use.
            match cmd_rx.recv() {
                Ok(WakewordServiceCommand::Start) => match try_open_stream() {
                    Ok((stream, rx)) => {
                        live = Some((stream, rx));
                        running.store(true, Ordering::Release);
                        info!("Wakeword background streaming");
                    }
                    Err(e) => {
                        warn!("Wakeword background failed to open stream: {e}");
                    }
                },
                Ok(WakewordServiceCommand::Stop) => {
                    // Already idle; nothing to do.
                }
                Err(_) => {
                    running.store(false, Ordering::Release);
                    return;
                }
            }
        }
    }
}

fn process_audio_chunk(
    engine: &mut WakeWordEngine,
    chunk: &[i16],
    orchestrator_tx: &tmpsc::Sender<OrchestratorCommand>,
    running: &Arc<AtomicBool>,
) -> bool {
    let events = engine.process_chunk(chunk);
    for event in events {
        if let WakeWordEvent::Triggered(name, confidence) = event {
            info!(
                "Background wakeword fired: '{}' confidence={:.3} - popping widget",
                name, confidence
            );
            let _ = orchestrator_tx.try_send(OrchestratorCommand::WakewordTriggered(confidence));
            let _ = orchestrator_tx.try_send(OrchestratorCommand::ShowWidget);
            let _ = orchestrator_tx.try_send(OrchestratorCommand::StartListening);

            // Stop the mic so transcription can take over; user already has
            // the widget back. The service loop drops the stream immediately.
            running.store(false, Ordering::Release);
            return true;
        }
    }
    false
}

#[derive(Debug)]
enum OpenError {
    NoInputDevice,
    DefaultConfig(cpal::DefaultStreamConfigError),
    BuildStream(cpal::BuildStreamError),
    PlayStream(cpal::PlayStreamError),
    UnsupportedFormat(SampleFormat),
}

impl std::fmt::Display for OpenError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            OpenError::NoInputDevice => write!(f, "no input device"),
            OpenError::DefaultConfig(e) => write!(f, "default input config: {e}"),
            OpenError::BuildStream(e) => write!(f, "build stream: {e}"),
            OpenError::PlayStream(e) => write!(f, "play stream: {e}"),
            OpenError::UnsupportedFormat(format) => {
                write!(f, "unsupported input sample format: {format}")
            }
        }
    }
}

fn try_open_stream() -> Result<(LiveStream, mpsc::Receiver<Vec<i16>>), OpenError> {
    let host = cpal::default_host();
    let device = host
        .default_input_device()
        .ok_or(OpenError::NoInputDevice)?;

    let supported = device
        .default_input_config()
        .map_err(OpenError::DefaultConfig)?;
    let sample_format = supported.sample_format();
    let config: cpal::StreamConfig = supported.into();
    let channels = config.channels;
    let sample_rate = config.sample_rate.0;

    let (tx_chunks, rx_chunks) = mpsc::sync_channel::<Vec<i16>>(32);
    let stream = match sample_format {
        SampleFormat::F32 => {
            let mut normalizer = InputNormalizer::new(channels, sample_rate);
            device.build_input_stream(
                &config,
                move |data: &[f32], _: &_| {
                    normalizer.process_f32(data, |chunk| {
                        let _ = tx_chunks.try_send(chunk);
                    });
                },
                |err| tracing::error!("Wakeword bg stream error: {err}"),
                None,
            )
        }
        SampleFormat::I16 => {
            let mut normalizer = InputNormalizer::new(channels, sample_rate);
            device.build_input_stream(
                &config,
                move |data: &[i16], _: &_| {
                    normalizer.process_i16(data, |chunk| {
                        let _ = tx_chunks.try_send(chunk);
                    });
                },
                |err| tracing::error!("Wakeword bg stream error: {err}"),
                None,
            )
        }
        SampleFormat::U16 => {
            let mut normalizer = InputNormalizer::new(channels, sample_rate);
            device.build_input_stream(
                &config,
                move |data: &[u16], _: &_| {
                    normalizer.process_u16(data, |chunk| {
                        let _ = tx_chunks.try_send(chunk);
                    });
                },
                |err| tracing::error!("Wakeword bg stream error: {err}"),
                None,
            )
        }
        SampleFormat::I32 => {
            let mut normalizer = InputNormalizer::new(channels, sample_rate);
            device.build_input_stream(
                &config,
                move |data: &[i32], _: &_| {
                    normalizer.process_i32(data, |chunk| {
                        let _ = tx_chunks.try_send(chunk);
                    });
                },
                |err| tracing::error!("Wakeword bg stream error: {err}"),
                None,
            )
        }
        other => return Err(OpenError::UnsupportedFormat(other)),
    }
    .map_err(OpenError::BuildStream)?;

    stream.play().map_err(OpenError::PlayStream)?;
    info!(
        "Wakeword bg input stream: {:?}, {} channel(s), {} Hz -> {} Hz mono ({} frames)",
        sample_format, channels, sample_rate, TARGET_SAMPLE_RATE, TARGET_CHUNK_FRAMES
    );

    Ok((LiveStream(stream), rx_chunks))
}
