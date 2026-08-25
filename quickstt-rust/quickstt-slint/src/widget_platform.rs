#[cfg(target_os = "windows")]
use windows::Win32::Foundation::HWND;
#[cfg(target_os = "windows")]
use windows::Win32::UI::WindowsAndMessaging::{
    GetWindowLongW, SetWindowLongW, SetWindowPos, GWL_EXSTYLE, GWL_STYLE, HWND_TOPMOST,
    SWP_NOMOVE, SWP_NOSIZE, SWP_FRAMECHANGED,
    WS_CAPTION, WS_THICKFRAME, WS_MINIMIZEBOX, WS_MAXIMIZEBOX, WS_SYSMENU,
    WS_EX_LAYERED, WS_EX_TOOLWINDOW, WS_POPUP,
};
use raw_window_handle::{HasWindowHandle, RawWindowHandle};

pub fn configure_widget_window(window: &slint::Window) {
    #[cfg(target_os = "windows")]
    if let Ok(wh) = window.window_handle().window_handle() {
        if let RawWindowHandle::Win32(win32) = wh.as_raw() {
            let hwnd = HWND(win32.hwnd.get() as _);
            unsafe {
                let mut style = GetWindowLongW(hwnd, GWL_STYLE);
                style &= !(WS_CAPTION.0 | WS_THICKFRAME.0 | WS_MINIMIZEBOX.0 | WS_MAXIMIZEBOX.0 | WS_SYSMENU.0) as i32;
                style |= WS_POPUP.0 as i32;
                SetWindowLongW(hwnd, GWL_STYLE, style);

                let mut ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE);
                ex_style |= (WS_EX_LAYERED.0 | WS_EX_TOOLWINDOW.0) as i32;
                SetWindowLongW(hwnd, GWL_EXSTYLE, ex_style);

                SetWindowPos(
                    hwnd,
                    HWND_TOPMOST,
                    0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED,
                ).ok();
            }
        }
    }
}
