//! The UI has to be wired end to end.
//!
//! A button that looks alive but does nothing is the worst kind of bug: the
//! app looks finished and is not. So the scene and the Rust code are checked
//! against each other here:
//!
//!   * every callback the scene calls has to be handled in the Rust code —
//!     a callback that is called but never handled does nothing at all,
//!   * a tap on a button has to arrive in the Rust code: the test builds the
//!     real window (the scene the phone shows), taps the status card and
//!     checks that the Rust handler ran.
//!
//! The test runs on the desktop with a software window, so it needs no phone
//! and no emulator.

use std::rc::Rc;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;

use slint::platform::software_renderer::{MinimalSoftwareWindow, RepaintBufferType};
use slint::platform::{Platform, PlatformError, PointerEventButton, WindowAdapter, WindowEvent};
use slint::{ComponentHandle, LogicalPosition, LogicalSize, PhysicalSize};

/// The status card is the first thing on the page, right under the header.
/// Tapping it has to reach the Rust code — the card carries the touch probe.
const CARD_X: f32 = 30.0;
const CARD_Y: f32 = 110.0;

/// The bottom bar: the third of its four entries is the "Models" page.
const NAV_X: f32 = 258.0;
const NAV_Y: f32 = 728.0;

struct TestPlatform {
    window: Rc<MinimalSoftwareWindow>,
}

impl Platform for TestPlatform {
    fn create_window_adapter(&self) -> Result<Rc<dyn WindowAdapter>, PlatformError> {
        Ok(self.window.clone())
    }
}

/// Tap a point of the window: press and release, like a finger.
fn tap(window: &slint::Window, x: f32, y: f32) {
    let position = LogicalPosition::new(x, y);
    window.dispatch_event(WindowEvent::PointerPressed {
        position,
        button: PointerEventButton::Left,
    });
    window.dispatch_event(WindowEvent::PointerReleased {
        position,
        button: PointerEventButton::Left,
    });
}

#[test]
fn the_ui_is_wired_end_to_end() {
    // ── 1. Every callback the window declares is handled in the Rust code ──
    let scene = std::fs::read_to_string(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/ui/android.slint"
    ))
    .expect("the scene must be readable");
    let code = std::fs::read_to_string(concat!(env!("CARGO_MANIFEST_DIR"), "/src/lib.rs"))
        .expect("the Rust code must be readable");

    // The callbacks of the window itself (the ones after the export).
    let window_scene = scene
        .split_once("export component AppWindow")
        .expect("the scene must export the window")
        .1;
    let mut declared: Vec<String> = Vec::new();
    let mut at = 0;
    while let Some(found) = window_scene[at..].find("callback ") {
        let start = at + found + "callback ".len();
        let rest = &window_scene[start..];
        let name: String = rest
            .chars()
            .take_while(|c| c.is_ascii_alphanumeric() || *c == '-')
            .collect();
        if !name.is_empty() && !declared.contains(&name) {
            declared.push(name.clone());
        }
        at = start;
    }
    assert!(!declared.is_empty(), "the window must declare callbacks");

    for name in &declared {
        let rust_name = name.replace('-', "_");
        assert!(
            code.contains(&format!("on_{rust_name}(")),
            "the window declares the callback {name}() but the Rust code never \
             handles it — the button would do nothing"
        );
    }

    // ── 2. A tap on the screen arrives in the Rust code ────────────────────
    let window = MinimalSoftwareWindow::new(RepaintBufferType::ReusedBuffer);
    window.set_size(PhysicalSize::new(412, 760));
    slint::platform::set_platform(Box::new(TestPlatform {
        window: window.clone(),
    }))
    .expect("the platform can only be set once");

    let ui = quickstt_android::build_ui();
    ui.window().set_size(LogicalSize::new(412.0, 760.0));

    // The probe starts with the size of the window: nothing has been tapped
    // yet.
    let before = ui.get_touch_probe();
    assert!(
        before.contains("Window"),
        "the touch probe must describe the window before the first tap"
    );

    tap(ui.window(), CARD_X, CARD_Y);

    assert_ne!(
        ui.get_touch_probe(),
        before,
        "the tap on the status card did not reach the Rust code — \
         the touch areas of the scene are not hit"
    );

    // ── 3. The bottom bar changes the page ─────────────────────────────────
    let counter = Arc::new(AtomicUsize::new(0));
    let seen = counter.clone();
    ui.on_nav(move |_tab| {
        seen.fetch_add(1, Ordering::SeqCst);
    });
    tap(ui.window(), NAV_X, NAV_Y);
    assert_eq!(
        counter.load(Ordering::SeqCst),
        1,
        "a tap on the bottom bar did not reach the Rust code"
    );
}
