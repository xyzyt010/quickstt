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
pub extern "C" fn android_main(app: slint::android::AndroidApp) {
    if let Err(e) = slint::android::init(app) {
        eprintln!("QuickSTT: Slint Android backend failed to start: {e}");
        return;
    }
    run();
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
                                ui.set_mic_health(card.into());
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
        ui.on_test_mic(move || {
            let Some(ui) = weak.upgrade() else { return };
            if !platform::has_mic_permission() {
                platform::request_mic_permission();
                ui.set_status_text("Microphone permission needed".into());
                return;
            }
            ui.set_status_text("Testing the microphone…".into());
            let weak = weak.clone();
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
                        let _ = e;
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
                            ui.set_status_text(status.into());
                            ui.set_mic_health(card.into());
                        }
                    }
                });
            });
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
