#![windows_subsystem = "windows"]

mod widget_platform;

slint::include_modules!();

// Need to compile textboard as well
// Wait, include_modules compiles everything in the .slint file given to build.rs


use quickstt_core::orchestration::{AppOrchestrator, OrchestratorCommand};
use quickstt_core::error::QuickSttResult;
use std::sync::Arc;
use slint::{Timer, TimerMode};

fn main() -> QuickSttResult<()> {
    let (orchestrator, rx_cmd) = AppOrchestrator::new()?;
    let state = orchestrator.get_state();
    let tx_cmd = orchestrator.get_command_sender();

    // Start background tokio runtime for orchestrator
    std::thread::spawn({
        let state = state.clone();
        let audio_control_tx = orchestrator.audio_control_tx_clone();
        let audio_tx = orchestrator.audio_tx_clone();
        move || {
            let rt = tokio::runtime::Runtime::new().unwrap();
            rt.block_on(async {
                AppOrchestrator::run_command_loop(state, rx_cmd, audio_control_tx, audio_tx).await;
            });
        }
    });

    let main_window = PillWidget::new().unwrap();
    let textboard = TextBoardWidget::new().unwrap();
    let dashboard = DashboardWidget::new().unwrap();
    
    // Setup transparent, borderless, always-on-top window using Windows API
    widget_platform::configure_widget_window(main_window.window());
    widget_platform::configure_widget_window(textboard.window());
    
    dashboard.hide().unwrap();

    // Basic Logic hooks
    main_window.set_status_text("Ready".into());
    main_window.set_model_name("base.en".into());
    main_window.set_is_recording(false);
    main_window.set_is_listening(false);
    main_window.set_textboard_open(false);

    main_window.on_toggle_recording({
        let tx_cmd = tx_cmd.clone();
        let main_window = main_window.as_weak();
        move || {
            if let Some(main_window) = main_window.upgrade() {
                let rec = main_window.get_is_listening();
                if !rec {
                    let _ = tx_cmd.try_send(OrchestratorCommand::StartListening);
                } else {
                    let _ = tx_cmd.try_send(OrchestratorCommand::StopListening);
                }
            }
        }
    });

    let textboard_weak = textboard.as_weak();
    main_window.on_toggle_textboard({
        let main_window = main_window.as_weak();
        move || {
            if let (Some(main_window), Some(textboard)) = (main_window.upgrade(), textboard_weak.upgrade()) {
                let open = main_window.get_textboard_open();
                main_window.set_textboard_open(!open);
                if !open {
                    textboard.show().unwrap();
                } else {
                    textboard.hide().unwrap();
                }
            }
        }
    });

    textboard.on_toggle_attach({
        let textboard = textboard.as_weak();
        move || {
            if let Some(textboard) = textboard.upgrade() {
                let attached = textboard.get_is_attached();
                textboard.set_is_attached(!attached);
            }
        }
    });

    textboard.set_text_content("Initial text load...\n[Speech will appear here]".into());

    main_window.on_close_app({
        move || {
            std::process::exit(0);
        }
    });

    textboard.hide().unwrap(); // Start hidden

    // ── System Tray Initialization ──
    let icon_data = include_bytes!("../../assets/icon_app.ico");
    let img = image::load_from_memory_with_format(icon_data, image::ImageFormat::Ico).unwrap().to_rgba8();
    let (width, height) = img.dimensions();
    let tray_icon_img = tray_icon::Icon::from_rgba(img.into_raw(), width, height).unwrap();

    let dash_item = muda::MenuItem::new("Dashboard", true, None);
    let show_item = muda::MenuItem::new("Show", true, None);
    let hide_item = muda::MenuItem::new("Hide", true, None);
    let quit_item = muda::MenuItem::new("Quit", true, None);

    let menu = muda::Menu::new();
    menu.append_items(&[
        &dash_item,
        &show_item,
        &hide_item,
        &muda::PredefinedMenuItem::separator(),
        &quit_item,
    ]).unwrap();

    let menu_dash_id = dash_item.id().clone();
    let menu_show_id = show_item.id().clone();
    let menu_hide_id = hide_item.id().clone();
    let menu_quit_id = quit_item.id().clone();

    let _tray_icon = tray_icon::TrayIconBuilder::new()
        .with_tooltip("QuickSTT")
        .with_icon(tray_icon_img)
        .with_menu(Box::new(menu))
        .build()
        .expect("tray");

    let timer = Timer::default();
    let main_window_weak = main_window.as_weak();
    let textboard_weak = textboard.as_weak();
    let dashboard_weak = dashboard.as_weak();
    
    timer.start(
        TimerMode::Repeated,
        std::time::Duration::from_millis(50),
        move || {
            let (main_window, textboard, dashboard) = match (main_window_weak.upgrade(), textboard_weak.upgrade(), dashboard_weak.upgrade()) {
                (Some(m), Some(t), Some(d)) => (m, t, d),
                _ => return,
            };

            // Tray Events
            while let Ok(event) = tray_icon::TrayIconEvent::receiver().try_recv() {
                if let tray_icon::TrayIconEvent::Click { button: tray_icon::MouseButton::Left, button_state: tray_icon::MouseButtonState::Up, .. } = event {
                    let mut s = state.lock().unwrap();
                    s.widget_visible = !s.widget_visible;
                }
            }

            // Tray Menu Events
            while let Ok(event) = muda::MenuEvent::receiver().try_recv() {
                if event.id == menu_dash_id {
                    dashboard.show().unwrap();
                } else if event.id == menu_show_id {
                    state.lock().unwrap().widget_visible = true;
                } else if event.id == menu_hide_id {
                    state.lock().unwrap().widget_visible = false;
                } else if event.id == menu_quit_id {
                    std::process::exit(0);
                }
            }

            if let Ok(s) = state.lock() {
                main_window.set_status_text(s.status_message.clone().into());
                main_window.set_is_listening(
                    s.mode == quickstt_core::orchestration::AppMode::Recording ||
                    s.mode == quickstt_core::orchestration::AppMode::WakewordListening
                );
                main_window.set_is_recording(s.mode == quickstt_core::orchestration::AppMode::Recording);

                if let Some(entry) = s.model_entries.get(s.selected_model) {
                    main_window.set_model_name(entry.name.clone().into());
                }

                // Handle widget visibility from tray state
                if s.widget_visible {
                    if !main_window.window().is_visible() {
                        main_window.window().show().unwrap();
                    }
                } else {
                    if main_window.window().is_visible() {
                        main_window.window().hide().unwrap();
                    }
                }

                // Update textboard
                let tb_text = format!("{}\n{}", s.transcript_buffer, s.partial_result).trim().to_string();
                if tb_text != textboard.get_text_content().as_str() {
                    textboard.set_text_content(tb_text.into());
                }
            }
        }
    );

    main_window.run().unwrap();

    Ok(())
}
