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
use slint::Model;
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

/// The engines the app knows about.
///
/// Only the whisper.cpp tiny.en model runs on Android — the bigger engines
/// are part of the desktop build. The catalog is built from the *real* state
/// of the device, so the Models tab never shows a button that cannot work.
fn model_catalog() -> Vec<ModelItem> {
    let installed = stt::model_installed();
    vec![
        ModelItem {
            name: "Whisper Tiny EN".into(),
            detail: "English · on-device · fastest · auto-selected".into(),
            size: "32 MB".into(),
            installed,
            supported: true,
        },
        ModelItem {
            name: "SenseVoice Small".into(),
            detail: "5 languages · auto-detect".into(),
            size: "229 MB".into(),
            installed: false,
            supported: false,
        },
        ModelItem {
            name: "Parakeet TDT 0.6B".into(),
            detail: "English · highest accuracy".into(),
            size: "640 MB".into(),
            installed: false,
            supported: false,
        },
        ModelItem {
            name: "Phonon-2 0.6B".into(),
            detail: "English · highest compact accuracy".into(),
            size: "688 MB".into(),
            installed: false,
            supported: false,
        },
    ]
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

    // Shared between the UI thread and the capture thread.
    let stop_flag: Arc<AtomicBool> = Arc::new(AtomicBool::new(false));
    let pending: Arc<Mutex<Option<PendingTurn>>> = Arc::new(Mutex::new(None));
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
            let weak = weak.clone();
            let log = log.clone();
            // Download off the UI thread; the UI only receives progress.
            std::thread::spawn(move || {
                note(&weak, &log, "Downloading the speech model (32 MB)…");
                let result = stt::download_model(|done, total| {
                    let frac = if total > 0 {
                        (done as f32 / total as f32).clamp(0.0, 1.0)
                    } else {
                        0.0
                    };
                    let progress_weak = weak.clone();
                    let _ = slint::invoke_from_event_loop(move || {
                        if let Some(ui) = progress_weak.upgrade() {
                            ui.set_model_progress(frac);
                            ui.set_model_downloading(true);
                            ui.set_status_text(
                                format!("Downloading the model… {}%", (frac * 100.0) as i32).into(),
                            );
                        }
                    });
                });
                let done_weak = weak.clone();
                let done_log = log.clone();
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(ui) = done_weak.upgrade() {
                        ui.set_model_downloading(false);
                        match result {
                            Ok(()) => {
                                ui.set_model_installed(true);
                                ui.set_model_progress(1.0);
                                ui.set_models(Rc::new(slint::VecModel::from(model_catalog())).into());
                                ui.set_status_text("Model ready — tap the mic".into());
                                note(&done_weak, &done_log, "Model installed.");
                            }
                            Err(e) => {
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
            let allowed = platform::can_draw_overlays();
            if allowed {
                platform::hide_overlay();
                note(&weak, &log, "Floating bubble hidden.");
            } else {
                platform::show_overlay("QuickSTT ready", "");
                note(&weak, &log, "Floating bubble shown.");
            }
            if let Some(ui) = weak.upgrade() {
                ui.set_overlay_ok(allowed);
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
            if !stt::model_installed() {
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
            platform::show_overlay("Listening…", "");

            let weak = weak.clone();
            let stop_flag = stop_flag.clone();
            let pending = pending.clone();
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
                transcribe(samples, health, &pending);
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

    // Model picker — only engines that exist on Android can be selected.
    {
        let weak = ui.as_weak();
        let log = log.clone();
        ui.on_pick_model(move |idx| {
            let Some(ui) = weak.upgrade() else { return };
            let models = ui.get_models();
            let Some(model) = models.row_data(idx.max(0) as usize) else { return };
            if !model.supported {
                note(&weak, &log, &format!("{} is not available on Android yet.", model.name));
                ui.set_status_text(format!("{} runs on the desktop build only.", model.name).into());
                return;
            }
            ui.set_selected_model(idx);
            note(&weak, &log, &format!("Model: {}", model.name));
        });
    }

    // Breathing ring while listening + finished-turn pickup (~20 fps).
    {
        let weak = ui.as_weak();
        let turns = turns.clone();
        let pending = pending.clone();
        let log = log.clone();
        let perm_wait = perm_wait.clone();
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
                    let installed = stt::model_installed();
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
                        ui.set_status_text(turn.status.clone().into());
                        platform::copy_to_clipboard(&turn.text);
                        platform::show_overlay("Copied", &turn.text);
                        turns.push(Turn {
                            text: turn.text.into(),
                            when: "just now".into(),
                            seconds: turn.seconds,
                        });
                        note(&weak, &log, "Transcribed and copied to the clipboard.");
                    } else {
                        ui.set_status_text(turn.status.clone().into());
                        platform::show_overlay(&turn.status, "");
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
        let mic_ok = platform::has_mic_permission();
        let model_ok = stt::model_installed();
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
    }

    ui.run().expect("Slint event loop failed");
}

/// Transcribe the recorded audio (worker thread) and hand the result to the
/// UI thread.
///
/// `health` is the recorder's input health: a clipped or dead microphone
/// is reported as such instead of producing a bogus transcript.
fn transcribe(samples: Vec<f32>, health: u8, pending: &Arc<Mutex<Option<PendingTurn>>>) {
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
    // short words ("you", "yeah") on noise-only audio. The thresholds are
    // the desktop's proven ones (wav_is_silent), converted to the f32
    // scale: peak floor 600/32768, rms floor 50/32768, 0.25 s minimum.
    let peak = samples.iter().fold(0.0f32, |m, s| m.max(s.abs()));
    let sum: f32 = samples.iter().map(|s| s * s).sum();
    let rms = (sum / samples.len().max(1) as f32).sqrt();
    let secs = samples.len() as f32 / audio::SAMPLE_RATE as f32;
    if secs < 0.25 || peak < 600.0 / 32768.0 || rms < 50.0 / 32768.0 {
        fail("No speech detected".into());
        return;
    }
    let wav = std::env::temp_dir().join("quickstt-turn.wav");
    let result = audio::write_wav(&wav, &samples, audio::SAMPLE_RATE as u32)
        .map_err(|e| e.to_string())
        .and_then(|_| stt::transcribe(&wav));
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


