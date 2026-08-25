use crate::error::QuickSttResult;
use crate::models::catalog;
use crate::settings::Settings;
use crate::wakeword_loader;
use std::sync::{Arc, Mutex};
use tokio::sync::mpsc;
use tracing::info;
#[cfg(feature = "audio-capture")]
use tracing::warn;

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
    pub model_offloaded: bool,
    pub wakeword_active: bool,
    pub widget_visible: bool,
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
            model_offloaded: false,
            wakeword_active: false,
            widget_visible: true,
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
    WakewordTriggered(f32),
    SelectModel(usize),
    OffloadModel,
    ReloadModel,
    ToggleWakeword(bool),
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

fn model_name_matches(configured: &str, catalog_name: &str) -> bool {
    let configured = configured.trim();
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

    strip_suffix(configured).eq_ignore_ascii_case(&strip_suffix(catalog_name))
}

/// Internal commands sent to the dedicated audio-control thread. The audio
/// capture manager, the segmenter, and the live cpal stream are not `Send`
/// (cpal handlers hold raw pointers), so we run them on their own
/// `std::thread` and post these commands over a synchronous channel.
#[cfg(feature = "audio-capture")]
pub enum AudioControlCommand {
    /// Open the wakeword mic and create a fresh [`SpeechSegmenter`].
    Open { audio_tx: mpsc::Sender<Vec<i16>> },
    /// Close the wakeword mic and drop the segmenter.
    Close,
}

#[cfg(feature = "audio-capture")]
struct AudioControlState {
    capture: Option<AudioCaptureManager>,
    segmenter: Option<SpeechSegmenter>,
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
    };

    loop {
        // While a stream is open, drain audio chunks and process them; when no
        // stream is open, block waiting for the next command.
        if ctrl.segmenter.is_some() {
            // We are running. Try commands first so Stop is responsive.
            match cmd_rx.try_recv() {
                Ok(AudioControlCommand::Close) => {
                    close_audio(&mut ctrl);
                    continue;
                }
                Ok(AudioControlCommand::Open { .. }) => {
                    // Already running; ignore.
                }
                Err(std::sync::mpsc::TryRecvError::Empty) => {}
                Err(std::sync::mpsc::TryRecvError::Disconnected) => {
                    info!("Audio control channel disconnected; closing audio capture");
                    close_audio(&mut ctrl);
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
                    close_audio(&mut ctrl);
                    return;
                }
            }
        } else {
            // Idle: block waiting for commands.
            match cmd_rx.recv() {
                Ok(AudioControlCommand::Open { audio_tx }) => {
                    open_audio(&mut ctrl, audio_tx);
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
fn open_audio(ctrl: &mut AudioControlState, audio_tx: mpsc::Sender<Vec<i16>>) {
    if ctrl.capture.is_some() {
        return;
    }
    let mut manager = AudioCaptureManager::new();
    if let Err(e) = manager.start_wakeword_stream(audio_tx) {
        warn!("Failed to open wakeword audio stream: {}", e);
        return;
    }
    let cache_dir = std::env::temp_dir().join("quickstt").join("utterances");
    ctrl.capture = Some(manager);
    ctrl.segmenter = Some(SpeechSegmenter::new(cache_dir));
    info!("Foreground audio capture opened");
}

#[cfg(feature = "audio-capture")]
fn close_audio(ctrl: &mut AudioControlState) {
    if let Some(mut manager) = ctrl.capture.take() {
        manager.stop_wakeword_stream();
        manager.stop_transcription_stream();
    }
    ctrl.segmenter = None;
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

    // 2. Drive the speech segmenter.
    let event = match ctrl.segmenter.as_mut() {
        Some(seg) => seg.process_chunk(chunk),
        None => None,
    };
    if let Some(SegmenterEvent::UtteranceComplete(wav_path)) = event {
        if let Some(seg) = ctrl.segmenter.as_mut() {
            seg.reset();
        }
        let name = {
            let s = state.lock().unwrap();
            let idx = s.selected_model;
            s.model_entries.get(idx).map(|e| e.name.clone())
        };
        let Some(name) = name else {
            warn!("Utterance complete but no model selected");
            return;
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
                let config = crate::models::engine::SttEngineConfig::detect();
                match crate::models::engine::transcribe(&config, &descriptor, &wav_clone) {
                    Ok(text) => {
                        let _ = tx_clone.try_send(OrchestratorCommand::TextRecognized(text));
                    }
                    Err(e) => {
                        warn!("Transcribe failed for {:?}: {}", wav_clone, e);
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
        let discovered = wakeword_loader::discover_models(&models_dir);
        if discovered.is_empty() {
            info!("No wakeword models found in {:?}", models_dir);
            app_state.status_message = format!("No wakeword models in {:?}", models_dir);
        } else {
            let names: Vec<String> = discovered.iter().map(|m| m.config.phrase.clone()).collect();
            info!("Found {} wakeword models: {:?}", discovered.len(), names);
            app_state.discovered_wakewords = names;
            app_state.status_message = format!("{} wakeword models loaded", discovered.len());
        }

        let all_models = catalog::all_descriptors();
        let configured_widget_models = app_state
            .settings
            .widget_models
            .iter()
            .chain(app_state.settings.favorite_models.iter())
            .map(|m| m.trim())
            .filter(|m| !m.is_empty())
            .collect::<Vec<_>>();

        let mut selected_descriptors = if configured_widget_models.is_empty() {
            all_models
                .iter()
                .filter(|m| m.widget_selectable)
                .collect::<Vec<_>>()
        } else {
            all_models
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
            selected_descriptors = all_models.iter().filter(|m| m.widget_selectable).collect();
        }

        app_state.model_entries = selected_descriptors
            .iter()
            .map(|m| ModelEntry {
                name: m.name.clone(),
                installed: catalog::is_model_installed(m),
                size_mb: m.size_mb,
                engine_family: m.engine_family.to_string(),
            })
            .collect();

        if app_state.model_entries.is_empty() {
            app_state.model_entries.push(ModelEntry {
                name: "No models found".into(),
                installed: false,
                size_mb: 0,
                engine_family: "N/A".into(),
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
                        s.mode = AppMode::WakewordListening;
                        s.status_message = "Listening...".into();
                        s.model_offloaded = false;
                        info!("Mode → WakewordListening");
                    }
                    #[cfg(feature = "audio-capture")]
                    {
                        let _ = audio_control_tx.send(AudioControlCommand::Open {
                            audio_tx: audio_tx.clone(),
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
                OrchestratorCommand::WakewordTriggered(confidence) => {
                    #[cfg(feature = "audio-capture")]
                    {
                        // If the background wakeword service fired while the
                        // foreground mic wasn't running (widget was hidden),
                        // open it now so the audio pipeline can capture the
                        // user's response.
                        let _ = audio_control_tx.send(AudioControlCommand::Open {
                            audio_tx: audio_tx.clone(),
                        });
                    }
                    let mut s = state.lock().unwrap();
                    s.wakeword_confidence = confidence;
                    s.mode = AppMode::WakewordListening;
                    s.status_message = "Wakeword detected!".into();
                    s.widget_visible = true;
                    info!("Wakeword triggered with confidence {:.3}", confidence);
                }
                OrchestratorCommand::SelectModel(idx) => {
                    let mut s = state.lock().unwrap();
                    if idx < s.model_entries.len() {
                        s.selected_model = idx;
                        let name = s.model_entries[idx].name.clone();
                        s.status_message = format!("Selected: {}", name);
                        info!("Model selected: {}", name);
                    }
                }
                OrchestratorCommand::OffloadModel => {
                    let mut s = state.lock().unwrap();
                    s.model_offloaded = true;
                    s.status_message = "Model offloaded".into();
                    info!("Model offloaded");
                    crate::models::engine::compact_working_set();
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
                    info!("Wakeword active: {}", active);
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
