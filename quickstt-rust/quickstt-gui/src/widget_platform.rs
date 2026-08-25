#![allow(non_snake_case)]

#[cfg(target_os = "windows")]
use windows::Win32::{
    Foundation::{BOOL, HWND, LPARAM, WPARAM},
    Graphics::Dwm::{DwmSetWindowAttribute, DWMWINDOWATTRIBUTE},
    UI::{
        Input::KeyboardAndMouse::ReleaseCapture,
        WindowsAndMessaging::{
            GetWindowLongW, PostMessageW, SetWindowLongW, SetWindowPos, ShowWindow, GWL_EXSTYLE,
            GWL_STYLE, HTCAPTION, HWND_TOPMOST, SWP_FRAMECHANGED, SWP_NOACTIVATE, SWP_NOMOVE,
            SWP_NOSIZE, SW_HIDE, SW_SHOWNA, WM_NCLBUTTONDOWN, WS_CAPTION, WS_EX_LAYERED,
            WS_EX_NOACTIVATE, WS_EX_TOOLWINDOW, WS_EX_TOPMOST, WS_MAXIMIZEBOX, WS_MINIMIZEBOX,
            WS_POPUP, WS_SYSMENU, WS_THICKFRAME,
        },
    },
};

#[cfg(target_os = "windows")]
const DWMWA_TRANSITIONS_FORCEDISABLED: DWMWINDOWATTRIBUTE = DWMWINDOWATTRIBUTE(3);

#[cfg(target_os = "windows")]
pub unsafe fn configure_widget_window(hwnd: HWND) {
    let current_style = GetWindowLongW(hwnd, GWL_STYLE) as u32;
    let widget_style = (current_style
        & !(WS_CAPTION.0 | WS_THICKFRAME.0 | WS_MINIMIZEBOX.0 | WS_MAXIMIZEBOX.0 | WS_SYSMENU.0))
        | WS_POPUP.0;
    SetWindowLongW(hwnd, GWL_STYLE, widget_style as i32);

    let current_ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE) as u32;
    let widget_ex_style = current_ex_style
        | WS_EX_TOOLWINDOW.0
        | WS_EX_NOACTIVATE.0
        | WS_EX_TOPMOST.0
        | WS_EX_LAYERED.0;
    SetWindowLongW(hwnd, GWL_EXSTYLE, widget_ex_style as i32);

    let disabled = BOOL(1);
    let _ = DwmSetWindowAttribute(
        hwnd,
        DWMWA_TRANSITIONS_FORCEDISABLED,
        &disabled as *const _ as *const core::ffi::c_void,
        core::mem::size_of::<BOOL>() as u32,
    );

    let _ = SetWindowPos(
        hwnd,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_NOACTIVATE,
    );
}

#[cfg(target_os = "windows")]
pub unsafe fn begin_widget_drag(hwnd: HWND) {
    let _ = ReleaseCapture();
    let _ = PostMessageW(
        hwnd,
        WM_NCLBUTTONDOWN,
        WPARAM(HTCAPTION as usize),
        LPARAM(0),
    );
}

#[cfg(target_os = "windows")]
pub unsafe fn show_widget(hwnd: HWND) {
    let _ = ShowWindow(hwnd, SW_SHOWNA);
    let _ = SetWindowPos(
        hwnd,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE,
    );
}

#[cfg(target_os = "windows")]
pub unsafe fn hide_widget(hwnd: HWND) {
    let _ = ShowWindow(hwnd, SW_HIDE);
}

#[cfg(target_os = "linux")]
pub unsafe fn configure_widget_window(_hwnd: ()) {
    // On Linux egui/eframe + winit handles:
    //  - transparent: ViewportBuilder::with_transparent(true)
    //  - always-on-top: with_always_on_top()
    //  - no-decorations: with_decorations(false)
    //  - tool-window: with_taskbar(false)
    // Wayland layer-shell (wlr) is preferred for true dock behavior;
    // X11 fallback uses _NET_WM_WINDOW_TYPE_DOCK via winit's x11 hints.
    // All configured at viewport creation time, nothing to do here.
}

#[cfg(target_os = "linux")]
pub unsafe fn begin_widget_drag(_hwnd: ()) {
    // winit dragging is done via ViewportCommand::StartDrag or egui drag Sense.
    // This stub exists for API parity; actual drag handled in QuickSttApp::render_pill.
}

#[cfg(target_os = "linux")]
pub unsafe fn show_widget(_hwnd: ()) {}

#[cfg(target_os = "linux")]
pub unsafe fn hide_widget(_hwnd: ()) {}

#[cfg(not(any(target_os = "windows", target_os = "linux")))]
pub unsafe fn configure_widget_window(_: ()) {}

#[cfg(not(any(target_os = "windows", target_os = "linux")))]
pub unsafe fn begin_widget_drag(_: ()) {}

#[cfg(not(any(target_os = "windows", target_os = "linux")))]
pub unsafe fn show_widget(_: ()) {}

#[cfg(not(any(target_os = "windows", target_os = "linux")))]
pub unsafe fn hide_widget(_: ()) {}

/// Linux helper: return X11 _NET_WM_WINDOW_TYPE hint for Wayland-safe callers
#[cfg(target_os = "linux")]
pub fn linux_window_hints() -> (&'static str, &'static str) {
    // (x11_type, wayland_layer) — consumed by eframe/winit if we set env
    ("_NET_WM_WINDOW_TYPE_DOCK", "top")
}
