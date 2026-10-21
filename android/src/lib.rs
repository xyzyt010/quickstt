//! QuickSTT for Android — touch-first dictation.
//!
//! The UI is a dedicated touch-first Slint scene (`ui/android.slint`), not the
//! desktop dashboard: phones get a bottom nav, a thumb-reachable record orb
//! and 44+ DIP targets.
//!
//! What actually works in this build:
//!   * real microphone capture (NDK AAudio via the `src/aaudio.c` shim — cpal
//!     has no Android backend),
//!   * real on-device transcription (whisper.cpp built for arm64-v8a, shipped
//!     inside the APK and unpacked on first run),
//!   * the model is downloaded in-app (32 MB, whisper.cpp GGML tiny.en Q5_1),
//!   * the finished text is copied to the clipboard so it can be pasted into
//!     any app,
//!   * a floating bubble above other apps (WindowManager overlay) that shows
//!     the mic state and the last transcript.
//!
//! Every action reports what it did: the status line and the activity log in
//! the UI (and logcat) always say what happened, so a silent button is not
//! possible any more.
//!
//! Build:
//!   * `cargo apk build` → Android APK (`android_main` below)
//!   * `cargo run --example preview` → desktop preview window

mod audio;
mod conditioner;
mod platform;
mod stt;

slint::include_modules!();

use slint::ComponentHandle;
use std::rc::Rc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

/// Result of a finished turn, handed from the worker thread to the UI thread.
struct PendingTurn {
    text: String,
    seconds: i32,
    status: String,
    ok: bool,
}

/// Recording state owned by the capture thread.
struct Session {
    recorder: audio::Recorder,
    samples: Vec<f32>,
    /// Speech detected at least once (so silence-only takes are discarded).
    spoke: bool,
    /// Last time we saw a sample above the speech threshold.
    last_voice: std::time::Instant,
    started: std::time::Instant,
}

impl Session {
    fn new() -> Self {
        Self {
            recorder: audio::Recorder::new(),
            samples: Vec::new(),
            spoke: false,
            last_voice: std::time::Instant::now(),
            started: std::time::Instant::now(),
        }
    }
}

/// Android entry point.
///
/// The APK ships this crate as a `cdylib`, and the Java side loads it as
/// `System.loadLibrary("quickstt_android")` and looks for exactly this
/// symbol. Without it the library contains only the dependencies and the
/// app never starts (the activity dies with "UnsatisfiedLinkError").
#[cfg(target_os = "android")]
#[no_mangle]
pub fn android_main(app: slint::android::AndroidApp) {
    // The platform layer needs the Activity instance (for the permission
    // dialog, the clipboard and the overlay), so keep a handle before the
    // backend consumes it.
    platform::init(app.clone());
    if let Err(e) = slint::android::init(app) {
        eprintln!("QuickSTT: Slint Android backend failed to start: {e}");
        return;
    }
    run();
}

/// The UI the floating bubble talks to. The bubble is a Java view, so it
/// calls back into Rust through a native method.
static BUBBLE_UI: Mutex<Option<slint::Weak<AppWindow>>> = Mutex::new(None);

/// Called by the Java bubble when the user taps one of its buttons.
///
/// * 1 = start/stop the recording
/// * 2 = type the last transcript into the app in front
/// * 3 = hide the bubble
#[cfg(target_os = "android")]
#[no_mangle]
pub extern "C" fn Java_ai_quickstt_app_Bubble_nativeBubbleAction(
    _env: jni::JNIEnv,
    _class: jni::sys::jclass,
    action: jni::sys::jint,
) {
    bubble_action(action as i32);
}

/// Run a bubble action on the Slint event loop.
fn bubble_action(action: i32) {
    let ui = BUBBLE_UI.lock().ok().and_then(|g| g.clone());
    let Some(ui) = ui else { return };
    let _ = slint::invoke_from_event_loop(move || {
        let Some(ui) = ui.upgrade() else { return };
        match action {
            1 => ui.invoke_toggle_record(),
            2 => ui.invoke_type_into_app(),
            3 => platform::hide_overlay(),
            _ => {}
        }
    });
}

/// The engines the Models tab lists.
///
/// Only engines that really run on the phone are in here: the catalog is
/// built from `stt::MODELS` and each entry carries its *real* download
/// state. Nothing in the list is a placeholder, and nothing promises a
/// language or a feature the phone build cannot do.
fn model_catalog() -> Vec<ModelItem> {
    stt::MODELS
        .iter()
        .enumerate()
        .map(|(index, spec)| {
            let (downloading, progress) = download_state(index);
            ModelItem {
                name: spec.name.into(),
                detail: spec.detail.into(),
                size: spec.size.into(),
                installed: stt::model_installed(index),
                downloading,
                progress,
            }
        })
        .collect()
}

/// The download state of every model: (downloading, progress).
///
/// The download runs on a worker thread, so the state lives here and the UI
/// reads it when it rebuilds the model list.
static DOWNLOADS: Mutex<Vec<(bool, f32)>> = Mutex::new(Vec::new());

/// Remember what a model's download is doing.
fn set_download_state(index: usize, downloading: bool, progress: f32) {
    let Ok(mut list) = DOWNLOADS.lock() else { return };
    if list.len() < stt::MODELS.len() {
        list.resize(stt::MODELS.len(), (false, 0.0));
    }
    if let Some(slot) = list.get_mut(index) {
        *slot = (downloading, progress);
    }
}

/// What a model's download is doing.
fn download_state(index: usize) -> (bool, f32) {
    let Ok(list) = DOWNLOADS.lock() else { return (false, 0.0) };
    list.get(index).copied().unwrap_or((false, 0.0))
}

/// Run something on the UI thread.
///
/// Slint properties may only be set from the event-loop thread, so every
/// update from a worker thread goes through here.
fn on_ui<F: FnOnce() + Send + 'static>(f: F) {
    let _ = slint::invoke_from_event_loop(f);
}

/// Push a line into the activity log (shown in the UI and in logcat).
fn note(ui: &slint::Weak<AppWindow>, log: &Arc<Mutex<Vec<String>>>, message: &str) {
    platform::log(message);
    let text = {
        let Ok(mut lines) = log.lock() else { return };
        lines.push(message.to_string());
        while lines.len() > 6 {
            lines.remove(0);
        }
        lines.join("\n")
    };
    if let Some(ui) = ui.upgrade() {
        ui.set_log_text(text.into());
    }
}

/// Show the floating bubble and say what went wrong if it fails.
fn bubble(
    state: &str,
    transcript: &str,
    ui: &slint::Weak<AppWindow>,
    log: &Arc<Mutex<Vec<String>>>,
) {
    if let Err(e) = platform::show_overlay(state, transcript) {
        note(ui, log, &format!("The bubble could not be shown: {e}"));
    }
}

/// Wire callbacks and run the event loop.
pub fn run() {
    let ui = AppWindow::new().expect("could not create the QuickSTT window");

    // The desktop preview should look like a phone. (On Android the surface is
    // the device screen; the request is ignored there.)
    #[cfg(not(target_os = "android"))]
    {
        ui.window()
            .set_size(slint::LogicalSize::new(412.0, 760.0));
        // QS_TAB=0..3 opens the preview on a given tab (dev aid).
        if let Ok(tab) = std::env::var("QS_TAB") {
            if let Ok(tab) = tab.parse::<i32>() {
                ui.set_tab(tab.clamp(0, 3));
            }
        }
    }

    // Recent turns are a growable model so a finished dictation can appear
    // immediately, exactly like the desktop history list.
    let turns = Rc::new(slint::VecModel::from(Vec::<Turn>::new()));
    ui.set_turns(turns.clone().into());

    // The Models tab shows the real catalog.
    ui.set_models(Rc::new(slint::VecModel::from(model_catalog())).into());

    // The activity log: the last few things the app did.
    let log: Arc<Mutex<Vec<String>>> = Arc::new(Mutex::new(Vec::new()));

    // The floating bubble calls back into the UI through this handle.
    if let Ok(mut slot) = BUBBLE_UI.lock() {
        *slot = Some(ui.as_weak());
    }

    // Shared between the UI thread and the capture thread.
    let stop_flag: Arc<AtomicBool> = Arc::new(AtomicBool::new(false));
    let pending: Arc<Mutex<Option<PendingTurn>>> = Arc::new(Mutex::new(None));
    // The last finished transcript — what the "type into the app" and
    // "share" actions push out.
    let last_text: Arc<Mutex<String>> = Arc::new(Mutex::new(String::new()));
    // Set while a permission dialog is on screen.
    let perm_wait: Arc<Mutex<Option<std::time::Instant>>> = Arc::new(Mutex::new(None));

    // ── Setup state (permissions + model) ──────────────────────────────────
    {
        let weak = ui.as_weak();
        let log = log.clone();
        let perm_wait = perm_wait.clone();
        ui.on_grant_mic(move || {
            note(&weak, &log, "Requesting the microphone permission…");
            if let Ok(mut wait) = perm_wait.lock() {
                *wait = Some(std::time::Instant::now());
            }
            platform::request_mic_permission();
            if let Some(ui) = weak.upgrade() {
                ui.set_status_text("Waiting for the permission dialog…".into());
            }
        });
    }
    {
        let weak = ui.as_weak();
        let log = log.clone();
        ui.on_download_model(move || {
            let Some(ui) = weak.upgrade() else { return };
            let index = ui.get_selected_model().max(0) as usize;
            if ui.get_model_downloading() {
                note(&weak, &log, "A download is already running.");
                return;
            }
            if stt::model_installed(index) {
                note(&weak, &log, "That model is already installed.");
                ui.set_status_text("That model is already downloaded".into());
                return;
            }
            let spec = stt::model(index);
            ui.set_model_downloading(true);
            ui.set_model_progress(0.0);
            ui.set_status_text(format!("Downloading {}…", spec.name).into());
            set_download_state(index, true, 0.0);
            ui.set_models(Rc::new(slint::VecModel::from(model_catalog())).into());
            note(&weak, &log, &format!("Downloading {} ({}).", spec.name, spec.size));

            let weak = weak.clone();
            let log = log.clone();
            // Download off the UI thread; the UI only receives progress.
            std::thread::spawn(move || {
                let mut last_update = std::time::Instant::now() - std::time::Duration::from_secs(1);
                let result = stt::download_model(index, |done, total| {
                    // The UI is refreshed at most ten times a second.
                    if last_update.elapsed() < std::time::Duration::from_millis(100) {
                        return;
                    }
                    last_update = std::time::Instant::now();
                    let frac = if total > 0 {
                        (done as f32 / total as f32).clamp(0.0, 1.0)
                    } else {
                        0.0
                    };
                    let megabytes = done as f64 / 1_048_576.0;
                    let percent = if total > 0 {
                        format!("{}%", (frac * 100.0) as i32)
                    } else {
                        format!("{megabytes:.0} MB")
                    };
                    set_download_state(index, true, frac);
                    let progress_weak = weak.clone();
                    let name = spec.name;
                    on_ui(move || {
                        if let Some(ui) = progress_weak.upgrade() {
                            ui.set_model_progress(frac);
                            ui.set_model_downloading(true);
                            ui.set_status_text(
                                format!("Downloading {name}… {percent}").into(),
                            );
                            ui.set_models(
                                Rc::new(slint::VecModel::from(model_catalog())).into(),
                            );
                        }
                    });
                });
                set_download_state(index, false, if result.is_ok() { 1.0 } else { 0.0 });
                let done_weak = weak.clone();
                let done_log = log.clone();
                let name = spec.name;
                let error = result.err();
                on_ui(move || {
                    if let Some(ui) = done_weak.upgrade() {
                        ui.set_model_downloading(false);
                        match error {
                            None => {
                                ui.set_model_progress(1.0);
                                ui.set_models(
                                    Rc::new(slint::VecModel::from(model_catalog())).into(),
                                );
                                ui.set_status_text(format!("{name} is ready — tap the mic").into());
                                note(&done_weak, &done_log, &format!("{name} installed."));
                            }
                            Some(e) => {
                                ui.set_model_progress(0.0);
                                ui.set_models(
                                    Rc::new(slint::VecModel::from(model_catalog())).into(),
                                );
                                ui.set_status_text(format!("Download failed: {e}").into());
                                note(&done_weak, &done_log, &format!("Download failed: {e}"));
                            }
                        }
                    }
                });
            });
        });
    }
    {
        let weak = ui.as_weak();
        let log = log.clone();
        ui.on_open_overlay_settings(move || {
            note(&weak, &log, "Opening the “display over other apps” settings…");
            platform::open_overlay_settings();
            if let Some(ui) = weak.upgrade() {
                ui.set_status_text("Enable “Display over other apps”, then come back".into());
            }
        });
    }
    {
        let weak = ui.as_weak();
        let log = log.clone();
        ui.on_toggle_overlay(move || {
            let Some(ui) = weak.upgrade() else { return };
            let wanted = !ui.get_overlay_enabled();
            if wanted && !platform::can_draw_overlays() {
                note(
                    &weak,
                    &log,
                    "Android needs the “display over other apps” permission for the bubble.",
                );
                ui.set_status_text("Allow “display over other apps” first".into());
                platform::open_overlay_settings();
                return;
            }
            if wanted {
                match platform::show_overlay("QuickSTT ready", "") {
                    Ok(()) => {
                        ui.set_overlay_enabled(true);
                        platform::save_overlay_enabled(true);
                        note(&weak, &log, "Floating bubble on — it follows you across apps.");
                        ui.set_status_text("Floating bubble on".into());
                    }
                    Err(e) => {
                        ui.set_overlay_enabled(false);
                        platform::save_overlay_enabled(false);
                        note(&weak, &log, &format!("The bubble could not be shown: {e}"));
                        ui.set_status_text(format!("The bubble could not be shown: {e}").into());
                    }
                }
            } else {
                platform::hide_overlay();
                ui.set_overlay_enabled(false);
                platform::save_overlay_enabled(false);
                note(&weak, &log, "Floating bubble off.");
                ui.set_status_text("Floating bubble off".into());
            }
        });
    }

    // ── Record toggle ──────────────────────────────────────────────────────
    // One capture thread per session: it pulls 16 kHz frames, tracks the
    // level for the UI meter and stops on its own after 1.4 s of silence
    // (so a hands-free dictation ends by itself). The thread also owns the
    // transcription and hands the result back through `pending`.
    {
        let weak = ui.as_weak();
        let stop_flag = stop_flag.clone();
        let pending = pending.clone();
        let log = log.clone();
        let perm_wait = perm_wait.clone();
        ui.on_toggle_record(move || {
            let Some(ui) = weak.upgrade() else { return };

            if ui.get_recording() {
                // Stop: ask the capture thread to wrap up.
                stop_flag.store(true, Ordering::SeqCst);
                ui.set_recording(false);
                ui.set_status_text("Finishing…".into());
                note(&weak, &log, "Stopping the recording…");
                return;
            }

            // Start.
            if !platform::has_mic_permission() {
                note(&weak, &log, "The microphone permission is missing.");
                platform::request_mic_permission();
                if let Ok(mut wait) = perm_wait.lock() {
                    *wait = Some(std::time::Instant::now());
                }
                ui.set_status_text("Microphone permission needed".into());
                ui.set_tab(0);
                return;
            }
            if !stt::model_installed(ui.get_selected_model().max(0) as usize) {
                note(&weak, &log, "The speech model is not downloaded yet.");
                ui.set_status_text("Download the model first (step 2)".into());
                ui.set_tab(0);
                return;
            }
            let mut session = Session::new();
            if let Err(e) = session.recorder.start() {
                ui.set_status_text(format!("Microphone failed: {e}").into());
                note(&weak, &log, &format!("Microphone failed: {e}"));
                return;
            }
            stop_flag.store(false, Ordering::SeqCst);
            ui.set_recording(true);
            ui.set_busy(false);
            ui.set_live_text("Listening… speak now.".into());
            ui.set_status_text("Recording".into());
            note(&weak, &log, "Recording started.");
            bubble("Listening…", "", &weak, &log);

            let weak = weak.clone();
            let stop_flag = stop_flag.clone();
            let pending = pending.clone();
            let model_index = ui.get_selected_model().max(0) as usize;
            std::thread::spawn(move || {
                let mut session = session;
                let mut chunk = vec![0f32; 1600]; // 100 ms
                // Last input-health complaint shown to the user (throttled
                // so a broken mic does not spam the status line).
                let mut last_health: u8 = conditioner::HEALTH_OK;
                let mut health_since = std::time::Instant::now();
                loop {
                    let n = session.recorder.read(&mut chunk);
                    if n == 0 {
                        std::thread::sleep(std::time::Duration::from_millis(20));
                        if stop_flag.load(Ordering::SeqCst) {
                            break;
                        }
                        continue;
                    }
                    session.samples.extend_from_slice(&chunk[..n]);
                    let rms = session.recorder.rms;
                    // Speech bar: the desktop's VAD floor (min_rms at the
                    // default sensitivity) — quiet speech still counts, so
                    // the 1.4 s auto-stop does not swallow a whole take.
                    if rms > 0.0055 {
                        session.spoke = true;
                        session.last_voice = std::time::Instant::now();
                    }
                    // Input health: a clipped or dead microphone must say
                    // so instead of pretending to listen to silence.
                    let health = session.recorder.health();
                    if health != last_health
                        && health_since.elapsed() > std::time::Duration::from_secs(2)
                    {
                        last_health = health;
                        health_since = std::time::Instant::now();
                        let msg = match health {
                            conditioner::HEALTH_SATURATED => {
                                "Microphone input is clipped — lower the input gain"
                            }
                            conditioner::HEALTH_DEAD => {
                                "No audio is arriving from the microphone"
                            }
                            _ => "Listening… speak now.",
                        };
                        let card_msg = match health {
                            conditioner::HEALTH_SATURATED => "Input clipped — lower the mic gain",
                            conditioner::HEALTH_DEAD => "No signal from the microphone",
                            _ => "",
                        };
                        let hweak = weak.clone();
                        let card = card_msg.to_string();
                        let _ = slint::invoke_from_event_loop(move || {
                            if let Some(ui) = hweak.upgrade() {
                                ui.set_status_text(msg.into());
                                ui.set_mic_health(card.clone().into());
                            }
                            if !card.is_empty() {
                                platform::log(&card);
                            }
                        });
                    }
                    // Auto-stop after 1.4 s of silence (only if we heard
                    // something) or after 60 s of recording.
                    let quiet = session.last_voice.elapsed() > std::time::Duration::from_millis(1400);
                    let too_long = session.started.elapsed() > std::time::Duration::from_secs(60);
                    let level = rms.min(1.0);
                    let meter_weak = weak.clone();
                    let _ = slint::invoke_from_event_loop(move || {
                        if let Some(ui) = meter_weak.upgrade() {
                            if ui.get_recording() {
                                ui.set_live_text(format!("Listening… {:.0}%", level * 100.0).into());
                            }
                        }
                    });
                    if (quiet && session.spoke) || too_long {
                        break;
                    }
                    if stop_flag.load(Ordering::SeqCst) {
                        break;
                    }
                }
                session.recorder.stop();
                let samples = std::mem::take(&mut session.samples);
                let health = session.recorder.health();
                let status_weak = weak.clone();
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(ui) = status_weak.upgrade() {
                        ui.set_busy(true);
                        ui.set_status_text("Transcribing…".into());
                    }
                });
                transcribe(samples, health, model_index, &pending);
            });
        });
    }

    // Bottom navigation.
    {
        let weak = ui.as_weak();
        ui.on_nav(move |tab| {
            if let Some(ui) = weak.upgrade() {
                ui.set_tab(tab.clamp(0, 3));
            }
        });
    }

    // ── Microphone self-test ────────────────────────────────────────────────
    // Opens the mic for ~1 s and reports the level and the input health.
    // The user can verify the microphone without recording a take.
    {
        let weak = ui.as_weak();
        let log = log.clone();
        let perm_wait = perm_wait.clone();
        ui.on_test_mic(move || {
            let Some(ui) = weak.upgrade() else { return };
            if !platform::has_mic_permission() {
                note(&weak, &log, "The microphone permission is missing.");
                platform::request_mic_permission();
                if let Ok(mut wait) = perm_wait.lock() {
                    *wait = Some(std::time::Instant::now());
                }
                ui.set_status_text("Microphone permission needed".into());
                return;
            }
            ui.set_status_text("Testing the microphone…".into());
            note(&weak, &log, "Testing the microphone…");
            let weak = weak.clone();
            let log = log.clone();
            std::thread::spawn(move || {
                let mut rec = audio::Recorder::new();
                let result = match rec.start() {
                    Ok(()) => {
                        let mut buf = vec![0f32; 1600];
                        let mut peak = 0.0f32;
                        let mut sum = 0f32;
                        let mut count = 0usize;
                        let deadline = std::time::Instant::now() + std::time::Duration::from_millis(1200);
                        while std::time::Instant::now() < deadline {
                            let n = rec.read(&mut buf);
                            if n > 0 {
                                for v in &buf[..n] {
                                    let a = v.abs();
                                    if a > peak {
                                        peak = a;
                                    }
                                    sum += v * v;
                                }
                                count += n;
                            } else {
                                std::thread::sleep(std::time::Duration::from_millis(20));
                            }
                        }
                        let rms = if count > 0 {
                            (sum / count as f32).sqrt()
                        } else {
                            0.0
                        };
                        rec.stop();
                        Some((peak, rms, rec.health()))
                    }
                    Err(e) => {
                        rec.stop();
                        platform::log(&format!("the microphone could not be opened: {e}"));
                        None
                    }
                };
                let _ = slint::invoke_from_event_loop(move || {
                    let Some(ui) = weak.upgrade() else { return };
                    match result {
                        None => {
                            ui.set_status_text("Could not open the microphone".into());
                            ui.set_mic_health("Could not open the microphone".into());
                        }
                        Some((peak, rms, health)) => {
                            let (status, card) = match health {
                                conditioner::HEALTH_SATURATED => (
                                    "The microphone input is clipped — lower the input gain".to_string(),
                                    "Input clipped — lower the mic gain".to_string(),
                                ),
                                conditioner::HEALTH_DEAD => (
                                    "No audio is arriving from the microphone".to_string(),
                                    "No signal from the microphone".to_string(),
                                ),
                                _ => (
                                    format!(
                                        "Microphone OK — level {:.0}%, peak {:.0}%",
                                        rms * 100.0,
                                        peak * 100.0
                                    ),
                                    String::new(),
                                ),
                            };
                            ui.set_status_text(status.clone().into());
                            ui.set_mic_health(card.into());
                            platform::log(&status);
                            let _ = &log;
                        }
                    }
                });
            });
        });
    }

    // Model picker. Every entry is a whisper.cpp model the bundled engine can
    // really load, so picking one always works.
    {
        let weak = ui.as_weak();
        let log = log.clone();
        ui.on_pick_model(move |idx| {
            let Some(ui) = weak.upgrade() else { return };
            let index = idx.max(0) as usize;
            let spec = stt::model(index);
            ui.set_selected_model(index as i32);
            stt::save_selected(index);
            note(&weak, &log, &format!("Model: {}", spec.name));
            if stt::model_installed(index) {
                ui.set_status_text(format!("{} is ready", spec.name).into());
            } else {
                ui.set_status_text(
                    format!("{} is not downloaded yet — tap Download", spec.name).into(),
                );
            }
        });
    }

    // Type the last transcript into the app in front, with real key events.
    {
        let weak_a = ui.as_weak();
        let log_a = log.clone();
        let text_a = last_text.clone();
        ui.on_type_into_app(move || {
            let Some(ui) = weak_a.upgrade() else { return };
            let text = text_a.lock().ok().map(|t| t.clone()).unwrap_or_default();
            if text.trim().is_empty() {
                note(&weak_a, &log_a, "There is nothing to type yet.");
                ui.set_status_text("Record something first".into());
                return;
            }
            match platform::inject_text(&text) {
                Ok(()) => {
                    note(&weak_a, &log_a, "Typed into the app in front.");
                    ui.set_status_text("Typed into the app in front".into());
                    bubble("Typed", &text, &weak_a, &log_a);
                }
                Err(e) => {
                    note(&weak_a, &log_a, &format!("Could not type: {e}"));
                    platform::copy_to_clipboard(&text);
                    ui.set_status_text("Typing is not allowed — copied instead".into());
                    bubble("Copied", &text, &weak_a, &log_a);
                }
            }
        });
        // Share the last transcript with the picker as well.
        let weak_b = ui.as_weak();
        let log_b = log.clone();
        let text_b = last_text.clone();
        ui.on_share_text(move || {
            let Some(ui) = weak_b.upgrade() else { return };
            let text = text_b.lock().ok().map(|t| t.clone()).unwrap_or_default();
            if text.trim().is_empty() {
                ui.set_status_text("Record something first".into());
                return;
            }
            if platform::share_text(&text) {
                note(&weak_b, &log_b, "Opened the share sheet.");
            }
        });
    }

    // Breathing ring while listening + finished-turn pickup (~20 fps).
    {
        let weak = ui.as_weak();
        let turns = turns.clone();
        let pending = pending.clone();
        let log = log.clone();
        let perm_wait = perm_wait.clone();
        let last_text = last_text.clone();
        let mut phase: f32 = 0.0;
        let mut ticks: u32 = 0;
        let timer = slint::Timer::default();
        timer.start(
            slint::TimerMode::Repeated,
            std::time::Duration::from_millis(50),
            move || {
                let Some(ui) = weak.upgrade() else { return };
                if ui.get_recording() {
                    phase = (phase + 0.17) % std::f32::consts::TAU;
                    let p = (phase.sin() + 1.0) * 0.5;
                    ui.set_pulse(p);
                } else if ui.get_pulse() != 0.0 {
                    phase = 0.0;
                    ui.set_pulse(0.0);
                }

                // A permission dialog is open: watch for the answer.
                let waiting = perm_wait.lock().ok().and_then(|w| *w);
                if let Some(since) = waiting {
                    if platform::has_mic_permission() {
                        if let Ok(mut w) = perm_wait.lock() {
                            *w = None;
                        }
                        ui.set_mic_ok(true);
                        ui.set_status_text("Microphone ready — tap the mic".into());
                        note(&weak, &log, "Microphone permission granted.");
                    } else if since.elapsed() > std::time::Duration::from_secs(90) {
                        if let Ok(mut w) = perm_wait.lock() {
                            *w = None;
                        }
                        ui.set_status_text("Microphone permission was not granted".into());
                        note(&weak, &log, "Microphone permission was not granted.");
                    }
                }

                // Did the floating bubble fail? Tell the user instead of
                // pretending everything is fine.
                if let Some(message) = platform::take_overlay_error() {
                    note(&weak, &log, &message);
                    ui.set_status_text(message.into());
                }

                // The overlay permission is granted in the system settings:
                // pick it up when the user comes back.
                ticks += 1;
                #[cfg(not(target_os = "android"))]
                if ticks == 2 {
                    // The winit backend sizes the window from the content, so
                    // the phone size has to be applied once it exists.
                    ui.window()
                        .set_size(slint::LogicalSize::new(412.0, 760.0));
                }
                if ticks % 40 == 0 {
                    let overlay = platform::can_draw_overlays();
                    if overlay != ui.get_overlay_ok() {
                        ui.set_overlay_ok(overlay);
                        note(&weak, &log, "Floating bubble permission updated.");
                    }
                    // The model may have been downloaded in the background.
                    let index = ui.get_selected_model().max(0) as usize;
                    let installed = stt::model_installed(index);
                    if installed != ui.get_model_installed() {
                        ui.set_model_installed(installed);
                        ui.set_models(Rc::new(slint::VecModel::from(model_catalog())).into());
                    }
                }

                // A finished turn arrived from the worker thread.
                let done = pending.lock().ok().and_then(|mut p| p.take());
                if let Some(turn) = done {
                    ui.set_recording(false);
                    ui.set_busy(false);
                    if turn.ok {
                        ui.set_live_text(turn.text.clone().into());
                        ui.set_has_text(true);
                        // Remember it for the "type into the app" action.
                        if let Ok(mut slot) = last_text.lock() {
                            *slot = turn.text.clone();
                        }
                        // The clipboard always gets the text — that works
                        // everywhere, even when the app may not inject keys.
                        platform::copy_to_clipboard(&turn.text);
                        turns.push(Turn {
                            text: turn.text.clone().into(),
                            when: "just now".into(),
                            seconds: turn.seconds,
                        });

                        // If another app is in front, type the text straight
                        // into it with real key events.
                        let mut status = "Copied to the clipboard".to_string();
                        if !platform::has_window_focus() {
                            match platform::inject_text(&turn.text) {
                                Ok(()) => {
                                    status = "Typed into the app in front".to_string();
                                    note(&weak, &log, "Typed into the app in front.");
                                }
                                Err(e) => {
                                    note(
                                        &weak,
                                        &log,
                                        &format!("Typing into the app is not allowed: {e}"),
                                    );
                                }
                            }
                        }
                        ui.set_status_text(status.clone().into());
                        bubble(&status, &turn.text, &weak, &log);
                        note(&weak, &log, "Transcribed and copied to the clipboard.");
                    } else {
                        ui.set_status_text(turn.status.clone().into());
                        bubble(&turn.status, "", &weak, &log);
                        note(&weak, &log, &turn.status);
                    }
                }
            },
        );
        // Leak the timer into the UI lifetime (the event loop owns it).
        std::mem::forget(timer);
    }

    // Initial setup state.
    {
        let weak = ui.as_weak();
        let log = log.clone();
        let mic_ok = platform::has_mic_permission();
        // Pick up the model the user chose last time.
        let index = stt::selected();
        ui.set_selected_model(index as i32);
        let model_ok = stt::model_installed(index);
        let overlay_ok = platform::can_draw_overlays();
        ui.set_mic_ok(mic_ok);
        ui.set_model_installed(model_ok);
        ui.set_overlay_ok(overlay_ok);
        ui.set_status_text(if !mic_ok {
            "Step 1: allow the microphone".into()
        } else if !model_ok {
            "Step 2: download the model".into()
        } else {
            "Ready — tap the mic".into()
        });
        note(&weak, &log, "QuickSTT started.");

        // The bubble stays on if the user left it on: put it back on the
        // screen so it is there in every app, not only inside QuickSTT.
        let bubble_on = platform::overlay_enabled() && overlay_ok;
        ui.set_overlay_enabled(bubble_on);
        if bubble_on {
            bubble("QuickSTT ready", "", &weak, &log);
        }
    }

    ui.run().expect("Slint event loop failed");
}

/// Is this take silent?
///
/// The thresholds are the desktop app's proven ones (`wav_is_silent`),
/// converted to the f32 scale: a peak floor of 600/32768, an rms floor of
/// 50/32768 and a minimum length of 0.25 s. Whisper hallucinates short
/// words ("you", "yeah") on noise-only audio, so a silent take must never
/// reach the engine.
fn is_silent(samples: &[f32]) -> bool {
    if samples.is_empty() {
        return true;
    }
    let peak = samples.iter().fold(0.0f32, |m, s| m.max(s.abs()));
    let sum: f32 = samples.iter().map(|s| s * s).sum();
    let rms = (sum / samples.len() as f32).sqrt();
    let secs = samples.len() as f32 / audio::SAMPLE_RATE as f32;
    secs < 0.25 || peak < 600.0 / 32768.0 || rms < 50.0 / 32768.0
}

/// Transcribe the recorded audio (worker thread) and hand the result to the
/// UI thread.
///
/// `health` is the recorder's input health: a clipped or dead microphone
/// is reported as such instead of producing a bogus transcript.
/// `model` is the index of the whisper model to run.
fn transcribe(
    samples: Vec<f32>,
    health: u8,
    model: usize,
    pending: &Arc<Mutex<Option<PendingTurn>>>,
) {
    let seconds = (samples.len() as f32 / audio::SAMPLE_RATE as f32).ceil() as i32;
    let fail = |status: String| {
        if let Ok(mut slot) = pending.lock() {
            *slot = Some(PendingTurn {
                text: String::new(),
                seconds: 0,
                status,
                ok: false,
            });
        }
    };
    if samples.is_empty() {
        fail("Nothing was recorded".into());
        return;
    }
    // A broken microphone is a setup problem, not a transcription problem:
    // say exactly that instead of shipping noise to the engine.
    if health == conditioner::HEALTH_SATURATED {
        fail("Microphone input is clipped — lower the input gain".into());
        return;
    }
    if health == conditioner::HEALTH_DEAD {
        fail("No audio is arriving from the microphone".into());
        return;
    }
    // Silence gate: never hand a silent take to whisper — it hallucinates
    // short words ("you", "yeah") on noise-only audio.
    if is_silent(&samples) {
        fail("No speech detected".into());
        return;
    }
    let wav = std::env::temp_dir().join("quickstt-turn.wav");
    let result = audio::write_wav(&wav, &samples, audio::SAMPLE_RATE as u32)
        .map_err(|e| e.to_string())
        .and_then(|_| stt::transcribe(&wav, model));
    if let Ok(mut slot) = pending.lock() {
        *slot = Some(match result {
            Ok(text) if !text.trim().is_empty() => PendingTurn {
                text,
                seconds,
                status: "Copied to the clipboard".into(),
                ok: true,
            },
            Ok(_) => PendingTurn {
                text: String::new(),
                seconds,
                status: "No speech detected".into(),
                ok: false,
            },
            Err(e) => PendingTurn {
                text: String::new(),
                seconds,
                status: format!("Transcription failed: {e}"),
                ok: false,
            },
        });
    }
}



#[cfg(test)]
mod tests {
    use super::*;

    /// Silence must be caught before the engine sees it — whisper answers
    /// "you" or "yeah" to noise, which is worse than no answer.
    #[test]
    fn silence_gate_rejects_noise_only_audio() {
        assert!(is_silent(&[]));
        // A very short take is not worth transcribing.
        assert!(is_silent(&vec![0.2; 100]));
        // Steady noise far below the speech floor.
        let noise: Vec<f32> = (0..16_000)
            .map(|i| ((i as f32) * 0.01).sin() * 0.001)
            .collect();
        assert!(is_silent(&noise));
    }

    /// Real speech passes the gate.
    #[test]
    fn silence_gate_lets_speech_through() {
        let speech: Vec<f32> = (0..16_000)
            .map(|i| ((i as f32) * 0.05).sin() * 0.2)
            .collect();
        assert!(!is_silent(&speech));
    }
}
