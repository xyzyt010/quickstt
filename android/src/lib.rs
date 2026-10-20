//! QuickSTT for Android — touch-first dictation.
//!
//! The UI is a dedicated touch-first Slint scene (`ui/android.slint`), not the
//! desktop dashboard: phones get a bottom nav, a thumb-reachable record orb
//! and 48+ DIP targets.
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
//! Build:
//!   * `cargo apk build` → Android APK (`android_main` below)
//!   * `cargo run --bin quickstt-android-preview` → desktop preview window

mod audio;
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

/// Wire callbacks and run the event loop.
pub fn run() {
    let ui = AppWindow::new().expect("could not create the QuickSTT window");

    // The desktop preview should look like a phone. (On Android the surface is
    // the device screen; the request is ignored there.)
    #[cfg(not(target_os = "android"))]
    ui.window()
        .set_size(slint::LogicalSize::new(412.0, 760.0));

    // Recent turns are a growable model so a finished dictation can appear
    // immediately, exactly like the desktop history list.
    let turns = Rc::new(slint::VecModel::from(Vec::<Turn>::new()));
    ui.set_turns(turns.clone().into());

    // Shared between the UI thread and the capture thread.
    let stop_flag: Arc<AtomicBool> = Arc::new(AtomicBool::new(false));
    let pending: Arc<Mutex<Option<PendingTurn>>> = Arc::new(Mutex::new(None));

    // ── Setup state (permissions + model) ──────────────────────────────────
    {
        let weak = ui.as_weak();
        ui.on_grant_mic(move || {
            platform::request_mic_permission();
            if let Some(ui) = weak.upgrade() {
                ui.set_status_text("Waiting for the permission dialog…".into());
            }
        });
    }
    {
        let weak = ui.as_weak();
        ui.on_download_model(move || {
            let weak = weak.clone();
            // Download off the UI thread; the UI only receives progress.
            std::thread::spawn(move || {
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
                let _ = slint::invoke_from_event_loop(move || {
                    if let Some(ui) = done_weak.upgrade() {
                        ui.set_model_downloading(false);
                        match result {
                            Ok(()) => {
                                ui.set_model_installed(true);
                                ui.set_model_progress(1.0);
                                ui.set_status_text("Model ready — tap the mic".into());
                            }
                            Err(e) => {
                                ui.set_status_text(format!("Download failed: {e}").into());
                            }
                        }
                    }
                });
            });
        });
    }
    {
        let weak = ui.as_weak();
        ui.on_open_overlay_settings(move || {
            platform::open_overlay_settings();
            if let Some(ui) = weak.upgrade() {
                ui.set_status_text("Enable “Display over other apps”, then come back".into());
            }
        });
    }
    {
        let weak = ui.as_weak();
        ui.on_toggle_overlay(move || {
            let allowed = platform::can_draw_overlays();
            if allowed {
                platform::hide_overlay();
            } else {
                platform::show_overlay("QuickSTT ready", "");
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
        ui.on_toggle_record(move || {
            let Some(ui) = weak.upgrade() else { return };

            if ui.get_recording() {
                // Stop: ask the capture thread to wrap up.
                stop_flag.store(true, Ordering::SeqCst);
                ui.set_recording(false);
                ui.set_status_text("Finishing…".into());
                return;
            }

            // Start.
            if !platform::has_mic_permission() {
                platform::request_mic_permission();
                ui.set_status_text("Microphone permission needed".into());
                return;
            }
            if !stt::model_installed() {
                ui.set_status_text("Download the model first (step 2)".into());
                ui.set_tab(0);
                return;
            }
            let mut session = Session::new();
            if let Err(e) = session.recorder.start() {
                ui.set_status_text(format!("Microphone failed: {e}").into());
                return;
            }
            stop_flag.store(false, Ordering::SeqCst);
            ui.set_recording(true);
            ui.set_busy(false);
            ui.set_live_text("Listening… speak now.".into());
            ui.set_status_text("Recording".into());
            platform::show_overlay("Listening…", "");

            let weak = weak.clone();
            let stop_flag = stop_flag.clone();
            let pending = pending.clone();
            std::thread::spawn(move || {
                let mut session = session;
                let mut chunk = vec![0f32; 1600]; // 100 ms
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
                    if rms > 0.012 {
                        session.spoke = true;
                        session.last_voice = std::time::Instant::now();
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
                transcribe(samples, &pending);
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

    // Model picker.
    {
        let weak = ui.as_weak();
        ui.on_pick_model(move |idx| {
            if let Some(ui) = weak.upgrade() {
                let count = ui.get_models().row_count() as i32;
                if idx >= 0 && idx < count {
                    ui.set_selected_model(idx);
                }
            }
        });
    }

    // Breathing ring while listening + finished-turn pickup (~20 fps).
    {
        let weak = ui.as_weak();
        let turns = turns.clone();
        let pending = pending.clone();
        let mut phase: f32 = 0.0;
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
                    } else {
                        ui.set_status_text(turn.status.clone().into());
                        platform::show_overlay(&turn.status, "");
                    }
                }
            },
        );
        // Leak the timer into the UI lifetime (the event loop owns it).
        std::mem::forget(timer);
    }

    // Initial setup state.
    {
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
    }

    ui.run().expect("Slint event loop failed");
}

/// Transcribe the recorded audio (worker thread) and hand the result to the
/// UI thread.
fn transcribe(samples: Vec<f32>, pending: &Arc<Mutex<Option<PendingTurn>>>) {
    let seconds = (samples.len() as f32 / audio::SAMPLE_RATE as f32).ceil() as i32;
    if samples.is_empty() {
        if let Ok(mut slot) = pending.lock() {
            *slot = Some(PendingTurn {
                text: String::new(),
                seconds: 0,
                status: "Nothing was recorded".into(),
                ok: false,
            });
        }
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
