//! QuickSTT for Android — touch-first on-device speech-to-text.
//!
//! Features:
//!   * Real 16 kHz microphone capture via NDK AAudio (`src/aaudio.c`),
//!   * Three ARM64 NEON-optimized speech engines (`needle`, `whisper-cli`,
//!     `transcribe-cli`) with **Cactus Whistle 16M** (`whistle.cact`) inbuilt
//!     directly inside the APK,
//!   * Real RAM model onloading & offloading (persistent `transcribe-cli --worker`
//!     sessions + pinned model buffers + idle auto-offload),
//!   * Always-on expandable floating bubble (`Bubble.java`): a sleek rounded-corner
//!     icon that morphs on press into a circular stadium pill with a pure black
//!     background, slight purple highlights, and 60/120fps super-smooth white
//!     waveforms (no microphone icons).

mod audio;
mod conditioner;
mod platform;
mod stt;

slint::include_modules!();

use slint::{ComponentHandle, Model};
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
    spoke: bool,
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
#[cfg(target_os = "android")]
#[no_mangle]
pub fn android_main(app: slint::android::AndroidApp) {
    platform::init(app.clone());
    if let Err(e) = slint::android::init_with_event_listener(app, |event| {
        platform::log(&format!("android event: {event:?}"));
    }) {
        eprintln!("QuickSTT: Slint Android backend failed to start: {e}");
        return;
    }
    run();
}

/// The UI handle used by JNI callbacks from the Java overlay bubble.
static BUBBLE_UI: Mutex<Option<slint::Weak<AppWindow>>> = Mutex::new(None);

/// Called by the Java bubble when the user taps one of its interactive zones.
///
/// * 1 = start/stop recording
/// * 2 = type the last transcript into the focused app
/// * 3 = hide the bubble
#[no_mangle]
pub extern "C" fn Java_ai_quickstt_app_Bubble_nativeBubbleAction(
    _env: *mut std::ffi::c_void,
    _class: *mut std::ffi::c_void,
    action: i32,
) {
    bubble_action(action);
}

fn bubble_action(action: i32) {
    let ui = BUBBLE_UI.lock().ok().and_then(|g| g.clone());
    let Some(ui) = ui else { return };
    let _ = slint::invoke_from_event_loop(move || {
        let Some(ui) = ui.upgrade() else { return };
        match action {
            1 => {
                ui.set_bubble_expanded(true);
                ui.invoke_toggle_record();
            }
            2 => ui.invoke_type_into_app(),
            3 => platform::hide_overlay(),
            _ => {}
        }
    });
}

/// Called by the Java overlay helper when the Activity resumes.
#[no_mangle]
pub extern "C" fn Java_ai_quickstt_app_Overlay_nativeActivityResumed(
    _env: *mut std::ffi::c_void,
    _class: *mut std::ffi::c_void,
) {
    let ui = BUBBLE_UI.lock().ok().and_then(|g| g.clone());
    let Some(ui) = ui else { return };
    let _ = slint::invoke_from_event_loop(move || {
        let Some(ui) = ui.upgrade() else { return };
        refresh_overlay_state(&ui);
    });
}

/// Per-model download state: `(downloading, progress, info_text)`.
static DOWNLOADS: Mutex<Vec<(bool, f32, String)>> = Mutex::new(Vec::new());

fn set_download_state(index: usize, downloading: bool, progress: f32, info: String) {
    let Ok(mut list) = DOWNLOADS.lock() else { return };
    if list.len() < stt::MODELS.len() {
        list.resize(stt::MODELS.len(), (false, 0.0, String::new()));
    }
    if let Some(slot) = list.get_mut(index) {
        *slot = (downloading, progress, info);
    }
}

fn download_state(index: usize) -> (bool, f32, String) {
    let Ok(list) = DOWNLOADS.lock() else {
        return (false, 0.0, String::new());
    };
    list.get(index)
        .cloned()
        .unwrap_or((false, 0.0, String::new()))
}

/// Build the Slint model catalog with live installation, download, and RAM onload states.
fn model_catalog() -> Vec<ModelItem> {
    let loaded_info = stt::loaded_model_status();
    stt::MODELS
        .iter()
        .enumerate()
        .map(|(index, spec)| {
            let (downloading, progress, dl_info) = download_state(index);
            let installed = stt::model_installed(index);
            let is_loaded = loaded_info.map(|(li, _, _, _)| li == index).unwrap_or(false);
            let ram_status = if let Some((li, ram_bytes, load_ms, idle_s)) = loaded_info {
                if li == index {
                    format!(
                        "Onloaded in RAM · {} · {} ms (idle {}s)",
                        stt::human_size(ram_bytes),
                        load_ms,
                        idle_s
                    )
                } else if installed {
                    format!(
                        "Offloaded (on disk · {})",
                        stt::human_size(stt::model_disk_bytes(index))
                    )
                } else {
                    format!("Not downloaded · {}", spec.size)
                }
            } else if installed {
                if spec.bundled {
                    format!(
                        "Installed (inbuilt · {}) — ready to onload",
                        stt::human_size(stt::model_disk_bytes(index).max(spec.min_bytes))
                    )
                } else {
                    format!(
                        "Installed on disk ({}) — tap Onload for RAM",
                        stt::human_size(stt::model_disk_bytes(index))
                    )
                }
            } else if spec.bundled {
                "Bundled in APK — tap Restore Inbuilt".to_string()
            } else {
                format!("Not downloaded · requires {}", spec.size)
            };

            ModelItem {
                name: spec.name.into(),
                detail: spec.detail.into(),
                size: spec.size.into(),
                installed,
                bundled: spec.bundled,
                downloading,
                progress,
                download_info: dl_info.into(),
                loaded: is_loaded,
                ram_status: ram_status.into(),
                engine: stt::engine_name(spec.engine).into(),
            }
        })
        .collect()
}

/// Sync the top-level UI properties with the current model selection and RAM state.
fn sync_model_ui(ui: &AppWindow) {
    let index = ui.get_selected_model().max(0) as usize;
    let installed = stt::model_installed(index);
    ui.set_model_installed(installed);

    if let Some((loaded_idx, ram_bytes, load_ms, _idle_s)) = stt::loaded_model_status() {
        let loaded_spec = stt::model(loaded_idx);
        ui.set_model_loaded(true);
        ui.set_model_ram_status(
            format!(
                "Onloaded in RAM: {} ({} · {} ms)",
                loaded_spec.name,
                stt::human_size(ram_bytes),
                load_ms
            )
            .into(),
        );
    } else {
        ui.set_model_loaded(false);
        let spec = stt::model(index);
        if installed {
            ui.set_model_ram_status(
                format!("Offloaded (RAM free) · {} ready on disk", spec.name).into(),
            );
        } else {
            ui.set_model_ram_status(
                format!("{} not downloaded yet ({})", spec.name, spec.size).into(),
            );
        }
    }
    ui.set_models(Rc::new(slint::VecModel::from(model_catalog())).into());
}

fn on_ui<F: FnOnce() + Send + 'static>(f: F) {
    let _ = slint::invoke_from_event_loop(f);
}

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

fn refresh_overlay_state(ui: &AppWindow) {
    ui.set_auto_type_ok(platform::accessibility_enabled());
    let granted = platform::can_draw_overlays();
    let changed = granted != ui.get_overlay_ok();
    ui.set_overlay_ok(granted);
    if !changed {
        return;
    }
    if granted {
        if platform::overlay_enabled() {
            match platform::show_overlay("QuickSTT ready", "") {
                Ok(()) => {
                    ui.set_overlay_enabled(true);
                    let _ = platform::start_service();
                    ui.set_background_ok(true);
                    ui.set_status_text("Floating squircle pill active".into());
                }
                Err(e) => {
                    ui.set_status_text(format!("Pill could not be shown: {e}").into());
                }
            }
        } else {
            ui.set_status_text("Overlay allowed — tap Show Pill to enable".into());
        }
    } else {
        platform::hide_overlay();
        ui.set_overlay_enabled(false);
        ui.set_status_text("Overlay permission withdrawn".into());
    }
}

static LAST_TEST: Mutex<String> = Mutex::new(String::new());

fn set_last_test(report: &str) {
    if let Ok(mut slot) = LAST_TEST.lock() {
        *slot = report.to_string();
    }
}

fn last_test() -> String {
    LAST_TEST.lock().ok().map(|s| s.clone()).unwrap_or_default()
}

/// Test a model end to end: four seconds of speech through the engine.
fn run_model_test(index: usize, weak: slint::Weak<AppWindow>, log: Arc<Mutex<Vec<String>>>) {
    let spec = stt::model(index);
    let name = spec.name.to_string();
    let engine_name = stt::engine_name(spec.engine).to_string();
    std::thread::spawn(move || {
        let mut lines = vec![
            format!("{name} · {engine_name}"),
            format!("Engine: {}", stt::engine_info(spec.engine)),
            format!("Model: {}", stt::model_info(index)),
        ];
        if !stt::model_installed(index) {
            if spec.bundled {
                let _ = platform::extract_bundled_model(spec.file, spec.min_bytes);
            }
            if !stt::model_installed(index) {
                lines.push("Result: the model file is not downloaded yet".to_string());
                finish_test(lines.join("\n"), weak, log);
                return;
            }
        }

        // Onload model first and record onload time
        match stt::onload_model(index) {
            Ok(msg) => lines.push(format!("RAM State: {msg}")),
            Err(e) => lines.push(format!("RAM State: {e}")),
        }

        let mut recorder = audio::Recorder::new();
        if let Err(e) = recorder.start() {
            lines.push(format!("Result: the microphone could not be opened — {e}"));
            finish_test(lines.join("\n"), weak, log);
            return;
        }
        let mut samples: Vec<f32> = Vec::new();
        let mut chunk = vec![0f32; 1600];
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(4);
        while std::time::Instant::now() < deadline {
            let n = recorder.read(&mut chunk);
            if n > 0 {
                samples.extend_from_slice(&chunk[..n]);
                let lvl = recorder.rms.min(1.0);
                platform::update_overlay_level(lvl, true, false);
                let w = weak.clone();
                on_ui(move || {
                    if let Some(ui) = w.upgrade() {
                        ui.set_audio_level(lvl);
                    }
                });
            } else {
                std::thread::sleep(std::time::Duration::from_millis(10));
            }
        }
        recorder.stop();
        platform::update_overlay_level(0.0, false, true);
        let seconds = (samples.len() as f32 / audio::SAMPLE_RATE as f32).ceil() as i32;
        lines.push(format!("Recorded: {seconds} s (16 kHz mono PCM)"));

        let wav = stt::wav_path("quickstt-test.wav");
        let result = audio::write_wav(&wav, &samples, audio::SAMPLE_RATE as u32)
            .map_err(|e| format!("failed to write WAV ({}): {e}", wav.display()))
            .and_then(|_| stt::transcribe_timed(&wav, index));
        platform::update_overlay_level(0.0, false, false);
        match result {
            Ok((text, took)) => {
                let text = text.trim();
                if text.is_empty() {
                    lines.push(
                        "Result: engine ran cleanly (no speech detected in the 4s clip)".to_string(),
                    );
                } else {
                    lines.push(format!("Transcript: “{text}”"));
                }
                lines.push(format!("Inference Latency: {:.2} s", took));
            }
            Err(e) => lines.push(format!("Result: FAILED — {e}")),
        }
        finish_test(lines.join("\n"), weak, log);
    });
}

fn finish_test(report: String, weak: slint::Weak<AppWindow>, log: Arc<Mutex<Vec<String>>>) {
    set_last_test(&report);
    platform::log(&report);
    let ui_weak = weak.clone();
    let _ = slint::invoke_from_event_loop(move || {
        if let Some(ui) = ui_weak.upgrade() {
            ui.set_test_result(report.clone().into());
            ui.set_test_running(false);
            ui.set_audio_level(0.0);
            sync_model_ui(&ui);
        }
    });
    note(
        &weak,
        &log,
        "Model benchmark finished — see Benchmark card.",
    );
}

fn diagnostics_report(index: usize) -> String {
    let mut lines = vec![
        format!(
            "QuickSTT {} · ai.quickstt.app (ARM64 NEON)",
            env!("CARGO_PKG_VERSION")
        ),
        format!(
            "Microphone: {}",
            if platform::has_mic_permission() {
                "granted"
            } else {
                "missing"
            }
        ),
        format!(
            "Overlay permission: {}",
            if platform::can_draw_overlays() {
                "granted"
            } else {
                "missing"
            }
        ),
        format!(
            "Background keep-alive: {}",
            if platform::ignoring_battery_optimizations() {
                "unrestricted"
            } else {
                "battery restricted"
            }
        ),
        format!(
            "Notifications: {}",
            if platform::can_post_notifications() {
                "allowed"
            } else {
                "not allowed"
            }
        ),
    ];
    if let Some((li, ram, ms, idle)) = stt::loaded_model_status() {
        lines.push(format!(
            "RAM Onload: {} ({} · loaded in {} ms · idle {}s)",
            stt::model(li).name,
            stt::human_size(ram),
            ms,
            idle
        ));
    } else {
        lines.push("RAM Onload: no model currently resident in RAM".to_string());
    }
    for (id, name, _) in stt::ENGINES {
        lines.push(format!("Engine {name}: {}", stt::engine_info(id)));
    }
    for (idx, spec) in stt::MODELS.iter().enumerate() {
        let marker = if idx == index { "*" } else { " " };
        lines.push(format!(
            "[{marker}] {}: {}",
            spec.name,
            stt::model_info(idx)
        ));
    }
    let test = last_test();
    if test.is_empty() {
        lines.push("Last benchmark: not run yet".to_string());
    } else {
        lines.push(format!("Last benchmark:\n{test}"));
    }
    lines.join("\n")
}

/// Build the window and wire every callback.
pub fn build_ui() -> AppWindow {
    let ui = AppWindow::new().expect("could not create the QuickSTT window");

    // 1. Extract the inbuilt Cactus Whistle 16M (`whistle.cact`) model FIRST
    //    before building the model catalog so the UI immediately sees it installed!
    stt::ensure_bundled_models();

    #[cfg(not(target_os = "android"))]
    {
        ui.window()
            .set_size(slint::LogicalSize::new(412.0, 760.0));
        if let Ok(tab) = std::env::var("QS_TAB") {
            if let Ok(tab) = tab.parse::<i32>() {
                ui.set_tab(tab.clamp(0, 3));
            }
        }
    }

    let turns = Rc::new(slint::VecModel::from(Vec::<Turn>::new()));
    ui.set_turns(turns.clone().into());

    let log: Arc<Mutex<Vec<String>>> = Arc::new(Mutex::new(Vec::new()));

    if let Ok(mut slot) = BUBBLE_UI.lock() {
        *slot = Some(ui.as_weak());
    }

    let stop_flag: Arc<AtomicBool> = Arc::new(AtomicBool::new(false));
    let pending: Arc<Mutex<Option<PendingTurn>>> = Arc::new(Mutex::new(None));
    let last_text: Arc<Mutex<String>> = Arc::new(Mutex::new(String::new()));
    let perm_wait: Arc<Mutex<Option<std::time::Instant>>> = Arc::new(Mutex::new(None));

    // ── Permissions & Model Download / Uninstall / Onload / Offload ────────
    {
        let weak = ui.as_weak();
        let log = log.clone();
        let perm_wait = perm_wait.clone();
        ui.on_grant_mic(move || {
            note(&weak, &log, "Requesting microphone permission…");
            if let Ok(mut wait) = perm_wait.lock() {
                *wait = Some(std::time::Instant::now());
            }
            platform::request_mic_permission();
            if let Some(ui) = weak.upgrade() {
                ui.set_status_text("Waiting for microphone permission dialog…".into());
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
                note(&weak, &log, "A model download is already in progress.");
                return;
            }
            if stt::model_installed(index) {
                sync_model_ui(&ui);
                note(&weak, &log, "Model is already installed — onloading to RAM.");
                ui.invoke_onload_model(index as i32);
                return;
            }
            let spec = stt::model(index);
            let start_info = format!("Connecting to HuggingFace · 0% of {}", spec.size);
            ui.set_model_downloading(true);
            ui.set_model_progress(0.0);
            ui.set_model_download_info(start_info.clone().into());
            ui.set_status_text(format!("Downloading {} ({})…", spec.name, spec.size).into());
            set_download_state(index, true, 0.0, start_info);
            ui.set_models(Rc::new(slint::VecModel::from(model_catalog())).into());
            note(&weak, &log, &format!("Downloading {} ({})…", spec.name, spec.size));

            let weak = weak.clone();
            let log = log.clone();
            std::thread::spawn(move || {
                let started_at = std::time::Instant::now();
                let mut last_update =
                    std::time::Instant::now() - std::time::Duration::from_secs(1);
                let progress_weak = weak.clone();
                let name = spec.name;
                let fallback_total = spec.min_bytes;
                let result = stt::download_model(index, move |done, total| {
                    let eff_total = if total > 0 { total } else { fallback_total };
                    let is_complete = eff_total > 0 && done >= eff_total;
                    if !is_complete
                        && last_update.elapsed() < std::time::Duration::from_millis(80)
                    {
                        return;
                    }
                    last_update = std::time::Instant::now();
                    let frac = if eff_total > 0 {
                        (done as f32 / eff_total as f32).clamp(0.0, 1.0)
                    } else {
                        0.0
                    };
                    let done_mb = done as f64 / 1_048_576.0;
                    let total_mb = eff_total as f64 / 1_048_576.0;
                    let elapsed_s = started_at.elapsed().as_secs_f64().max(0.1);
                    let speed_mb = done_mb / elapsed_s;
                    let pct = (frac * 100.0).round() as i32;
                    let info = format!(
                        "{done_mb:.1} / {total_mb:.1} MB · {speed_mb:.1} MB/s · {pct}%"
                    );
                    set_download_state(index, true, frac, info.clone());
                    let progress_weak = progress_weak.clone();
                    on_ui(move || {
                        if let Some(ui) = progress_weak.upgrade() {
                            ui.set_model_progress(frac);
                            ui.set_model_downloading(true);
                            ui.set_model_download_info(info.clone().into());
                            ui.set_status_text(
                                format!("Downloading {name}… {pct}% ({done_mb:.1} MB)").into(),
                            );
                            ui.set_models(
                                Rc::new(slint::VecModel::from(model_catalog())).into(),
                            );
                        }
                    });
                });

                set_download_state(
                    index,
                    false,
                    if result.is_ok() { 1.0 } else { 0.0 },
                    String::new(),
                );

                // Automatically onload the newly downloaded model into RAM!
                let onload_outcome = if result.is_ok() {
                    Some(stt::onload_model(index))
                } else {
                    None
                };

                let done_weak = weak.clone();
                let done_log = log.clone();
                let name = spec.name;
                let error = result.err();
                on_ui(move || {
                    if let Some(ui) = done_weak.upgrade() {
                        ui.set_model_downloading(false);
                        ui.set_model_download_info("".into());
                        match error {
                            None => {
                                ui.set_model_progress(1.0);
                                sync_model_ui(&ui);
                                let status_msg = match onload_outcome {
                                    Some(Ok(msg)) => format!("{name} installed & {msg}"),
                                    _ => format!("{name} installed and ready"),
                                };
                                ui.set_status_text(status_msg.clone().into());
                                note(&done_weak, &done_log, &status_msg);
                            }
                            Some(e) => {
                                ui.set_model_progress(0.0);
                                sync_model_ui(&ui);
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
        ui.on_uninstall_model(move |idx| {
            let Some(ui) = weak.upgrade() else { return };
            let index = idx.max(0) as usize;
            let spec = stt::model(index);
            match stt::uninstall_model(index) {
                Ok(()) => {
                    note(&weak, &log, &format!("{} removed from disk.", spec.name));
                    ui.set_status_text(format!("{} deleted from storage", spec.name).into());
                    if ui.get_selected_model().max(0) as usize == index {
                        // Fall back to the inbuilt Cactus Whistle model (index 0)
                        ui.set_selected_model(0);
                        stt::save_selected(0);
                        let _ = stt::onload_model(0);
                    }
                    sync_model_ui(&ui);
                }
                Err(e) => {
                    note(
                        &weak,
                        &log,
                        &format!("Could not delete {}: {e}", spec.name),
                    );
                    ui.set_status_text(format!("Could not delete: {e}").into());
                }
            }
        });
    }
    {
        let weak = ui.as_weak();
        let log = log.clone();
        ui.on_onload_model(move |idx| {
            let Some(ui) = weak.upgrade() else { return };
            let index = idx.max(0) as usize;
            let spec = stt::model(index);
            ui.set_selected_model(index as i32);
            stt::save_selected(index);
            ui.set_model_loading(true);
            ui.set_loading_label("Onloading…".into());
            ui.set_status_text(format!("Onloading {}…", spec.name).into());
            ui.set_model_ram_status(format!("Onloading {} into RAM…", spec.name).into());
            platform::update_overlay_level(0.0, false, true);
            if ui.get_overlay_enabled() {
                bubble("Onloading…", "", &weak, &log);
            }
            note(&weak, &log, &format!("Onloading {} into RAM…", spec.name));

            let weak = weak.clone();
            let log = log.clone();
            std::thread::spawn(move || {
                let started = std::time::Instant::now();
                let res = stt::onload_model(index);
                let elapsed = started.elapsed();
                if elapsed < std::time::Duration::from_millis(320) {
                    std::thread::sleep(std::time::Duration::from_millis(320) - elapsed);
                }
                platform::update_overlay_level(0.0, false, false);
                on_ui(move || {
                    if let Some(ui) = weak.upgrade() {
                        ui.set_model_loading(false);
                        ui.set_loading_label("".into());
                        sync_model_ui(&ui);
                        if ui.get_overlay_enabled() {
                            bubble("Ready", "", &weak, &log);
                        }
                        match res {
                            Ok(msg) => {
                                ui.set_status_text(msg.clone().into());
                                note(&weak, &log, &msg);
                            }
                            Err(e) => {
                                ui.set_status_text(format!("Onload failed: {e}").into());
                                note(&weak, &log, &format!("Onload failed: {e}"));
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
        ui.on_offload_model(move || {
            let Some(ui) = weak.upgrade() else { return };
            ui.set_model_loading(true);
            ui.set_loading_label("Offloading…".into());
            ui.set_status_text("Offloading model from RAM…".into());
            platform::update_overlay_level(0.0, false, true);
            if ui.get_overlay_enabled() {
                bubble("Offloading…", "", &weak, &log);
            }
            let weak = weak.clone();
            let log = log.clone();
            std::thread::spawn(move || {
                let started = std::time::Instant::now();
                let res = stt::offload_model();
                let elapsed = started.elapsed();
                if elapsed < std::time::Duration::from_millis(320) {
                    std::thread::sleep(std::time::Duration::from_millis(320) - elapsed);
                }
                platform::update_overlay_level(0.0, false, false);
                on_ui(move || {
                    if let Some(ui) = weak.upgrade() {
                        ui.set_model_loading(false);
                        ui.set_loading_label("".into());
                        sync_model_ui(&ui);
                        if ui.get_overlay_enabled() {
                            bubble("Ready", "", &weak, &log);
                        }
                        match res {
                            Ok(msg) => {
                                ui.set_status_text(msg.clone().into());
                                note(&weak, &log, &msg);
                            }
                            Err(e) => {
                                ui.set_status_text(format!("Offload failed: {e}").into());
                                note(&weak, &log, &format!("Offload failed: {e}"));
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
        ui.on_toggle_auto_offload(move || {
            let Some(ui) = weak.upgrade() else { return };
            let next = !ui.get_auto_offload();
            ui.set_auto_offload(next);
            let msg = if next {
                "Auto onload/offload enabled"
            } else {
                "Model stays pinned in RAM after stop"
            };
            ui.set_status_text(msg.into());
            note(&weak, &log, msg);
        });
    }
    {
        let weak = ui.as_weak();
        let log = log.clone();
        ui.on_toggle_live_mode(move || {
            let Some(ui) = weak.upgrade() else { return };
            let next = !ui.get_live_mode();
            ui.set_live_mode(next);
            let msg = if next {
                "Live mode on — types into focused text box as you speak"
            } else {
                "On-stop mode — types or copies once when stopped"
            };
            ui.set_status_text(msg.into());
            note(&weak, &log, msg);
        });
    }
    {
        let weak = ui.as_weak();
        let log = log.clone();
        ui.on_open_auto_type_settings(move || {
            let Some(ui) = weak.upgrade() else { return };
            let already_enabled = platform::accessibility_enabled();
            match platform::open_accessibility_settings() {
                Ok(()) => {
                    ui.set_auto_type_ok(already_enabled);
                    if already_enabled {
                        ui.set_status_text("Select “QuickSTT Voice Auto-Type” in the keyboard picker".into());
                        note(&weak, &log, "Opened keyboard picker for QuickSTT Voice Auto-Type.");
                    } else {
                        ui.set_status_text("Enable “QuickSTT Voice Auto-Type” in On-screen keyboards".into());
                        note(&weak, &log, "Opened keyboard settings for QuickSTT Voice Auto-Type.");
                    }
                }
                Err(e) => {
                    ui.set_status_text(format!("Could not open keyboard settings: {e}").into());
                    note(&weak, &log, &format!("Keyboard settings error: {e}"));
                }
            }
        });
    }

    // ── Floating Bubble & Overlay Settings ─────────────────────────────────
    {
        let weak = ui.as_weak();
        let log = log.clone();
        ui.on_open_overlay_settings(move || {
            note(&weak, &log, "Opening system overlay permission settings…");
            let Some(ui) = weak.upgrade() else { return };
            match platform::open_overlay_settings() {
                Ok(()) => {
                    ui.set_status_text(
                        "Allow “Display over other apps”, then return — the waveform bubble appears automatically"
                            .into(),
                    );
                }
                Err(e) => {
                    note(&weak, &log, &format!("Settings could not be opened: {e}"));
                    ui.set_status_text(format!("Settings could not be opened: {e}").into());
                }
            }
        });
    }
    {
        let weak = ui.as_weak();
        let log = log.clone();
        ui.on_toggle_bubble_expand(move || {
            let Some(ui) = weak.upgrade() else { return };
            let next = !ui.get_bubble_expanded();
            ui.set_bubble_expanded(next);
            platform::set_overlay_expanded(next);
            let msg = if next {
                "Expanded bubble into circular waveform pill"
            } else {
                "Collapsed bubble into rounded-corner icon"
            };
            ui.set_status_text(msg.into());
            note(&weak, &log, msg);
        });
    }
    {
        let weak = ui.as_weak();
        let log = log.clone();
        ui.on_toggle_overlay(move || {
            let Some(ui) = weak.upgrade() else { return };
            let wanted = !ui.get_overlay_enabled();
            if !wanted {
                platform::hide_overlay();
                platform::stop_service();
                ui.set_overlay_enabled(false);
                platform::save_overlay_enabled(false);
                note(&weak, &log, "Floating waveform bubble disabled.");
                ui.set_status_text("Floating waveform bubble off".into());
                return;
            }
            platform::save_overlay_enabled(true);
            if !platform::can_draw_overlays() {
                note(
                    &weak,
                    &log,
                    "Allow “Display over other apps” in system settings — the bubble will appear automatically.",
                );
                ui.set_status_text("Allow “Display over other apps”, then come back".into());
                if let Err(e) = platform::open_overlay_settings() {
                    note(&weak, &log, &format!("Could not open settings: {e}"));
                    ui.set_status_text(format!("Could not open settings: {e}").into());
                }
                return;
            }
            match platform::show_overlay("QuickSTT ready", "") {
                Ok(()) => {
                    ui.set_overlay_enabled(true);
                    if !platform::can_post_notifications() {
                        platform::request_permissions(&[
                            "android.permission.POST_NOTIFICATIONS",
                        ]);
                    }
                    match platform::start_service() {
                        Ok(()) => {
                            ui.set_background_ok(true);
                            note(
                                &weak,
                                &log,
                                "Floating waveform bubble active — press the rounded icon to expand into the circular pill.",
                            );
                        }
                        Err(e) => {
                            note(
                                &weak,
                                &log,
                                &format!("Bubble active, background service warning: {e}"),
                            );
                        }
                    }
                    ui.set_status_text(
                        "Floating bubble active — press icon to expand into circular waveform pill"
                            .into(),
                    );
                }
                Err(e) => {
                    ui.set_overlay_enabled(false);
                    platform::save_overlay_enabled(false);
                    note(&weak, &log, &format!("Bubble failed: {e}"));
                    ui.set_status_text(format!("Bubble failed: {e}").into());
                }
            }
        });
    }

    // ── Record Toggle (16 kHz AAudio + Live Waveform + Auto-VAD) ───────────
    {
        let weak = ui.as_weak();
        let stop_flag = stop_flag.clone();
        let pending = pending.clone();
        let log = log.clone();
        let perm_wait = perm_wait.clone();
        ui.on_toggle_record(move || {
            let Some(ui) = weak.upgrade() else { return };

            if ui.get_recording() {
                stop_flag.store(true, Ordering::SeqCst);
                ui.set_recording(false);
                ui.set_busy(true);
                ui.set_loading_label("Transcribing…".into());
                ui.set_status_text("Transcribing…".into());
                platform::update_overlay_level(0.0, false, true);
                if ui.get_overlay_enabled() {
                    bubble("Transcribing…", "", &weak, &log);
                }
                note(&weak, &log, "Stopping & transcribing…");
                return;
            }

            if !platform::has_mic_permission() {
                note(&weak, &log, "Microphone permission needed — requesting now.");
                platform::request_mic_permission();
                if let Ok(mut wait) = perm_wait.lock() {
                    *wait = Some(std::time::Instant::now());
                }
                ui.set_status_text("Allow microphone permission to start dictating".into());
                ui.set_tab(0);
                return;
            }

            let mut model_index = ui.get_selected_model().max(0) as usize;
            if !stt::model_installed(model_index) {
                stt::ensure_bundled_models();
                if stt::model_installed(model_index) {
                    sync_model_ui(&ui);
                } else if stt::model_installed(0) {
                    model_index = 0;
                    ui.set_selected_model(0);
                    stt::save_selected(0);
                    sync_model_ui(&ui);
                    note(&weak, &log, "Using built-in Cactus Whistle 16M.");
                } else {
                    note(&weak, &log, "Selected model is not downloaded yet.");
                    ui.set_status_text("Download or select an installed model first".into());
                    ui.set_tab(2);
                    return;
                }
            }

            platform::begin_typing_turn();

            let mut session = Session::new();
            if let Err(e) = session.recorder.start() {
                ui.set_status_text(format!("Microphone error: {e}").into());
                note(&weak, &log, &format!("Microphone error: {e}"));
                return;
            }
            let live_mode = ui.get_live_mode();
            let needs_onload = stt::loaded_model_index() != Some(model_index);
            stop_flag.store(false, Ordering::SeqCst);
            ui.set_recording(true);
            ui.set_bubble_expanded(true);
            ui.set_busy(false);
            ui.set_model_loading(true);
            ui.set_loading_label("Onloading…".into());
            ui.set_live_text("Listening… speak now.".into());
            ui.set_status_text(
                format!("Onloading {} & listening…", stt::model(model_index).name).into(),
            );
            note(&weak, &log, "Automatic model onload & recording started.");
            platform::set_overlay_expanded(true);
            platform::update_overlay_level(0.15, false, true);
            if ui.get_overlay_enabled() {
                bubble("Onloading…", "", &weak, &log);
            }

            // Spawn automatic onload on background thread with visible round loading spinner
            {
                let onload_weak = weak.clone();
                let onload_log = log.clone();
                std::thread::spawn(move || {
                    let started = std::time::Instant::now();
                    if needs_onload {
                        let _ = stt::onload_model(model_index);
                    }
                    let elapsed = started.elapsed();
                    if elapsed < std::time::Duration::from_millis(280) {
                        std::thread::sleep(std::time::Duration::from_millis(280) - elapsed);
                    }
                    on_ui(move || {
                        if let Some(ui) = onload_weak.upgrade() {
                            ui.set_model_loading(false);
                            ui.set_loading_label("".into());
                            sync_model_ui(&ui);
                            if ui.get_recording() {
                                ui.set_status_text(
                                    format!("Listening ({})…", stt::model(model_index).name)
                                        .into(),
                                );
                                if ui.get_overlay_enabled() {
                                    bubble("Listening…", "", &onload_weak, &onload_log);
                                }
                            }
                        }
                    });
                });
            }

            let weak = weak.clone();
            let log_rec = log.clone();
            let stop_flag = stop_flag.clone();
            let pending = pending.clone();
            std::thread::spawn(move || {
                let mut session = session;
                let mut chunk = vec![0f32; 800];
                let mut last_health: u8 = conditioner::HEALTH_OK;
                let mut health_since = std::time::Instant::now();
                let mut last_live_decode = std::time::Instant::now();
                let live_in_flight = Arc::new(AtomicBool::new(false));
                let live_partial_seen = Arc::new(AtomicBool::new(false));

                loop {
                    let n = session.recorder.read(&mut chunk);
                    if n == 0 {
                        std::thread::sleep(std::time::Duration::from_millis(15));
                        if stop_flag.load(Ordering::SeqCst) {
                            break;
                        }
                        continue;
                    }
                    session.samples.extend_from_slice(&chunk[..n]);
                    let rms = session.recorder.rms;
                    let peak = session.recorder.peak;
                    if rms > 0.0055 {
                        session.spoke = true;
                        session.last_voice = std::time::Instant::now();
                    }

                    let visual_level = (rms * 4.2 + peak * 0.35).clamp(0.0, 1.0);
                    if session.started.elapsed() >= std::time::Duration::from_millis(280) {
                        platform::update_overlay_level(visual_level, true, false);
                    }

                    // Live real-time partial transcription & typing into focused text box
                    if live_mode
                        && session.spoke
                        && session.samples.len() >= 14_400
                        && last_live_decode.elapsed() >= std::time::Duration::from_millis(1200)
                        && !live_in_flight.swap(true, Ordering::SeqCst)
                    {
                        last_live_decode = std::time::Instant::now();
                        let snap = session.samples.clone();
                        let live_weak = weak.clone();
                        let live_stop = stop_flag.clone();
                        let flag = live_in_flight.clone();
                        let seen_flag = live_partial_seen.clone();
                        std::thread::spawn(move || {
                            let wav = stt::wav_path("quickstt-live.wav");
                            if audio::write_wav(&wav, &snap, audio::SAMPLE_RATE as u32).is_ok() {
                                if let Ok(partial) = stt::transcribe(&wav, model_index) {
                                    let trimmed = partial.trim().to_string();
                                    if !trimmed.is_empty() && !live_stop.load(Ordering::SeqCst) {
                                        seen_flag.store(true, Ordering::SeqCst);
                                        if platform::has_focused_text_box() {
                                            let _ = platform::type_live_partial(&trimmed);
                                        }
                                        on_ui(move || {
                                            if let Some(ui) = live_weak.upgrade() {
                                                if ui.get_recording() {
                                                    ui.set_live_text(trimmed.into());
                                                }
                                            }
                                        });
                                    }
                                }
                            }
                            flag.store(false, Ordering::SeqCst);
                        });
                    }

                    let health = session.recorder.health();
                    if health != last_health
                        && health_since.elapsed() > std::time::Duration::from_secs(2)
                    {
                        last_health = health;
                        health_since = std::time::Instant::now();
                        let (msg, card_msg) = match health {
                            conditioner::HEALTH_SATURATED => (
                                "Mic input clipped — lower gain",
                                "Input clipped",
                            ),
                            conditioner::HEALTH_DEAD => (
                                "No audio arriving from microphone",
                                "No mic signal",
                            ),
                            _ => ("Listening…", ""),
                        };
                        let hweak = weak.clone();
                        let card = card_msg.to_string();
                        on_ui(move || {
                            if let Some(ui) = hweak.upgrade() {
                                ui.set_status_text(msg.into());
                                ui.set_mic_health(card.into());
                            }
                        });
                    }

                    let quiet = session.last_voice.elapsed()
                        > std::time::Duration::from_millis(1600);
                    let too_long =
                        session.started.elapsed() > std::time::Duration::from_secs(60);
                    let secs_elapsed = session.started.elapsed().as_secs_f32();
                    let has_partial = live_partial_seen.load(Ordering::SeqCst);
                    let meter_weak = weak.clone();
                    on_ui(move || {
                        if let Some(ui) = meter_weak.upgrade() {
                            if ui.get_recording() {
                                ui.set_audio_level(visual_level);
                                if !has_partial {
                                    ui.set_live_text(
                                        format!("Listening ({secs_elapsed:.1}s)…").into(),
                                    );
                                }
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
                platform::update_overlay_level(0.0, false, true);
                let samples = std::mem::take(&mut session.samples);
                let health = session.recorder.health();
                let status_weak = weak.clone();
                let status_log = log_rec.clone();
                on_ui(move || {
                    if let Some(ui) = status_weak.upgrade() {
                        ui.set_recording(false);
                        ui.set_audio_level(0.0);
                        ui.set_busy(true);
                        ui.set_loading_label("Transcribing…".into());
                        ui.set_status_text("Transcribing…".into());
                        if ui.get_overlay_enabled() {
                            bubble("Transcribing…", "", &status_weak, &status_log);
                        }
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
                sync_model_ui(&ui);
            }
        });
    }

    // Touch probe.
    {
        let weak = ui.as_weak();
        ui.on_report_tap(move |x, y| {
            let (w, h, scale) = match weak.upgrade() {
                Some(ui) => {
                    let size = ui.window().size();
                    (size.width, size.height, ui.window().scale_factor())
                }
                None => (0, 0, 0.0),
            };
            let text = format!(
                "Tap at {x:.0}, {y:.0} px · window {w:.0}x{h:.0} · scale {scale:.2}"
            );
            platform::log(&format!("touch probe: {text}"));
            if let Some(ui) = weak.upgrade() {
                ui.set_touch_probe(text.into());
            }
        });
    }

    // Microphone self-test.
    {
        let weak = ui.as_weak();
        let log = log.clone();
        let perm_wait = perm_wait.clone();
        ui.on_test_mic(move || {
            let Some(ui) = weak.upgrade() else { return };
            if !platform::has_mic_permission() {
                note(&weak, &log, "Microphone permission missing.");
                platform::request_mic_permission();
                if let Ok(mut wait) = perm_wait.lock() {
                    *wait = Some(std::time::Instant::now());
                }
                ui.set_status_text("Microphone permission needed".into());
                return;
            }
            ui.set_status_text("Testing microphone (1.2s)…".into());
            note(&weak, &log, "Testing microphone…");
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
                        let deadline = std::time::Instant::now()
                            + std::time::Duration::from_millis(1200);
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
                        platform::log(&format!("microphone could not be opened: {e}"));
                        None
                    }
                };
                on_ui(move || {
                    let Some(ui) = weak.upgrade() else { return };
                    match result {
                        None => {
                            ui.set_status_text("Could not open the microphone".into());
                            ui.set_mic_health("Could not open the microphone".into());
                        }
                        Some((peak, rms, health)) => {
                            let (status, card) = match health {
                                conditioner::HEALTH_SATURATED => (
                                    "Microphone input is clipped — lower input gain".to_string(),
                                    "Input clipped — lower mic gain".to_string(),
                                ),
                                conditioner::HEALTH_DEAD => (
                                    "No audio arriving from microphone".to_string(),
                                    "No signal from microphone".to_string(),
                                ),
                                _ => (
                                    format!(
                                        "Microphone OK — RMS {:.0}%, peak {:.0}%",
                                        rms * 100.0,
                                        peak * 100.0
                                    ),
                                    String::new(),
                                ),
                            };
                            ui.set_status_text(status.clone().into());
                            ui.set_mic_health(card.into());
                            note(&weak, &log, &status);
                        }
                    }
                });
            });
        });
    }

    // Model picker: selects model and onloads it if installed.
    {
        let weak = ui.as_weak();
        let log = log.clone();
        ui.on_pick_model(move |idx| {
            let Some(ui) = weak.upgrade() else { return };
            let index = idx.max(0) as usize;
            let spec = stt::model(index);
            ui.set_selected_model(index as i32);
            stt::save_selected(index);
            sync_model_ui(&ui);
            note(&weak, &log, &format!("Selected model: {}", spec.name));
            if stt::model_installed(index) {
                ui.invoke_onload_model(index as i32);
            } else {
                ui.set_status_text(
                    format!("{} is not downloaded yet — tap Download", spec.name).into(),
                );
            }
        });
    }

    // 4-second live model benchmark.
    {
        let weak = ui.as_weak();
        let log = log.clone();
        ui.on_test_model(move |idx| {
            let Some(ui) = weak.upgrade() else { return };
            if ui.get_test_running() {
                note(&weak, &log, "A benchmark test is already running.");
                return;
            }
            let index = idx.max(0) as usize;
            ui.set_selected_model(index as i32);
            stt::save_selected(index);
            sync_model_ui(&ui);
            if !platform::has_mic_permission() {
                note(&weak, &log, "Microphone permission is required for the benchmark.");
                platform::request_mic_permission();
                ui.set_status_text("Allow microphone permission first".into());
                return;
            }
            ui.set_test_running(true);
            ui.set_test_result("Recording 4 seconds — speak a short phrase now…".into());
            ui.set_status_text(
                format!("Benchmarking {} (4s)…", stt::model(index).name).into(),
            );
            note(&weak, &log, "Benchmarking model — speak now…");
            run_model_test(index, weak.clone(), log.clone());
        });
    }

    // Diagnostics & self-test.
    {
        let weak = ui.as_weak();
        let log = log.clone();
        ui.on_run_diagnostics(move || {
            let Some(ui) = weak.upgrade() else { return };
            let index = ui.get_selected_model().max(0) as usize;
            let report = diagnostics_report(index);
            platform::log(&report);
            ui.set_diagnostics(report.into());
            ui.set_status_text("Self-test complete — see Diagnostics report".into());
            note(&weak, &log, "Self-test completed.");
        });
    }
    {
        let weak = ui.as_weak();
        let log = log.clone();
        ui.on_copy_diagnostics(move || {
            let Some(ui) = weak.upgrade() else { return };
            let index = ui.get_selected_model().max(0) as usize;
            let report = if ui.get_diagnostics().is_empty() {
                diagnostics_report(index)
            } else {
                ui.get_diagnostics().to_string()
            };
            platform::copy_to_clipboard(&report);
            ui.set_status_text("Diagnostics report copied to clipboard".into());
            note(&weak, &log, "Diagnostics report copied to clipboard.");
        });
    }

    // Battery exemption.
    {
        let weak = ui.as_weak();
        let log = log.clone();
        ui.on_request_battery_exemption(move || {
            let Some(ui) = weak.upgrade() else { return };
            match platform::request_battery_exemption() {
                Ok(()) => {
                    note(
                        &weak,
                        &log,
                        "Opening battery settings — allow QuickSTT unrestricted background usage.",
                    );
                    ui.set_status_text(
                        "Allow unrestricted background usage, then return".into(),
                    );
                }
                Err(e) => {
                    note(&weak, &log, &format!("Battery settings failed: {e}"));
                    ui.set_status_text(format!("Battery settings failed: {e}").into());
                }
            }
        });
    }

    // Output actions: Type into app, Copy turn, Share, Clear history.
    {
        let weak_a = ui.as_weak();
        let log_a = log.clone();
        let text_a = last_text.clone();
        ui.on_type_into_app(move || {
            let Some(ui) = weak_a.upgrade() else { return };
            let text = text_a.lock().ok().map(|t| t.clone()).unwrap_or_default();
            if text.trim().is_empty() {
                note(&weak_a, &log_a, "Nothing to type yet — record a phrase first.");
                ui.set_status_text("Record something first".into());
                return;
            }
            match platform::inject_text(&text) {
                Ok(()) => {
                    note(&weak_a, &log_a, "Typed directly into focused app.");
                    ui.set_status_text("Typed into the focused app".into());
                    if ui.get_overlay_enabled() {
                        bubble("Typed", &text, &weak_a, &log_a);
                    }
                }
                Err(e) => {
                    note(&weak_a, &log_a, &format!("Direct key injection fallback: {e}"));
                    platform::copy_to_clipboard(&text);
                    ui.set_status_text("Copied to clipboard — ready to paste".into());
                    if ui.get_overlay_enabled() {
                        bubble("Copied", &text, &weak_a, &log_a);
                    }
                }
            }
        });

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
                note(&weak_b, &log_b, "Opened Android share sheet.");
            }
        });

        let weak_c = ui.as_weak();
        let log_c = log.clone();
        ui.on_copy_turn(move |text| {
            let Some(ui) = weak_c.upgrade() else { return };
            let s = text.to_string();
            if !s.trim().is_empty() {
                platform::copy_to_clipboard(&s);
                ui.set_status_text("Copied transcript to clipboard".into());
                note(&weak_c, &log_c, "Copied transcript to clipboard.");
            }
        });

        let weak_d = ui.as_weak();
        let log_d = log.clone();
        let turns_clear = turns.clone();
        ui.on_clear_history(move || {
            let Some(ui) = weak_d.upgrade() else { return };
            while turns_clear.row_count() > 0 {
                turns_clear.remove(0);
            }
            ui.set_status_text("Cleared dictation history".into());
            note(&weak_d, &log_d, "Cleared dictation history.");
        });
    }

    // ── Smooth Waveform Animation & Background Event Loop (~25 fps) ────────
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
            std::time::Duration::from_millis(40),
            move || {
                let Some(ui) = weak.upgrade() else { return };
                if ui.get_busy() || ui.get_model_loading() {
                    phase = (phase + 0.24) % std::f32::consts::TAU;
                    ui.set_wave_phase(phase / std::f32::consts::TAU);
                }

                // Watch for microphone permission grant
                let waiting = perm_wait.lock().ok().and_then(|w| *w);
                if let Some(since) = waiting {
                    if platform::has_mic_permission() {
                        if let Ok(mut w) = perm_wait.lock() {
                            *w = None;
                        }
                        ui.set_mic_ok(true);
                        ui.set_status_text("Microphone ready — tap Start to dictate".into());
                        note(&weak, &log, "Microphone permission granted.");
                    } else if since.elapsed() > std::time::Duration::from_secs(90) {
                        if let Ok(mut w) = perm_wait.lock() {
                            *w = None;
                        }
                        ui.set_status_text("Microphone permission was not granted".into());
                        note(&weak, &log, "Microphone permission was not granted.");
                    }
                }

                if let Some(message) = platform::take_overlay_error() {
                    note(&weak, &log, &message);
                    ui.set_status_text(message.into());
                }

                ticks += 1;
                #[cfg(not(target_os = "android"))]
                if ticks == 2 {
                    ui.window()
                        .set_size(slint::LogicalSize::new(412.0, 760.0));
                }

                // Once per second: refresh permissions, battery, and idle auto-offload
                if ticks % 25 == 0 {
                    refresh_overlay_state(&ui);
                    ui.set_battery_ok(platform::ignoring_battery_optimizations());
                    if ui.get_auto_offload()
                        && !ui.get_recording()
                        && !ui.get_busy()
                        && !ui.get_model_loading()
                    {
                        if let Some(offload_msg) = stt::check_idle_offload(120) {
                            sync_model_ui(&ui);
                            ui.set_status_text(offload_msg.clone().into());
                            note(&weak, &log, &offload_msg);
                        }
                    }
                    if !ui.get_model_downloading() && !ui.get_model_loading() {
                        sync_model_ui(&ui);
                    }
                }

                // Pick up finished transcription from worker thread
                let done = pending.lock().ok().and_then(|mut p| p.take());
                if let Some(turn) = done {
                    ui.set_recording(false);
                    ui.set_busy(false);
                    ui.set_audio_level(0.0);
                    sync_model_ui(&ui);

                    let final_status = if turn.ok {
                        ui.set_live_text(turn.text.clone().into());
                        ui.set_has_text(true);
                        if let Ok(mut slot) = last_text.lock() {
                            *slot = turn.text.clone();
                        }
                        turns.insert(
                            0,
                            Turn {
                                text: turn.text.clone().into(),
                                when: "just now".into(),
                                seconds: turn.seconds,
                            },
                        );

                        // Automatically type into focused text box; if no text box is focused,
                        // copy the full transcript straight to the clipboard all at once.
                        let typed_in_box = if platform::has_focused_text_box() {
                            platform::commit_final_text(&turn.text)
                        } else if !platform::has_window_focus() {
                            platform::inject_text(&turn.text).is_ok()
                        } else {
                            false
                        };

                        platform::copy_to_clipboard(&turn.text);

                        let status = if typed_in_box {
                            note(&weak, &log, "Automatically typed into focused text box.");
                            "Typed into text box".to_string()
                        } else {
                            note(
                                &weak,
                                &log,
                                "No focused text box — copied full transcript to clipboard.",
                            );
                            "Copied to clipboard".to_string()
                        };
                        ui.set_status_text(status.clone().into());
                        status
                    } else {
                        ui.set_status_text(turn.status.clone().into());
                        note(&weak, &log, &turn.status);
                        turn.status.clone()
                    };

                    // Automatic offload on stop (with round circular loading icon)
                    if ui.get_auto_offload() && stt::loaded_model_index().is_some() {
                        ui.set_model_loading(true);
                        ui.set_loading_label("Offloading…".into());
                        platform::update_overlay_level(0.0, false, true);
                        if ui.get_overlay_enabled() {
                            bubble("Offloading…", &turn.text, &weak, &log);
                        }
                        let off_weak = weak.clone();
                        let off_log = log.clone();
                        let off_text = turn.text.clone();
                        let status_after = final_status.clone();
                        std::thread::spawn(move || {
                            let started = std::time::Instant::now();
                            let _ = stt::offload_model();
                            let elapsed = started.elapsed();
                            if elapsed < std::time::Duration::from_millis(320) {
                                std::thread::sleep(std::time::Duration::from_millis(320) - elapsed);
                            }
                            platform::update_overlay_level(0.0, false, false);
                            on_ui(move || {
                                if let Some(ui) = off_weak.upgrade() {
                                    ui.set_model_loading(false);
                                    ui.set_loading_label("".into());
                                    sync_model_ui(&ui);
                                    ui.set_status_text(
                                        format!("{status_after} · RAM offloaded").into(),
                                    );
                                    if ui.get_overlay_enabled() {
                                        bubble(&status_after, &off_text, &off_weak, &off_log);
                                    }
                                }
                            });
                        });
                    } else {
                        ui.set_loading_label("".into());
                        platform::update_overlay_level(0.0, false, false);
                        if ui.get_overlay_enabled() {
                            bubble(&final_status, &turn.text, &weak, &log);
                        }
                    }
                }
            },
        );
        std::mem::forget(timer);
    }

    // ── Initial Startup State: Inbuilt Needle Model + Auto-Onload ──────────
    {
        let weak = ui.as_weak();
        let log = log.clone();
        let mic_ok = platform::has_mic_permission();
        let index = stt::selected();
        ui.set_selected_model(index as i32);
        ui.set_auto_type_ok(platform::accessibility_enabled());

        let initial_onload = stt::model_installed(index);
        if initial_onload {
            ui.set_model_loading(true);
            ui.set_loading_label("Onloading…".into());
            let weak = ui.as_weak();
            let log = log.clone();
            std::thread::spawn(move || {
                let res = stt::onload_model(index);
                on_ui(move || {
                    if let Some(ui) = weak.upgrade() {
                        ui.set_model_loading(false);
                        ui.set_loading_label("".into());
                        sync_model_ui(&ui);
                        if let Ok(msg) = res {
                            ui.set_status_text(msg.clone().into());
                            note(&weak, &log, &msg);
                        }
                    }
                });
            });
        }
        sync_model_ui(&ui);

        let model_ok = stt::model_installed(index);
        let overlay_ok = platform::can_draw_overlays();
        ui.set_mic_ok(mic_ok);
        ui.set_overlay_ok(overlay_ok);
        ui.set_status_text(if !mic_ok {
            "Step 1: Allow microphone access below".into()
        } else if !model_ok {
            "Step 2: Tap Onload / Download model".into()
        } else if initial_onload {
            format!("Onloading {} into RAM…", stt::model(index).name).into()
        } else {
            format!(
                "{} is ready — press the waveform pill to dictate",
                stt::model(index).name
            )
            .into()
        });
        note(
            &weak,
            &log,
            &format!(
                "QuickSTT ready · {} ({})",
                stt::model(index).name,
                if model_ok { "installed" } else { "pending" }
            ),
        );

        {
            let size = ui.window().size();
            let scale = ui.window().scale_factor();
            platform::log(&format!(
                "window: {:.0}x{:.0} logical, scale {scale:.2}",
                size.width, size.height
            ));
            let text = format!(
                "Window {:.0}x{:.0} · scale {scale:.2} — tap card to test touch",
                size.width, size.height
            );
            ui.set_touch_probe(text.into());
        }

        // Show the floating bubble automatically whenever overlay permission is granted
        let bubble_wish = platform::overlay_enabled();
        if overlay_ok {
            platform::save_overlay_enabled(true);
            ui.set_overlay_enabled(true);
            bubble("QuickSTT ready", "", &weak, &log);
            match platform::start_service() {
                Ok(()) => ui.set_background_ok(true),
                Err(e) => note(&weak, &log, &format!("Background service: {e}")),
            }
        } else {
            ui.set_overlay_enabled(bubble_wish && overlay_ok);
        }
        ui.set_battery_ok(platform::ignoring_battery_optimizations());
        platform::watch_overlay_permission();
    }

    ui
}

pub fn run() {
    build_ui().run().expect("Slint event loop failed");
}

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
    if health == conditioner::HEALTH_SATURATED {
        fail("Microphone input is clipped — lower the input gain".into());
        return;
    }
    if health == conditioner::HEALTH_DEAD {
        fail("No audio is arriving from the microphone".into());
        return;
    }
    if is_silent(&samples) {
        fail("No speech detected".into());
        return;
    }
    let wav = stt::wav_path("quickstt-turn.wav");
    let result = audio::write_wav(&wav, &samples, audio::SAMPLE_RATE as u32)
        .map_err(|e| format!("WAV write error ({}): {e}", wav.display()))
        .and_then(|_| stt::transcribe(&wav, model));
    if let Ok(mut slot) = pending.lock() {
        *slot = Some(match result {
            Ok(text) if !text.trim().is_empty() => PendingTurn {
                text,
                seconds,
                status: "Transcribed & copied to clipboard".into(),
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

    #[test]
    fn silence_gate_rejects_noise_only_audio() {
        assert!(is_silent(&[]));
        assert!(is_silent(&vec![0.2; 100]));
        let noise: Vec<f32> = (0..16_000)
            .map(|i| ((i as f32) * 0.01).sin() * 0.001)
            .collect();
        assert!(is_silent(&noise));
    }

    #[test]
    fn silence_gate_lets_speech_through() {
        let speech: Vec<f32> = (0..16_000)
            .map(|i| ((i as f32) * 0.05).sin() * 0.2)
            .collect();
        assert!(!is_silent(&speech));
    }
}
