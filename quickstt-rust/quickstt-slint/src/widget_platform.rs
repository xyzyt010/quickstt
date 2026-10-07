#[cfg(target_os = "windows")]
use windows::Win32::Foundation::{HWND, LPARAM, LRESULT, WPARAM};
#[cfg(target_os = "windows")]
use windows::Win32::UI::WindowsAndMessaging::{
    GetCursorPos, GetWindowLongW, IsWindowVisible, SetWindowLongW, SetWindowPos, ShowWindow,
    GWL_EXSTYLE, GWL_STYLE, HWND_TOPMOST, SWP_FRAMECHANGED, SWP_NOACTIVATE, SWP_NOMOVE,
    SWP_NOSIZE, SW_SHOWNOACTIVATE, WS_CAPTION, WS_EX_APPWINDOW, WS_EX_LAYERED,
    WS_EX_NOACTIVATE, WS_EX_TOOLWINDOW, WS_EX_TRANSPARENT, WS_MAXIMIZEBOX, WS_MINIMIZEBOX,
    WS_POPUP, WS_SYSMENU, WS_THICKFRAME,
};
#[cfg(target_os = "windows")]
use windows::Win32::Graphics::Dwm::{DwmSetWindowAttribute, DWMWA_TRANSITIONS_FORCEDISABLED};
#[cfg(target_os = "windows")]
use windows::Win32::UI::HiDpi::GetDpiForWindow;
#[cfg(target_os = "windows")]
use windows::Win32::UI::Shell::{DefSubclassProc, RemoveWindowSubclass, SetWindowSubclass};
use raw_window_handle::{HasWindowHandle, RawWindowHandle};

#[cfg(target_os = "windows")]
unsafe extern "system" fn frameless_subclass_proc(
    hwnd: HWND,
    msg: u32,
    wparam: WPARAM,
    lparam: LPARAM,
    _uidsubclass: usize,
    _refdata: usize,
) -> LRESULT {
    const WM_NCCALCSIZE: u32 = 0x0083;
    const WM_NCPAINT: u32 = 0x0085;
    const WM_NCACTIVATE: u32 = 0x0086;
    const WM_STYLECHANGING: u32 = 0x007C;

    match msg {
        WM_NCCALCSIZE => {
            // When wParam is TRUE, returning 0 tells Windows the client area
            // fills the entire window rectangle, with zero non-client area.
            LRESULT(0)
        }
        WM_NCPAINT => {
            // Suppress all non-client frame / caption painting entirely.
            LRESULT(0)
        }
        WM_NCACTIVATE => {
            // Suppress activation/deactivation title bar repaint.
            LRESULT(1)
        }
        WM_STYLECHANGING => {
            if lparam.0 != 0 {
                use windows::Win32::UI::WindowsAndMessaging::STYLESTRUCT;
                let ss = &mut *(lparam.0 as *mut STYLESTRUCT);
                const FRAME_BITS: i32 = (WS_CAPTION.0
                    | WS_THICKFRAME.0
                    | WS_MINIMIZEBOX.0
                    | WS_MAXIMIZEBOX.0
                    | WS_SYSMENU.0) as i32;
                ss.styleNew = ((ss.styleNew as i32 & !FRAME_BITS) | WS_POPUP.0 as i32) as u32;
            }
            LRESULT(0)
        }
        _ => DefSubclassProc(hwnd, msg, wparam, lparam),
    }
}

pub fn hwnd_of(window: &slint::Window) -> Option<isize> {
    let binding = window.window_handle();
    let wh = binding.window_handle().ok()?;
    if let RawWindowHandle::Win32(w) = wh.as_raw() {
        Some(w.hwnd.get() as isize)
    } else {
        None
    }
}

/// Force WS_EX_LAYERED on a HIDDEN window (the only time it can be set).
/// Call immediately after new() + hide() — the window is hidden at that
/// point so the flag sticks. Returns true on success.
#[cfg(target_os = "windows")]
pub fn force_layered(window: &slint::Window) -> bool {
    let Some(raw) = hwnd_of(window) else {
        return false;
    };
    let hwnd = HWND(raw as _);
    unsafe {
        let ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
        if ex & WS_EX_LAYERED.0 as i32 != 0 {
            return true; // already set
        }
        SetWindowLongW(
            hwnd,
            GWL_EXSTYLE,
            ex | WS_EX_LAYERED.0 as i32 | WS_EX_TOOLWINDOW.0 as i32 | WS_EX_NOACTIVATE.0 as i32,
        );
        // Verify it stuck.
        let ex2 = GetWindowLongW(hwnd, GWL_EXSTYLE);
        ex2 & WS_EX_LAYERED.0 as i32 != 0
    }
}
#[cfg(not(target_os = "windows"))]
pub fn force_layered(_window: &slint::Window) -> bool {
    false
}

/// True when the native HWND exists (false before first show). A VISIBLE
/// window without an HWND is a real anomaly (frame-strip and styling silently
/// no-op) — callers log it rate-limited.
pub fn has_hwnd(window: &slint::Window) -> bool {
    hwnd_of(window).is_some()
}

/// Borderless always-on-top tool window that never steals typing focus.
/// Must be called right after window creation, before show().
pub fn configure_widget_window(window: &slint::Window) {
    #[cfg(target_os = "windows")]
    if let Some(raw) = hwnd_of(window) {
        let hwnd = HWND(raw as _);
        unsafe {
            // Empty window title so Windows non-client painter has no text to display
            let _ = windows::Win32::UI::WindowsAndMessaging::SetWindowTextW(hwnd, windows::core::w!(""));

            // Subclass to intercept WM_NCCALCSIZE, WM_NCPAINT, WM_NCACTIVATE, WM_STYLECHANGING
            let _ = SetWindowSubclass(hwnd, Some(frameless_subclass_proc), 0x51554943, 0);

            let mut style = GetWindowLongW(hwnd, GWL_STYLE);
            style &= !(WS_CAPTION.0 | WS_THICKFRAME.0 | WS_MINIMIZEBOX.0 | WS_MAXIMIZEBOX.0
                | WS_SYSMENU.0) as i32;
            style |= WS_POPUP.0 as i32;
            SetWindowLongW(hwnd, GWL_STYLE, style);

            let mut ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
            // TOOLWINDOW keeps it off the taskbar; the backend sets
            // APPWINDOW by default, which would force a taskbar button —
            // a proper widget must never have one.
            ex &= !(WS_EX_APPWINDOW.0 as i32);
            ex |= (WS_EX_LAYERED.0 | WS_EX_TOOLWINDOW.0 | WS_EX_NOACTIVATE.0) as i32;
            SetWindowLongW(hwnd, GWL_EXSTYLE, ex);

            // Disable DWM fade/slide so show/hide feels instant
            let disable: i32 = 1;
            let _ = DwmSetWindowAttribute(
                hwnd,
                DWMWA_TRANSITIONS_FORCEDISABLED,
                &disable as *const _ as _,
                std::mem::size_of::<i32>() as u32,
            );

            SetWindowPos(
                hwnd,
                HWND_TOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED,
            )
            .ok();
        }
    }
}

/// Proper native Windows application window (WS_OVERLAPPEDWINDOW with caption,
/// sysmenu, minimize, maximize, thickframe, taskbar button WS_EX_APPWINDOW,
/// and immersive dark mode title bar via DWM).
pub fn configure_app_window(window: &slint::Window) {
    #[cfg(target_os = "windows")]
    if let Some(raw) = hwnd_of(window) {
        let hwnd = HWND(raw as _);
        unsafe {
            use windows::Win32::UI::WindowsAndMessaging::{
                HWND_NOTOPMOST, SetForegroundWindow, SetWindowTextW,
            };

            let _ = RemoveWindowSubclass(hwnd, Some(frameless_subclass_proc), 0x51554943);
            let _ = SetWindowTextW(hwnd, windows::core::w!("QuickSTT Control Center"));

            let mut style = GetWindowLongW(hwnd, GWL_STYLE);
            style &= !(WS_POPUP.0 as i32);
            style |= (WS_CAPTION.0 | WS_THICKFRAME.0 | WS_MINIMIZEBOX.0 | WS_MAXIMIZEBOX.0 | WS_SYSMENU.0) as i32;
            SetWindowLongW(hwnd, GWL_STYLE, style);

            let mut ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
            ex &= !(WS_EX_TOOLWINDOW.0 | WS_EX_NOACTIVATE.0 | WS_EX_LAYERED.0) as i32;
            ex |= WS_EX_APPWINDOW.0 as i32;
            SetWindowLongW(hwnd, GWL_EXSTYLE, ex);

            // Immersive dark mode for native Windows title bar
            let dark: i32 = 1;
            let _ = DwmSetWindowAttribute(
                hwnd,
                windows::Win32::Graphics::Dwm::DWMWINDOWATTRIBUTE(20),
                &dark as *const _ as _,
                std::mem::size_of::<i32>() as u32,
            );
            let _ = DwmSetWindowAttribute(
                hwnd,
                windows::Win32::Graphics::Dwm::DWMWINDOWATTRIBUTE(19),
                &dark as *const _ as _,
                std::mem::size_of::<i32>() as u32,
            );

            let _ = SetWindowPos(
                hwnd,
                HWND_NOTOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED,
            );
            SetForegroundWindow(hwnd);
        }
    }
}

pub fn show_app_window(window: &slint::Window) {
    if let Err(e) = window.show() {
        crate::log_line(&format!("show_app_window FAILED: {e:?}"));
    }
    configure_app_window(window);
    #[cfg(target_os = "windows")]
    if let Some(raw) = hwnd_of(window) {
        let hwnd = HWND(raw as _);
        unsafe {
            use windows::Win32::UI::WindowsAndMessaging::{
                SetForegroundWindow, ShowWindow, SW_SHOWNORMAL,
            };
            let _ = ShowWindow(hwnd, SW_SHOWNORMAL);
            SetForegroundWindow(hwnd);
        }
    }
    window.request_redraw();
}

/// Show a widget window, then strip OS chrome. Styling MUST run after
/// show(): before that the HWND does not exist yet and the call is a silent
/// no-op — which is how the pill ended up with an app-like title bar and
/// min/max/close buttons. Safe to call on every show (idempotent).
/// show() errors are logged (a silent failure here = invisible window that
/// Slint still reports as visible).
pub fn show_widget(window: &slint::Window) {
    configure_widget_window(window);
    if let Err(e) = window.show() {
        crate::log_line(&format!("show_widget FAILED: {e:?}"));
    }
    configure_widget_window(window);
    #[cfg(target_os = "windows")]
    if let Some(raw) = hwnd_of(window) {
        let hwnd = HWND(raw as _);
        unsafe {
            let _ = ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            let ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
            if ex & WS_EX_LAYERED.0 as i32 == 0 {
                crate::log_line(&format!(
                    "WARN: WS_EX_LAYERED NOT set after configure_widget_window (ex=0x{:08X})",
                    ex
                ));
            }
        }
    }
    // Staged style: ensure window styles and subclass are firmly locked in.
    for _ in 0..20 {
        configure_widget_window(window);
        if has_hwnd(window) && !ensure_frameless(window, false) {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(1));
    }
    // Force the first frame now: a shown-but-unpainted window can linger as
    // a white flash on a loaded machine before the backend gets to it.
    window.request_redraw();
}

/// Show the docking overlay (same show-then-style ordering requirement).
pub fn show_overlay(window: &slint::Window) {
    configure_overlay_window(window);
    if let Err(e) = window.show() {
        crate::log_line(&format!("show_overlay FAILED: {e:?}"));
    }
    configure_overlay_window(window);
    // Same staged style as widgets (see show_widget): no white-framed first
    // paint, ever.
    for _ in 0..20 {
        configure_overlay_window(window);
        if has_hwnd(window) && !ensure_frameless(window, true) {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(1));
    }
    window.request_redraw();
}
/// Click-through fullscreen docking overlay: never activates, never takes
/// focus, never intercepts mouse — it is purely visual (dark shade + outlines).
pub fn configure_overlay_window(window: &slint::Window) {
    configure_widget_window(window);
    #[cfg(target_os = "windows")]
    if let Some(raw) = hwnd_of(window) {
        let hwnd = HWND(raw as _);
        unsafe {
            let mut ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
            ex |= WS_EX_TRANSPARENT.0 as i32;
            SetWindowLongW(hwnd, GWL_EXSTYLE, ex);
            SetWindowPos(
                hwnd,
                HWND_TOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED,
            )
            .ok();
        }
    }
}

/// Keep the pill above the docking overlay while dragging. Pure Z-order
/// restack — deliberately NO SWP_FRAMECHANGED: a frame recalc forces Windows
/// to repaint the non-client frame, which flashes the OS caption ("QuickSTT"
/// tile) on press. Called on every press, so it must be paint-neutral.
pub fn restack_topmost(window: &slint::Window) {
    #[cfg(target_os = "windows")]
    if let Some(raw) = hwnd_of(window) {
        unsafe {
            SetWindowPos(
                HWND(raw as _),
                HWND_TOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE,
            )
            .ok();
        }
    }
    #[cfg(not(target_os = "windows"))]
    let _ = window;
}

/// Re-assert frameless widget chrome on an already-visible window.
/// The Slint/winit backend can (re)apply OS decorations asynchronously after
/// show, winning the race against one-shot styling — the pill kept its
/// CAPTION|SYSMENU bits because of exactly this. Called from the 50ms poll
/// for every visible widget window; only touches the OS when the frame bits
/// are actually present. Returns true when it changed something.
#[cfg(target_os = "windows")]
pub fn ensure_frameless(window: &slint::Window, click_through: bool) -> bool {
    let Some(raw) = hwnd_of(window) else {
        return false;
    };
    let hwnd = HWND(raw as _);
    unsafe {
        let _ = windows::Win32::UI::WindowsAndMessaging::SetWindowTextW(hwnd, windows::core::w!(""));
        let _ = SetWindowSubclass(hwnd, Some(frameless_subclass_proc), 0x51554943, 0);
        let mut changed = false;
        let style = GetWindowLongW(hwnd, GWL_STYLE);
        const FRAME_BITS: i32 =
            (WS_CAPTION.0 | WS_THICKFRAME.0 | WS_MINIMIZEBOX.0 | WS_MAXIMIZEBOX.0 | WS_SYSMENU.0)
                as i32;
        if style & FRAME_BITS != 0 {
            SetWindowLongW(hwnd, GWL_STYLE, (style & !FRAME_BITS) | WS_POPUP.0 as i32);
            changed = true;
        }
        let ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
        let mut want =
            (WS_EX_LAYERED.0 | WS_EX_TOOLWINDOW.0 | WS_EX_NOACTIVATE.0) as i32;
        if click_through {
            want |= WS_EX_TRANSPARENT.0 as i32;
        }
        // APPWINDOW forces a taskbar button even on tool windows — strip it.
        if ex & want != want || ex & (WS_EX_APPWINDOW.0 as i32) != 0 {
            SetWindowLongW(
                hwnd,
                GWL_EXSTYLE,
                (ex | want) & !(WS_EX_APPWINDOW.0 as i32),
            );
            changed = true;
        }
        if changed {
            SetWindowPos(
                hwnd,
                HWND_TOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED,
            )
            .ok();
        }
        changed
    }
}

#[cfg(not(target_os = "windows"))]
pub fn ensure_frameless(_window: &slint::Window, _click_through: bool) -> bool {
    false
}

/// One-line snapshot of a Slint window's REAL OS state: Slint's belief vs
/// Win32 truth (IsWindowVisible), native rect, layered bit, raw style bits.
/// Use when a phantom window (e.g. the white tile) is on screen: it tells
/// us exactly which of our windows Win32 considers visible and where.
#[cfg(target_os = "windows")]
pub fn window_state_debug(name: &str, window: &slint::Window) -> String {
    let slint_vis = window.is_visible();
    let Some(raw) = hwnd_of(window) else {
        return format!("{name}: slint={slint_vis} hwnd=NONE");
    };
    let hwnd = HWND(raw as _);
    unsafe {
        let win_vis = IsWindowVisible(hwnd).as_bool();
        let style = GetWindowLongW(hwnd, GWL_STYLE);
        let ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
        let layered = ex & WS_EX_LAYERED.0 as i32 != 0;
        let r = native_rect(window)
            .map(|(l, t, ri, b)| format!("{l},{t}-{ri},{b} {}x{}", ri - l, b - t))
            .unwrap_or_else(|| "rect=?".to_string());
        format!("{name}: slint={slint_vis} win32={win_vis} layered={layered} rect={r} style=0x{style:08X} ex=0x{ex:08X}")
    }
}

#[cfg(not(target_os = "windows"))]
pub fn window_state_debug(name: &str, window: &slint::Window) -> String {
    format!("{name}: slint={}", window.is_visible())
}

/// Native window rect via GetWindowRect (physical px) for drag-timeline
/// diagnostics: proves where Slint windows really are, independent of what
/// Slint reports.
pub fn native_rect(window: &slint::Window) -> Option<(i32, i32, i32, i32)> {
    #[cfg(target_os = "windows")]
    {
        use windows::Win32::Foundation::RECT;
        use windows::Win32::UI::WindowsAndMessaging::GetWindowRect;
        let raw = hwnd_of(window)?;
        unsafe {
            let mut rc = RECT::default();
            if GetWindowRect(HWND(raw as _), &mut rc).is_ok() {
                return Some((rc.left, rc.top, rc.right, rc.bottom));
            }
        }
        None
    }
    #[cfg(not(target_os = "windows"))]
    {
        let _ = window;
        None
    }
}

/// Press-time tile trap: cursor + foreground window + FULL visible-window
/// census (class/title/owner-pid/rect/style — catches tooltips, foreign
/// caption bars and drag images, not just our windows) + one fullscreen
/// screenshot. Pure Win32, so it runs on a worker thread mid-press while the
/// tile is on screen. Fixed filenames per tag (caller rotates a small ring).
pub fn press_forensics(tag: &str) {
    press_census(tag);
    #[cfg(target_os = "windows")]
    {
        let path = std::env::temp_dir().join(format!("quickstt-press-{tag}.bmp"));
        let ok = save_screen_bmp(&path);
        crate::log_line(&format!("presscap {tag} shot={ok} {}", path.display()));
    }
}

/// Census half of [`press_forensics`] (no screenshot): cheap enough to run
/// several times through one hold, so a sub-350ms popup still lands in the log.
pub fn press_census(tag: &str) {
    #[cfg(target_os = "windows")]
    {
        use windows::Win32::Foundation::{BOOL, HWND, LPARAM, POINT, RECT};
        use windows::Win32::UI::WindowsAndMessaging::{
            EnumWindows, GetClassNameW, GetCursorPos, GetForegroundWindow, GetWindowLongW,
            GetWindowRect, GetWindowTextW, GetWindowThreadProcessId, IsWindowVisible,
            GWL_EXSTYLE, GWL_STYLE,
        };
        unsafe {
            let mut pt = POINT { x: 0, y: 0 };
            let _ = GetCursorPos(&mut pt);
            let fg = GetForegroundWindow();
            let mut ft = [0u16; 64];
            let fl = GetWindowTextW(fg, &mut ft) as usize;
            let mut fpid: u32 = 0;
            GetWindowThreadProcessId(fg, Some(&mut fpid as *mut u32));
            crate::log_line(&format!(
                "presscap {tag} cursor={},{} fg={} fpid={} '{}'",
                pt.x,
                pt.y,
                fg.0 as isize,
                fpid,
                String::from_utf16_lossy(&ft[..fl.min(64)])
            ));
            struct W {
                n: i32,
            }
            unsafe extern "system" fn visit(h: HWND, lp: LPARAM) -> BOOL {
                let t = &mut *(lp.0 as *mut W);
                if t.n >= 90 {
                    return true.into();
                }
                if !IsWindowVisible(h).as_bool() {
                    return true.into();
                }
                let mut rc = RECT::default();
                if GetWindowRect(h, &mut rc).is_err() {
                    return true.into();
                }
                if rc.right - rc.left <= 1 || rc.bottom - rc.top <= 1 {
                    return true.into();
                }
                let mut buf = [0u16; 64];
                let len = GetWindowTextW(h, &mut buf) as usize;
                let title = String::from_utf16_lossy(&buf[..len.min(64)]);
                let mut cls = [0u16; 64];
                let cl = GetClassNameW(h, &mut cls) as usize;
                let class = String::from_utf16_lossy(&cls[..cl.min(64)]);
                let mut pid: u32 = 0;
                GetWindowThreadProcessId(h, Some(&mut pid as *mut u32));
                let st = GetWindowLongW(h, GWL_STYLE);
                let ex = GetWindowLongW(h, GWL_EXSTYLE);
                crate::log_line(&format!(
                    "presswin pid={pid} cls='{class}' rect={},{},{},{} st={st:#x} ex={ex:#x} '{title}'",
                    rc.left, rc.top, rc.right, rc.bottom
                ));
                t.n += 1;
                true.into()
            }
            let mut w = W { n: 0 };
            let _ = EnumWindows(Some(visit), LPARAM(&mut w as *mut W as isize));
        }
    }
    #[cfg(not(target_os = "windows"))]
    {
        let _ = tag;
    }
}

/// Window census for phantom-tile hunts: every visible non-empty top-level
/// window with owner PID, rect and style bits. One write_all per line (see
/// log_line) so IPC + main threads can't garble each other.
pub fn log_winlist() {    #[cfg(target_os = "windows")]
    {
        use windows::Win32::Foundation::{BOOL, HWND, LPARAM, RECT};
        use windows::Win32::UI::WindowsAndMessaging::{
            EnumWindows, GetWindowLongW, GetWindowRect, GetWindowTextW,
            GetWindowThreadProcessId, IsWindowVisible, GWL_EXSTYLE, GWL_STYLE,
        };
        struct W {
            n: i32,
        }
        unsafe extern "system" fn visit(h: HWND, lp: LPARAM) -> BOOL {
            let t = &mut *(lp.0 as *mut W);
            if t.n >= 60 {
                return true.into();
            }
            if !IsWindowVisible(h).as_bool() {
                return true.into();
            }
            let mut rc = RECT::default();
            if GetWindowRect(h, &mut rc).is_err() {
                return true.into();
            }
            if rc.right - rc.left <= 0 || rc.bottom - rc.top <= 0 {
                return true.into();
            }
            let mut buf = [0u16; 48];
            let len = GetWindowTextW(h, &mut buf) as usize;
            let title = String::from_utf16_lossy(&buf[..len.min(48)]);
            let mut pid: u32 = 0;
            GetWindowThreadProcessId(h, Some(&mut pid as *mut u32));
            let st = GetWindowLongW(h, GWL_STYLE);
            let ex = GetWindowLongW(h, GWL_EXSTYLE);
            crate::log_line(&format!(
                "winlist pid={pid} rect={},{},{},{} st={st:#x} ex={ex:#x} '{title}'",
                rc.left, rc.top, rc.right, rc.bottom
            ));
            t.n += 1;
            true.into()
        }
        let mut w = W { n: 0 };
        unsafe {
            let _ = EnumWindows(Some(visit), LPARAM(&mut w as *mut W as isize));
        }
        crate::log_line(&format!("winlist done n={}", w.n));
    }
    #[cfg(not(target_os = "windows"))]
    crate::log_line("winlist: windows-only");
}

/// Full-screen BMP screenshot (DWM-composed: includes our layered/topmost
/// windows, exactly what the eye sees). Worker-thread safe (pure Win32, no
/// Slint handles) — the field recorder calls this mid-drag.
pub fn save_screen_bmp(path: &std::path::Path) -> bool {
    #[cfg(target_os = "windows")]
    {
        use windows::Win32::Foundation::HWND;
        use windows::Win32::Graphics::Gdi::*;
        use windows::Win32::UI::WindowsAndMessaging::{
            GetSystemMetrics, SM_CXSCREEN, SM_CYSCREEN,
        };
        unsafe {
            let w = GetSystemMetrics(SM_CXSCREEN);
            let h = GetSystemMetrics(SM_CYSCREEN);
            if w <= 0 || h <= 0 {
                return false;
            }
            let hdc = GetDC(HWND(0));
            if hdc.0 == 0 {
                return false;
            }
            let mem = CreateCompatibleDC(hdc);
            if mem.0 == 0 {
                ReleaseDC(HWND(0), hdc);
                return false;
            }
            let bmp = CreateCompatibleBitmap(hdc, w, h);
            if bmp.0 == 0 {
                DeleteDC(mem);
                ReleaseDC(HWND(0), hdc);
                return false;
            }
            let old: HGDIOBJ = SelectObject(mem, bmp);
            let blt_ok = BitBlt(mem, 0, 0, w, h, hdc, 0, 0, SRCCOPY).is_ok();
            SelectObject(mem, old);
            let mut bmi = BITMAPINFO {
                bmiHeader: BITMAPINFOHEADER {
                    biSize: std::mem::size_of::<BITMAPINFOHEADER>() as u32,
                    biWidth: w,
                    biHeight: h, // bottom-up DIB (GDI default): rows land
                    // in the file exactly as BMP expects them.
                    biPlanes: 1,
                    biBitCount: 32,
                    biCompression: 0, // BI_RGB
                    biSizeImage: 0,
                    biXPelsPerMeter: 0,
                    biYPelsPerMeter: 0,
                    biClrUsed: 0,
                    biClrImportant: 0,
                },
                bmiColors: [RGBQUAD::default()],
            };
            let mut px = vec![0u8; (w as usize) * 4 * (h as usize)];
            let lines = GetDIBits(
                mem,
                bmp,
                0,
                h as u32,
                Some(px.as_mut_ptr() as *mut std::ffi::c_void),
                &mut bmi,
                DIB_RGB_COLORS,
            );
            DeleteObject(bmp);
            DeleteDC(mem);
            ReleaseDC(HWND(0), hdc);
            if !blt_ok || lines == 0 {
                return false;
            }
            let mut hdr = [0u8; 54];
            hdr[0] = b'B';
            hdr[1] = b'M';
            hdr[2..6].copy_from_slice(&((54 + px.len()) as u32).to_le_bytes());
            hdr[10] = 54;
            hdr[14] = 40;
            hdr[18..22].copy_from_slice(&(w as u32).to_le_bytes());
            hdr[22..26].copy_from_slice(&(h as u32).to_le_bytes());
            hdr[26] = 1;
            hdr[28] = 32;
            match std::fs::File::create(path) {
                Ok(mut f) => {
                    use std::io::Write;
                    f.write_all(&hdr).is_ok() && f.write_all(&px).is_ok()
                }
                Err(_) => false,
            }
        }
    }
    #[cfg(not(target_os = "windows"))]
    {
        let _ = path;
        false
    }
}

/// Small region BMP around a point (clamped to the screen): ~1MB, fast
/// enough for a dense burst through one press-hold to catch sub-150ms
/// flashes that full-screen captures straddle. Same worker-thread rules.
pub fn save_region_bmp(path: &std::path::Path, cx: i32, cy: i32, w: i32, h: i32) -> bool {
    #[cfg(target_os = "windows")]
    {
        use windows::Win32::Foundation::HWND;
        use windows::Win32::Graphics::Gdi::*;
        use windows::Win32::UI::WindowsAndMessaging::{
            GetSystemMetrics, SM_CXSCREEN, SM_CYSCREEN,
        };
        unsafe {
            let sw = GetSystemMetrics(SM_CXSCREEN);
            let sh = GetSystemMetrics(SM_CYSCREEN);
            if sw <= 0 || sh <= 0 {
                return false;
            }
            let x = cx.saturating_sub(w / 2).clamp(0, (sw - 1).max(0));
            let y = cy.saturating_sub(h / 2).clamp(0, (sh - 1).max(0));
            let w = w.min(sw - x).max(1);
            let h = h.min(sh - y).max(1);
            let hdc = GetDC(HWND(0));
            if hdc.0 == 0 {
                return false;
            }
            let mem = CreateCompatibleDC(hdc);
            if mem.0 == 0 {
                ReleaseDC(HWND(0), hdc);
                return false;
            }
            let bmp = CreateCompatibleBitmap(hdc, w, h);
            if bmp.0 == 0 {
                DeleteDC(mem);
                ReleaseDC(HWND(0), hdc);
                return false;
            }
            let old: HGDIOBJ = SelectObject(mem, bmp);
            let blt_ok = BitBlt(mem, 0, 0, w, h, hdc, x, y, SRCCOPY).is_ok();
            SelectObject(mem, old);
            let mut bmi = BITMAPINFO {
                bmiHeader: BITMAPINFOHEADER {
                    biSize: std::mem::size_of::<BITMAPINFOHEADER>() as u32,
                    biWidth: w,
                    biHeight: h,
                    biPlanes: 1,
                    biBitCount: 32,
                    biCompression: 0,
                    biSizeImage: 0,
                    biXPelsPerMeter: 0,
                    biYPelsPerMeter: 0,
                    biClrUsed: 0,
                    biClrImportant: 0,
                },
                bmiColors: [RGBQUAD::default()],
            };
            let mut px = vec![0u8; (w as usize) * 4 * (h as usize)];
            let lines = GetDIBits(
                mem,
                bmp,
                0,
                h as u32,
                Some(px.as_mut_ptr() as *mut std::ffi::c_void),
                &mut bmi,
                DIB_RGB_COLORS,
            );
            DeleteObject(bmp);
            DeleteDC(mem);
            ReleaseDC(HWND(0), hdc);
            if !blt_ok || lines == 0 {
                return false;
            }
            let mut hdr = [0u8; 54];
            hdr[0] = b'B';
            hdr[1] = b'M';
            hdr[2..6].copy_from_slice(&((54 + px.len()) as u32).to_le_bytes());
            hdr[10] = 54;
            hdr[14] = 40;
            hdr[18..22].copy_from_slice(&(w as u32).to_le_bytes());
            hdr[22..26].copy_from_slice(&(h as u32).to_le_bytes());
            hdr[26] = 1;
            hdr[28] = 32;
            match std::fs::File::create(path) {
                Ok(mut f) => {
                    use std::io::Write;
                    f.write_all(&hdr).is_ok() && f.write_all(&px).is_ok()
                }
                Err(_) => false,
            }
        }
    }
    #[cfg(not(target_os = "windows"))]
    {
        let _ = (path, cx, cy, w, h);
        false
    }
}

/// Global cursor position in physical pixels.
pub fn cursor_position() -> (i32, i32) {
    #[cfg(target_os = "windows")]
    unsafe {
        let mut pt = windows::Win32::Foundation::POINT { x: 0, y: 0 };
        if GetCursorPos(&mut pt).is_ok() {
            return (pt.x, pt.y);
        }
        (0, 0)
    }
    // Linux/X11 (and XWayland on Wayland sessions): the hover-expand math
    // polls this every 50ms — a stuck (0,0) means the pill can never
    // expand. Pure-Rust XQueryPointer, graceful (0,0) when no X is up.
    #[cfg(not(target_os = "windows"))]
    {
        x11q::cursor_pos().unwrap_or((0, 0))
    }
}

/// Logical (DIP) scale of the monitor hosting this window.
pub fn window_scale(window: &slint::Window) -> f32 {
    #[cfg(target_os = "windows")]
    if let Some(raw) = hwnd_of(window) {
        return dpi_scale(raw);
    }
    #[cfg(not(target_os = "windows"))]
    {
        // Slint 1.9 exposes no per-screen scale; the X server's EDID-based
        // DPI (via XRandR) matches the desktop's scale on XFCE4/Cinnamon and
        // under XWayland. Cached process-wide (monitors rarely change).
        let _ = window;
        x11q::ui_scale()
    }
    #[cfg(target_os = "windows")]
    1.0
}
/// Screen size in physical pixels (primary monitor).
pub fn screen_size() -> (i32, i32) {
    #[cfg(target_os = "windows")]
    unsafe {
        use windows::Win32::UI::WindowsAndMessaging::{GetSystemMetrics, SM_CXSCREEN, SM_CYSCREEN};
        (
            GetSystemMetrics(SM_CXSCREEN),
            GetSystemMetrics(SM_CYSCREEN),
        )
    }
    // Linux: XRandR union over all CRTCs (multi-display), root geometry
    // fallback, hardcoded last resort (never reached when X is up).
    #[cfg(not(target_os = "windows"))]
    {
        x11q::screen_size().unwrap_or((1920, 1080))
    }
}

/// Primary work area in physical pixels (left, top, right, bottom): the
/// screen minus the taskbar/appbars. Bottom-dock math MUST use this, not
/// screen_size(), or the pill hides behind the taskbar in both collapsed
/// and expanded states. Tracks the taskbar setting automatically: an
/// always-on taskbar shrinks the work area, an auto-hidden one spans the
/// full screen.
#[cfg(target_os = "windows")]
pub fn work_area() -> (i32, i32, i32, i32) {
    unsafe {
        use windows::Win32::Foundation::RECT;
        use windows::Win32::UI::WindowsAndMessaging::{
            SystemParametersInfoW, SPI_GETWORKAREA,
            SYSTEM_PARAMETERS_INFO_UPDATE_FLAGS,
        };
        let mut rc = RECT::default();
        let pv = &mut rc as *mut RECT as *mut std::ffi::c_void;
        if SystemParametersInfoW(
            SPI_GETWORKAREA,
            0,
            Some(pv),
            SYSTEM_PARAMETERS_INFO_UPDATE_FLAGS(0),
        )
        .is_ok()
        {
            return (rc.left, rc.top, rc.right, rc.bottom);
        }
    }
    let (sw, sh) = screen_size();
    (0, 0, sw, sh)
}

#[cfg(not(target_os = "windows"))]
pub fn work_area() -> (i32, i32, i32, i32) {
    // Panel-aware via _NET_WORKAREA (set by XFCE4, Cinnamon, MATE panels and
    // visible under XWayland too): the bottom dock sits above the panel
    // instead of hiding behind it. Falls back to the full screen.
    let (sw, sh) = screen_size();
    if let Some(wa) = x11q::work_area() {
        return wa;
    }
    (0, 0, sw, sh)
}

/// True when the taskbar is set to auto-hide (ABM_GETSTATE & ABS_AUTOHIDE).
#[cfg(target_os = "windows")]
fn taskbar_autohide() -> bool {
    unsafe {
        use windows::Win32::Foundation::{HWND, LPARAM, RECT};
        use windows::Win32::UI::Shell::{
            ABM_GETSTATE, ABS_AUTOHIDE, APPBARDATA, SHAppBarMessage,
        };
        let mut abd = APPBARDATA {
            cbSize: std::mem::size_of::<APPBARDATA>() as u32,
            hWnd: HWND(0),
            uCallbackMessage: 0,
            uEdge: 0,
            rc: RECT::default(),
            lParam: LPARAM(0),
        };
        (SHAppBarMessage(ABM_GETSTATE, &mut abd) as u32 & ABS_AUTOHIDE) != 0
    }
}

/// Current taskbar ("Shell_TrayWnd") rect in physical pixels, when it is
/// actually drawn on-screen. An auto-hidden taskbar reports None while
/// tucked away and Some(...) while revealed (user hovering the edge).
#[cfg(target_os = "windows")]
fn tray_rect() -> Option<(i32, i32, i32, i32)> {
    unsafe {
        use windows::Win32::Foundation::RECT;
        use windows::Win32::UI::WindowsAndMessaging::{
            FindWindowW, GetWindowRect, IsWindowVisible,
        };
        use windows::core::{w, PCWSTR};
        let hwnd = FindWindowW(w!("Shell_TrayWnd"), PCWSTR::null());
        if hwnd.0 == 0 || !IsWindowVisible(hwnd).as_bool() {
            return None;
        }
        let mut rc = RECT::default();
        if GetWindowRect(hwnd, &mut rc).is_ok() {
            // A tucked-away auto-hide bar is a 2px sliver (or off-screen);
            // only a genuinely revealed bar counts.
            if rc.bottom - rc.top <= 2 || rc.right - rc.left <= 2 {
                return None;
            }
            Some((rc.left, rc.top, rc.right, rc.bottom))
        } else {
            None
        }
    }
}

/// Bottom-dock Y (physical px) for a footprint `ph` tall: just above the
/// taskbar in every mode. Always-on taskbar -> work-area bottom minus gap.
/// Auto-hide -> screen edge while tucked, taskbar-top minus gap while the
/// user has it revealed (hovering the edge). `gap` is physical px.
#[cfg(target_os = "windows")]
fn bottom_dock_y(ph: i32, gap: i32) -> i32 {
    let (sw, sh) = screen_size();
    let (_, _, _, wb) = work_area();
    if !taskbar_autohide() {
        return wb - ph - gap;
    }
    if let Some((l, t, r, b)) = tray_rect() {
        let h = b - t;
        let w = r - l;
        // Revealed bottom taskbar: a wide strip sitting on the screen bottom.
        if h > 8 && h < 300 && b >= sh - 2 && w > sw / 2 {
            return t - ph - gap;
        }
    }
    sh - ph - 8
}

/// Snap positions for the 3 docking stations.
/// 0=bottom-center (default), 1=right-middle, 2=left-middle.
/// Window size is LOGICAL px (Slint units); the screen is physical, so the
/// footprint is scaled here. Taskbar-aware: the bottom station sits just
/// above the taskbar (always-on -> work-area bottom; auto-hide -> screen
/// edge while tucked, taskbar-top while revealed), in BOTH collapsed and
/// expanded footprints. Sides centre in the work area and hug its edges,
/// so a side taskbar never covers them either. Gaps stay tight (8px bottom,
/// `margin`/6px sides) — docked, never floating mid-air.
pub fn dock_position(
    index: usize,
    logical_w: i32,
    logical_h: i32,
    margin: i32,
    scale: f32,
) -> (i32, i32) {
    let s = scale.max(0.5);
    let w = (logical_w as f32 * s).round() as i32;
    let h = (logical_h as f32 * s).round() as i32;
    #[cfg(target_os = "windows")]
    {
        let (sw, _) = screen_size();
        let (wl, wt, wr, wb) = work_area();
        match index % 3 {
            0 => ((sw - w) / 2, bottom_dock_y(h, 8)),
            1 => (wr - w - margin, wt + (wb - wt - h) / 2),
            _ => (wl + margin, wt + (wb - wt - h) / 2),
        }
    }
    #[cfg(not(target_os = "windows"))]
    {
        let (sw, sh) = screen_size();
        match index % 3 {
            0 => ((sw - w) / 2, sh - h - 10),
            1 => (sw - w - margin, (sh - h) / 2),
            _ => (margin, (sh - h) / 2),
        }
    }
}

#[cfg(target_os = "windows")]
pub fn dpi_scale(hwnd_raw: isize) -> f32 {
    unsafe {
        let dpi = GetDpiForWindow(HWND(hwnd_raw as _));
        dpi as f32 / 96.0
    }
}

/// Measure single-line text width in LOGICAL px with GDI using the same
/// family/weight Slint renders (Segoe UI). Used to shrink-wrap the upper
/// text pill tightly around its content. Falls back to a char estimate.
pub fn measure_text(window: &slint::Window, text: &str, logical_px: f32, semibold: bool) -> f32 {
    if text.is_empty() {
        return 0.0;
    }
    #[cfg(target_os = "windows")]
    {
        use windows::Win32::Foundation::{HWND, RECT};
        use windows::Win32::Graphics::Gdi::{
            CreateFontW, DeleteObject, DrawTextW, GetDC, ReleaseDC, SelectObject,
            HGDIOBJ, DT_CALCRECT, DT_NOPREFIX, DT_SINGLELINE,
        };
        use windows::core::PCWSTR;
        let scale = window_scale(window).max(0.5);
        unsafe {
            let hdc = GetDC(HWND(0));
            if hdc.0 == 0 {
                return estimate(text, logical_px);
            }
            let phys_h = -((logical_px * scale).round() as i32).max(1);
            let weight: i32 = if semibold { 600 } else { 400 };
            let face: Vec<u16> = "Segoe UI".encode_utf16().chain(std::iter::once(0)).collect();
            let hfont = CreateFontW(
                phys_h, 0, 0, 0, weight, 0, 0, 0, 0, 0, 0, 0, 0,
                PCWSTR(face.as_ptr()),
            );
            let mut w = estimate(text, logical_px) * scale;
            if hfont.0 != 0 {
                let hgdi = HGDIOBJ(hfont.0);
                let old: HGDIOBJ = SelectObject(hdc, hgdi);
                let mut wide: Vec<u16> = text.encode_utf16().collect();
                let mut rc = RECT::default();
                if DrawTextW(hdc, &mut wide, &mut rc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX) > 0
                {
                    w = (rc.right - rc.left) as f32;
                }
                SelectObject(hdc, old);
                let _ = DeleteObject(hgdi);
            }
            let _ = ReleaseDC(HWND(0), hdc);
            w / scale
        }
    }
    #[cfg(not(target_os = "windows"))]
    {
        let _ = window;
        let _ = semibold;
        // No GDI equivalent without a font stack: Slint falls back to
        // fontconfig sans (DejaVu Sans on Mint — wider than Segoe UI), so
        // the Windows 0.55 average would under-measure and the pill would
        // jump widths on expand. 0.60 matches DejaVu Sans Latin average.
        estimate(text, logical_px) * 1.09
    }
}

#[allow(dead_code)]
fn estimate(text: &str, logical_px: f32) -> f32 {
    text.chars().count() as f32 * logical_px * 0.55
}

/// True when an X display can be opened (Linux hotkey-thread guard: the
/// global-hotkey backend segfaults in XDefaultRootWindow on a null display,
/// so the app skips hotkey registration entirely instead of crashing).
pub fn can_open_x_display() -> bool {
    #[cfg(target_os = "windows")]
    {
        true
    }
    #[cfg(not(target_os = "windows"))]
    {
        x11q::can_connect()
    }
}

/// Pure-Rust X11 queries backing the Linux widget math (cursor, screens,
/// work area, DPI). Every query connects fresh and degrades to None when no
/// X server is reachable (pure-Wayland session, headless CI) — callers fall
/// back to safe constants, never panic.
#[cfg(not(target_os = "windows"))]
mod x11q {
    use x11rb::connection::Connection;
    use x11rb::protocol::randr::ConnectionExt as _;
    use x11rb::protocol::xproto::{AtomEnum, ConnectionExt as _};
    use x11rb::rust_connection::RustConnection;

    fn connect() -> Option<(RustConnection, u32)> {
        let (conn, screen) = RustConnection::connect(None).ok()?;
        let root = conn.setup().roots.get(screen)?.root;
        Some((conn, root))
    }

    pub fn can_connect() -> bool {
        connect().is_some()
    }

    pub fn cursor_pos() -> Option<(i32, i32)> {
        let (conn, root) = connect()?;
        let r = x11rb::protocol::xproto::query_pointer(&conn, root)
            .ok()?
            .reply()
            .ok()?;
        Some((r.root_x as i32, r.root_y as i32))
    }

    /// Union of all enabled CRTCs (multi-display); root geometry fallback.
    pub fn screen_size() -> Option<(i32, i32)> {
        let (conn, root) = connect()?;
        if let Ok(res) = randr_get_resources(&conn, root) {
            let (mut x1, mut y1) = (i32::MAX, i32::MAX);
            let (mut x2, mut y2) = (i32::MIN, i32::MIN);
            let mut any = false;
            for info in res {
                any = true;
                x1 = x1.min(info.0);
                y1 = y1.min(info.1);
                x2 = x2.max(info.0 + info.2);
                y2 = y2.max(info.1 + info.3);
            }
            if any {
                return Some((x2 - x1, y2 - y1));
            }
        }
        let g = x11rb::protocol::xproto::get_geometry(&conn, root)
            .ok()?
            .reply()
            .ok()?;
        Some((g.width as i32, g.height as i32))
    }

    /// Enabled CRTC rects as (x, y, w, h).
    fn randr_get_resources(
        conn: &RustConnection,
        root: u32,
    ) -> Result<Vec<(i32, i32, i32, i32)>, ()> {
        let res = conn
            .randr_get_screen_resources_current(root)
            .map_err(|_| ())?
            .reply()
            .map_err(|_| ())?;
        let mut out = Vec::new();
        for crtc in &res.crtcs {
            let info = conn
                .randr_get_crtc_info(*crtc, res.config_timestamp)
                .map_err(|_| ())?
                .reply()
                .map_err(|_| ())?;
            if info.mode != 0 && info.width > 0 && info.height > 0 {
                out.push((
                    info.x as i32,
                    info.y as i32,
                    info.width as i32,
                    info.height as i32,
                ));
            }
        }
        Ok(out)
    }

    /// First _NET_WORKAREA rect as (left, top, right, bottom).
    /// Property format is [x, y, width, height] CARDINALs per desktop.
    pub fn work_area() -> Option<(i32, i32, i32, i32)> {
        let (conn, root) = connect()?;
        let atom = conn
            .intern_atom(false, b"_NET_WORKAREA")
            .ok()?
            .reply()
            .ok()?
            .atom;
        let prop = conn
            .get_property(false, root, atom, AtomEnum::CARDINAL, 0, 4)
            .ok()?
            .reply()
            .ok()?;
        if prop.format != 32 || prop.value.len() < 16 {
            return None;
        }
        let mut v = [0u32; 4];
        for (i, chunk) in prop.value.chunks_exact(4).take(4).enumerate() {
            v[i] = u32::from_ne_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]);
        }
        let (x, y, w, h) = (v[0] as i32, v[1] as i32, v[2] as i32, v[3] as i32);
        if w <= 0 || h <= 0 {
            return None;
        }
        Some((x, y, x + w, y + h))
    }

    /// Desktop UI scale from EDID physical size (DPI/96, snapped to 0.25,
    /// clamped 1–3). Cached process-wide.
    pub fn ui_scale() -> f32 {
        static SCALE: std::sync::OnceLock<f32> = std::sync::OnceLock::new();
        *SCALE.get_or_init(|| edid_scale().unwrap_or(1.0))
    }

    fn edid_scale() -> Option<f32> {
        let (conn, root) = connect()?;
        let res = conn
            .randr_get_screen_resources_current(root)
            .ok()?
            .reply()
            .ok()?;
        for output in &res.outputs {
            let info = conn
                .randr_get_output_info(*output, res.config_timestamp)
                .ok()?
                .reply()
                .ok()?;
            // Lit CRTC + known physical size => trustworthy DPI.
            if info.crtc != 0 && info.mm_width > 0 {
                let crtc = conn
                    .randr_get_crtc_info(info.crtc, res.config_timestamp)
                    .ok()?
                    .reply()
                    .ok()?;
                if crtc.width == 0 {
                    continue;
                }
                let dpi = crtc.width as f32 * 25.4 / info.mm_width as f32;
                let s = ((dpi / 96.0) * 4.0).round() / 4.0;
                return Some(s.clamp(1.0, 3.0));
            }
        }
        None
    }
}
