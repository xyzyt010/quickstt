use crate::error::QuickSttResult;
use crate::models::catalog;
use crate::settings::Settings;
use crate::wakeword_loader;
use std::sync::{Arc, Mutex};
use tokio::sync::mpsc;
use tracing::{info, warn};

#[cfg(feature = "audio-capture")]
use crate::audio::capture::AudioCaptureManager;
#[cfg(feature = "audio-capture")]
use crate::audio::pipeline::{SegmenterEvent, SpeechSegmenter};

#[derive(Clone, Debug, PartialEq)]
pub enum AppMode {
    Idle,
    WakewordListening,
    Recording,
    Transcribing,
}

#[derive(Clone, Debug)]
pub struct ModelEntry {
    pub name: String,
    pub installed: bool,
    pub size_mb: u32,
    pub engine_family: String,
    pub languages: Vec<String>,
    pub accuracy: u8,
    pub speed: u8,
    pub blurb: String,
}

#[derive(Clone)]
pub struct AppState {
    pub mode: AppMode,
    pub transcript_buffer: String,
    pub partial_result: String,
    pub wakeword_confidence: f32,
    pub settings: Settings,
    pub discovered_wakewords: Vec<String>,
    pub status_message: String,
    pub audio_level: u8,
    pub model_entries: Vec<ModelEntry>,
    pub selected_model: usize,
    pub selected_language: String,
    pub model_offloaded: bool,
    pub wakeword_active: bool,
    pub widget_visible: bool,
    /// True while the foreground capture stream is open (mic live).
    pub mic_open: bool,
    pub download_progress: u8,
    pub is_downloading: bool,
    pub download_status: String,
    /// Active download identity + byte-accurate progress (dashboard MB/s UI).
    pub download_name: String,
    pub download_file_label: String,
    pub download_received_bytes: u64,
    pub download_total_bytes: u64,
    pub download_speed_bps: f64,
    pub download_speed_text: String,
    pub download_bytes_text: String,
    /// Hardware-aware recommendation (entry index into model_entries).
    pub recommended_model: Option<usize>,
    pub recommend_reason: String,
    pub hardware_summary: String,
    /// Dashboard catalog UI state (search + language filter, Slint-driven).
    pub catalog_search: String,
    pub catalog_lang_filter: String,
}

impl AppState {
    pub fn new(settings: Settings) -> Self {
        Self {
            mode: AppMode::Idle,
            transcript_buffer: String::new(),
            partial_result: String::new(),
            wakeword_confidence: 0.0,
            settings,
            discovered_wakewords: Vec::new(),
            status_message: String::new(),
            audio_level: 0,
            model_entries: Vec::new(),
            selected_model: 0,
            selected_language: "Auto".to_string(),
            model_offloaded: false,
            wakeword_active: false,
            widget_visible: true,
            mic_open: false,
            download_progress: 0,
            is_downloading: false,
            download_status: String::new(),
            download_name: String::new(),
            download_file_label: String::new(),
            download_received_bytes: 0,
            download_total_bytes: 0,
            download_speed_bps: 0.0,
            download_speed_text: String::new(),
            download_bytes_text: String::new(),
            recommended_model: None,
            recommend_reason: String::new(),
            hardware_summary: String::new(),
            catalog_search: String::new(),
            catalog_lang_filter: "All Languages".to_string(),
        }
    }
}

#[derive(Debug)]
pub enum OrchestratorCommand {
    StartListening,
    StopListening,
    AudioChunk(Vec<i16>),
    AudioLevel(u8),
    TranscribeChunk(Vec<f32>),
    TextRecognized(String),
    PartialText(String),
    /// Watchdog: the transcription engine stopped responding (hung load or
    /// dead helper). Unstick the turn so the widget never sits in
    /// "Transcribing…" forever; the next turn respawns the engine session.
    AbortTranscribe,
    WakewordTriggered(f32),
    /// A clap transient fired with action=Start. Carries the sound kind
    /// ("clap") for logging. Gated on `transient_action == 0`.
    TransientStart(String),
    SelectModel(usize),
    SelectLanguage(String),
    /// Select the capture microphone by OS device name ("" = system default).
    /// Applies on next mic open; restarts the live stream when capturing.
    SelectMicrophone(String),
    /// Download + install the model at this catalog index (background thread).
    DownloadModel(usize),
    OffloadModel,
    ReloadModel,
    ToggleWakeword(bool),
    SetWakewordSensitivity(u32),
    ShowWidget,
    HideWidget,
    /// Enable background wakeword detection (runs when widget is hidden).
    EnableBackgroundWakeword,
    /// Disable background wakeword detection.
    DisableBackgroundWakeword,
}

pub struct AppOrchestrator {
    state: Arc<Mutex<AppState>>,
    tx_cmd: mpsc::Sender<OrchestratorCommand>,
    #[cfg(feature = "audio-capture")]
    #[allow(dead_code)]
    audio_control_tx: std::sync::mpsc::Sender<AudioControlCommand>,
    /// Sender side of the wakeword audio channel. The audio control thread
    /// receives a clone of this each time it opens the mic.
    #[cfg(feature = "audio-capture")]
    #[allow(dead_code)]
    audio_tx: mpsc::Sender<Vec<i16>>,
}

/// Release every persistent STT worker EXCEPT `active_family` (catalog
/// runtime string: "photon" | "parakeet-rust" | "nemotron" | "canary" | …).
/// Empty string releases all (model switch: nothing is active yet). Each
/// unload is a fast JSON round-trip that no-ops when its worker was never
/// started, so calling all four unconditionally is cheap and cannot strand
/// a worker after a family change.
fn unload_all_workers_except(active_family: &str) {
    if active_family != "parakeet-rust" {
        crate::models::engine::parakeet_unload();
    }
    if active_family != "nemotron" {
        crate::models::engine::nemotron_unload();
    }
    if active_family != "canary" {
        crate::models::engine::canary_unload();
    }
    if active_family != "photon" {
        crate::models::engine::photon_unload();
    }
}

fn model_name_matches(configured: &str, catalog_name: &str) -> bool {    let configured = configured.trim();
    let catalog_name = catalog_name.trim();
    if configured.eq_ignore_ascii_case(catalog_name) {
        return true;
    }

    let strip_suffix = |value: &str| {
        value
            .replace(" (ONNX/Rust)", "")
            .replace(" (onnx/rust)", "")
            .trim()
            .to_string()
    };

    if strip_suffix(configured).eq_ignore_ascii_case(&strip_suffix(catalog_name)) {
        return true;
    }

    // Quant migration: Nemotron Q8_0 (broken, all-<unk>) -> Q4_K_M (working).
    // Old saved selections must keep matching the renamed descriptor.
    let low_cfg = configured.to_lowercase();
    let low_cat = catalog_name.to_lowercase();
    if low_cfg.contains("nemotron") && low_cat.contains("nemotron") {
        return true;
    }
    // Moonshine family alias (v2 base vs streaming naming).
    if low_cfg.contains("moonshine") && low_cat.contains("moonshine") {
        return true;
    }
    // Canary family alias (quant renames).
    if low_cfg.contains("canary") && low_cat.contains("canary") {
        return true;
    }
    // Whisper Large Turbo alias.
    if low_cfg.contains("large") && low_cfg.contains("turbo") && low_cat.contains("large") {
        return true;
    }
    false
}

/// Recompute the smart recommendation from the live entries + fresh hardware
/// probe. Called on startup and whenever the language changes (CPU scoring
/// carries a multilingual bonus).
fn refresh_recommendation(s: &mut AppState) {
    let hw = crate::engine::HardwareInfo::detect();
    let descs: Vec<catalog::ModelDescriptor> = s
        .model_entries
        .iter()
        .filter_map(|e| {
            catalog::all_descriptors().into_iter().find(|d| d.name == e.name)
        })
        .collect();
    let (rec_name, reason) = catalog::recommend_model(
        &descs,
        hw.has_discrete_gpu,
        &hw.vendor,
        &hw.device_name,
        hw.vram_mb,
        hw.system_ram_gb,
        &s.selected_language,
    );
    s.hardware_summary = hw.summary();
    s.recommend_reason = reason;
    s.recommended_model = rec_name
        .and_then(|n| s.model_entries.iter().position(|e| e.name == n));
}

/// Internal commands sent to the dedicated audio-control thread. The audio
/// capture manager, the segmenter, and the live cpal stream are not `Send`
/// (cpal handlers hold raw pointers), so we run them on their own
/// `std::thread` and post these commands over a synchronous channel.
#[cfg(feature = "audio-capture")]
pub enum AudioControlCommand {
    /// Open the wakeword mic and create a fresh [`SpeechSegmenter`].
    Open {
        audio_tx: mpsc::Sender<Vec<i16>>,
        ptt_mode: bool,
    },
    /// Close the wakeword mic and drop the segmenter.
    Close,
}

#[cfg(feature = "audio-capture")]
struct AudioControlState {
    capture: Option<AudioCaptureManager>,
    segmenter: Option<SpeechSegmenter>,
    /// Clap detector (same type as the background thread's, so every mic
    /// path shares one function and one action).
    transient: crate::audio::transient::TransientDetector,
    /// One-chunk isolation arbiter for STOP transients (see background).
    transient_arbiter: crate::audio::transient::TransientArbiter,
}

#[cfg(feature = "audio-capture")]
fn spawn_audio_control_thread(
    state: Arc<Mutex<AppState>>,
    tx_cmd: mpsc::Sender<OrchestratorCommand>,
    audio_rx: mpsc::Receiver<Vec<i16>>,
) -> std::sync::mpsc::Sender<AudioControlCommand> {
    let (tx, rx) = std::sync::mpsc::channel::<AudioControlCommand>();
    std::thread::Builder::new()
        .name("audio-control".to_string())
        .spawn(move || run_audio_control_thread(rx, state, tx_cmd, audio_rx))
        .expect("failed to spawn audio control thread");
    tx
}

#[cfg(feature = "audio-capture")]
fn run_audio_control_thread(
    cmd_rx: std::sync::mpsc::Receiver<AudioControlCommand>,
    state: Arc<Mutex<AppState>>,
    tx_cmd: mpsc::Sender<OrchestratorCommand>,
    mut audio_rx: mpsc::Receiver<Vec<i16>>,
) {
    let mut ctrl = AudioControlState {
        capture: None,
        segmenter: None,
        transient: crate::audio::transient::TransientDetector::new(),
        transient_arbiter: crate::audio::transient::TransientArbiter::new(),
    };

    loop {
        // While a stream is open, drain audio chunks and process them; when no
        // stream is open, block waiting for the next command.
        if ctrl.segmenter.is_some() {
            // We are running. Try commands first so Stop is responsive.
            match cmd_rx.try_recv() {
                Ok(AudioControlCommand::Close) => {
                    close_audio(&mut ctrl, &state, &tx_cmd);
                    continue;
                }
                Ok(AudioControlCommand::Open { .. }) => {
                    // Already running; ignore.
                }
                Err(std::sync::mpsc::TryRecvError::Empty) => {}
                Err(std::sync::mpsc::TryRecvError::Disconnected) => {
                    info!("Audio control channel disconnected; closing audio capture");
                    close_audio(&mut ctrl, &state, &tx_cmd);
                    return;
                }
            }
            match audio_rx.try_recv() {
                Ok(chunk) => process_audio_chunk(&chunk, &mut ctrl, &state, &tx_cmd),
                Err(mpsc::error::TryRecvError::Empty) => {
                    // Tiny sleep to avoid burning CPU.
                    std::thread::sleep(std::time::Duration::from_millis(2));
                }
                Err(mpsc::error::TryRecvError::Disconnected) => {
                    info!("Audio rx closed while running");
                    close_audio(&mut ctrl, &state, &tx_cmd);
                    return;
                }
            }
        } else {
            // Idle: block waiting for commands.
            match cmd_rx.recv() {
                Ok(AudioControlCommand::Open { audio_tx, ptt_mode }) => {
                    open_audio(&mut ctrl, &state, audio_tx, ptt_mode);
                }
                Ok(AudioControlCommand::Close) => {
                    // No-op: already idle.
                }
                Err(_) => return,
            }
        }
    }
}

#[cfg(feature = "audio-capture")]
fn open_audio(
    ctrl: &mut AudioControlState,
    state: &Arc<Mutex<AppState>>,
    audio_tx: mpsc::Sender<Vec<i16>>,
    ptt_mode: bool,
) {
    // Instant second press: reuse the live cpal stream, just reset PTT state.
    if let Some(seg) = ctrl.segmenter.as_mut() {
        seg.reset();
        seg.set_ptt_mode(ptt_mode);
        return;
    }
    let preferred = state
        .lock()
        .map(|s| s.settings.selected_microphone.clone())
        .unwrap_or_default();
    let mut manager = AudioCaptureManager::new();
    if let Err(e) = manager.start_wakeword_stream(audio_tx, &preferred) {
        warn!("Failed to open wakeword audio stream: {}", e);
        return;
    }
    let cache_dir = std::env::temp_dir().join("quickstt").join("utterances");
    let mut seg = SpeechSegmenter::new(cache_dir);
    seg.set_ptt_mode(ptt_mode);
    if let Ok(s) = state.lock() {
        seg.set_vad_sensitivity(s.settings.vad_sensitivity);
    }
    ctrl.capture = Some(manager);
    ctrl.segmenter = Some(seg);
    // Fresh mic → fresh transient state (open pops must not fire it).
    ctrl.transient.reset();
    ctrl.transient_arbiter.reset();
    if let Ok(mut s) = state.lock() {
        s.mic_open = true;
    }
    info!("Foreground audio capture opened (ptt_mode={ptt_mode})");
}

#[cfg(feature = "audio-capture")]
fn wav_is_silent(path: &std::path::Path) -> bool {
    // Silence gate: never send near-silence to the engine. Parakeet (like
    // most ASR) hallucinates "yeah"/"yes" on noise-only clips, which is
    // exactly the reported "say nothing -> prints yeah".
    // Trigger-blind: the first ~500ms holds the trigger onset (a clap
    // peaks 5000+ and would whitewash any whole-file measurement), so
    // peak/RMS are measured on the TAIL only. A clap-then-silence turn
    // scores silent; genuine speech continues past the onset and passes.
    let reader = match hound::WavReader::open(path) {
        Ok(r) => r,
        Err(_) => return false,
    };
    let spec = reader.spec();
    if spec.sample_rate == 0 {
        return false;
    }
    let skip = (spec.sample_rate as u64) / 2;
    let mut peak: i32 = 0;
    let mut sum_sq: f64 = 0.0;
    let mut n: u64 = 0;
    for (i, s) in reader.into_samples::<i16>().enumerate() {
        if (i as u64) < skip {
            continue;
        }
        let v = match s {
            Ok(v) => v as i32,
            Err(_) => continue,
        };
        let a = v.abs();
        if a > peak {
            peak = a;
        }
        sum_sq += (v as f64) * (v as f64);
        n += 1;
    }
    if n == 0 {
        return true;
    }
    let secs = n as f32 / spec.sample_rate as f32;
    let rms = (sum_sq / n as f64).sqrt();
    // ~0.25s min (keeps "ok"), peak floor ~600, rms floor ~50. Tuned so
    // genuine whispers pass but mic hiss / empty taps do not.
    if secs < 0.25 {
        return true;
    }
    if peak < 600 {
        return true;
    }
    if rms < 50.0 {
        return true;
    }
    false
}

/// Single-word ASR hallucinations ("you", "yeah", ...) that every engine
/// emits for silence/noise. The UI keeps its own copy for display gating;
/// this one guards accumulation so a ghost never even enters the buffer.
#[cfg(feature = "audio-capture")]
fn is_ghost_singleton(text: &str) -> bool {
    matches!(
        text.trim().to_lowercase().as_str(),
        "yeah" | "yeah." | "yes" | "yes." | "you" | "you." | "uh" | "uh." | "um"
            | "um." | "hmm" | "hmm." | "oh" | "oh." | "hey" | "hey."
            | "thank you" | "thank you." | "thanks" | "thanks." | "..." | "." | ""
    )
}

/// Peak of the utterance TAIL (trigger onset skipped, same 500ms as
/// wav_is_silent). MAX i16 on read error.
#[cfg(feature = "audio-capture")]
fn wav_tail_peak(path: &std::path::Path) -> i32 {
    let reader = match hound::WavReader::open(path) {
        Ok(r) => r,
        Err(_) => return i32::MAX,
    };
    let skip = (reader.spec().sample_rate as u64) / 2;
    let mut peak: i32 = 0;
    for (i, s) in reader.into_samples::<i16>().enumerate() {
        if (i as u64) < skip {
            continue;
        }
        if let Ok(v) = s {
            peak = peak.max(v.abs() as i32);
        }
    }
    peak
}

#[cfg(feature = "audio-capture")]
fn flush_pending_utterance(
    ctrl: &mut AudioControlState,
    state: &Arc<Mutex<AppState>>,
    tx_cmd: &mpsc::Sender<OrchestratorCommand>,
) {
    let wav_path = match ctrl.segmenter.as_mut().and_then(|s| s.flush()) {
        Some(p) => p,
        None => return,
    };
    // Silent tap (no speech buffered): complete the turn silently without
    // ever invoking the engine — no spinner flash, no "yeah".
    if wav_is_silent(&wav_path) {
        info!("Silent flush skipped (no engine call): {:?}", wav_path);
        let _ = tx_cmd.try_send(OrchestratorCommand::TextRecognized(String::new()));
        return;
    }
    let (name, language) = {
        let s = state.lock().unwrap();
        match s.model_entries.get(s.selected_model).map(|e| e.name.clone()) {
            Some(n) => (n, s.selected_language.clone()),
            None => return,
        }
    };
    let descriptor = match catalog::all_descriptors().into_iter().find(|d| d.name == name) {
        Some(d) => d,
        None => return,
    };
    // Mark transcribing instantly so the pill swaps mic->spinner with no delay.
    {
        let mut s = state.lock().unwrap();
        s.mode = AppMode::Transcribing;
        s.status_message = "Transcribing…".into();
    }
    let tx_clone = tx_cmd.clone();
    std::thread::Builder::new()
        .name("stt-transcribe".to_string())
        .spawn(move || {
            // Cached engine detection (no per-utterance probing).
            let config = crate::models::engine::cached_config();
            let t0 = std::time::Instant::now();
            match crate::models::engine::transcribe_with_language(
                &config,
                &descriptor,
                &wav_path,
                &language,
            ) {
                Ok(text) => {
                    info!(
                        "transcribe done (flush): model='{}' wav={:?} took={}ms chars={}",
                        descriptor.name,
                        wav_path,
                        t0.elapsed().as_millis(),
                        text.len()
                    );
                    let _ = tx_clone.try_send(OrchestratorCommand::TextRecognized(text));
                }
                Err(e) => {
                    // Complete the turn even on failure (empty result): the
                    // old PartialText("") path left mode=Transcribing forever
                    // and wedged the widget on every engine error.
                    warn!("Transcribe failed for {:?}: {}", wav_path, e);
                    let _ = tx_clone.try_send(OrchestratorCommand::TextRecognized(String::new()));
                }
            }
        })
        .ok();
}

#[cfg(feature = "audio-capture")]
fn close_audio(
    ctrl: &mut AudioControlState,
    state: &Arc<Mutex<AppState>>,
    tx_cmd: &mpsc::Sender<OrchestratorCommand>,
) {
    // PTT release: transcribe the buffered word FIRST, then stop the mic.
    flush_pending_utterance(ctrl, state, tx_cmd);
    if let Some(mut manager) = ctrl.capture.take() {
        manager.stop_wakeword_stream();
        manager.stop_transcription_stream();
    }
    ctrl.segmenter = None;
    if let Ok(mut s) = state.lock() {
        s.mic_open = false;
    }
    info!("Foreground audio capture closed");
}

#[cfg(feature = "audio-capture")]
fn process_audio_chunk(
    chunk: &[i16],
    ctrl: &mut AudioControlState,
    state: &Arc<Mutex<AppState>>,
    tx_cmd: &mpsc::Sender<OrchestratorCommand>,
) {
    // 1. Update audio level meter.
    let level = crate::audio::vad::audio_level_0_100(chunk);
    let _ = tx_cmd.try_send(OrchestratorCommand::AudioLevel(level));

    // 1b. Foreground transient stop: the background mic is parked while a
    // session is live, so a "Stop Dictation" clap can only be heard
    // here. Only acts when recording — never interrupts idle.
    {
        let action = state
            .lock()
            .map(|s| s.settings.transient_action)
            .unwrap_or(crate::audio::transient::TRANSIENT_DISABLED);
        if action == crate::audio::transient::TRANSIENT_STOP {
            // STOP goes through the isolation arbiter too: only an isolated
            // impulse stops the session, sustained speech never does.
            let detected = ctrl.transient.process(chunk);
            let peak = crate::audio::transient::chunk_peak(chunk);
            let width = ctrl.transient.last_stats().6;
            if let Some(kind) = ctrl.transient_arbiter.update(detected, peak, width) {
                ctrl.transient_arbiter.reset();
                let live = state
                    .lock()
                    .map(|s| s.mode == AppMode::Recording || s.mode == AppMode::Transcribing)
                    .unwrap_or(false);
                if live {
                    // Same one-chunk lag as the background fire line: the
                    // confirmation chunk is quiet, so onset numbers come
                    // from `last_onset_stats()`.
                    let (cpeak, cratio, cjump, _, _, cfloor, cwidth) =
                        ctrl.transient.last_stats();
                    if let Some((opeak, oratio, ojump, ofloor, owidth)) =
                        ctrl.transient.last_onset_stats()
                    {
                        info!(
                            "Acoustic {} → stop dictation (onset peak={:.0} ratio={:.1} jump={:.1} floor={:.0} width={} / confirm peak={:.0} width={})",
                            kind.as_str(),
                            opeak,
                            oratio,
                            ojump,
                            ofloor,
                            owidth,
                            cpeak,
                            cwidth
                        );
                    } else {
                        info!(
                            "Acoustic {} → stop dictation (peak={:.0} ratio={:.1} jump={:.1} floor={:.0} width={})",
                            kind.as_str(),
                            cpeak,
                            cratio,
                            cjump,
                            cfloor,
                            cwidth
                        );
                    }
                    let _ = tx_cmd.try_send(OrchestratorCommand::StopListening);
                    return;
                }
            }
        } else if action != crate::audio::transient::TRANSIENT_DISABLED {
            // Keep the detector's noise floor honest when armed for Start
            // (cheap), so switching actions mid-session just works.
            // Classification results are ignored here.
            let _ = ctrl.transient.process(chunk);
        }
    }

    // 2. Drive the speech segmenter.
    let event = match ctrl.segmenter.as_mut() {
        Some(seg) => seg.process_chunk(chunk),
        None => None,
    };
    if matches!(event, Some(SegmenterEvent::SpeechStarted | SegmenterEvent::SpeechContinues)) {
        if let Ok(mut s) = state.lock() {
            if s.mode != AppMode::Recording && s.mode != AppMode::Transcribing {
                s.mode = AppMode::Recording;
                s.status_message = "Listening...".into();
            }
        }
    }
    if let Some(SegmenterEvent::UtteranceComplete(wav_path)) = event {
        if let Some(seg) = ctrl.segmenter.as_mut() {
            seg.reset();
        }
        // Silence gate (same as flush): noise-only segments never reach the
        // engine, so they can never come back as "yeah".
        if wav_is_silent(&wav_path) {
            info!("Silent segment skipped (no engine call): {:?}", wav_path);
            let _ = tx_cmd.try_send(OrchestratorCommand::TextRecognized(String::new()));
            return;
        }
        let (name, language) = {
            let s = state.lock().unwrap();
            let idx = s.selected_model;
            match s.model_entries.get(idx).map(|e| e.name.clone()) {
                Some(n) => (n, s.selected_language.clone()),
                None => {
                    warn!("Utterance complete but no model selected");
                    return;
                }
            }
        };
        let descriptor = catalog::all_descriptors()
            .into_iter()
            .find(|d| d.name == name);
        let Some(descriptor) = descriptor else {
            warn!("No descriptor matches model entry name '{}'", name);
            return;
        };

        // Transcription runs in a one-shot thread so we don't block the audio
        // loop for the multi-second inference.
        let tx_clone = tx_cmd.clone();
        let wav_clone = wav_path.clone();
        let _ = std::thread::Builder::new()
            .name("stt-transcribe".to_string())
            .spawn(move || {
                let config = crate::models::engine::cached_config();
                let t0 = std::time::Instant::now();
                match crate::models::engine::transcribe_with_language(
                    &config,
                    &descriptor,
                    &wav_clone,
                    &language,
                ) {
                    Ok(text) => {
                        info!(
                            "transcribe done (segment): model='{}' wav={:?} took={}ms chars={}",
                            descriptor.name,
                            wav_clone,
                            t0.elapsed().as_millis(),
                            text.len()
                        );
                        // Ghost post-filter: a clap-triggered turn records
                        // the clap itself, so the pre-engine silence gate
                        // passes and whisper returns "you" for the silence
                        // after it. Drop ghost singletons whose TAIL (past
                        // the trigger onset) is near-silent — deliberate
                        // single words carry real tail energy and pass.
                        #[cfg(feature = "audio-capture")]
                        let text = if is_ghost_singleton(&text)
                            && wav_tail_peak(&wav_clone) < 400
                        {
                            info!(
                                "ghost singleton {:?} dropped (silent tail): {:?}",
                                text.trim(),
                                wav_clone
                            );
                            String::new()
                        } else {
                            text
                        };
                        #[cfg(not(feature = "audio-capture"))]
                        let text = text;
                        let _ = tx_clone.try_send(OrchestratorCommand::TextRecognized(text));
                    }
                    Err(e) => {
                        // Complete the turn even on failure (see above).
                        warn!("Transcribe failed for {:?}: {}", wav_clone, e);
                        let _ = tx_clone.try_send(OrchestratorCommand::TextRecognized(
                            String::new(),
                        ));
                    }
                }
            });
    }
}

impl AppOrchestrator {
    pub fn new() -> QuickSttResult<(Self, mpsc::Receiver<OrchestratorCommand>)> {
        let settings = Settings::load()?;
        let mut app_state = AppState::new(settings);

        let models_dir = wakeword_loader::default_models_dir();
        // Community heads only (customs quarantined — see discover_oww_heads).
        let discovered = wakeword_loader::discover_oww_heads(&models_dir);
        if discovered.is_empty() {
            info!("No wakeword heads found in {:?}", models_dir);
            app_state.status_message = format!("No wakeword heads in {:?}", models_dir);
        } else {
            let names: Vec<String> = discovered.iter().map(|(_, _, p)| p.clone()).collect();
            info!("Found {} wakeword heads: {:?}", discovered.len(), names);
            app_state.discovered_wakewords = names;
            app_state.status_message = format!("{} wakeword heads loaded", discovered.len());
        }

        let configured_widget_models = app_state
            .settings
            .widget_models
            .iter()
            .chain(app_state.settings.favorite_models.iter())
            .map(|m| m.trim())
            .filter(|m| !m.is_empty())
            .collect::<Vec<_>>();

        // Hardware-aware list: discrete GPU → the two Whisper GPU models only;
        // CPU/iGPU → every non-gpu_only model (existing five + Canary).
        // GPU-only Large Turbo stays hidden on CPU machines so nobody fetches
        // 800MB they cannot run well. New models are registered but NEVER
        // auto-downloaded — user-initiated 1-click only.
        let hw = crate::engine::HardwareInfo::detect();
        let hw_models = catalog::models_for_hardware(hw.has_discrete_gpu);
        // Respect an explicit widget_models allowlist when present, but always
        // intersect it with the hardware-appropriate set (a stale allowlist
        // naming a GPU-only model on a CPU box must not resurrect it).
        let mut selected_descriptors = if configured_widget_models.is_empty() {
            hw_models
                .iter()
                .filter(|m| m.widget_selectable)
                .collect::<Vec<_>>()
        } else {
            hw_models
                .iter()
                .filter(|m| {
                    m.widget_selectable
                        && configured_widget_models
                            .iter()
                            .any(|name| model_name_matches(name, &m.name))
                })
                .collect::<Vec<_>>()
        };

        if selected_descriptors.is_empty() {
            selected_descriptors = hw_models.iter().filter(|m| m.widget_selectable).collect();
        }

        app_state.model_entries = selected_descriptors
            .iter()
            .map(|m| ModelEntry {
                name: m.name.clone(),
                installed: catalog::is_model_installed(m),
                size_mb: m.size_mb,
                engine_family: m.engine_family.to_string(),
                languages: catalog::supported_languages(m),
                accuracy: m.accuracy,
                speed: m.speed,
                blurb: m.blurb.clone(),
            })
            .collect();

        if app_state.model_entries.is_empty() {
            app_state.model_entries.push(ModelEntry {
                name: "No models found".into(),
                installed: false,
                size_mb: 0,
                engine_family: "N/A".into(),
                languages: vec!["Auto".to_string()],
                accuracy: 0,
                speed: 0,
                blurb: String::new(),
            });
        }
        if !app_state.settings.selected_model.trim().is_empty() {
            if let Some(idx) = app_state
                .model_entries
                .iter()
                .position(|m| model_name_matches(app_state.settings.selected_model.trim(), &m.name))
            {
                app_state.selected_model = idx;
            }
        }
        // If the selected model is not installed, prefer the first installed model so transcription works immediately
        if !app_state.model_entries.is_empty() && !app_state.model_entries[app_state.selected_model].installed {
            if let Some(idx) = app_state.model_entries.iter().position(|m| m.installed) {
                info!("Selected fallback installed model: {}", app_state.model_entries[idx].name);
                app_state.selected_model = idx;
            }
        }
        // Restore the saved language when the current model supports it,
        // otherwise fall back to that model's default (first entry).
        {
            let saved = app_state.settings.selected_language.trim().to_string();
            let langs = app_state.model_entries[app_state.selected_model]
                .languages
                .clone();
            app_state.selected_language = if langs.iter().any(|l| l == &saved) {
                saved
            } else {
                langs.into_iter().next().unwrap_or_else(|| "Auto".to_string())
            };
        }
        // Smart recommendation over the hardware-filtered entries.
        {
            let descs: Vec<catalog::ModelDescriptor> = app_state
                .model_entries
                .iter()
                .filter_map(|e| {
                    catalog::all_descriptors().into_iter().find(|d| d.name == e.name)
                })
                .collect();
            let (rec_name, reason) = catalog::recommend_model(
                &descs,
                hw.has_discrete_gpu,
                &hw.vendor,
                &hw.device_name,
                hw.vram_mb,
                hw.system_ram_gb,
                &app_state.selected_language,
            );
            app_state.hardware_summary = hw.summary();
            app_state.recommend_reason = reason;
            app_state.recommended_model = rec_name.and_then(|n| {
                app_state.model_entries.iter().position(|e| e.name == n)
            });
            if let Some(idx) = app_state.recommended_model {
                info!(
                    "Recommended model: {} ({})",
                    app_state.model_entries[idx].name, app_state.hardware_summary
                );
            }
        }

        let state = Arc::new(Mutex::new(app_state));
        let (tx_cmd, rx_cmd) = mpsc::channel(256);

        #[cfg(feature = "audio-capture")]
        {
            let (audio_tx, audio_rx) = mpsc::channel::<Vec<i16>>(64);
            // Spawn the dedicated thread that owns the cpal stream + segmenter
            // and runs the audio pipeline. The orchestrator just dispatches
            // Start/Stop signals to it.
            let audio_control_tx =
                spawn_audio_control_thread(state.clone(), tx_cmd.clone(), audio_rx);
            Ok((
                Self {
                    state,
                    tx_cmd,
                    audio_control_tx,
                    audio_tx,
                },
                rx_cmd,
            ))
        }

        #[cfg(not(feature = "audio-capture"))]
        {
            Ok((Self { state, tx_cmd }, rx_cmd))
        }
    }

    pub fn get_state(&self) -> Arc<Mutex<AppState>> {
        self.state.clone()
    }

    pub fn get_command_sender(&self) -> mpsc::Sender<OrchestratorCommand> {
        self.tx_cmd.clone()
    }

    /// Clone of the wakeword audio channel sender. Used by the foreground
    /// command loop when it opens a new mic.
    #[cfg(feature = "audio-capture")]
    pub fn audio_tx_clone(&self) -> mpsc::Sender<Vec<i16>> {
        self.audio_tx.clone()
    }

    /// Clone of the audio control sender used by the GUI to dispatch Open /
    /// Close commands to the audio control thread.
    #[cfg(feature = "audio-capture")]
    pub fn audio_control_tx_clone(&self) -> std::sync::mpsc::Sender<AudioControlCommand> {
        self.audio_control_tx.clone()
    }

    pub async fn run_command_loop(
        state: Arc<Mutex<AppState>>,
        mut rx: mpsc::Receiver<OrchestratorCommand>,
        #[cfg(feature = "audio-capture")] audio_control_tx: std::sync::mpsc::Sender<
            AudioControlCommand,
        >,
        #[cfg(feature = "audio-capture")] audio_tx: mpsc::Sender<Vec<i16>>,
    ) {
        while let Some(cmd) = rx.recv().await {
            match cmd {
                OrchestratorCommand::StartListening => {
                    {
                        let mut s = state.lock().unwrap();
                        s.mode = AppMode::Recording;
                        s.status_message = "Listening...".into();
                        s.model_offloaded = false;
                        info!("Mode → Recording");
                    }
                    #[cfg(feature = "audio-capture")]
                    {
                        let _ = audio_control_tx.send(AudioControlCommand::Open {
                            audio_tx: audio_tx.clone(),
                            ptt_mode: true,
                        });
                    }
                }
                OrchestratorCommand::StopListening => {
                    #[cfg(feature = "audio-capture")]
                    {
                        let _ = audio_control_tx.send(AudioControlCommand::Close);
                    }
                    let mut s = state.lock().unwrap();
                    s.mode = AppMode::Idle;
                    s.status_message = "Ready".into();
                    info!("Mode → Idle");
                }
                OrchestratorCommand::AudioLevel(level) => {
                    let mut s = state.lock().unwrap();
                    s.audio_level = level;
                }
                OrchestratorCommand::TextRecognized(text) => {
                    let mut s = state.lock().unwrap();
                    if !text.trim().is_empty() {
                        if !s.transcript_buffer.is_empty() {
                            s.transcript_buffer.push('\n');
                        }
                        s.transcript_buffer.push_str(text.trim());
                        s.partial_result.clear();
                        info!("Recognized: {}", text.trim());
                    }
                    s.mode = AppMode::WakewordListening;
                    s.status_message = "Listening...".into();
                }
                OrchestratorCommand::PartialText(text) => {
                    let mut s = state.lock().unwrap();
                    s.partial_result = text;
                }
                OrchestratorCommand::AbortTranscribe => {
                    let mut s = state.lock().unwrap();
                    if s.mode == AppMode::Transcribing {
                        s.mode = AppMode::Idle;
                        s.status_message = "Ready".into();
                        s.partial_result.clear();
                        warn!("Transcribe aborted by watchdog (engine hung)");
                    }
                }
                OrchestratorCommand::WakewordTriggered(confidence) => {
                    // Explicit-consent gate: background detection only triggers when
                    // enabled by user.
                    let allowed = {
                        let s = state.lock().unwrap();
                        let sens = s.settings.wakeword_sensitivity.clamp(0, 100) as f32;
                        // Sensitivity slider (0-100): 50 -> 0.27 floor, 100 -> 0.15 floor, 0 -> 0.40 floor.
                        let floor = (0.40 - (sens / 100.0) * 0.25).clamp(0.15, 0.50);
                        (s.wakeword_active || (!s.settings.wake_word_mode.eq_ignore_ascii_case("Off") && !s.settings.wake_word_mode.trim().is_empty()))
                            && confidence >= floor
                    };
                    if !allowed {
                        info!(
                            "Wakeword trigger ignored — background wake not enabled or confidence too low (confidence {:.3})",
                            confidence
                        );
                        continue;
                    }
                    #[cfg(feature = "audio-capture")]
                    {
                        let _ = audio_control_tx.send(AudioControlCommand::Open {
                            audio_tx: audio_tx.clone(),
                            ptt_mode: false,
                        });
                    }
                    let mut s = state.lock().unwrap();
                    s.wakeword_confidence = confidence;
                    s.mode = AppMode::Recording;
                    s.status_message = "Listening...".into();
                    s.widget_visible = true;
                    info!("Wakeword triggered with confidence {:.3} -> Mode::Recording (VAD auto-finalize)", confidence);
                }
                OrchestratorCommand::TransientStart(kind) => {
                    // Clap gate: fires only when the clap action is Start.
                    // Independent of wakewords, so claps work with wakewords
                    // fully disabled. The const lives in the `audio` module
                    // (compiled out without `audio-capture`), so the
                    // no-audio build compares against its literal value
                    // (TRANSIENT_START = 0) instead of failing to compile —
                    // this arm is what bare `cargo test -p quickstt-core`
                    // builds on CI.
                    #[cfg(feature = "audio-capture")]
                    let want_start = crate::audio::transient::TRANSIENT_START;
                    #[cfg(not(feature = "audio-capture"))]
                    let want_start = 0u32;
                    let allowed = state
                        .lock()
                        .map(|s| s.settings.transient_action == want_start)
                        .unwrap_or(false);
                    if !allowed {
                        info!(
                            "Acoustic {} ignored — transient action is not Start",
                            kind
                        );
                        continue;
                    }
                    #[cfg(feature = "audio-capture")]
                    {
                        let _ = audio_control_tx.send(AudioControlCommand::Open {
                            audio_tx: audio_tx.clone(),
                            ptt_mode: false,
                        });
                    }
                    let mut s = state.lock().unwrap();
                    s.wakeword_confidence = 1.0;
                    s.mode = AppMode::Recording;
                    s.status_message = "Listening...".into();
                    s.widget_visible = true;
                    info!("Acoustic {} → start dictation (Mode::Recording)", kind);
                }
                OrchestratorCommand::SelectModel(idx) => {
                    let mut s = state.lock().unwrap();
                    if idx < s.model_entries.len() {
                        s.selected_model = idx;
                        let name = s.model_entries[idx].name.clone();
                        // Resync language: keep it only if the new model
                        // supports it, else fall back to its default.
                        let langs = s.model_entries[idx].languages.clone();
                        if !langs.iter().any(|l| l == &s.selected_language) {
                            s.selected_language =
                                langs.into_iter().next().unwrap_or_else(|| "Auto".to_string());
                        }
                        s.settings.selected_model = name.clone();
                        s.settings.selected_language = s.selected_language.clone();
                        if let Err(e) = s.settings.save_all() {
                            warn!("Failed to persist model selection: {}", e);
                        }
                        s.status_message = format!("Selected: {}", name);
                        info!("Model selected: {} ({})", name, s.selected_language.clone());
                        // Model switch strands the previous worker: the idle
                        // timer only fires after a turn ends, so without this
                        // the old model's GBs (e.g. Ultra's python worker)
                        // stay resident forever when the user just switches.
                        // Release everything async (the new model loads on
                        // demand at the next turn) + re-arm the idle timer.
                        s.model_offloaded = false;
                        std::thread::Builder::new()
                            .name("stt-model-switch-offload".to_string())
                            .spawn(|| {
                                unload_all_workers_except("");
                                crate::models::engine::compact_working_set();
                            })
                            .ok();
                    }
                }
                OrchestratorCommand::SelectLanguage(lang) => {
                    let mut s = state.lock().unwrap();
                    let langs = s.model_entries[s.selected_model].languages.clone();
                    if langs.iter().any(|l| l == &lang) {
                        s.selected_language = lang.clone();
                        s.settings.selected_language = lang.clone();
                        if let Err(e) = s.settings.save_all() {
                            warn!("Failed to persist language selection: {}", e);
                        }
                        s.status_message = format!("Language: {}", lang);
                        info!("Language selected: {}", lang);
                        // Language changes CPU scoring (multilingual bonus).
                        refresh_recommendation(&mut s);
                    } else {
                        warn!("Language '{}' not supported by current model", lang);
                    }
                }
                OrchestratorCommand::SelectMicrophone(name) => {
                    {
                        let mut s = state.lock().unwrap();
                        s.settings.selected_microphone = name.clone();
                        if let Err(e) = s.settings.save_all() {
                            warn!("Failed to persist microphone selection: {}", e);
                        }
                        s.status_message = if name.trim().is_empty() {
                            "Microphone: System default".into()
                        } else {
                            format!("Microphone: {}", name.trim())
                        };
                    }
                    info!("Microphone selected: {:?}", name);
                    // A live stream keeps the OLD device until reopened:
                    // bounce it so the new microphone takes over now.
                    #[cfg(feature = "audio-capture")]
                    {
                        let reopen = state.lock().map(|s| s.mic_open).unwrap_or(false);
                        if reopen {
                            let _ = audio_control_tx.send(AudioControlCommand::Close);
                            let _ = audio_control_tx.send(AudioControlCommand::Open {
                                audio_tx: audio_tx.clone(),
                                ptt_mode: true,
                            });
                        }
                    }
                }
                OrchestratorCommand::DownloadModel(idx) => {
                    // Single-flight: a second tap while a model is downloading
                    // just nudges the status line instead of interleaving two
                    // writers on the same progress fields.
                    let already: Option<String> = state
                        .lock()
                        .map(|s| {
                            if s.is_downloading {
                                Some(s.download_name.clone())
                            } else {
                                None
                            }
                        })
                        .unwrap_or(None);
                    if let Some(active) = already {
                        if let Ok(mut s) = state.lock() {
                            s.status_message =
                                format!("Already downloading {} — wait…", active);
                            s.download_status = s.status_message.clone();
                        }
                        info!("DownloadModel ignored (busy with {})", active);
                        continue;
                    }
                    let desc = {
                        let s = state.lock().unwrap();
                        s.model_entries.get(idx).and_then(|e| {
                            crate::models::catalog::all_descriptors()
                                .into_iter()
                                .find(|d| d.name == e.name)
                        })
                    };
                    match desc {
                        Some(d) => {
                            // Persist selection so the widget uses it once ready.
                            if let Ok(mut s) = state.lock() {
                                s.selected_model = idx.min(s.model_entries.len().saturating_sub(1));
                            }
                            info!("Downloading model: {}", d.name);
                            crate::models::downloader::spawn_download(d, state.clone());
                        }
                        None => warn!("Download requested for unknown model index {}", idx),
                    }
                }
                OrchestratorCommand::OffloadModel => {
                    // Fast return: the persistent workers (Parakeet/Nemotron/
                    // Canary/Photon) free their models on a background thread
                    // so the command loop never blocks on the JSON round-trip.
                    // One-shot CLIs (whisper/sherpa/vosk) hold no persistent
                    // state — compact only. Same semantics as before (model
                    // dropped, next turn reloads), just async so offload feels
                    // as instant as the one-shot engines.
                    // Skip the ACTIVE model's worker: freeing it would only
                    // force a slow reload on the very next turn (Ultra's load
                    // is minutes). Idle workers for the other models are what
                    // actually leak GBs after a model switch.
                    let active_family = {
                        state
                            .lock()
                            .ok()
                            .and_then(|s| {
                                s.model_entries.get(s.selected_model).map(|e| {
                                    e.engine_family.clone()
                                })
                            })
                            .unwrap_or_default()
                    };
                    {
                        let mut s = state.lock().unwrap();
                        s.model_offloaded = true;
                        s.status_message = "Model offloaded (RAM released)".into();
                        info!(
                            "Model offloaded (async worker release, incl. Photon; active family kept: '{}')",
                            active_family
                        );
                    }
                    std::thread::Builder::new()
                        .name("stt-offload".to_string())
                        .spawn(move || {
                            unload_all_workers_except(&active_family);
                            crate::models::engine::compact_working_set();
                        })
                        .ok();
                }
                OrchestratorCommand::ReloadModel => {
                    let mut s = state.lock().unwrap();
                    s.model_offloaded = false;
                    s.status_message = "Model reloaded".into();
                    info!("Model reloaded");
                }
                OrchestratorCommand::ToggleWakeword(active) => {
                    let mut s = state.lock().unwrap();
                    s.wakeword_active = active;
                    s.settings.wake_word_mode = if active {
                        "Always On".to_string()
                    } else {
                        "Off".to_string()
                    };
                    if let Err(e) = s.settings.save_all() {
                        warn!("Failed to persist wakeword settings: {}", e);
                    }
                    info!("Wakeword active: {}, mode: {}", active, s.settings.wake_word_mode);
                }
                OrchestratorCommand::SetWakewordSensitivity(v) => {
                    let mut s = state.lock().unwrap();
                    s.settings.wakeword_sensitivity = v.clamp(0, 100);
                    if let Err(e) = s.settings.save_all() {
                        warn!("Failed to persist wakeword sensitivity: {}", e);
                    }
                    info!("Wakeword sensitivity: {}", s.settings.wakeword_sensitivity);
                }
                OrchestratorCommand::ShowWidget => {
                    let mut s = state.lock().unwrap();
                    s.widget_visible = true;
                }
                OrchestratorCommand::HideWidget => {
                    let mut s = state.lock().unwrap();
                    s.widget_visible = false;
                }
                OrchestratorCommand::EnableBackgroundWakeword
                | OrchestratorCommand::DisableBackgroundWakeword => {
                    // Background wakeword toggling is handled outside the
                    // orchestrator's main loop (it talks directly to the
                    // WakewordBackgroundService via a separate channel).
                    // The orchestrator just keeps state consistent: bumping
                    // the offload timer so we don't immediately drop the
                    // model after the wakeword fires.
                    info!("Background wakeword toggle: {:?}", cmd);
                    crate::models::engine::compact_working_set();
                }
                OrchestratorCommand::AudioChunk(_) | OrchestratorCommand::TranscribeChunk(_) => {
                    // Handled by the audio control thread, not here.
                }
            }
        }
    }
}

#[cfg(all(test, feature = "audio-capture"))]
mod ghost_tests {
    use super::*;

    fn write_wav(path: &std::path::Path, samples: &[i16]) {
        let spec = hound::WavSpec {
            channels: 1,
            sample_rate: 16000,
            bits_per_sample: 16,
            sample_format: hound::SampleFormat::Int,
        };
        let mut w = hound::WavWriter::create(path, spec).expect("wav writable");
        for &s in samples {
            w.write_sample(s).expect("sample writable");
        }
        w.finalize().expect("wav finalizable");
    }

    fn wav_path(tag: &str) -> std::path::PathBuf {
        std::env::temp_dir().join(format!(
            "quickstt-ghosttest-{}-{}.wav",
            tag,
            std::process::id()
        ))
    }

    #[test]
    fn clap_then_silence_is_silent() {
        // Trigger-blindness: a loud 300ms onset (the clap) followed by 2s
        // of room silence must score SILENT — the old whole-file peak
        // measurement passed this straight to the engine ("you").
        let mut pcm = vec![0i16; 16000 * 2 + 4800];
        for (i, s) in pcm.iter_mut().enumerate().take(4800) {
            *s = if i % 2 == 0 { 6000 } else { -6000 };
        }
        let p = wav_path("clap-silence");
        write_wav(&p, &pcm);
        assert!(wav_is_silent(&p), "clap-then-silence must be silent");
        // ...and its tail peak is room-level.
        assert!(wav_tail_peak(&p) < 400, "tail must be quiet");
        std::fs::remove_file(&p).ok();
    }

    #[test]
    fn speech_after_onset_is_not_silent() {
        // Same onset, then conversational-level speech: must NOT be gated.
        let mut pcm = vec![0i16; 16000 * 2 + 4800];
        for (i, s) in pcm.iter_mut().enumerate().take(4800) {
            *s = if i % 2 == 0 { 6000 } else { -6000 };
        }
        for (i, s) in pcm.iter_mut().enumerate().skip(8000).take(16000) {
            let ph = (i as f32 * 220.0 * 2.0 * std::f32::consts::PI / 16000.0).sin();
            *s = (ph * 3000.0) as i16;
        }
        let p = wav_path("clap-speech");
        write_wav(&p, &pcm);
        assert!(!wav_is_silent(&p), "speech tail must pass the gate");
        assert!(wav_tail_peak(&p) >= 400, "speech tail peak must be real");
        std::fs::remove_file(&p).ok();
    }

    #[test]
    fn ghost_singletons_recognized() {
        for g in ["you", "You.", "  yeah  ", "THANK YOU", "...", ""] {
            assert!(is_ghost_singleton(g), "{g:?} must be a ghost");
        }
        for real in ["you are here", "hello world", "yes please go", "ok"] {
            assert!(!is_ghost_singleton(real), "{real:?} must NOT be a ghost");
        }
    }
}
