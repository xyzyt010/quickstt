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

use crate::audio::normalize::InputNormalizer;
use crate::audio::transient::{
    chunk_peak, TransientArbiter, TransientDetector, TRANSIENT_DISABLED, TRANSIENT_START,
    TRANSIENT_STOP,
};
use crate::audio::vad::EnergyVad;
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
#[derive(Clone, Debug)]
pub enum WakewordServiceCommand {
    Start,
    Stop,
    SetSensitivity(u32),
    /// Dashboard "VAD Speech Gate" slider (0..100). Drives ONLY the
    /// background voice gate — independent of wakeword sensitivity.
    SetVadSensitivity(u32),
    SetPhraseEnabled(String, bool),
    /// Shared clap action (0 = Start, 1 = Stop, 2 = Disabled).
    SetTransientAction(u32),
    Suppress(f32),
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
    pub fn set_sensitivity(&self, sensitivity: u32) {
        let _ = self.cmd_tx.send(WakewordServiceCommand::SetSensitivity(sensitivity));
    }
    pub fn set_vad_sensitivity(&self, sensitivity: u32) {
        let _ = self
            .cmd_tx
            .send(WakewordServiceCommand::SetVadSensitivity(sensitivity));
    }
    pub fn set_phrase_enabled(&self, phrase: String, enabled: bool) {
        let _ = self.cmd_tx.send(WakewordServiceCommand::SetPhraseEnabled(phrase, enabled));
    }
    pub fn set_transient_action(&self, action: u32) {
        let _ = self.cmd_tx.send(WakewordServiceCommand::SetTransientAction(action));
    }
    pub fn suppress(&self, duration_secs: f32) {
        let _ = self.cmd_tx.send(WakewordServiceCommand::Suppress(duration_secs));
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
    // Community openWakeWord heads only. The legacy custom heads are
    // quarantined (proven non-discriminating) — see discover_oww_heads.
    let discovered = wakeword_loader::discover_oww_heads(models_dir);
    if discovered.is_empty() {
        warn!(
            "Wakeword background: no OWW heads in {:?} - background detection disabled",
            models_dir
        );
        return None;
    }

    let engine = match WakeWordEngine::from_oww_heads(&discovered) {
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

/// Service thread main loop. Owns the engine, VAD, and the (optional) live stream
/// for the lifetime of the thread.
fn run_service_thread(
    mut engine: WakeWordEngine,
    cmd_rx: mpsc::Receiver<WakewordServiceCommand>,
    running: Arc<AtomicBool>,
    orchestrator_tx: tmpsc::Sender<OrchestratorCommand>,
) {
    let mut live: Option<(LiveStream, mpsc::Receiver<Vec<i16>>)> = None;
    let mut vad = EnergyVad::with_default_threshold();
    // Clap detector + shared action. Defaults Disabled until
    // the UI syncs the persisted value at startup.
    let mut transient = TransientDetector::new();
    let mut arbiter = TransientArbiter::new();
    let mut transient_action: u32 = TRANSIENT_DISABLED;

    loop {
        // 1. Drain queued commands (non-blocking).
        while let Ok(cmd) = cmd_rx.try_recv() {
            match cmd {
                WakewordServiceCommand::Start => {
                    if live.is_none() {
                        match try_open_stream() {
                            Ok((stream, rx)) => {
                                live = Some((stream, rx));
                                vad.reset();
                                transient.reset();
                                arbiter.reset();
                                engine.reset();
                                running.store(true, Ordering::Release);
                                info!("Wakeword background streaming");
                            }
                            Err(e) => {
                                warn!("Wakeword background failed to open stream: {e}");
                            }
                        }
                    }
                }
                WakewordServiceCommand::Stop => {
                    if live.take().is_some() {
                        running.store(false, Ordering::Release);
                        vad.reset();
                        transient.reset();
                        arbiter.reset();
                        engine.reset();
                        info!("Wakeword background stopped");
                    }
                }
                WakewordServiceCommand::SetSensitivity(sens) => {
                    engine.set_sensitivity(sens);
                }
                WakewordServiceCommand::SetVadSensitivity(sens) => {
                    vad.set_sensitivity(sens);
                }
                WakewordServiceCommand::SetPhraseEnabled(phrase, enabled) => {
                    engine.set_phrase_enabled(&phrase, enabled);
                }
                WakewordServiceCommand::SetTransientAction(action) => {
                    transient_action = action.min(2);
                    info!("Transient action → {}", transient_action);
                }
                WakewordServiceCommand::Suppress(secs) => {
                    engine.suppress(secs);
                }
            }
        }

        // 2. If streaming, process up to one audio chunk.
        if let Some((_, rx_chunks)) = live.as_ref() {
            match rx_chunks.try_recv() {
                Ok(chunk) => {
                    if process_audio_chunk(
                        &mut engine,
                        &mut vad,
                        &mut transient,
                        &mut arbiter,
                        &mut transient_action,
                        &chunk,
                        &orchestrator_tx,
                        &running,
                    ) {
                        let _ = live.take();
                    }
                }
                Err(mpsc::TryRecvError::Empty) => {
                    std::thread::sleep(std::time::Duration::from_millis(2));
                }
                Err(mpsc::TryRecvError::Disconnected) => {
                    let _ = live.take();
                    running.store(false, Ordering::Release);
                }
            }
        } else {
            // Idle: block waiting for a command so we don't burn CPU when idle.
            match cmd_rx.recv() {
                Ok(WakewordServiceCommand::Start) => match try_open_stream() {
                    Ok((stream, rx)) => {
                        live = Some((stream, rx));
                        vad.reset();
                        transient.reset();
                        arbiter.reset();
                        engine.reset();
                        running.store(true, Ordering::Release);
                        info!("Wakeword background streaming");
                    }
                    Err(e) => {
                        warn!("Wakeword background failed to open stream: {e}");
                    }
                },
                Ok(WakewordServiceCommand::Stop) => {}
                Ok(WakewordServiceCommand::SetSensitivity(sens)) => {
                    engine.set_sensitivity(sens);
                }
                Ok(WakewordServiceCommand::SetVadSensitivity(sens)) => {
                    vad.set_sensitivity(sens);
                }
                Ok(WakewordServiceCommand::SetPhraseEnabled(phrase, enabled)) => {
                    engine.set_phrase_enabled(&phrase, enabled);
                }
                Ok(WakewordServiceCommand::SetTransientAction(action)) => {
                    transient_action = action.min(2);
                    info!("Transient action → {}", transient_action);
                }
                Ok(WakewordServiceCommand::Suppress(secs)) => {
                    engine.suppress(secs);
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
    vad: &mut EnergyVad,
    transient: &mut TransientDetector,
    arbiter: &mut TransientArbiter,
    transient_action: &mut u32,
    chunk: &[i16],
    orchestrator_tx: &tmpsc::Sender<OrchestratorCommand>,
    running: &Arc<AtomicBool>,
) -> bool {
    // Clap first: detections go through the one-chunk arbiter: only
    // ISOLATED impulses act, while sustained sounds (speech syllables,
    // laughter, coughing fits) are vetoed on the follow-up chunk. A clap's
    // tail must not score as a wakeword, so suppress the engine briefly on
    // every detection.
    // Background handles START (idle → listen); STOP is handled in the
    // foreground thread, which owns the mic while a session is live.
    // The level meter below always runs regardless of the clap action.
    if *transient_action != TRANSIENT_DISABLED {
        let detected = transient.process(chunk);
        if detected.is_some() {
            engine.suppress(0.4);
        }
        // Raw detections log at info (pre-arbiter): separates "clap never
        // heard" from "heard but vetoed as sustained" in the log.
        if let Some(kind) = detected {
            let (peak, ratio, jump, _, _, floor, width) = transient.last_stats();
            info!(
                "Background acoustic {} onset (peak={:.0} ratio={:.1} jump={:.1} floor={:.0} width={})",
                kind.as_str(),
                peak,
                ratio,
                jump,
                floor,
                width
            );
        }
        if let Some(kind) = arbiter.update(detected, chunk_peak(chunk)) {
            arbiter.reset();
            if *transient_action == TRANSIENT_START {
                // NOTE: `last_stats()` here is the QUIET confirmation chunk
                // (the arbiter holds one chunk), so the onset numbers come
                // from `last_onset_stats()` — the detection chunk.
                let (cpeak, cratio, cjump, _, _, cfloor, cwidth) = transient.last_stats();
                if let Some((opeak, oratio, ojump, ofloor, owidth)) =
                    transient.last_onset_stats()
                {
                    info!(
                        "Background acoustic {} fired (onset peak={:.0} ratio={:.1} jump={:.1} floor={:.0} width={} / confirm peak={:.0} ratio={:.1} jump={:.1} floor={:.0} width={}) — popping widget",
                        kind.as_str(),
                        opeak,
                        oratio,
                        ojump,
                        ofloor,
                        owidth,
                        cpeak,
                        cratio,
                        cjump,
                        cfloor,
                        cwidth
                    );
                } else {
                    info!(
                        "Background acoustic {} fired (confirm peak={:.0} ratio={:.1} jump={:.1} floor={:.0} width={}) — popping widget",
                        kind.as_str(),
                        cpeak,
                        cratio,
                        cjump,
                        cfloor,
                        cwidth
                    );
                }
                let _ = orchestrator_tx
                    .try_send(OrchestratorCommand::TransientStart(kind.as_str().to_string()));
                let _ = orchestrator_tx.try_send(OrchestratorCommand::ShowWidget);
                running.store(false, Ordering::Release);
                return true;
            } else if *transient_action == TRANSIENT_STOP {
                // Parked background has no session to stop; the foreground
                // detector covers STOP while recording.
            }
        }
    }

    let vad_speech = vad.process(chunk).speech_likely;

    // Dashboard level meter: the background mic is the only open mic while
    // idle, so it must report levels too (previously foreground-only, which
    // is why the meter sat flat during background listening and made audio
    // look dead). Throttled: every 4th chunk is plenty for a meter.
    {
        use std::sync::atomic::{AtomicU32, Ordering as AOrd};
        static TICKS: AtomicU32 = AtomicU32::new(0);
        if TICKS.fetch_add(1, AOrd::Relaxed) % 4 == 0 {
            let level = crate::audio::vad::audio_level_0_100(chunk);
            let _ = orchestrator_tx.try_send(OrchestratorCommand::AudioLevel(level));
        }
    }

    let events = engine.process_chunk(chunk, vad_speech);
    // Heartbeat every ~5s of streaming: mic level, gate verdict, cache
    // fullness. Reads like "rms=0.023 peak=1800 vad=true mel=210 emb=18
    // heads=42" — proves audio reaches the detector and shows WHY quiet
    // speech never fires (gated) vs scores low (heads).
    {
        use std::sync::atomic::{AtomicU32, Ordering as AOrd};
        static TICKS: AtomicU32 = AtomicU32::new(0);
        if TICKS.fetch_add(1, AOrd::Relaxed) % 64 == 0 {
            let mut sum = 0f64;
            let mut peak = 0f64;
            for &s in chunk.iter() {
                sum += (s as f64) * (s as f64);
                peak = peak.max((s as f64).abs());
            }
            let rms = ((sum / chunk.len().max(1) as f64).sqrt() / 32768.0) as f32;
            let (mel, emb, heads) = engine.cache_stats();
            info!(
                "bg stats: rms={:.4} peak={:.0} vad={} mel={} emb={} heads={}",
                rms, peak, vad_speech, mel, emb, heads
            );
        }
    }
    for event in events {
        if let WakeWordEvent::Triggered(name, confidence) = event {
            info!(
                "Background wakeword fired: '{}' confidence={:.3} - popping widget",
                name, confidence
            );
            let _ = orchestrator_tx.try_send(OrchestratorCommand::WakewordTriggered(confidence));
            let _ = orchestrator_tx.try_send(OrchestratorCommand::ShowWidget);

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
    let dev_name = device.name().unwrap_or_else(|_| "<unknown>".into());

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
        format => return Err(OpenError::UnsupportedFormat(format)),
    }
    .map_err(OpenError::BuildStream)?;

    stream.play().map_err(OpenError::PlayStream)?;
    info!(
        "Wakeword mic open: '{}' {}Hz {}ch {:?}",
        dev_name, sample_rate, channels, sample_format
    );
    Ok((LiveStream(stream), rx_chunks))
}
