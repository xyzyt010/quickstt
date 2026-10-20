//! QuickSTT for Android — application shell.
//!
//! The UI is a dedicated touch-first Slint scene (`ui/android.slint`), not the
//! desktop dashboard: phones get a bottom nav, a thumb-reachable record orb and
//! 48+ DIP targets.
//!
//! This crate is a preview shell: the audio capture / model runtime wiring is
//! the next step (see `android/README.md`). It builds two ways:
//!   * `cargo apk build` → Android APK (`android_main` below)
//!   * `cargo run --bin quickstt-android-preview` → desktop preview window

slint::include_modules!();

use slint::ComponentHandle;
use slint::Model;
use std::rc::Rc;

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
    let turns = Rc::new(slint::VecModel::from(vec![
        Turn {
            text: "Schedule the release review for Thursday morning.".into(),
            when: "2 minutes ago".into(),
            seconds: 5,
        },
        Turn {
            text: "Remind me to pick up the dry cleaning on the way home.".into(),
            when: "26 minutes ago".into(),
            seconds: 4,
        },
        Turn {
            text: "The new encoder cuts latency roughly in half.".into(),
            when: "1 hour ago".into(),
            seconds: 6,
        },
    ]));
    ui.set_turns(turns.clone().into());

    // Record toggle: flips the orb into its listening state, then commits a
    // placeholder turn on stop (the real transcript arrives with the runtime).
    {
        let weak = ui.as_weak();
        let turns = turns.clone();
        ui.on_toggle_record(move || {
            let Some(ui) = weak.upgrade() else { return };
            let rec = !ui.get_recording();
            ui.set_recording(rec);
            if rec {
                ui.set_live_text("Listening… speak naturally.".into());
            } else {
                ui.set_live_text("Tap the orb, then speak naturally.".into());
                turns.push(Turn {
                    text: "New dictation — runtime wiring lands next build.".into(),
                    when: "just now".into(),
                    seconds: 3,
                });
            }
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

    // Breathing ring while listening (~20 fps, cheap: one property write).
    {
        let weak = ui.as_weak();
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
            },
        );
        // Leak the timer into the UI lifetime (the event loop owns it).
        std::mem::forget(timer);
    }

    ui.run().expect("Slint event loop failed");
}

#[cfg(target_os = "android")]
#[unsafe(no_mangle)]
fn android_main(app: slint::android::AndroidApp) {
    slint::android::init(app).expect("Slint Android backend init failed");
    run();
}
