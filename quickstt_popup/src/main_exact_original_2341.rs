//! QuickSTT Popup — Solid overlay widget using egui viewport visibility
//! Hold Ctrl+Space = show + record, Release = stop + type + hide.

mod hotkey;
mod typing;

use std::ffi::c_void;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use hotkey::HotkeyEvent;

// Global app start time for animation timing (Instant::now().elapsed() is always 0!)
static APP_START: std::sync::LazyLock<Instant> = std::sync::LazyLock::new(Instant::now);

// ─── Win32 API (for widget style only) ──────────────────────────────────────

#[link(name = "user32")]
extern "system" {
    fn EnumWindows(lpEnumFunc: usize, lParam: isize) -> i32;
    fn GetWindowThreadProcessId(hWnd: isize, lpdwProcessId: *mut u32) -> u32;
    fn GetCurrentProcessId() -> u32;
    fn SetWindowLongPtrW(hWnd: isize, nIndex: i32, dwNewLong: isize) -> isize;
    fn GetWindowLongPtrW(hWnd: isize, nIndex: i32) -> isize;
    fn SetWindowPos(
        hWnd: isize,
        hWndInsertAfter: isize,
        x: i32,
        y: i32,
        cx: i32,
        cy: i32,
        uFlags: u32,
    ) -> i32;
    fn GetWindowRect(hWnd: isize, lpRect: *mut RECT) -> i32;
    fn GetCursorPos(lpPoint: *mut POINT) -> i32;
    fn GetForegroundWindow() -> isize;
    fn MonitorFromPoint(pt: POINT, dwFlags: u32) -> isize;
    fn MonitorFromWindow(hWnd: isize, dwFlags: u32) -> isize;
    fn GetMonitorInfoW(hMonitor: isize, lpmi: *mut MONITORINFO) -> i32;
    fn SystemParametersInfoW(
        uiAction: u32,
        uiParam: u32,
        pvParam: *mut c_void,
        fWinIni: u32,
    ) -> i32;
    fn ShowWindow(hWnd: isize, nCmdShow: i32) -> i32;
    fn IsWindowVisible(hWnd: isize) -> i32;
    fn InvalidateRect(hWnd: isize, lpRect: *const RECT, bErase: i32) -> i32;
    fn UpdateWindow(hWnd: isize) -> i32;
    fn RegisterClassW(lpWndClass: *const WNDCLASSW) -> u16;
    fn CreateWindowExW(
        dwExStyle: u32,
        lpClassName: *const u16,
        lpWindowName: *const u16,
        dwStyle: u32,
        x: i32,
        y: i32,
        nWidth: i32,
        nHeight: i32,
        hWndParent: isize,
        hMenu: isize,
        hInstance: isize,
        lpParam: *const c_void,
    ) -> isize;
    fn DefWindowProcW(hWnd: isize, msg: u32, wParam: usize, lParam: isize) -> isize;
    fn PeekMessageW(
        lpMsg: *mut NATIVE_MSG,
        hWnd: isize,
        wMsgFilterMin: u32,
        wMsgFilterMax: u32,
        wRemoveMsg: u32,
    ) -> i32;
    fn TranslateMessage(lpMsg: *const NATIVE_MSG) -> i32;
    fn DispatchMessageW(lpMsg: *const NATIVE_MSG) -> isize;
    fn PostQuitMessage(nExitCode: i32);
    fn GetClientRect(hWnd: isize, lpRect: *mut RECT) -> i32;
    fn GetDC(hWnd: isize) -> isize;
    fn ReleaseDC(hWnd: isize, hDC: isize) -> i32;
    fn FillRect(hDC: isize, lprc: *const RECT, hbr: isize) -> i32;
    fn SetWindowRgn(hWnd: isize, hRgn: isize, bRedraw: i32) -> i32;
    fn DrawTextW(
        hdc: isize,
        lpchText: *const u16,
        cchText: i32,
        lprc: *mut RECT,
        format: u32,
    ) -> i32;
    fn SetCapture(hWnd: isize) -> isize;
    fn ReleaseCapture() -> i32;
    fn GetSystemMetrics(nIndex: i32) -> i32;
    fn LoadCursorW(hInstance: isize, lpCursorName: *const u16) -> isize;
    fn SetCursor(hCursor: isize) -> isize;
}

const DT_WORDBREAK: u32 = 0x00000010;
const DT_LEFT: u32 = 0x00000000;
const DT_NOPREFIX: u32 = 0x00000800;

#[link(name = "kernel32")]
extern "system" {
    fn GetModuleHandleW(lpModuleName: *const u16) -> isize;
}

#[link(name = "gdi32")]
extern "system" {
    fn CreateFontW(
        cHeight: i32,
        cWidth: i32,
        cEscapement: i32,
        cOrientation: i32,
        cWeight: i32,
        bItalic: u32,
        bUnderline: u32,
        bStrikeOut: u32,
        iCharSet: u32,
        iOutPrecision: u32,
        iClipPrecision: u32,
        iQuality: u32,
        iPitchAndFamily: u32,
        pszFaceName: *const u16,
    ) -> isize;
    fn CreateRoundRectRgn(x1: i32, y1: i32, x2: i32, y2: i32, w: i32, h: i32) -> isize;
    fn CreateSolidBrush(color: u32) -> isize;
    fn CreatePen(iStyle: i32, cWidth: i32, color: u32) -> isize;
    fn SelectObject(hdc: isize, h: isize) -> isize;
    fn FillRgn(hdc: isize, hrgn: isize, hbr: isize) -> i32;
    fn DeleteObject(ho: isize) -> i32;
    fn SetBkMode(hdc: isize, mode: i32) -> i32;
    fn SetTextColor(hdc: isize, color: u32) -> u32;
    fn TextOutW(hdc: isize, x: i32, y: i32, text: *const u16, length: i32) -> i32;
    fn Ellipse(hdc: isize, left: i32, top: i32, right: i32, bottom: i32) -> i32;
    fn MoveToEx(hdc: isize, x: i32, y: i32, lpPoint: *mut POINT) -> i32;
    fn LineTo(hdc: isize, x: i32, y: i32) -> i32;
    fn SetPixel(hdc: isize, x: i32, y: i32, color: u32) -> u32;
}

const WS_EX_LAYERED: isize = 0x00080000;
const ULW_ALPHA: u32 = 0x00000002;
const AC_SRC_OVER: u8 = 0x00;
const AC_SRC_ALPHA: u8 = 0x01;

#[repr(C)]
#[derive(Clone, Copy)]
struct BLENDFUNCTION {
    blend_op: u8,
    blend_flags: u8,
    source_constant_alpha: u8,
    alpha_format: u8,
}

#[repr(C)]
struct BITMAPINFOHEADER {
    bi_size: u32,
    bi_width: i32,
    bi_height: i32,
    bi_planes: u16,
    bi_bit_count: u16,
    bi_compression: u32,
    bi_size_image: u32,
    bi_xpels_per_meter: i32,
    bi_ypels_per_meter: i32,
    bi_clr_used: u32,
    bi_clr_important: u32,
}

#[repr(C)]
struct BITMAPINFO {
    bmi_header: BITMAPINFOHEADER,
    bmi_colors: [u32; 1],
}

#[link(name = "user32")]
extern "system" {
    fn UpdateLayeredWindow(
        hwnd: isize,
        hdc_dst: isize,
        pt_dst: *const POINT,
        size: *const POINT,
        hdc_src: isize,
        pt_src: *const POINT,
        cr_key: u32,
        pblend: *const BLENDFUNCTION,
        dw_flags: u32,
    ) -> i32;
}

#[link(name = "gdi32")]
extern "system" {
    fn CreateCompatibleDC(hdc: isize) -> isize;
    fn DeleteDC(hdc: isize) -> i32;
    fn CreateDIBSection(
        hdc: isize,
        pbmi: *const BITMAPINFO,
        usage: u32,
        ppv_bits: *mut *mut c_void,
        h_section: isize,
        offset: u32,
    ) -> isize;
}

const PS_SOLID: i32 = 0;
// Handy compact pill geometry (RecordingOverlay.css)
const OV_REST_W: i32 = 172;
const OV_WORK_W: i32 = 216;
const OV_OPEN_W: i32 = 392; // Handy Live panel when streaming text is visible
const OV_BASE_H: i32 = 40;
const OV_OPEN_H: i32 = 104; // control row + live text cap
const WAVE_BARS: usize = 9;

const GWL_EXSTYLE: i32 = -20;
const WS_EX_TOOLWINDOW: isize = 0x00000080;
const WS_EX_APPWINDOW: isize = 0x00040000;
const WS_EX_TOPMOST: isize = 0x00000008;
const WS_EX_NOACTIVATE: isize = 0x08000000;
const WS_EX_TRANSPARENT: isize = 0x00000020;
const HWND_TOPMOST: isize = -1;
const SWP_NOACTIVATE: u32 = 0x0010;
const SWP_FRAMECHANGED: u32 = 0x0020;
const SWP_NOOWNERZORDER: u32 = 0x0200;
const SWP_SHOWWINDOW: u32 = 0x0040;
const MONITOR_DEFAULTTONEAREST: u32 = 2;
const SPI_GETWORKAREA: u32 = 0x0030;
const SW_SHOWNOACTIVATE: i32 = 4;
const SW_HIDE: i32 = 0;
const WS_POPUP: u32 = 0x80000000;
const PM_REMOVE: u32 = 0x0001;
const WM_DESTROY: u32 = 0x0002;
const WM_QUIT: u32 = 0x0012;
const TRANSPARENT: i32 = 1;
// Keep the capsule clear of the taskbar, including auto-hidden taskbars.
const OVERLAY_BOTTOM_GAP: i32 = 96;

// ─── Always-on micro pill & morph dimensions ───
const IDLE_PILL_W: i32 = 60;
const IDLE_PILL_H: i32 = 8;
const IDLE_WINDOW_W: i32 = 60;
const IDLE_WINDOW_H: i32 = 8;
const MORPH_PILL_W: i32 = 180;
const MORPH_PILL_H: i32 = 68;
const MORPH_DURATION_MS: f32 = 200.0;
const HOVER_LEAVE_DELAY_MS: u64 = 600;
// Context menu
const MENU_W: i32 = 240;
const MENU_ITEM_H: i32 = 36;
const MENU_SEPARATOR_H: i32 = 1;
const MENU_PADDING: i32 = 28;
const MENU_ITEMS: usize = 6;
// No-signal popup
const NO_SIGNAL_W: i32 = 360;
const NO_SIGNAL_H: i32 = 100;
const NO_SIGNAL_DELAY_SECS: u64 = 5;

// Mouse messages
const WM_MOUSEMOVE: u32 = 0x0200;
const WM_RBUTTONDOWN: u32 = 0x0204;
const WM_MOUSELEAVE: u32 = 0x02A3;
const TME_LEAVE: u32 = 0x00000002;

#[repr(C)]
struct TRACKMOUSEEVENT {
    cb_size: u32,
    dw_flags: u32,
    hwnd_track: isize,
    dw_hover_time: u32,
}

#[link(name = "user32")]
extern "system" {
    fn TrackMouseEvent(lpEventTrack: *mut TRACKMOUSEEVENT) -> i32;
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct POINT {
    x: i32,
    y: i32,
}

#[repr(C)]
struct WNDCLASSW {
    style: u32,
    lpfn_wnd_proc: Option<unsafe extern "system" fn(isize, u32, usize, isize) -> isize>,
    cb_cls_extra: i32,
    cb_wnd_extra: i32,
    h_instance: isize,
    h_icon: isize,
    h_cursor: isize,
    hbr_background: isize,
    lpsz_menu_name: *const u16,
    lpsz_class_name: *const u16,
}

#[repr(C)]
#[derive(Default)]
struct NATIVE_MSG {
    hwnd: isize,
    message: u32,
    w_param: usize,
    l_param: isize,
    time: u32,
    pt: POINT,
    l_private: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct RECT {
    left: i32,
    top: i32,
    right: i32,
    bottom: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct MONITORINFO {
    cb_size: u32,
    rc_monitor: RECT,
    rc_work: RECT,
    dw_flags: u32,
}

static mut OUR_HWND: isize = 0;

fn widget_size() -> Option<(i32, i32)> {
    unsafe {
        if OUR_HWND == 0 {
            return None;
        }
        let mut rect = RECT::default();
        if GetWindowRect(OUR_HWND, &mut rect) == 0 {
            return None;
        }
        Some((rect.right - rect.left, rect.bottom - rect.top))
    }
}

fn refresh_widget_region(_width: i32, _height: i32) {
    // No-op: WS_EX_LAYERED layered window handles per-pixel alpha anti-aliased composition natively.
}

unsafe extern "system" fn enum_windows_proc(hwnd: isize, _lparam: isize) -> i32 {
    let mut pid: u32 = 0;
    GetWindowThreadProcessId(hwnd, &mut pid);
    if pid == GetCurrentProcessId() {
        OUR_HWND = hwnd;
        return 0;
    }
    1
}

fn apply_widget_style() {
    unsafe {
        OUR_HWND = 0;
        EnumWindows(enum_windows_proc as *const () as usize, 0);
        if OUR_HWND == 0 {
            return;
        }
        let ex = GetWindowLongPtrW(OUR_HWND, GWL_EXSTYLE);
        let new_ex = (ex | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE) & !WS_EX_APPWINDOW;
        SetWindowLongPtrW(OUR_HWND, GWL_EXSTYLE, new_ex);
        let (width, height) = widget_size().unwrap_or((OV_REST_W, OV_BASE_H));
        refresh_widget_region(width, height);
        // Move off-screen initially
        SetWindowPos(
            OUR_HWND,
            HWND_TOPMOST,
            -width - 32,
            -height - 32,
            width,
            height,
            SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED,
        );
        let hwnd = OUR_HWND;
        eprintln!(
            "[POPUP] Widget style applied (HWND={:#x}), capsule region set",
            hwnd
        );
    }
}

fn move_to_dock_position(dock_side: u8) {
    unsafe {
        if OUR_HWND == 0 {
            return;
        }
        let (width, height) = widget_size().unwrap_or((OV_REST_W, OV_BASE_H));
        let foreground = GetForegroundWindow();
        let mut cursor = POINT::default();
        let monitor = if foreground != 0 && foreground != OUR_HWND {
            MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST)
        } else if GetCursorPos(&mut cursor) != 0 {
            MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST)
        } else {
            0
        };
        let mut info = MONITORINFO {
            cb_size: std::mem::size_of::<MONITORINFO>() as u32,
            rc_monitor: RECT::default(),
            rc_work: RECT::default(),
            dw_flags: 0,
        };
        let work = if monitor != 0 && GetMonitorInfoW(monitor, &mut info) != 0 {
            info.rc_work
        } else {
            RECT { left: 0, top: 0, right: 1920, bottom: 1080 }
        };
        let gap = if height <= MORPH_PILL_H { 4 } else { OVERLAY_BOTTOM_GAP };
        let (x, y) = match dock_side {
            1 => (work.left + gap, work.top + ((work.bottom - work.top - height) / 2)),
            2 => (work.right - width - gap, work.top + ((work.bottom - work.top - height) / 2)),
            _ => (
                work.left + ((work.right - work.left - width) / 2),
                (work.bottom - height - gap).max(work.top + 12),
            ),
        };
        refresh_widget_region(width, height);
        ShowWindow(OUR_HWND, SW_SHOWNOACTIVATE);
        SetWindowPos(
            OUR_HWND,
            HWND_TOPMOST,
            x,
            y,
            width,
            height,
            SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW,
        );
        InvalidateRect(OUR_HWND, std::ptr::null(), 1);
        UpdateWindow(OUR_HWND);
    }
}

fn move_off_screen() {
    unsafe {
        if OUR_HWND == 0 {
            return;
        }
        let (width, height) = widget_size().unwrap_or((OV_REST_W, OV_BASE_H));
        SetWindowPos(
            OUR_HWND,
            HWND_TOPMOST,
            -width - 32,
            -height - 32,
            width,
            height,
            SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED,
        );
    }
}

// ─── Shared State ───────────────────────────────────────────────────────────

#[derive(Debug, Clone, Copy, PartialEq)]
enum OverlayState {
    Idle,        // Always-on micro pill (visible, docked to edge)
    Hidden,      // Fully hidden (always_on_pill disabled)
    Recording,
    Transcribing,
    Done,
}

struct PopupState {
    state: OverlayState,
    final_text: String,
    // A held Ctrl+Space session may produce several VAD-delimited Parakeet
    // segments. They remain local until the user releases the chord.
    captured_text: String,
    release_requested: bool,
    commit_sent: bool,
    audio_levels: Vec<f32>,
    /// Exponentially smoothed levels for Handy-style waveform (0.7 prev + 0.3 new).
    smoothed_levels: Vec<f32>,
    show_until: Option<Instant>,
    transcribing_since: Option<Instant>,
    activation_mode: u8, // 0=push-to-talk, 1=toggle
    output_mode: u8,     // 0=type, 1=clipboard, 2=none
    toggle_active: bool, // For toggle mode: is recording active?
    /// True while the user is holding a session (for cancel → popup_sleep).
    session_live: bool,
    /// Wall-clock start of the current visible session (for optional timer).
    session_started: Option<Instant>,
    /// Live streaming text (Handy Live panel): committed prefix + tentative tail.
    live_committed: String,
    live_tentative: String,
    /// True when backend advertised streaming capability for this session.
    streaming_mode: bool,
    /// Incremental session ID to prevent cross-session text leakage or delayed commits.
    session_id: u64,
    // ─── Always-on pill & morph ───
    always_on_pill: bool,
    dock_side: u8,              // 0=bottom, 1=left, 2=right
    morph_progress: f32,        // 0.0=micro pill, 1.0=full morph
    morph_target: f32,
    morph_last_tick: Instant,
    hover_active: bool,
    hover_leave_time: Option<Instant>,
    mouse_tracking_set: bool,
    // Drag-to-dock
    dragging: bool,
    drag_start_x: i32,
    drag_start_y: i32,
    drag_preview_dock: i32,
    // Context menu
    context_menu_open: bool,
    context_menu_hover: i32,    // -1 = none, 0..5 = item index
    // No-signal detection
    no_signal_shown: bool,
    no_signal_since: Option<Instant>,
    last_voice_time: Instant,
    // Hide timer
    hidden_until: Option<Instant>,
}

// ─── Main ───────────────────────────────────────────────────────────────────

fn main() {
    // Force-initialize the global epoch for animation timing
    let _ = *APP_START;
    eprintln!("[POPUP] QuickSTT popup starting (v3 solid)...");

    let state = Arc::new(Mutex::new(PopupState {
        state: OverlayState::Idle,
        final_text: String::new(),
        captured_text: String::new(),
        release_requested: false,
        commit_sent: false,
        audio_levels: vec![0.0; WAVE_BARS],
        smoothed_levels: vec![0.0; WAVE_BARS],
        show_until: None,
        transcribing_since: None,
        activation_mode: 0,
        output_mode: 0,
        toggle_active: false,
        session_live: false,
        session_started: None,
        live_committed: String::new(),
        live_tentative: String::new(),
        streaming_mode: false,
        session_id: 0,
        always_on_pill: true,
        dock_side: 0,
        morph_progress: 0.0,
        morph_target: 0.0,
        morph_last_tick: Instant::now(),
        hover_active: false,
        hover_leave_time: None,
        mouse_tracking_set: false,
        dragging: false,
        drag_start_x: 0,
        drag_start_y: 0,
        drag_preview_dock: -1,
        context_menu_open: false,
        context_menu_hover: -1,
        no_signal_shown: false,
        no_signal_since: None,
        last_voice_time: Instant::now(),
        hidden_until: None,
    }));

    let _ = GLOBAL_STATE.set(state.clone());

    let tcp_writer: Arc<Mutex<Option<std::net::TcpStream>>> = Arc::new(Mutex::new(None));
    let tcp_connected = Arc::new(AtomicBool::new(false));

    // Shared with the native overlay message pump so the cancel (X) button can
    // send popup_sleep without owning the hotkey thread's handles.
    let _ = CANCEL_TCP_WRITER.set(tcp_writer.clone());
    let _ = CANCEL_TCP_CONNECTED.set(tcp_connected.clone());

    // TCP reader
    let st = state.clone();
    let tw = tcp_writer.clone();
    let tc = tcp_connected.clone();
    std::thread::spawn(move || tcp_loop(st, tw, tc));

    // Hotkey thread
    let (hotkey_tx, hotkey_rx) = mpsc::channel::<HotkeyEvent>();
    let tw2 = tcp_writer.clone();
    let tc2 = tcp_connected.clone();
    let st2 = state.clone();
    std::thread::spawn(move || hotkey_handler(hotkey_tx, tw2, tc2, st2));

    // This is deliberately a direct Win32 Rust overlay. eframe's window was
    // reporting visible while its surface was not painted on this desktop.
    // A native WS_POPUP window lets us own show, paint, z-order and focus.
    run_native_overlay(state, hotkey_rx);
}

// ─── Hotkey Handler ─────────────────────────────────────────────────────────

fn send_popup_command(
    tcp_writer: &Arc<Mutex<Option<std::net::TcpStream>>>,
    tcp_connected: &Arc<AtomicBool>,
    command: &[u8],
) -> bool {
    use std::io::Write;

    if !tcp_connected.load(Ordering::SeqCst) {
        return false;
    }
    let mut writer = tcp_writer.lock().unwrap();
    let Some(stream) = writer.as_mut() else {
        tcp_connected.store(false, Ordering::SeqCst);
        return false;
    };
    if stream
        .write_all(command)
        .and_then(|_| stream.flush())
        .is_ok()
    {
        true
    } else {
        *writer = None;
        tcp_connected.store(false, Ordering::SeqCst);
        false
    }
}

fn hotkey_handler(
    tx: mpsc::Sender<HotkeyEvent>,
    tcp_writer: Arc<Mutex<Option<std::net::TcpStream>>>,
    tcp_connected: Arc<AtomicBool>,
    state: Arc<Mutex<PopupState>>,
) {
    let (hook_tx, hook_rx) = mpsc::channel::<HotkeyEvent>();
    std::thread::spawn(move || hotkey::run_hotkey_loop(hook_tx));

    loop {
        match hook_rx.recv() {
            Ok(HotkeyEvent::Press) => {
                let mode = { state.lock().unwrap().activation_mode };
                if mode == 0 {
                    // Push-to-talk: start on press
                    eprintln!("[POPUP] PTT PRESS");
                    if !send_popup_command(
                        &tcp_writer,
                        &tcp_connected,
                        b"{\"cmd\":\"popup_start\"}\n",
                    ) {
                        eprintln!("[POPUP] Ignoring hotkey: audio engine is not connected");
                        continue;
                    }
                    {
                        let mut st = state.lock().unwrap();
                        begin_recording_session(&mut st);
                    }
                    let _ = tx.send(HotkeyEvent::Press);
                } else {
                    // Toggle mode: press to start, press again to stop
                    let is_active = { state.lock().unwrap().toggle_active };
                    if !is_active {
                        eprintln!("[POPUP] TOGGLE START");
                        if !send_popup_command(
                            &tcp_writer,
                            &tcp_connected,
                            b"{\"cmd\":\"popup_start\"}\n",
                        ) {
                            eprintln!("[POPUP] Ignoring hotkey: audio engine is not connected");
                            continue;
                        }
                        {
                            let mut st = state.lock().unwrap();
                            begin_recording_session(&mut st);
                            st.toggle_active = true;
                        }
                        let _ = tx.send(HotkeyEvent::Press);
                    } else {
                        eprintln!("[POPUP] TOGGLE STOP");
                        let sent = send_popup_command(
                            &tcp_writer,
                            &tcp_connected,
                            b"{\"cmd\":\"popup_stop\"}\n",
                        );
                        if !sent {
                            eprintln!("[POPUP] Audio engine disconnected during toggle stop");
                        }
                        {
                            let mut st = state.lock().unwrap();
                            if sent && st.state == OverlayState::Recording {
                                enter_transcribing(&mut st);
                            } else if !sent {
                                hide_session(&mut st);
                            }
                            st.toggle_active = false;
                        }
                        let _ = tx.send(HotkeyEvent::Release);
                    }
                }
            }
            Ok(HotkeyEvent::Release) => {
                let mode = { state.lock().unwrap().activation_mode };
                if mode == 0 {
                    // Push-to-talk: stop on release
                    eprintln!("[POPUP] PTT RELEASE");
                    let sent = send_popup_command(
                        &tcp_writer,
                        &tcp_connected,
                        b"{\"cmd\":\"popup_stop\"}\n",
                    );
                    {
                        let mut st = state.lock().unwrap();
                        if sent && st.state == OverlayState::Recording {
                            enter_transcribing(&mut st);
                        } else if !sent {
                            hide_session(&mut st);
                        }
                    }
                    let _ = tx.send(HotkeyEvent::Release);
                }
                // In toggle mode, release does nothing
            }
            Err(_) => break,
        }
    }
}

fn begin_recording_session(st: &mut PopupState) {
    st.session_id += 1;
    st.state = OverlayState::Recording;
    st.final_text.clear();
    st.captured_text.clear();
    st.live_committed.clear();
    st.live_tentative.clear();
    st.release_requested = false;
    st.commit_sent = false;
    st.audio_levels = vec![0.0; WAVE_BARS];
    st.smoothed_levels = vec![0.0; WAVE_BARS];
    st.show_until = None;
    st.transcribing_since = None;
    st.session_live = true;
    st.session_started = Some(Instant::now());
    eprintln!("[POPUP] Started recording session #{}", st.session_id);
}

fn enter_transcribing(st: &mut PopupState) {
    st.state = OverlayState::Transcribing;
    st.transcribing_since = Some(Instant::now());
    st.release_requested = true;
    st.session_live = true;
}

fn hide_session(st: &mut PopupState) {
    // Go to Idle (micro pill) if always-on is enabled, otherwise fully hide
    st.state = if st.always_on_pill {
        OverlayState::Idle
    } else {
        OverlayState::Hidden
    };
    st.session_live = false;
    st.session_started = None;
    st.toggle_active = false;
    st.release_requested = false;
    st.transcribing_since = None;
    st.live_committed.clear();
    st.live_tentative.clear();
    st.context_menu_open = false;
    st.context_menu_hover = -1;
    st.no_signal_shown = false;
    st.no_signal_since = None;
    st.morph_target = 0.0;
}

/// Cancel the current session without committing text (Handy cancel button).
fn cancel_session(
    state: &Arc<Mutex<PopupState>>,
    tcp_writer: &Arc<Mutex<Option<std::net::TcpStream>>>,
    tcp_connected: &Arc<AtomicBool>,
) {
    eprintln!("[POPUP] CANCEL");
    let _ = send_popup_command(tcp_writer, tcp_connected, b"{\"cmd\":\"popup_sleep\"}\n");
    let mut st = state.lock().unwrap();
    st.captured_text.clear();
    st.final_text.clear();
    st.commit_sent = true;
    st.release_requested = false;
    hide_session(&mut st);
}

// ─── TCP Loop ───────────────────────────────────────────────────────────────

fn tcp_loop(
    state: Arc<Mutex<PopupState>>,
    tcp_writer: Arc<Mutex<Option<std::net::TcpStream>>>,
    tcp_connected: Arc<AtomicBool>,
) {
    use std::io::{BufRead, BufReader};
    use std::net::TcpStream;

    loop {
        eprintln!("[POPUP] Connecting...");
        let stream = match TcpStream::connect("127.0.0.1:19876") {
            Ok(s) => {
                eprintln!("[POPUP] Connected!");
                s
            }
            Err(_) => {
                std::thread::sleep(Duration::from_secs(1));
                continue;
            }
        };
        let wc = stream.try_clone().unwrap();
        *tcp_writer.lock().unwrap() = Some(wc);
        tcp_connected.store(true, Ordering::SeqCst);

        let reader = BufReader::new(stream);
        for line in reader.lines() {
            let line = match line {
                Ok(l) => l,
                Err(_) => break,
            };
            let msg: serde_json::Value = match serde_json::from_str(&line) {
                Ok(v) => v,
                Err(_) => continue,
            };
            let event = msg.get("event").and_then(|v| v.as_str()).unwrap_or("");
            match event {
                "AUDIO_LEVEL" => {
                    let level = msg.get("level").and_then(|v| v.as_f64()).unwrap_or(0.0) as f32;
                    let mut st = state.lock().unwrap();
                    let raw = (level / 100.0).clamp(0.0, 1.0);
                    st.audio_levels.remove(0);
                    st.audio_levels.push(raw);
                    // Track voice activity for no-signal detection
                    if raw > 0.05 {
                        st.last_voice_time = Instant::now();
                        st.no_signal_shown = false;
                        st.no_signal_since = None;
                    }
                    // Multi-bar spatial envelope generation (center bars taller, dynamic phase variation)
                    let now_sec = APP_START.elapsed().as_secs_f32();
                    for i in 0..WAVE_BARS {
                        let center_dist = ((i as f32 - 4.0) / 4.0).abs();
                        let spatial_weight = (1.0 - center_dist * 0.6).max(0.3);
                        let phase = (now_sec * 12.0 + i as f32 * 0.85).sin() * 0.25 + 0.75;
                        let target = raw * spatial_weight * phase;
                        let prev = st.smoothed_levels.get(i).copied().unwrap_or(0.0);
                        st.smoothed_levels[i] = prev * 0.5 + target * 0.5;
                    }
                }
                "FINAL_TEXT" => {
                    let text = msg
                        .get("text")
                        .and_then(|v| v.as_str())
                        .unwrap_or("")
                        .to_string();
                    eprintln!("[POPUP] FINAL_TEXT received: '{}'", text);
                    let trimmed = text.trim().to_string();
                    if !trimmed.is_empty() {
                        let out_mode = {
                            let mut st = state.lock().unwrap();
                            st.captured_text = trimmed.clone();
                            st.final_text = trimmed.clone();
                            st.live_committed = trimmed.clone();
                            st.live_tentative.clear();
                            st.state = OverlayState::Done;
                            st.show_until = Some(Instant::now() + Duration::from_millis(1500));
                            st.output_mode
                        };
                        commit_text(trimmed, out_mode);
                    } else {
                        let mut st = state.lock().unwrap();
                        if st.state == OverlayState::Transcribing {
                            st.state = if st.always_on_pill { OverlayState::Idle } else { OverlayState::Hidden };
                        }
                    }
                }
                "PARTIAL_TEXT" => {
                    let text = msg
                        .get("text")
                        .and_then(|v| v.as_str())
                        .unwrap_or("")
                        .to_string();
                    let mut st = state.lock().unwrap();
                    st.live_tentative = text;
                    if st.state == OverlayState::Recording || st.state == OverlayState::Transcribing
                    {
                        // Keep recording/transcribing; size loop expands Live.
                    }
                }
                "STREAM_TEXT" => {
                    // Handy Live: committed stable prefix + tentative tail.
                    let committed = msg
                        .get("committed")
                        .and_then(|v| v.as_str())
                        .unwrap_or("")
                        .to_string();
                    let tentative = msg
                        .get("tentative")
                        .or_else(|| msg.get("partial"))
                        .and_then(|v| v.as_str())
                        .unwrap_or("")
                        .to_string();
                    let mut st = state.lock().unwrap();
                    st.streaming_mode = true;
                    if !committed.is_empty() {
                        st.live_committed = committed;
                    }
                    st.live_tentative = tentative;
                    if !st.live_committed.is_empty() || !st.live_tentative.is_empty() {
                        // Mirror into capture buffer for commit-on-release.
                        let tentative = st.live_tentative.clone();
                        st.captured_text = st.live_committed.clone();
                        if !tentative.is_empty() {
                            if !st.captured_text.is_empty()
                                && !st
                                    .captured_text
                                    .chars()
                                    .last()
                                    .map(char::is_whitespace)
                                    .unwrap_or(false)
                            {
                                st.captured_text.push(' ');
                            }
                            st.captured_text.push_str(&tentative);
                        }
                    }
                }
                "MODEL_CAP" => {
                    let streaming = msg
                        .get("streaming")
                        .and_then(|v| v.as_bool())
                        .unwrap_or(false);
                    let mut st = state.lock().unwrap();
                    st.streaming_mode = streaming;
                    eprintln!("[POPUP] MODEL_CAP streaming={}", streaming);
                }
                "CONFIG" => {
                    let mode = msg.get("mode").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                    let output = msg.get("output").and_then(|v| v.as_u64()).unwrap_or(0) as u8;
                    let always_on = msg.get("always_on_pill").and_then(|v| v.as_bool());
                    let mut st = state.lock().unwrap();
                    st.activation_mode = mode;
                    st.output_mode = output;
                    if let Some(on) = always_on {
                        st.always_on_pill = on;
                        if on && st.state == OverlayState::Hidden {
                            st.state = OverlayState::Idle;
                        } else if !on && st.state == OverlayState::Idle {
                            st.state = OverlayState::Hidden;
                        }
                    }
                    eprintln!("[POPUP] CONFIG: mode={} output={} always_on={:?}", mode, output, always_on);
                }
                "STATE" => {
                    let state_text = msg.get("text").and_then(|v| v.as_str()).unwrap_or("");
                    eprintln!(
                        "[POPUP] STATE: {}",
                        state_text
                    );
                    if is_terminal_popup_state(state_text) {
                        let commit = {
                            let mut st = state.lock().unwrap();
                            take_release_commit(&mut st)
                        };
                        if let Some((text, output_mode)) = commit {
                            commit_text(text, output_mode);
                        }
                    }
                }
                _ => {}
            }
        }
        eprintln!("[POPUP] Disconnected");
        tcp_connected.store(false, Ordering::SeqCst);
        *tcp_writer.lock().unwrap() = None;
        std::thread::sleep(Duration::from_secs(1));
    }
}

fn append_transcript(destination: &mut String, segment: &str) {
    let segment = segment.trim();
    if segment.is_empty() {
        return;
    }
    let ends_in_whitespace = destination
        .chars()
        .last()
        .map(char::is_whitespace)
        .unwrap_or(false);
    if !destination.is_empty() && !ends_in_whitespace {
        destination.push(' ');
    }
    destination.push_str(segment);
}

fn is_terminal_popup_state(state: &str) -> bool {
    matches!(state.trim(), "Ready" | "Hidden" | "Paused" | "Model Missing")
}

fn take_release_commit(state: &mut PopupState) -> Option<(String, u8)> {
    if !state.release_requested || state.commit_sent {
        return None;
    }
    state.commit_sent = true;
    state.release_requested = false;
    state.transcribing_since = None;
    state.session_live = false;
    state.session_started = None;
    state.state = if state.always_on_pill { OverlayState::Idle } else { OverlayState::Hidden };
    Some((state.captured_text.trim().to_string(), state.output_mode))
}

fn commit_text(text: String, output_mode: u8) {
    if text.is_empty() {
        eprintln!("[POPUP] Release completed with no recognized text");
        return;
    }
    eprintln!("[POPUP] COMMIT on release: {} chars", text.len());
    match output_mode {
        0 => {
            std::thread::spawn(move || typing::type_text(&text));
        }
        1 => {
            std::thread::spawn(move || typing::copy_only(&text));
        }
        _ => {}
    }
}

// ─── Direct Win32 overlay ──────────────────────────────────────────────────

fn wide(value: &str) -> Vec<u16> {
    value.encode_utf16().chain(std::iter::once(0)).collect()
}

fn colorref(red: u8, green: u8, blue: u8) -> u32 {
    u32::from(red) | (u32::from(green) << 8) | (u32::from(blue) << 16)
}

static GLOBAL_STATE: std::sync::OnceLock<Arc<Mutex<PopupState>>> = std::sync::OnceLock::new();

unsafe extern "system" fn native_overlay_proc(
    hwnd: isize,
    message: u32,
    w_param: usize,
    l_param: isize,
) -> isize {
    if message == WM_DESTROY {
        PostQuitMessage(0);
        return 0;
    }
    // WM_SETCURSOR (0x0020): force arrow cursor (IDC_ARROW = 32512 as *const u16)
    if message == 0x0020 {
        SetCursor(LoadCursorW(0, 32512 as *const u16));
        return 1;
    }
    // WM_MOUSEMOVE (0x0200): trigger micro pill hover morphing
    if message == 0x0200 {
        let mut tme = TRACKMOUSEEVENT {
            cb_size: std::mem::size_of::<TRACKMOUSEEVENT>() as u32,
            dw_flags: TME_LEAVE,
            hwnd_track: hwnd,
            dw_hover_time: 0,
        };
        TrackMouseEvent(&mut tme);
        if let Some(global_st) = GLOBAL_STATE.get() {
            if let Ok(mut st) = global_st.lock() {
                if st.state == OverlayState::Idle {
                    st.hover_active = true;
                    st.hover_leave_time = None;
                }
            }
        }
    }
    // WM_MOUSELEAVE (0x02A3): handle hover leave
    if message == 0x02A3 {
        if let Some(global_st) = GLOBAL_STATE.get() {
            if let Ok(mut st) = global_st.lock() {
                st.hover_active = false;
                st.hover_leave_time = Some(Instant::now());
            }
        }
    }
    DefWindowProcW(hwnd, message, w_param, l_param)
}

fn has_live_text(st: &PopupState) -> bool {
    !st.live_committed.is_empty() || !st.live_tentative.is_empty()
}

fn lerp_i32(a: i32, b: i32, t: f32) -> i32 {
    (a as f32 + (b as f32 - a as f32) * t.clamp(0.0, 1.0)) as i32
}

fn overlay_size_for(st: &PopupState) -> (i32, i32) {
    let live = has_live_text(st);
    let menu_h = if st.context_menu_open {
        MENU_ITEMS as i32 * MENU_ITEM_H + MENU_PADDING + 8
    } else {
        0
    };
    match st.state {
        OverlayState::Idle if st.context_menu_open => {
            (MENU_W, MORPH_PILL_H + menu_h)
        }
        OverlayState::Idle => {
            let w = lerp_i32(IDLE_WINDOW_W, MORPH_PILL_W, st.morph_progress);
            let h = lerp_i32(IDLE_WINDOW_H, MORPH_PILL_H, st.morph_progress);
            (w, h)
        }
        OverlayState::Recording if st.no_signal_shown => (NO_SIGNAL_W, NO_SIGNAL_H),
        OverlayState::Recording if live => (OV_OPEN_W, OV_OPEN_H),
        OverlayState::Recording => (OV_REST_W, OV_BASE_H),
        OverlayState::Transcribing | OverlayState::Done if live => (OV_OPEN_W, OV_OPEN_H),
        OverlayState::Transcribing | OverlayState::Done => (OV_WORK_W, OV_BASE_H),
        OverlayState::Hidden => (OV_REST_W, OV_BASE_H),
    }
}

fn create_native_overlay_window() -> bool {
    unsafe {
        let class_name = wide("QuickSTTNativeOverlay");
        let title = wide("QuickSTT voice input");
        let instance = GetModuleHandleW(std::ptr::null());
        let class = WNDCLASSW {
            style: 0x0008, // CS_DBLCLKS
            lpfn_wnd_proc: Some(native_overlay_proc),
            cb_cls_extra: 0,
            cb_wnd_extra: 0,
            h_instance: instance,
            h_icon: 0,
            h_cursor: LoadCursorW(0, 32512 as *const u16), // IDC_ARROW — prevents loading cursor
            hbr_background: 0,
            lpsz_menu_name: std::ptr::null(),
            lpsz_class_name: class_name.as_ptr(),
        };
        // A pre-existing class only means a previous instance registered it.
        let _ = RegisterClassW(&class);
        // No WS_EX_TRANSPARENT: cancel button must receive clicks (Handy-like).
        // WS_EX_NOACTIVATE keeps focus on the dictation target app.
        let ex_style = (WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED) as u32;
        let hwnd = CreateWindowExW(
            ex_style,
            class_name.as_ptr(),
            title.as_ptr(),
            WS_POPUP,
            -1000,
            -1000,
            OV_REST_W,
            OV_BASE_H,
            0,
            0,
            instance,
            std::ptr::null(),
        );
        if hwnd == 0 {
            eprintln!("[POPUP] FATAL: unable to create native overlay window");
            return false;
        }
        OUR_HWND = hwnd;
        refresh_widget_region(OV_REST_W, OV_BASE_H);
        SetWindowPos(
            OUR_HWND,
            HWND_TOPMOST,
            -400,
            -100,
            OV_REST_W,
            OV_BASE_H,
            SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED,
        );
        ShowWindow(OUR_HWND, SW_HIDE);
        eprintln!("[POPUP] Native Win32 overlay created (HWND={:#x})", OUR_HWND);
        true
    }
}

unsafe fn fill_native_rect(hdc: isize, rect: &RECT, color: u32) {
    let brush = CreateSolidBrush(color);
    if brush != 0 {
        FillRect(hdc, rect, brush);
        DeleteObject(brush);
    }
}

unsafe fn fill_native_ellipse(hdc: isize, left: i32, top: i32, right: i32, bottom: i32, color: u32) {
    let brush = CreateSolidBrush(color);
    let pen = CreatePen(PS_SOLID, 1, color);
    if brush != 0 && pen != 0 {
        let old_brush = SelectObject(hdc, brush);
        let old_pen = SelectObject(hdc, pen);
        Ellipse(hdc, left, top, right, bottom);
        SelectObject(hdc, old_brush);
        SelectObject(hdc, old_pen);
    }
    if brush != 0 {
        DeleteObject(brush);
    }
    if pen != 0 {
        DeleteObject(pen);
    }
}

/// Handy `.sx` cancel hit target: 22px circle on the right of the control row.
/// For the Live (open) panel the control row sits at the bottom (`OV_BASE_H`).
fn cancel_button_rect(width: i32, height: i32) -> RECT {
    let size = 22;
    let right = width - 8;
    let left = right - size;
    let row_top = (height - OV_BASE_H).max(0);
    let top = row_top + (OV_BASE_H - size) / 2;
    RECT {
        left,
        top,
        right,
        bottom: top + size,
    }
}

fn point_in_rect(x: i32, y: i32, r: &RECT) -> bool {
    x >= r.left && x < r.right && y >= r.top && y < r.bottom
}

/// Handy waveform bar height: max(3, min(18, 3 + pow(v, 0.7) * 15)).
fn handy_bar_height(level: f32) -> i32 {
    let v = level.clamp(0.0, 1.0);
    let h = 3.0 + v.powf(0.7) * 15.0;
    h.round().clamp(3.0, 18.0) as i32
}

fn resize_overlay_to(st: &PopupState) {
    let (width, height) = overlay_size_for(st);
    unsafe {
        if OUR_HWND == 0 {
            return;
        }
        let mut rect = RECT::default();
        if GetWindowRect(OUR_HWND, &mut rect) == 0 {
            return;
        }
        let cur_w = rect.right - rect.left;
        let cur_h = rect.bottom - rect.top;
        if cur_w == width && cur_h == height {
            refresh_widget_region(width, height);
            return;
        }
        // Keep bottom-center anchor while size changes (rest ↔ working ↔ Live).
        let cx = rect.left + cur_w / 2;
        let bottom = rect.bottom;
        let x = cx - width / 2;
        let y = bottom - height;
        refresh_widget_region(width, height);
        SetWindowPos(
            OUR_HWND,
            HWND_TOPMOST,
            x,
            y,
            width,
            height,
            SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW,
        );
    }
}

/// Write a filled circle directly into the ARGB pixel buffer (anti-aliased).
unsafe fn draw_aa_circle(
    pixels: *mut u32,
    w: i32,
    h: i32,
    cx: f32,
    cy: f32,
    r: f32,
    cr: u8,
    cg: u8,
    cb: u8,
    ca: u8,
) {
    let r2 = r + 1.0;
    let x0 = ((cx - r2) as i32).max(0);
    let x1 = ((cx + r2) as i32 + 1).min(w);
    let y0 = ((cy - r2) as i32).max(0);
    let y1 = ((cy + r2) as i32 + 1).min(h);
    for py in y0..y1 {
        for px in x0..x1 {
            let fx = px as f32 + 0.5;
            let fy = py as f32 + 0.5;
            let dist = ((fx - cx) * (fx - cx) + (fy - cy) * (fy - cy)).sqrt();
            let edge = (r + 0.5 - dist).clamp(0.0, 1.0);
            if edge > 0.0 {
                let a = (ca as f32 * edge) as u32;
                let pr = (cr as u32 * a) / 255;
                let pg = (cg as u32 * a) / 255;
                let pb = (cb as u32 * a) / 255;
                let new = (a << 24) | (pr << 16) | (pg << 8) | pb;
                let idx = (py * w + px) as usize;
                let old = *pixels.add(idx);
                // Alpha-over compositing (premultiplied)
                let oa = (old >> 24) & 0xFF;
                let inv = 255 - a;
                let ra = a + (oa * inv) / 255;
                let rr = pr + (((old >> 16) & 0xFF) * inv) / 255;
                let rg = pg + (((old >> 8) & 0xFF) * inv) / 255;
                let rb = pb + ((old & 0xFF) * inv) / 255;
                *pixels.add(idx) = (ra << 24) | (rr << 16) | (rg << 8) | rb;
            }
        }
    }
}

/// Write a filled rounded rectangle directly into the ARGB pixel buffer (anti-aliased).
unsafe fn draw_aa_rrect(
    pixels: *mut u32,
    w: i32,
    _h: i32,
    left: i32,
    top: i32,
    right: i32,
    bottom: i32,
    corner_r: f32,
    cr: u8,
    cg: u8,
    cb: u8,
    ca: u8,
) {
    for py in top..bottom {
        for px in left..right {
            let fx = px as f32 + 0.5;
            let fy = py as f32 + 0.5;
            // Signed distance from rounded rect
            let rw = (right - left) as f32 / 2.0;
            let rh = (bottom - top) as f32 / 2.0;
            let rcx = left as f32 + rw;
            let rcy = top as f32 + rh;
            let qx = (fx - rcx).abs() - (rw - corner_r);
            let qy = (fy - rcy).abs() - (rh - corner_r);
            let dist = if qx > 0.0 && qy > 0.0 {
                (qx * qx + qy * qy).sqrt() - corner_r
            } else {
                qx.max(qy).max(0.0) - corner_r
            };
            let edge = (-dist + 0.5).clamp(0.0, 1.0);
            if edge > 0.0 {
                let a = (ca as f32 * edge) as u32;
                let pr = (cr as u32 * a) / 255;
                let pg = (cg as u32 * a) / 255;
                let pb = (cb as u32 * a) / 255;
                let idx = (py * w + px) as usize;
                let old = *pixels.add(idx);
                let oa = (old >> 24) & 0xFF;
                let inv = 255 - a;
                let ra = a + (oa * inv) / 255;
                let rr = pr + (((old >> 16) & 0xFF) * inv) / 255;
                let rg = pg + (((old >> 8) & 0xFF) * inv) / 255;
                let rb = pb + ((old & 0xFF) * inv) / 255;
                *pixels.add(idx) = (ra << 24) | (rr << 16) | (rg << 8) | rb;
            }
        }
    }
}

/// Fix alpha channel for pixels written by GDI (TextOutW, DrawTextW, etc).
/// For white/bright GDI text drawn on a transparent DIBSection (A == 0, RGB != 0),
/// GDI writes grayscale antialiased intensity in RGB.
/// Using the peak intensity as both alpha and RGB values prevents double-multiplied dark halos.
unsafe fn fix_gdi_alpha(pixels: *mut u32, total: usize) {
    for i in 0..total {
        let px = *pixels.add(i);
        let a = (px >> 24) & 0xFF;
        let r = (px >> 16) & 0xFF;
        let g = (px >> 8) & 0xFF;
        let b = px & 0xFF;
        if a == 0 && (r != 0 || g != 0 || b != 0) {
            let max_val = r.max(g).max(b);
            *pixels.add(i) = (max_val << 24) | (max_val << 16) | (max_val << 8) | max_val;
        }
    }
}

fn get_overlay_font() -> isize {
    static FONT: std::sync::atomic::AtomicIsize = std::sync::atomic::AtomicIsize::new(0);
    let f = FONT.load(Ordering::Relaxed);
    if f != 0 {
        return f;
    }
    unsafe {
        let font_name = wide("Segoe UI");
        let hf = CreateFontW(
            -13, 0, 0, 0, 400 /* FW_NORMAL */, 0, 0, 0,
            0, 0, 0, 5 /* CLEARTYPE_QUALITY */, 0, font_name.as_ptr(),
        );
        FONT.store(hf, Ordering::Relaxed);
        hf
    }
}

fn get_bold_overlay_font() -> isize {
    static BOLD_FONT: std::sync::atomic::AtomicIsize = std::sync::atomic::AtomicIsize::new(0);
    let f = BOLD_FONT.load(Ordering::Relaxed);
    if f != 0 {
        return f;
    }
    unsafe {
        let font_name = wide("Segoe UI");
        let hf = CreateFontW(
            -13, 0, 0, 0, 700 /* FW_BOLD */, 0, 0, 0,
            0, 0, 0, 5 /* CLEARTYPE_QUALITY */, 0, font_name.as_ptr(),
        );
        BOLD_FONT.store(hf, Ordering::Relaxed);
        hf
    }
}

fn draw_native_overlay(state: &PopupState) {
    unsafe {
        if OUR_HWND == 0 {
            return;
        }
        let mut bounds = RECT::default();
        if GetClientRect(OUR_HWND, &mut bounds) == 0 {
            return;
        }
        let width = bounds.right - bounds.left;
        let height = bounds.bottom - bounds.top;
        if width <= 0 || height <= 0 {
            return;
        }

        let hdc_screen = GetDC(0);
        if hdc_screen == 0 {
            return;
        }
        let hdc_mem = CreateCompatibleDC(hdc_screen);
        if hdc_mem == 0 {
            ReleaseDC(0, hdc_screen);
            return;
        }

        let bmi = BITMAPINFO {
            bmi_header: BITMAPINFOHEADER {
                bi_size: std::mem::size_of::<BITMAPINFOHEADER>() as u32,
                bi_width: width,
                bi_height: -height,
                bi_planes: 1,
                bi_bit_count: 32,
                bi_compression: 0,
                bi_size_image: (width * height * 4) as u32,
                bi_xpels_per_meter: 0,
                bi_ypels_per_meter: 0,
                bi_clr_used: 0,
                bi_clr_important: 0,
            },
            bmi_colors: [0],
        };

        let mut bits: *mut c_void = std::ptr::null_mut();
        let hbmp = CreateDIBSection(hdc_mem, &bmi, 0, &mut bits, 0, 0);
        if hbmp == 0 || bits.is_null() {
            DeleteDC(hdc_mem);
            ReleaseDC(0, hdc_screen);
            return;
        }

        let old_bmp = SelectObject(hdc_mem, hbmp);
        let old_font = SelectObject(hdc_mem, get_overlay_font());
        let pixels = bits as *mut u32;
        let total_pixels = (width * height) as usize;

        // Clear to transparent
        std::ptr::write_bytes(pixels, 0, total_pixels);

        // ── Background: anti-aliased capsule (#181820 at 95% alpha) ──
        // Skip for Idle state — it draws its own custom shapes with different dimensions
        if state.state != OverlayState::Idle {
            let radius = (OV_BASE_H / 2) as f32;
            for y in 0..height {
                for x in 0..width {
                    let fx = x as f32 + 0.5;
                    let fy = y as f32 + 0.5;
                    let (cx, cy_ref) = if height > OV_BASE_H {
                        let r = 16.0f32;
                        (fx.clamp(r, width as f32 - r), fy.clamp(r, height as f32 - r))
                    } else {
                        (fx.clamp(radius, width as f32 - radius), fy.clamp(radius, height as f32 - radius))
                    };
                    let dist = ((fx - cx) * (fx - cx) + (fy - cy_ref) * (fy - cy_ref)).sqrt();
                    let max_r = if height > OV_BASE_H { 16.0f32 } else { radius };
                    let af = (max_r + 0.5 - dist).clamp(0.0, 1.0);
                    if af > 0.0 {
                        let is_border = dist >= (max_r - 1.0) && dist <= max_r;
                        let (r, g, b, base_a) = if is_border {
                            (200u8, 200u8, 205u8, 0.15f32)
                        } else {
                            (0u8, 0u8, 0u8, 0.80f32)
                        };
                        let fa = (base_a * af * 255.0) as u32;
                        let pr = (r as u32 * fa) / 255;
                        let pg = (g as u32 * fa) / 255;
                        let pb = (b as u32 * fa) / 255;
                        *pixels.add((y * width + x) as usize) = (fa << 24) | (pr << 16) | (pg << 8) | pb;
                    }
                }
            }
        }

        // ── Layout ──
        let live = has_live_text(state);
        let row_top = if live && height > OV_BASE_H { height - OV_BASE_H } else { 0 };
        let cy = row_top + OV_BASE_H / 2;
        let cancel = cancel_button_rect(width, height);

        // ── Live text panel ──
        if live {
            let mut display = state.live_committed.clone();
            if !state.live_tentative.is_empty() {
                if !display.is_empty()
                    && !display.chars().last().map(char::is_whitespace).unwrap_or(false)
                {
                    display.push(' ');
                }
                display.push_str(&state.live_tentative);
            }

            // Draw dark rounded container box behind text board
            draw_aa_rrect(
                pixels, width, height,
                10, 8, width - 10, row_top - 4,
                8.0, 20, 20, 28, 220,
            );

            SetBkMode(hdc_mem, TRANSPARENT);
            SetTextColor(hdc_mem, colorref(240, 240, 245));
            let label = wide(&display);
            let mut text_rect = RECT {
                left: 18,
                top: 14,
                right: width - 18,
                bottom: row_top - 8,
            };
            DrawTextW(
                hdc_mem,
                label.as_ptr(),
                label.len().saturating_sub(1) as i32,
                &mut text_rect,
                DT_WORDBREAK | DT_LEFT | DT_NOPREFIX,
            );
            fix_gdi_alpha(pixels, total_pixels);

            // Copy button hint (top-right of text container)
            if !display.is_empty() {
                let btn_size = 16;
                let btn_x = width - 28;
                let btn_y = 12;
                // Small clipboard icon (two overlapping squares)
                draw_aa_rrect(pixels, width, height, btn_x + 2, btn_y, btn_x + btn_size - 2, btn_y + btn_size - 4, 2.0, 160, 160, 170, 180);
                draw_aa_rrect(pixels, width, height, btn_x, btn_y + 3, btn_x + btn_size - 4, btn_y + btn_size - 1, 2.0, 200, 200, 210, 200);
            }
        }

        // ── State-specific drawing ──
        match state.state {
            OverlayState::Idle => {
                if state.context_menu_open {
                    // ── Dark Context Menu ──
                    draw_aa_rrect(
                        pixels, width, height,
                        0, 0, width, height,
                        12.0, 18, 18, 26, 250,
                    );
                    
                    // Subtle header
                    SetBkMode(hdc_mem, TRANSPARENT);
                    SetTextColor(hdc_mem, colorref(100, 100, 115));
                    let header = wide("QuickSTT");
                    TextOutW(hdc_mem, 14, 8, header.as_ptr(), header.len().saturating_sub(1) as i32);

                    // Menu items list
                    let items = [
                        "  ⏰  Hide for 1 hour",
                        "  ⚙️  Settings",
                        "  🎤  Microphone                 >",
                        "  🗣️  Languages                  >",
                        "  📁  Transcript history",
                        "  📋  Paste last transcript",
                    ];
                    SetBkMode(hdc_mem, TRANSPARENT);
                    for (i, label_str) in items.iter().enumerate() {
                        let item_y = MENU_PADDING + i as i32 * MENU_ITEM_H;
                        
                        if i == 2 || i == 4 {
                            draw_aa_rrect(pixels, width, height, 8, item_y - MENU_ITEM_H / 2, width - 8, item_y - MENU_ITEM_H / 2 + 1, 0.0, 60, 60, 70, 80);
                        }

                        if state.context_menu_hover == i as i32 {
                            draw_aa_rrect(
                                pixels, width, height,
                                6, item_y, width - 6, item_y + MENU_ITEM_H - 2,
                                8.0, 40, 40, 55, 200,
                            );
                            SetTextColor(hdc_mem, colorref(255, 255, 255));
                        } else {
                            SetTextColor(hdc_mem, colorref(225, 225, 235));
                        }
                        let label = wide(label_str);
                        TextOutW(
                            hdc_mem,
                            14,
                            item_y + 8,
                            label.as_ptr(),
                            label.len().saturating_sub(1) as i32,
                        );
                    }
                } else if state.morph_progress > 0.05 {
                    // ── Morphed Dictate Pill (Matches morphed_pill.png) ──
                    let main_w = 160;
                    let main_h = 34;
                    let main_x = (width - main_w) / 2;
                    let main_y = 0;
                    // Outer capsule body (#0C0C12 fill, #222230 border)
                    draw_aa_rrect(
                        pixels, width, height,
                        main_x, main_y, main_x + main_w, main_y + main_h,
                        17.0, 10, 10, 12, 230,
                    );
                    // Text: "Dictate " (regular white) + "Ctrl + Space" (bold white)
                    SetBkMode(hdc_mem, TRANSPARENT);
                    SetTextColor(hdc_mem, colorref(220, 220, 230));
                    let t1 = wide("Dictate ");
                    TextOutW(hdc_mem, main_x + 22, main_y + 8, t1.as_ptr(), t1.len().saturating_sub(1) as i32);
                    
                    SelectObject(hdc_mem, get_bold_overlay_font());
                    SetTextColor(hdc_mem, colorref(255, 255, 255));
                    let t2 = wide("Ctrl + Space");
                    TextOutW(hdc_mem, main_x + 68, main_y + 8, t2.as_ptr(), t2.len().saturating_sub(1) as i32);
                    SelectObject(hdc_mem, get_overlay_font());

                    // Hanging Mic Circle Button (#14141E fill, #3898FF glow border)
                    let mic_r = 12.0f32;
                    let mic_cx = (width / 2) as f32;
                    let mic_cy = (main_y + main_h + 14) as f32;
                    draw_aa_circle(pixels, width, height, mic_cx, mic_cy, mic_r, 20, 20, 30, 240);
                    draw_aa_circle(pixels, width, height, mic_cx, mic_cy, mic_r - 1.0, 56, 152, 255, 180);
                    draw_aa_circle(pixels, width, height, mic_cx, mic_cy, mic_r - 2.5, 20, 20, 30, 255);
                    // Crisp white microphone vector icon inside circle
                    let mx = mic_cx;
                    let my = mic_cy;
                    draw_aa_rrect(pixels, width, height, (mx - 2.0) as i32, (my - 5.0) as i32, (mx + 2.0) as i32, (my + 2.0) as i32, 2.0, 240, 240, 250, 255);
                    draw_aa_line(pixels, width, height, mx - 4.0, my, mx - 4.0, my + 3.0, 240, 240, 250, 255, 1.2);
                    draw_aa_line(pixels, width, height, mx + 4.0, my, mx + 4.0, my + 3.0, 240, 240, 250, 255, 1.2);
                    draw_aa_line(pixels, width, height, mx - 4.0, my + 3.0, mx + 4.0, my + 3.0, 240, 240, 250, 255, 1.2);
                    draw_aa_line(pixels, width, height, mx, my + 3.0, mx, my + 6.0, 240, 240, 250, 255, 1.2);
                } else {
                    // ── Always-on Micro Pill Handle ──
                    // Fill entire window with alpha=1 (invisible but receives mouse events)
                    for py in 0..height {
                        for px in 0..width {
                            *pixels.add((py * width + px) as usize) = 0x01000000; // alpha=1, rgb=0
                        }
                    }
                    // Draw the visible micro pill bar
                    let pill_x = (width - IDLE_PILL_W) / 2;
                    let pill_y = (height - IDLE_PILL_H) / 2;
                    draw_aa_rrect(
                        pixels, width, height,
                        pill_x, pill_y, pill_x + IDLE_PILL_W, pill_y + IDLE_PILL_H,
                        3.0, 8, 8, 10, 245,
                    );
                }
            }
            OverlayState::Recording if state.no_signal_shown => {
                // ── "Is your microphone muted?" Notification Card ──
                draw_aa_rrect(
                    pixels, width, height,
                    0, 0, width, height,
                    14.0, 20, 20, 30, 248,
                );
                // Warning icon & Title
                SetBkMode(hdc_mem, TRANSPARENT);
                SetTextColor(hdc_mem, colorref(255, 200, 80));
                let title = wide("(!) Is your microphone muted?");
                TextOutW(hdc_mem, 16, 12, title.as_ptr(), title.len().saturating_sub(1) as i32);
                SetTextColor(hdc_mem, colorref(170, 170, 185));
                let body = wide("We didn't pick up any audio from Auto-detect");
                TextOutW(hdc_mem, 16, 34, body.as_ptr(), body.len().saturating_sub(1) as i32);

                // Action buttons: [ Select microphone ]  [ Troubleshoot ]
                draw_aa_rrect(pixels, width, height, 16, 56, 140, 80, 8.0, 45, 45, 58, 200);
                SetTextColor(hdc_mem, colorref(230, 230, 240));
                let btn1 = wide("Select microphone");
                TextOutW(hdc_mem, 24, 61, btn1.as_ptr(), btn1.len().saturating_sub(1) as i32);

                draw_aa_rrect(pixels, width, height, 148, 56, 240, 80, 8.0, 45, 45, 58, 200);
                let btn2 = wide("Troubleshoot");
                TextOutW(hdc_mem, 160, 61, btn2.as_ptr(), btn2.len().saturating_sub(1) as i32);

                // Auto-dismiss progress bar
                if let Some(since) = state.no_signal_since {
                    let elapsed = since.elapsed().as_secs_f32();
                    let progress = 1.0 - (elapsed / 8.0).min(1.0);
                    let bar_w = (width as f32 * progress) as i32;
                    draw_aa_rrect(pixels, width, height, 0, height - 3, bar_w, height, 0.0, 56, 152, 255, 180);
                }
            }
            OverlayState::Recording => {
                // Pulsing mic dot
                let t = APP_START.elapsed().as_millis() as f32;
                let pulse = ((t / 950.0) * std::f32::consts::PI).sin().abs();
                let dot_cx = 18.0;
                let dot_cy = cy as f32;
                // Outer glow
                draw_aa_circle(
                    pixels, width, height, dot_cx, dot_cy, 7.0,
                    (56.0 + 40.0 * pulse) as u8, (152.0 + 30.0 * pulse) as u8, 255, 180,
                );
                // Inner solid dot
                draw_aa_circle(pixels, width, height, dot_cx, dot_cy, 4.0, 56, 152, 255, 255);

                // Waveform bars
                let bar_w = 6;
                let gap = 3;
                let total_w = WAVE_BARS as i32 * bar_w + (WAVE_BARS as i32 - 1) * gap;
                let start_x = (width - total_w) / 2;
                for i in 0..WAVE_BARS {
                    let level = state.smoothed_levels.get(i).copied().unwrap_or(0.0).clamp(0.0, 1.0);
                    let bar_h = handy_bar_height(level);
                    let x = start_x + i as i32 * (bar_w + gap);
                    let y = cy - bar_h / 2;
                    let opacity = (0.3 + level * 0.7).min(1.0);
                    draw_aa_rrect(
                        pixels, width, height,
                        x, y, x + bar_w, y + bar_h, 2.0,
                        56, 152, 255, (opacity * 255.0) as u8,
                    );
                }
            }
            OverlayState::Transcribing => {
                // Modern Siri-style fluid glowing gradient pulse bar
                let t = APP_START.elapsed().as_millis() as f32;
                let phase = t * 0.005;
                let bar_total_w = 120;
                let bar_start_x = (width - bar_total_w) / 2;
                let bar_y = cy - 3;
                let bar_h = 6;
                // Outer glowing pill
                draw_aa_rrect(
                    pixels, width, height,
                    bar_start_x - 4, bar_y - 2, bar_start_x + bar_total_w + 4, bar_y + bar_h + 2,
                    5.0, 56, 152, 255, 60,
                );
                // Animated gradient segments inside pulse bar
                for px in 0..bar_total_w {
                    let fx = px as f32 / bar_total_w as f32;
                    let wave = ((fx * 4.0 * std::f32::consts::PI + phase).sin() * 0.5 + 0.5).clamp(0.0, 1.0);
                    let r = (56.0 + wave * 180.0) as u8;
                    let g = (152.0 - wave * 50.0) as u8;
                    let b = 255u8;
                    let x = bar_start_x + px;
                    draw_aa_rrect(
                        pixels, width, height,
                        x, bar_y, x + 1, bar_y + bar_h, 1.0,
                        r, g, b, 240,
                    );
                }
                // "Transcribing…" text
                SetBkMode(hdc_mem, TRANSPARENT);
                SetTextColor(hdc_mem, colorref(200, 200, 210));
                let label = wide("Transcribing…");
                TextOutW(hdc_mem, width / 2 - 42, cy + 8, label.as_ptr(), label.len().saturating_sub(1) as i32);
            }
            OverlayState::Done => {
                SetBkMode(hdc_mem, TRANSPARENT);
                SetTextColor(hdc_mem, colorref(180, 180, 185));
                let label = wide("Done");
                TextOutW(hdc_mem, width / 2 - 14, cy - 7, label.as_ptr(), label.len().saturating_sub(1) as i32);
            }
            OverlayState::Hidden => {}
        }

        // ── Cancel button (× circle) ──
        if state.state != OverlayState::Hidden && state.state != OverlayState::Idle {
            let ccx = (cancel.left + cancel.right) as f32 / 2.0;
            let ccy = (cancel.top + cancel.bottom) as f32 / 2.0;
            let cr = (cancel.right - cancel.left) as f32 / 2.0;
            // Circle background
            draw_aa_circle(pixels, width, height, ccx, ccy, cr, 50, 50, 55, 220);
            // × cross lines drawn directly into pixels
            let pad = 6.0;
            let x0 = cancel.left as f32 + pad;
            let y0 = cancel.top as f32 + pad;
            let x1 = cancel.right as f32 - pad;
            let y1 = cancel.bottom as f32 - pad;
            draw_aa_line(pixels, width, height, x0, y0, x1, y1, 180, 180, 185, 240, 1.5);
            draw_aa_line(pixels, width, height, x1, y0, x0, y1, 180, 180, 185, 240, 1.5);
        }

        // ── Fix alpha for any GDI-drawn pixels (text) ──
        fix_gdi_alpha(pixels, total_pixels);

        // ── Composite via UpdateLayeredWindow ──
        let mut window_rect = RECT::default();
        GetWindowRect(OUR_HWND, &mut window_rect);
        let pt_dst = POINT { x: window_rect.left, y: window_rect.top };
        let pt_src = POINT { x: 0, y: 0 };
        let size = POINT { x: width, y: height };
        let blend = BLENDFUNCTION {
            blend_op: AC_SRC_OVER,
            blend_flags: 0,
            source_constant_alpha: 255,
            alpha_format: AC_SRC_ALPHA,
        };
        UpdateLayeredWindow(OUR_HWND, hdc_screen, &pt_dst, &size, hdc_mem, &pt_src, 0, &blend, ULW_ALPHA);

        SelectObject(hdc_mem, old_bmp);
        DeleteObject(hbmp);
        DeleteDC(hdc_mem);
        ReleaseDC(0, hdc_screen);
    }
}

/// Anti-aliased line drawn directly into the ARGB pixel buffer.
unsafe fn draw_aa_line(
    pixels: *mut u32,
    w: i32,
    h: i32,
    x0: f32,
    y0: f32,
    x1: f32,
    y1: f32,
    cr: u8,
    cg: u8,
    cb: u8,
    ca: u8,
    thickness: f32,
) {
    let dx = x1 - x0;
    let dy = y1 - y0;
    let len = (dx * dx + dy * dy).sqrt();
    if len < 0.01 {
        return;
    }
    let nx = -dy / len;
    let ny = dx / len;
    let half_t = thickness / 2.0 + 0.5;
    let min_x = (x0.min(x1) - half_t - 1.0).max(0.0) as i32;
    let max_x = (x0.max(x1) + half_t + 1.0).min(w as f32) as i32;
    let min_y = (y0.min(y1) - half_t - 1.0).max(0.0) as i32;
    let max_y = (y0.max(y1) + half_t + 1.0).min(h as f32) as i32;
    for py in min_y..max_y {
        for px in min_x..max_x {
            let fx = px as f32 + 0.5;
            let fy = py as f32 + 0.5;
            // Distance to line segment
            let t_param = ((fx - x0) * dx + (fy - y0) * dy) / (len * len);
            let t_clamped = t_param.clamp(0.0, 1.0);
            let cx = x0 + t_clamped * dx;
            let cy_val = y0 + t_clamped * dy;
            let dist = ((fx - cx) * (fx - cx) + (fy - cy_val) * (fy - cy_val)).sqrt();
            let edge = (thickness / 2.0 + 0.5 - dist).clamp(0.0, 1.0);
            if edge > 0.0 {
                let a = (ca as f32 * edge) as u32;
                let pr = (cr as u32 * a) / 255;
                let pg = (cg as u32 * a) / 255;
                let pb = (cb as u32 * a) / 255;
                let idx = (py * w + px) as usize;
                let old = *pixels.add(idx);
                let oa = (old >> 24) & 0xFF;
                let inv = 255 - a;
                let ra = a + (oa * inv) / 255;
                let rr = pr + (((old >> 16) & 0xFF) * inv) / 255;
                let rg = pg + (((old >> 8) & 0xFF) * inv) / 255;
                let rb = pb + ((old & 0xFF) * inv) / 255;
                *pixels.add(idx) = (ra << 24) | (rr << 16) | (rg << 8) | rb;
            }
        }
    }
}

const WM_LBUTTONDOWN: u32 = 0x0201;
const WM_LBUTTONUP: u32 = 0x0202;
const WM_LBUTTONDBLCLK: u32 = 0x0203;

fn run_native_overlay(
    state: Arc<Mutex<PopupState>>,
    hotkey_rx: mpsc::Receiver<HotkeyEvent>,
) {
    // Shared TCP handles for cancel clicks from the paint loop's message pump.
    // Re-resolve via a side channel: hotkey thread owns the writer; we stash a
    // clone path through the same Arc used by main().
    // run_native_overlay is only called from main after tcp_writer is created —
    // pass cancel via a static-free approach: store weak refs on a pair we
    // re-read from the hotkey path is awkward. Instead, embed cancel into the
    // message loop using a channel the hotkey thread also watches.
    // Simpler: hold tcp arcs in a OnceLock set from main before this call.
    let tcp_writer = CANCEL_TCP_WRITER
        .get()
        .expect("CANCEL_TCP_WRITER must be set")
        .clone();
    let tcp_connected = CANCEL_TCP_CONNECTED
        .get()
        .expect("CANCEL_TCP_CONNECTED must be set")
        .clone();

    if !create_native_overlay_window() {
        return;
    }
    let mut was_visible = false;
    let mut last_size_key = (OverlayState::Hidden, false);
    loop {
        unsafe {
            let mut message = NATIVE_MSG::default();
            while PeekMessageW(&mut message, 0, 0, 0, PM_REMOVE) != 0 {
                if message.message == WM_QUIT {
                    return;
                }
                if message.message == WM_MOUSEMOVE && message.hwnd == OUR_HWND {
                    let y = ((message.l_param >> 16) & 0xFFFF) as i16 as i32;
                    let mut st = state.lock().unwrap();
                    if st.dragging {
                        let mut pt = POINT { x: 0, y: 0 };
                        GetCursorPos(&mut pt);
                        let sw = GetSystemMetrics(0); // SM_CXSCREEN
                        let sh = GetSystemMetrics(1); // SM_CYSCREEN
                        
                        let d_left = pt.x;
                        let d_right = sw - pt.x;
                        let d_bottom = sh - pt.y;
                        
                        st.drag_preview_dock = if d_bottom < d_left && d_bottom < d_right {
                            0
                        } else if d_left < d_right {
                            1
                        } else {
                            2
                        };
                    } else if st.state == OverlayState::Idle {
                        st.hover_active = true;
                        st.hover_leave_time = None;
                        if st.context_menu_open {
                            let hover_idx = (y - MENU_PADDING) / MENU_ITEM_H;
                            if hover_idx >= 0 && hover_idx < MENU_ITEMS as i32 {
                                st.context_menu_hover = hover_idx;
                            } else {
                                st.context_menu_hover = -1;
                            }
                        }
                        if !st.mouse_tracking_set {
                            st.mouse_tracking_set = true;
                            let mut tme = TRACKMOUSEEVENT {
                                cb_size: std::mem::size_of::<TRACKMOUSEEVENT>() as u32,
                                dw_flags: TME_LEAVE,
                                hwnd_track: OUR_HWND,
                                dw_hover_time: 0,
                            };
                            TrackMouseEvent(&mut tme);
                        }
                    }
                }
                if message.message == WM_MOUSELEAVE && message.hwnd == OUR_HWND {
                    let mut st = state.lock().unwrap();
                    st.hover_active = false;
                    st.mouse_tracking_set = false;
                    st.hover_leave_time = Some(Instant::now());
                    st.context_menu_hover = -1;
                    // Auto-dismiss context menu when mouse leaves widget
                    if st.context_menu_open {
                        st.context_menu_open = false;
                    }
                }
                if message.message == WM_RBUTTONDOWN && message.hwnd == OUR_HWND {
                    let mut st = state.lock().unwrap();
                    if st.state == OverlayState::Idle {
                        st.context_menu_open = !st.context_menu_open;
                        st.morph_target = 1.0;
                    }
                }
                if message.message == WM_LBUTTONDBLCLK && message.hwnd == OUR_HWND {
                    let mut st = state.lock().unwrap();
                    if st.state == OverlayState::Idle && st.morph_progress > 0.05 {
                        st.dragging = true;
                        SetCapture(OUR_HWND);
                        let mut pt = POINT { x: 0, y: 0 };
                        GetCursorPos(&mut pt);
                        st.drag_start_x = pt.x;
                        st.drag_start_y = pt.y;
                    }
                }
                if message.message == WM_LBUTTONUP && message.hwnd == OUR_HWND {
                    let mut st = state.lock().unwrap();
                    if st.dragging {
                        ReleaseCapture();
                        if st.drag_preview_dock >= 0 {
                            st.dock_side = st.drag_preview_dock as u8;
                        }
                        st.dragging = false;
                        st.drag_preview_dock = -1;
                    }
                }
                if message.message == WM_LBUTTONDOWN && message.hwnd == OUR_HWND {
                    let y = ((message.l_param >> 16) & 0xFFFF) as i16 as i32;
                    let mut st = state.lock().unwrap();
                    if st.state == OverlayState::Idle {
                        if st.context_menu_open {
                            let click_idx = (y - MENU_PADDING) / MENU_ITEM_H;
                            st.context_menu_open = false;
                            match click_idx {
                                0 => {
                                    // Hide for 1 hour
                                    st.state = OverlayState::Hidden;
                                    st.hidden_until = Some(Instant::now() + Duration::from_secs(3600));
                                }
                                1 => {
                                    // Settings -> launch QuickSTT_App.exe
                                    std::thread::spawn(|| {
                                        let _ = std::process::Command::new("QuickSTT_App.exe").spawn();
                                    });
                                }
                                4 => {
                                    // Transcript history -> open current directory / mp3
                                    std::thread::spawn(|| {
                                        let _ = std::process::Command::new("explorer.exe").arg(".").spawn();
                                    });
                                }
                                5 => {
                                    // Paste last transcript
                                    let text = st.captured_text.clone();
                                    if !text.trim().is_empty() {
                                        std::thread::spawn(move || typing::type_text(&text));
                                    }
                                }
                                _ => {}
                            }
                        } else if st.morph_progress > 0.1 {
                            // Clicked on morphed dictation pill -> start dictation!
                            drop(st);
                            let sent = send_popup_command(&tcp_writer, &tcp_connected, b"{\"cmd\":\"popup_start\"}\n");
                            if sent {
                                let mut st = state.lock().unwrap();
                                begin_recording_session(&mut st);
                            }
                        }
                    } else {
                        let (w, h) = widget_size().unwrap_or((OV_REST_W, OV_BASE_H));
                        let cancel = cancel_button_rect(w, h);
                        if point_in_rect((message.l_param & 0xFFFF) as i16 as i32, y, &cancel) {
                            drop(st);
                            cancel_session(&state, &tcp_writer, &tcp_connected);
                        } else if h > OV_BASE_H && y < (h - OV_BASE_H) {
                            let mut text = st.live_committed.clone();
                            if !st.live_tentative.is_empty() {
                                if !text.is_empty() { text.push(' '); }
                                text.push_str(&st.live_tentative);
                            }
                            drop(st);
                            if !text.trim().is_empty() {
                                std::thread::spawn(move || typing::copy_only(&text));
                            }
                        }
                    }
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        let mut force_show = false;
        while let Ok(event) = hotkey_rx.try_recv() {
            if event == HotkeyEvent::Press {
                force_show = true;
            }
        }

        let mut timeout_commit = None;
        {
            let mut current = state.lock().unwrap();
            // Smooth morph animation tick
            let now = Instant::now();
            let dt = now.duration_since(current.morph_last_tick).as_secs_f32() * 1000.0;
            current.morph_last_tick = now;

            // Handle 1 hour hide timer expiry
            if let Some(until) = current.hidden_until {
                if now >= until {
                    current.hidden_until = None;
                    if current.always_on_pill && current.state == OverlayState::Hidden {
                        current.state = OverlayState::Idle;
                    }
                }
            }

            if current.state == OverlayState::Idle {
                if let Some(leave_t) = current.hover_leave_time {
                    if now.duration_since(leave_t) > Duration::from_millis(HOVER_LEAVE_DELAY_MS) {
                        current.hover_active = false;
                        current.hover_leave_time = None;
                    }
                }
                current.morph_target = if current.hover_active || current.context_menu_open { 1.0 } else { 0.0 };
                let step = dt / MORPH_DURATION_MS;
                if current.morph_progress < current.morph_target {
                    current.morph_progress = (current.morph_progress + step).min(current.morph_target);
                } else if current.morph_progress > current.morph_target {
                    current.morph_progress = (current.morph_progress - step).max(current.morph_target);
                }
            }

            // No signal / quiet microphone detection
            if current.state == OverlayState::Recording && !current.no_signal_shown {
                if let Some(started) = current.session_started {
                    if now.duration_since(started) > Duration::from_secs(NO_SIGNAL_DELAY_SECS)
                        && now.duration_since(current.last_voice_time) > Duration::from_secs(NO_SIGNAL_DELAY_SECS)
                    {
                        current.no_signal_shown = true;
                        current.no_signal_since = Some(now);
                    }
                }
            }

            if current.state == OverlayState::Done {
                if let Some(deadline) = current.show_until {
                    if Instant::now() > deadline {
                        hide_session(&mut current);
                    }
                }
            }
            if current.state == OverlayState::Transcribing {
                if let Some(since) = current.transcribing_since {
                    if Instant::now().duration_since(since) > Duration::from_secs(12) {
                        timeout_commit = take_release_commit(&mut current);
                        if timeout_commit.is_none() {
                            hide_session(&mut current);
                        }
                    }
                }
            }
        }
        if let Some((text, output_mode)) = timeout_commit {
            commit_text(text, output_mode);
        }

        let (visible, cur_state, live, morphing) = {
            let st = state.lock().unwrap();
            let is_morphing = st.state == OverlayState::Idle
                && (st.morph_progress - st.morph_target).abs() > 0.01;
            (
                st.state != OverlayState::Hidden,
                st.state,
                has_live_text(&st),
                is_morphing,
            )
        };
        // During morph animation, force resize every frame
        let size_key = (cur_state, live);
        if visible && (size_key != last_size_key || morphing) {
            let st = state.lock().unwrap();
            resize_overlay_to(&st);
            last_size_key = size_key;
        }
        if !visible {
            last_size_key = (OverlayState::Hidden, false);
        }

        if visible != was_visible || (visible && force_show) {
            was_visible = visible;
            if visible {
                let st = state.lock().unwrap();
                resize_overlay_to(&st);
                let side = st.dock_side;
                drop(st);
                move_to_dock_position(side);
                eprintln!(
                    "[POPUP] Native overlay shown ({:?} live={})",
                    cur_state, live
                );
            } else {
                unsafe {
                    ShowWindow(OUR_HWND, SW_HIDE);
                }
                move_off_screen();
                eprintln!("[POPUP] Native overlay hidden");
            }
        }
        if visible {
            let current = state.lock().unwrap();
            draw_native_overlay(&current);
        }
        std::thread::sleep(Duration::from_millis(16));
    }
}

static CANCEL_TCP_WRITER: std::sync::OnceLock<Arc<Mutex<Option<std::net::TcpStream>>>> =
    std::sync::OnceLock::new();
static CANCEL_TCP_CONNECTED: std::sync::OnceLock<Arc<AtomicBool>> = std::sync::OnceLock::new();

// ─── egui App ───────────────────────────────────────────────────────────────

struct PopupApp {
    state: Arc<Mutex<PopupState>>,
    hotkey_rx: mpsc::Receiver<HotkeyEvent>,
    smoothed_levels: Vec<f32>,
    initialized: bool,
    was_visible: bool,
}

impl eframe::App for PopupApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        if !self.initialized {
            self.initialized = true;
            apply_widget_style();
            eprintln!("[POPUP] egui initialized");
        }

        // A press is also a native visibility request. Repositioning on every
        // press repairs an overlay that may have remained off-screen after a
        // prior timed-out transcription.
        let mut force_show = false;
        while let Ok(event) = self.hotkey_rx.try_recv() {
            if event == HotkeyEvent::Press {
                force_show = true;
            }
        }

        // Auto-hide logic
        {
            let mut st = self.state.lock().unwrap();
            if st.state == OverlayState::Done {
                if let Some(dl) = st.show_until {
                    if Instant::now() > dl {
                        st.state = OverlayState::Hidden;
                    }
                }
            }
            if st.state == OverlayState::Transcribing {
                if let Some(since) = st.transcribing_since {
                    // The shared Parakeet worker has a 10s inference timeout;
                    // do not hide the overlay before that backend result can
                    // be forwarded to it.
                    if Instant::now().duration_since(since) > Duration::from_secs(12) {
                        st.state = OverlayState::Hidden;
                    }
                }
            }
        }

        let should_be_visible = { self.state.lock().unwrap().state != OverlayState::Hidden };

        // Show/hide by moving on/off screen via Win32
        if should_be_visible != self.was_visible || (should_be_visible && force_show) {
            self.was_visible = should_be_visible;
            if should_be_visible {
                let side = self.state.lock().unwrap().dock_side;
                move_to_dock_position(side);
                eprintln!("[POPUP] SHOW at bottom-center");
            } else {
                move_off_screen();
                eprintln!("[POPUP] HIDE off-screen");
            }
        }

        ctx.request_repaint_after(Duration::from_millis(16));

        if !should_be_visible {
            return;
        }

        // Draw solid widget — Handy-style capsule with bright blue accent
        let st = self.state.lock().unwrap();
        egui::CentralPanel::default()
            .frame(
                egui::Frame::default()
                    .fill(egui::Color32::from_rgb(24, 24, 28))
                    .inner_margin(0.0),
            )
            .show(ctx, |ui| {
                let rect = ui.max_rect();
                let p = ui.painter();
                let cy = rect.center().y;

                // Capsule background (Handy-style: dark, fully rounded)
                let capsule = rect.shrink(2.0);
                p.rect_filled(
                    capsule,
                    egui::CornerRadius::same((capsule.height() / 2.0) as u8),
                    egui::Color32::from_rgb(24, 24, 28),
                );

                match st.state {
                    OverlayState::Recording => {
                        // Blue mic dot (left side)
                        let mic_x = capsule.left() + 16.0;
                        let blue = egui::Color32::from_rgb(56, 152, 255);
                        let blue_bright = egui::Color32::from_rgb(90, 180, 255);
                        p.circle_filled(egui::pos2(mic_x, cy), 4.0, blue);
                        p.circle_stroke(
                            egui::pos2(mic_x, cy),
                            6.0,
                            egui::Stroke::new(1.5, blue_bright),
                        );

                        // Waveform bars (center-right)
                        let bar_start = mic_x + 16.0;
                        let bar_count = 12;
                        let bar_spacing = 5.5;
                        for i in 0..bar_count {
                            let lvl = self.smoothed_levels.get(i % 9).copied().unwrap_or(0.0);
                            let x = bar_start + i as f32 * bar_spacing;
                            let h = 3.0 + lvl * 16.0;
                            let bar_rect =
                                egui::Rect::from_center_size(egui::pos2(x, cy), egui::vec2(3.0, h));
                            let alpha = 0.5 + lvl * 0.5;
                            p.rect_filled(
                                bar_rect,
                                egui::CornerRadius::same(2),
                                egui::Color32::from_rgba_premultiplied(
                                    (90.0 * alpha) as u8,
                                    (180.0 * alpha) as u8,
                                    255,
                                    255,
                                ),
                            );
                        }
                    }
                    OverlayState::Transcribing => {
                        let t = APP_START.elapsed().as_millis() as f32;
                        let pulse = (t / 400.0 * std::f32::consts::PI).sin().abs() * 0.4 + 0.6;
                        let blue = egui::Color32::from_rgb(
                            (56.0 * pulse) as u8,
                            (152.0 * pulse) as u8,
                            (255.0 * pulse) as u8,
                        );
                        p.text(
                            egui::pos2(capsule.center().x, cy),
                            egui::Align2::CENTER_CENTER,
                            "Transcribing...",
                            egui::FontId::proportional(11.0),
                            blue,
                        );
                    }
                    OverlayState::Done => {}
                    OverlayState::Hidden => {}
                    OverlayState::Idle => {}
                }
            });

        // Smooth audio
        for i in 0..9 {
            let t = st.audio_levels.get(i).copied().unwrap_or(0.0);
            self.smoothed_levels[i] = self.smoothed_levels[i] * 0.6 + t * 0.4;
        }
    }
}

