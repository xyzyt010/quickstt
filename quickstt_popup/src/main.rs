//! QuickSTT Popup — Solid overlay widget using native Win32 layered window
//! Hold Ctrl+Space = show + record, Release = stop + 500ms trailing audio drain + type + hide.
//! Features:
//! - Full 3-side drag-and-drop docking system with fullscreen translucent preview overlay and glowing snap targets
//! - Persistent context menu with Language selection, Microphone status, Settings, History, and Paste
//! - 500ms trailing audio capture buffer on PTT release so words are never cut off
//! - High-precision snug text capsule, authentic #0071bc mic icon, and solid micro-pill handle
//! - 100% crash-free bounded GDI alpha rendering

mod hotkey;
mod typing;

use std::ffi::c_void;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, Mutex, OnceLock};
use std::time::{Duration, Instant};

use hotkey::HotkeyEvent;

// Global app start time for animation timing
static APP_START: std::sync::LazyLock<Instant> = std::sync::LazyLock::new(Instant::now);

// ─── Win32 API ──────────────────────────────────────────────────────────────

#[link(name = "user32")]
extern "system" {
    fn GetWindowRect(hWnd: isize, lpRect: *mut RECT) -> i32;
    fn GetCursorPos(lpPoint: *mut POINT) -> i32;
    fn GetForegroundWindow() -> isize;
    fn MonitorFromPoint(pt: POINT, dwFlags: u32) -> isize;
    fn MonitorFromWindow(hWnd: isize, dwFlags: u32) -> isize;
    fn GetMonitorInfoW(hMonitor: isize, lpmi: *mut MONITORINFO) -> i32;
    fn ShowWindow(hWnd: isize, nCmdShow: i32) -> i32;
    fn SetWindowPos(
        hWnd: isize,
        hWndInsertAfter: isize,
        x: i32,
        y: i32,
        cx: i32,
        cy: i32,
        uFlags: u32,
    ) -> i32;
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
    fn GetClientRect(hWnd: isize, lpRect: *mut RECT) -> i32;
    fn GetDC(hWnd: isize) -> isize;
    fn ReleaseDC(hWnd: isize, hDC: isize) -> i32;
    fn SetCapture(hWnd: isize) -> isize;
    fn ReleaseCapture() -> i32;
    fn GetSystemMetrics(nIndex: i32) -> i32;
    fn LoadCursorW(hInstance: isize, lpCursorName: *const u16) -> isize;
    fn SetCursor(hCursor: isize) -> isize;
    fn TrackMouseEvent(lpEventTrack: *mut TRACKMOUSEEVENT) -> i32;
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
    fn SelectObject(hdc: isize, h: isize) -> isize;
    fn DeleteObject(ho: isize) -> i32;
    fn SetBkMode(hdc: isize, mode: i32) -> i32;
    fn SetTextColor(hdc: isize, color: u32) -> u32;
    fn TextOutW(hdc: isize, x: i32, y: i32, text: *const u16, length: i32) -> i32;
    fn DrawTextW(
        hdc: isize,
        lpchText: *const u16,
        cchText: i32,
        lprc: *mut RECT,
        format: u32,
    ) -> i32;
    fn GetTextExtentPoint32W(hdc: isize, lpString: *const u16, c: i32, psizl: *mut SIZE) -> i32;
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

// ─── Win32 Constants & Structs ──────────────────────────────────────────────

const DT_WORDBREAK: u32 = 0x00000010;
const DT_LEFT: u32 = 0x00000000;
const DT_NOPREFIX: u32 = 0x00000800;

const WS_EX_LAYERED: isize = 0x00080000;
const ULW_ALPHA: u32 = 0x00000002;
const AC_SRC_OVER: u8 = 0x00;
const AC_SRC_ALPHA: u8 = 0x01;

const OV_REST_W: i32 = 180;
const OV_WORK_W: i32 = 220;
const OV_OPEN_W: i32 = 392;
const OV_BASE_H: i32 = 40;
const OV_OPEN_H: i32 = 104;
const WAVE_BARS: usize = 9;

const WS_EX_TOOLWINDOW: isize = 0x00000080;
const WS_EX_TOPMOST: isize = 0x00000008;
const WS_EX_NOACTIVATE: isize = 0x08000000;
const WS_EX_TRANSPARENT: isize = 0x00000020;
const HWND_TOPMOST: isize = -1;
const SWP_NOACTIVATE: u32 = 0x0010;
const SWP_FRAMECHANGED: u32 = 0x0020;
const SWP_NOOWNERZORDER: u32 = 0x0200;
const SWP_SHOWWINDOW: u32 = 0x0040;
const MONITOR_DEFAULTTONEAREST: u32 = 2;
const SW_SHOWNOACTIVATE: i32 = 4;
const SW_HIDE: i32 = 0;
const WS_POPUP: u32 = 0x80000000;
const PM_REMOVE: u32 = 0x0001;
const WM_QUIT: u32 = 0x0012;
const TRANSPARENT: i32 = 1;
const OVERLAY_BOTTOM_GAP: i32 = 96;

const IDLE_PILL_W: i32 = 64;
const IDLE_PILL_H: i32 = 8;
const IDLE_WINDOW_W: i32 = 64;
const IDLE_WINDOW_H: i32 = 8;
const MORPH_PILL_W: i32 = 180;
const MORPH_PILL_H: i32 = 68;

const MENU_W: i32 = 250;
const MENU_ITEM_H: i32 = 34;
const MENU_PADDING: i32 = 12;
const MENU_ITEMS: usize = 7;

const NO_SIGNAL_W: i32 = 360;
const NO_SIGNAL_H: i32 = 100;
const NO_SIGNAL_DELAY_SECS: u64 = 5;

const WM_SETCURSOR: u32 = 0x0020;
const WM_MOUSEMOVE: u32 = 0x0200;
const WM_LBUTTONDOWN: u32 = 0x0201;
const WM_LBUTTONUP: u32 = 0x0202;
const WM_LBUTTONDBLCLK: u32 = 0x0203;
const WM_RBUTTONDOWN: u32 = 0x0204;
const WM_MOUSELEAVE: u32 = 0x02A3;
const TME_LEAVE: u32 = 0x00000002;
const IDC_ARROW: *const u16 = 32512 as *const u16;

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

#[repr(C)]
struct TRACKMOUSEEVENT {
    cb_size: u32,
    dw_flags: u32,
    hwnd_track: isize,
    dw_hover_time: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct POINT {
    x: i32,
    y: i32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct SIZE {
    cx: i32,
    cy: i32,
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
static mut PREVIEW_HWND: isize = 0;

static CANCEL_TCP_WRITER: OnceLock<Arc<Mutex<Option<std::net::TcpStream>>>> = OnceLock::new();
static CANCEL_TCP_CONNECTED: OnceLock<Arc<AtomicBool>> = OnceLock::new();

// ─── Application State ──────────────────────────────────────────────────────

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum OverlayState {
    Hidden,
    Idle,
    Recording,
    Transcribing,
    Done,
}

struct PopupState {
    state: OverlayState,
    activation_mode: u8,
    output_mode: u8,
    audio_levels: Vec<f32>,
    smoothed_levels: Vec<f32>,
    last_voice_time: Instant,
    captured_text: String,
    final_text: String,
    streaming_mode: bool,
    live_committed: String,
    live_tentative: String,
    live_typed_len: usize,
    session_live: bool,
    commit_sent: bool,
    release_requested: bool,
    toggle_active: bool,
    show_until: Option<Instant>,
    transcribing_since: Option<Instant>,
    always_on_pill: bool,
    dock_side: u8, // 0=bottom, 1=left, 2=right
    morph_progress: f32,
    morph_target: f32,
    hover_active: bool,
    mouse_tracking_set: bool,
    context_menu_open: bool,
    context_menu_hover: i32,
    selected_language: String,
    no_signal_shown: bool,
    no_signal_since: Option<Instant>,
    hidden_until: Option<Instant>,
    dragging: bool,
    drag_start_x: i32,
    drag_start_y: i32,
    drag_preview_dock: i32, // -1=none, 0=bottom, 1=left, 2=right
}

impl Default for PopupState {
    fn default() -> Self {
        Self {
            state: OverlayState::Idle,
            activation_mode: 0,
            output_mode: 0,
            audio_levels: vec![0.0; WAVE_BARS],
            smoothed_levels: vec![0.0; WAVE_BARS],
            last_voice_time: Instant::now(),
            captured_text: String::new(),
            final_text: String::new(),
            streaming_mode: false,
            live_committed: String::new(),
            live_tentative: String::new(),
            live_typed_len: 0,
            session_live: false,
            commit_sent: false,
            release_requested: false,
            toggle_active: false,
            show_until: None,
            transcribing_since: None,
            always_on_pill: true,
            dock_side: 0,
            morph_progress: 0.0,
            morph_target: 0.0,
            hover_active: false,
            mouse_tracking_set: false,
            context_menu_open: false,
            context_menu_hover: -1,
            selected_language: "English (US)".to_string(),
            no_signal_shown: false,
            no_signal_since: None,
            hidden_until: None,
            dragging: false,
            drag_start_x: 0,
            drag_start_y: 0,
            drag_preview_dock: 0,
        }
    }
}

// ─── Helpers ────────────────────────────────────────────────────────────────

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

fn colorref(r: u8, g: u8, b: u8) -> u32 {
    (r as u32) | ((g as u32) << 8) | ((b as u32) << 16)
}

fn lerp_i32(a: i32, b: i32, t: f32) -> i32 {
    (a as f32 + (b as f32 - a as f32) * t.clamp(0.0, 1.0)).round() as i32
}

fn send_popup_command(
    tcp_writer: &Arc<Mutex<Option<std::net::TcpStream>>>,
    tcp_connected: &Arc<AtomicBool>,
    cmd: &[u8],
) -> bool {
    use std::io::Write;
    if !tcp_connected.load(Ordering::SeqCst) {
        return false;
    }
    let mut writer_guard = tcp_writer.lock().unwrap();
    if let Some(ref mut stream) = *writer_guard {
        if stream.write_all(cmd).is_ok() {
            let _ = stream.flush();
            return true;
        }
    }
    false
}

fn begin_recording_session(st: &mut PopupState) {
    st.state = OverlayState::Recording;
    st.session_live = true;
    st.commit_sent = false;
    st.release_requested = false;
    st.show_until = None;
    st.transcribing_since = None;
    st.captured_text.clear();
    st.final_text.clear();
    st.live_committed.clear();
    st.live_tentative.clear();
    st.live_typed_len = 0;
    st.context_menu_open = false;
    st.no_signal_shown = false;
    st.no_signal_since = None;
    st.last_voice_time = Instant::now();
}

fn hide_session(st: &mut PopupState) {
    st.state = if st.always_on_pill {
        OverlayState::Idle
    } else {
        OverlayState::Hidden
    };
    st.session_live = false;
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

fn overlay_size_for(st: &PopupState) -> (i32, i32) {
    if let Some(until) = st.hidden_until {
        if Instant::now() < until {
            return (0, 0);
        }
    }
    let live = !st.live_committed.is_empty() || !st.live_tentative.is_empty();
    match st.state {
        OverlayState::Idle if st.context_menu_open => (MENU_W, MENU_PADDING * 2 + MENU_ITEM_H * MENU_ITEMS as i32),
        OverlayState::Idle if st.morph_progress > 0.05 => {
            let w = lerp_i32(IDLE_WINDOW_W, MORPH_PILL_W, st.morph_progress);
            let h = lerp_i32(IDLE_WINDOW_H, MORPH_PILL_H, st.morph_progress);
            (w, h)
        }
        OverlayState::Idle => match st.dock_side {
            1 | 2 => (IDLE_PILL_H, IDLE_PILL_W),
            _ => (IDLE_PILL_W, IDLE_PILL_H),
        },
        OverlayState::Recording if st.no_signal_shown => (NO_SIGNAL_W, NO_SIGNAL_H),
        OverlayState::Recording if live => (OV_OPEN_W, OV_OPEN_H),
        OverlayState::Recording => (OV_REST_W, OV_BASE_H),
        OverlayState::Transcribing | OverlayState::Done if live => (OV_OPEN_W, OV_OPEN_H),
        OverlayState::Transcribing | OverlayState::Done => (OV_WORK_W, OV_BASE_H),
        OverlayState::Hidden => (OV_REST_W, OV_BASE_H),
    }
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
            return;
        }
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
        let (x, y) = match st.dock_side {
            1 => (work.left + gap, work.top + ((work.bottom - work.top - height) / 2)),
            2 => (work.right - width - gap, work.top + ((work.bottom - work.top - height) / 2)),
            _ => (
                work.left + ((work.right - work.left - width) / 2),
                (work.bottom - height - gap).max(work.top + 12),
            ),
        };
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

fn handy_bar_height(level: f32) -> i32 {
    let v = level.clamp(0.0, 1.0);
    let h = 4.0 + v.powf(0.7) * 18.0;
    h.round().clamp(4.0, 22.0) as i32
}

// ─── Drawing Primitives ─────────────────────────────────────────────────────

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
    if w <= 0 || h <= 0 || pixels.is_null() {
        return;
    }
    let r2 = r + 1.0;
    let x0 = ((cx - r2).floor() as i32).clamp(0, w);
    let x1 = ((cx + r2).ceil() as i32 + 1).clamp(0, w);
    let y0 = ((cy - r2).floor() as i32).clamp(0, h);
    let y1 = ((cy + r2).ceil() as i32 + 1).clamp(0, h);
    if x0 >= x1 || y0 >= y1 {
        return;
    }
    let total_pixels = (w * h) as usize;
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
                let idx = (py * w + px) as usize;
                if idx < total_pixels {
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
}

unsafe fn draw_aa_rrect(
    pixels: *mut u32,
    w: i32,
    h: i32,
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
    if w <= 0 || h <= 0 || pixels.is_null() {
        return;
    }
    let x_start = left.clamp(0, w);
    let x_end = right.clamp(0, w);
    let y_start = top.clamp(0, h);
    let y_end = bottom.clamp(0, h);
    if x_start >= x_end || y_start >= y_end {
        return;
    }
    let total_pixels = (w * h) as usize;
    for py in y_start..y_end {
        for px in x_start..x_end {
            let fx = px as f32 + 0.5;
            let fy = py as f32 + 0.5;
            let rw = (right - left) as f32 / 2.0;
            let rh = (bottom - top) as f32 / 2.0;
            if rw <= 0.0 || rh <= 0.0 {
                continue;
            }
            let rcx = left as f32 + rw;
            let rcy = top as f32 + rh;
            let cr_eff = corner_r.min(rw).min(rh).max(0.0);
            let qx = (fx - rcx).abs() - (rw - cr_eff);
            let qy = (fy - rcy).abs() - (rh - cr_eff);
            let dist = if qx > 0.0 && qy > 0.0 {
                (qx * qx + qy * qy).sqrt() - cr_eff
            } else {
                qx.max(qy).max(0.0) - cr_eff
            };
            let edge = (-dist + 0.5).clamp(0.0, 1.0);
            if edge > 0.0 {
                let a = (ca as f32 * edge) as u32;
                let pr = (cr as u32 * a) / 255;
                let pg = (cg as u32 * a) / 255;
                let pb = (cb as u32 * a) / 255;
                let idx = (py * w + px) as usize;
                if idx < total_pixels {
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
}

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
    width: f32,
) {
    if w <= 0 || h <= 0 || pixels.is_null() {
        return;
    }
    let min_x = ((x0.min(x1) - width - 1.0).floor() as i32).clamp(0, w);
    let max_x = ((x0.max(x1) + width + 2.0).ceil() as i32).clamp(0, w);
    let min_y = ((y0.min(y1) - width - 1.0).floor() as i32).clamp(0, h);
    let max_y = ((y0.max(y1) + width + 2.0).ceil() as i32).clamp(0, h);

    if min_x >= max_x || min_y >= max_y {
        return;
    }

    let dx = x1 - x0;
    let dy = y1 - y0;
    let len_sq = dx * dx + dy * dy;
    let total_pixels = (w * h) as usize;

    for py in min_y..max_y {
        for px in min_x..max_x {
            let pfx = px as f32 + 0.5;
            let pfy = py as f32 + 0.5;
            let t = if len_sq == 0.0 {
                0.0
            } else {
                ((pfx - x0) * dx + (pfy - y0) * dy) / len_sq
            }
            .clamp(0.0, 1.0);
            let proj_x = x0 + t * dx;
            let proj_y = y0 + t * dy;
            let dist = ((pfx - proj_x) * (pfx - proj_x) + (pfy - proj_y) * (pfy - proj_y)).sqrt();
            let edge = (width / 2.0 + 0.5 - dist).clamp(0.0, 1.0);
            if edge > 0.0 {
                let a = (ca as f32 * edge) as u32;
                let pr = (cr as u32 * a) / 255;
                let pg = (cg as u32 * a) / 255;
                let pb = (cb as u32 * a) / 255;
                let idx = (py * w + px) as usize;
                if idx < total_pixels {
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
}

/// Renders authentic #0071bc vector microphone icon
unsafe fn draw_authentic_mic_icon(pixels: *mut u32, w: i32, h: i32, cx: f32, cy: f32, scale: f32) {
    let (r, g, b, a) = (0, 113, 188, 255); // #0071bc brand electric blue
    let bw = 2.8 * scale;
    let bh = 5.5 * scale;
    // Mic capsule head
    draw_aa_rrect(
        pixels, w, h,
        (cx - bw) as i32, (cy - bh) as i32,
        (cx + bw) as i32, (cy + 1.0 * scale) as i32,
        bw, r, g, b, a,
    );
    // Cradle arc (U-shaped)
    let cradle_r = 4.6 * scale;
    let cradle_top = cy - 1.0 * scale;
    let cradle_bot = cy + 3.0 * scale;
    draw_aa_line(pixels, w, h, cx - cradle_r, cradle_top, cx - cradle_r, cradle_bot, r, g, b, a, 1.2 * scale);
    draw_aa_line(pixels, w, h, cx + cradle_r, cradle_top, cx + cradle_r, cradle_bot, r, g, b, a, 1.2 * scale);
    draw_aa_line(pixels, w, h, cx - cradle_r, cradle_bot, cx + cradle_r, cradle_bot, r, g, b, a, 1.2 * scale);
    // Stem
    draw_aa_line(pixels, w, h, cx, cradle_bot, cx, cradle_bot + 3.5 * scale, r, g, b, a, 1.2 * scale);
    // Base
    draw_aa_line(pixels, w, h, cx - 3.5 * scale, cradle_bot + 3.5 * scale, cx + 3.5 * scale, cradle_bot + 3.5 * scale, r, g, b, a, 1.2 * scale);
}

/// Mathematical pre-multiplied alpha normalization for GDI text rendering into 32-bit DIBSections.
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

// ─── Drawing Layered Overlay ────────────────────────────────────────────────

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

        let mut bmi: BITMAPINFO = std::mem::zeroed();
        bmi.bmi_header.bi_size = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
        bmi.bmi_header.bi_width = width;
        bmi.bmi_header.bi_height = -height; // Top-down
        bmi.bmi_header.bi_planes = 1;
        bmi.bmi_header.bi_bit_count = 32;
        bmi.bmi_header.bi_compression = 0; // BI_RGB

        let mut pixels_ptr: *mut c_void = std::ptr::null_mut();
        let h_bitmap = CreateDIBSection(
            hdc_mem,
            &bmi,
            0,
            &mut pixels_ptr,
            0,
            0,
        );

        if h_bitmap == 0 || pixels_ptr.is_null() {
            DeleteDC(hdc_mem);
            ReleaseDC(0, hdc_screen);
            return;
        }

        let old_bitmap = SelectObject(hdc_mem, h_bitmap);
        let pixels = pixels_ptr as *mut u32;
        let total_pixels = (width * height) as usize;
        std::ptr::write_bytes(pixels, 0, total_pixels);

        let live = !state.live_committed.is_empty() || !state.live_tentative.is_empty();
        let row_top = if live { (height - OV_BASE_H).max(0) } else { 0 };
        let cy = row_top + OV_BASE_H / 2;
        let cancel = cancel_button_rect(width, height);

        // ── Main capsule background ──
        if state.state != OverlayState::Hidden && state.state != OverlayState::Idle {
            // Main control row
            draw_aa_rrect(
                pixels, width, height,
                0, row_top, width, height,
                20.0, 12, 12, 16, 245,
            );
        }

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

            // Dark rounded container box behind live text
            draw_aa_rrect(
                pixels, width, height,
                8, 8, width - 8, row_top - 4,
                10.0, 16, 16, 22, 235,
            );

            SetBkMode(hdc_mem, TRANSPARENT);
            SelectObject(hdc_mem, get_overlay_font());
            SetTextColor(hdc_mem, colorref(240, 240, 248));
            let label = wide(&display);
            let mut text_rect = RECT {
                left: 16,
                top: 14,
                right: width - 16,
                bottom: row_top - 8,
            };
            DrawTextW(
                hdc_mem,
                label.as_ptr(),
                label.len().saturating_sub(1) as i32,
                &mut text_rect,
                DT_WORDBREAK | DT_LEFT | DT_NOPREFIX,
            );

            // Copy hint icon
            if !display.is_empty() {
                let btn_x = width - 26;
                let btn_y = 12;
                draw_aa_rrect(pixels, width, height, btn_x + 2, btn_y, btn_x + 14, btn_y + 12, 2.0, 140, 140, 155, 180);
                draw_aa_rrect(pixels, width, height, btn_x, btn_y + 3, btn_x + 12, btn_y + 15, 2.0, 180, 180, 195, 200);
            }
        }

        // ── State-specific views ──
        match state.state {
            OverlayState::Idle => {
                if state.context_menu_open {
                    // Dark Context Menu with complete options
                    draw_aa_rrect(
                        pixels, width, height,
                        0, 0, width, height,
                        14.0, 18, 18, 26, 252,
                    );
                    // Subtle border outline
                    draw_aa_rrect(
                        pixels, width, height,
                        0, 0, width, 1, 0.0, 60, 60, 80, 180,
                    );
                    
                    let lang_str = format!("  🌐  Language ({})", state.selected_language);
                    let items = [
                        lang_str.as_str(),
                        "  🎤  Microphone (Auto)",
                        "  ⏰  Hide for 1 hour",
                        "  ⚙️  Settings Dashboard",
                        "  📁  Transcript History",
                        "  📋  Paste Last Transcript",
                        "  ❌  Close Menu",
                    ];
                    SetBkMode(hdc_mem, TRANSPARENT);
                    SelectObject(hdc_mem, get_overlay_font());

                    for (i, label_str) in items.iter().enumerate() {
                        let item_y = MENU_PADDING + i as i32 * MENU_ITEM_H;
                        
                        if i == 2 || i == 4 || i == 6 {
                            draw_aa_rrect(pixels, width, height, 8, item_y - 2, width - 8, item_y - 1, 0.0, 60, 60, 70, 90);
                        }

                        if state.context_menu_hover == i as i32 {
                            draw_aa_rrect(
                                pixels, width, height,
                                6, item_y, width - 6, item_y + MENU_ITEM_H - 2,
                                8.0, 0, 113, 188, 180,
                            );
                            SetTextColor(hdc_mem, colorref(255, 255, 255));
                        } else {
                            SetTextColor(hdc_mem, colorref(225, 225, 235));
                        }
                        let label = wide(label_str);
                        TextOutW(
                            hdc_mem,
                            14,
                            item_y + 6,
                            label.as_ptr(),
                            label.len().saturating_sub(1) as i32,
                        );
                    }
                } else if state.morph_progress > 0.05 {
                    // ── Morphed Dictate Pill (Snug text measurement + authentic mic icon) ──
                    SelectObject(hdc_mem, get_overlay_font());
                    let t1 = wide("Dictate ");
                    let mut sz1 = SIZE::default();
                    GetTextExtentPoint32W(hdc_mem, t1.as_ptr(), t1.len().saturating_sub(1) as i32, &mut sz1);

                    SelectObject(hdc_mem, get_bold_overlay_font());
                    let t2 = wide("Ctrl + Space");
                    let mut sz2 = SIZE::default();
                    GetTextExtentPoint32W(hdc_mem, t2.as_ptr(), t2.len().saturating_sub(1) as i32, &mut sz2);

                    let total_text_w = sz1.cx + sz2.cx;
                    let main_w = (total_text_w + 28).min(width);
                    let main_h = 32.min(height);
                    let main_x = ((width - main_w) / 2).max(0);
                    let main_y = 0;

                    // Outer capsule body (#0A0A0C fill, crisp outline)
                    draw_aa_rrect(
                        pixels, width, height,
                        main_x, main_y, main_x + main_w, main_y + main_h,
                        16.0, 10, 10, 12, 240,
                    );

                    // Text rendering
                    if width >= main_w && height >= main_h {
                        SetBkMode(hdc_mem, TRANSPARENT);
                        SelectObject(hdc_mem, get_overlay_font());
                        SetTextColor(hdc_mem, colorref(220, 220, 230));
                        TextOutW(hdc_mem, main_x + 14, main_y + 7, t1.as_ptr(), t1.len().saturating_sub(1) as i32);

                        SelectObject(hdc_mem, get_bold_overlay_font());
                        SetTextColor(hdc_mem, colorref(255, 255, 255));
                        TextOutW(hdc_mem, main_x + 14 + sz1.cx, main_y + 7, t2.as_ptr(), t2.len().saturating_sub(1) as i32);
                    }

                    // Hanging Mic Circle Button with authentic #0071bc blue icon
                    let mic_r = 13.0f32;
                    let mic_cx = (width / 2) as f32;
                    let mic_cy = (main_y + main_h + 14) as f32;
                    if mic_cy + mic_r <= height as f32 {
                        draw_aa_circle(pixels, width, height, mic_cx, mic_cy, mic_r, 16, 16, 22, 245);
                        draw_aa_circle(pixels, width, height, mic_cx, mic_cy, mic_r - 1.0, 0, 113, 188, 200);
                        draw_aa_circle(pixels, width, height, mic_cx, mic_cy, mic_r - 2.5, 16, 16, 22, 255);
                        draw_authentic_mic_icon(pixels, width, height, mic_cx, mic_cy, 1.0);
                    }
                } else {
                    // ── Always-on Micro Pill Handle (Solid pitch-black with crisp white border, NO center dash) ──
                    for py in 0..height {
                        for px in 0..width {
                            let idx = (py * width + px) as usize;
                            if idx < total_pixels {
                                *pixels.add(idx) = 0x01000000; // alpha=1 for hit-testing
                            }
                        }
                    }
                    let (pw, ph) = match state.dock_side {
                        1 | 2 => (IDLE_PILL_H, IDLE_PILL_W),
                        _ => (IDLE_PILL_W, IDLE_PILL_H),
                    };
                    let pill_x = (width - pw) / 2;
                    let pill_y = (height - ph) / 2;
                    // Solid black fill
                    draw_aa_rrect(
                        pixels, width, height,
                        pill_x, pill_y, pill_x + pw, pill_y + ph,
                        4.0, 0, 0, 0, 255,
                    );
                    // Crisp white border
                    draw_aa_line(pixels, width, height, pill_x as f32 + 4.0, pill_y as f32, (pill_x + pw) as f32 - 4.0, pill_y as f32, 255, 255, 255, 190, 1.0);
                    draw_aa_line(pixels, width, height, pill_x as f32 + 4.0, (pill_y + ph) as f32, (pill_x + pw) as f32 - 4.0, (pill_y + ph) as f32, 255, 255, 255, 190, 1.0);
                }
            }
            OverlayState::Recording if state.no_signal_shown => {
                // "Is your microphone muted?" Notification Card
                draw_aa_rrect(
                    pixels, width, height,
                    0, 0, width, height,
                    14.0, 20, 20, 30, 248,
                );
                SetBkMode(hdc_mem, TRANSPARENT);
                SelectObject(hdc_mem, get_bold_overlay_font());
                SetTextColor(hdc_mem, colorref(255, 200, 80));
                let title = wide("(!) Is your microphone muted?");
                TextOutW(hdc_mem, 16, 12, title.as_ptr(), title.len().saturating_sub(1) as i32);

                SelectObject(hdc_mem, get_overlay_font());
                SetTextColor(hdc_mem, colorref(170, 170, 185));
                let body = wide("We didn't pick up any audio from Auto-detect");
                TextOutW(hdc_mem, 16, 34, body.as_ptr(), body.len().saturating_sub(1) as i32);

                // Action buttons
                draw_aa_rrect(pixels, width, height, 16, 56, 140, 80, 8.0, 45, 45, 58, 200);
                SetTextColor(hdc_mem, colorref(230, 230, 240));
                let btn1 = wide("Select microphone");
                TextOutW(hdc_mem, 24, 61, btn1.as_ptr(), btn1.len().saturating_sub(1) as i32);

                draw_aa_rrect(pixels, width, height, 148, 56, 240, 80, 8.0, 45, 45, 58, 200);
                let btn2 = wide("Troubleshoot");
                TextOutW(hdc_mem, 160, 61, btn2.as_ptr(), btn2.len().saturating_sub(1) as i32);

                // Auto-dismiss countdown bar
                if let Some(since) = state.no_signal_since {
                    let elapsed = since.elapsed().as_secs_f32();
                    let progress = (1.0 - (elapsed / 8.0)).clamp(0.0, 1.0);
                    let bar_w = (width as f32 * progress) as i32;
                    draw_aa_rrect(pixels, width, height, 0, height - 3, bar_w, height, 0.0, 56, 152, 255, 180);
                }
            }
            OverlayState::Recording => {
                // Pulsing red recording dot
                let t = APP_START.elapsed().as_millis() as f32;
                let pulse = ((t / 950.0) * std::f32::consts::PI).sin().abs();
                let dot_cx = 20.0;
                let dot_cy = cy as f32;
                draw_aa_circle(
                    pixels, width, height, dot_cx, dot_cy, 7.0,
                    255, (59.0 + 40.0 * pulse) as u8, 48, (160.0 + 95.0 * pulse) as u8,
                );
                draw_aa_circle(pixels, width, height, dot_cx, dot_cy, 4.0, 255, 59, 48, 255);

                // Dynamic 9-bar spatial waveform bars
                let bar_w = 5;
                let gap = 4;
                let total_w = WAVE_BARS as i32 * bar_w + (WAVE_BARS as i32 - 1) * gap;
                let start_x = (width - total_w) / 2;
                for i in 0..WAVE_BARS {
                    let level = state.smoothed_levels.get(i).copied().unwrap_or(0.0).clamp(0.0, 1.0);
                    let bar_h = handy_bar_height(level);
                    let x = start_x + i as i32 * (bar_w + gap);
                    let y = cy - bar_h / 2;
                    let opacity = (0.4 + level * 0.6).min(1.0);
                    draw_aa_rrect(
                        pixels, width, height,
                        x, y, x + bar_w, y + bar_h, 2.5,
                        56, 152, 255, (opacity * 255.0) as u8,
                    );
                }
            }
            OverlayState::Transcribing => {
                // Modern fluid glowing gradient pulse bar
                let t = APP_START.elapsed().as_millis() as f32;
                let phase = t * 0.005;
                let bar_total_w = 120;
                let bar_start_x = (width - bar_total_w) / 2;
                let bar_y = cy - 3;
                let bar_h = 6;
                draw_aa_rrect(
                    pixels, width, height,
                    bar_start_x - 4, bar_y - 2, bar_start_x + bar_total_w + 4, bar_y + bar_h + 2,
                    5.0, 56, 152, 255, 60,
                );
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
                SetBkMode(hdc_mem, TRANSPARENT);
                SelectObject(hdc_mem, get_bold_overlay_font());
                SetTextColor(hdc_mem, colorref(200, 200, 215));
                let label = wide("Transcribing…");
                TextOutW(hdc_mem, width / 2 - 42, cy + 8, label.as_ptr(), label.len().saturating_sub(1) as i32);
            }
            OverlayState::Done => {
                SetBkMode(hdc_mem, TRANSPARENT);
                SelectObject(hdc_mem, get_bold_overlay_font());
                SetTextColor(hdc_mem, colorref(76, 217, 100));
                let label = wide("✓ Done");
                TextOutW(hdc_mem, width / 2 - 24, cy - 7, label.as_ptr(), label.len().saturating_sub(1) as i32);
            }
            OverlayState::Hidden => {}
        }

        // ── Cancel button (× circle) ──
        if state.state != OverlayState::Hidden && state.state != OverlayState::Idle {
            let ccx = (cancel.left + cancel.right) as f32 / 2.0;
            let ccy = (cancel.top + cancel.bottom) as f32 / 2.0;
            let cr = (cancel.right - cancel.left) as f32 / 2.0;
            draw_aa_circle(pixels, width, height, ccx, ccy, cr, 50, 50, 58, 220);
            let pad = 6.0;
            let x0 = cancel.left as f32 + pad;
            let y0 = cancel.top as f32 + pad;
            let x1 = cancel.right as f32 - pad;
            let y1 = cancel.bottom as f32 - pad;
            draw_aa_line(pixels, width, height, x0, y0, x1, y1, 190, 190, 200, 240, 1.5);
            draw_aa_line(pixels, width, height, x1, y0, x0, y1, 190, 190, 200, 240, 1.5);
        }

        // ── Fix alpha for GDI text ──
        fix_gdi_alpha(pixels, total_pixels);

        // ── Update layered window ──
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

        UpdateLayeredWindow(
            OUR_HWND,
            0,
            &pt_dst,
            &size,
            hdc_mem,
            &pt_src,
            0,
            &blend,
            ULW_ALPHA,
        );

        SelectObject(hdc_mem, old_bitmap);
        DeleteObject(h_bitmap);
        DeleteDC(hdc_mem);
        ReleaseDC(0, hdc_screen);
    }
}

// ─── Drag & Drop 3-Side Docking Preview Window ──────────────────────────────

fn draw_docking_preview_overlay(target_dock: i32) {
    unsafe {
        if PREVIEW_HWND == 0 {
            return;
        }
        let sw = GetSystemMetrics(0);
        let sh = GetSystemMetrics(1);
        if sw <= 0 || sh <= 0 {
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

        let mut bmi: BITMAPINFO = std::mem::zeroed();
        bmi.bmi_header.bi_size = std::mem::size_of::<BITMAPINFOHEADER>() as u32;
        bmi.bmi_header.bi_width = sw;
        bmi.bmi_header.bi_height = -sh; // Top-down
        bmi.bmi_header.bi_planes = 1;
        bmi.bmi_header.bi_bit_count = 32;
        bmi.bmi_header.bi_compression = 0;

        let mut pixels_ptr: *mut c_void = std::ptr::null_mut();
        let h_bitmap = CreateDIBSection(hdc_mem, &bmi, 0, &mut pixels_ptr, 0, 0);
        if h_bitmap == 0 || pixels_ptr.is_null() {
            DeleteDC(hdc_mem);
            ReleaseDC(0, hdc_screen);
            return;
        }

        let old_bitmap = SelectObject(hdc_mem, h_bitmap);
        let pixels = pixels_ptr as *mut u32;
        let total_pixels = (sw * sh) as usize;
        std::ptr::write_bytes(pixels, 0, total_pixels);

        // 1. Dark translucent backdrop across screen
        for i in 0..total_pixels {
            *pixels.add(i) = 0x50000000; // ~32% dark translucent overlay
        }

        // 2. Bottom Dock Slot (Dock 0)
        let b_w = 200;
        let b_h = 44;
        let b_x0 = (sw - b_w) / 2;
        let b_y0 = sh - 70;
        let b_x1 = b_x0 + b_w;
        let b_y1 = b_y0 + b_h;
        if target_dock == 0 {
            draw_aa_rrect(pixels, sw, sh, b_x0, b_y0, b_x1, b_y1, 16.0, 0, 113, 188, 220);
            draw_aa_rrect(pixels, sw, sh, b_x0 + 2, b_y0 + 2, b_x1 - 2, b_y1 - 2, 14.0, 10, 10, 16, 240);
        } else {
            draw_aa_rrect(pixels, sw, sh, b_x0, b_y0, b_x1, b_y1, 16.0, 255, 255, 255, 80);
            draw_aa_rrect(pixels, sw, sh, b_x0 + 2, b_y0 + 2, b_x1 - 2, b_y1 - 2, 14.0, 20, 20, 30, 160);
        }

        // 3. Left Dock Slot (Dock 1)
        let l_w = 44;
        let l_h = 200;
        let l_x0 = 20;
        let l_y0 = (sh - l_h) / 2;
        let l_x1 = l_x0 + l_w;
        let l_y1 = l_y0 + l_h;
        if target_dock == 1 {
            draw_aa_rrect(pixels, sw, sh, l_x0, l_y0, l_x1, l_y1, 16.0, 0, 113, 188, 220);
            draw_aa_rrect(pixels, sw, sh, l_x0 + 2, l_y0 + 2, l_x1 - 2, l_y1 - 2, 14.0, 10, 10, 16, 240);
        } else {
            draw_aa_rrect(pixels, sw, sh, l_x0, l_y0, l_x1, l_y1, 16.0, 255, 255, 255, 80);
            draw_aa_rrect(pixels, sw, sh, l_x0 + 2, l_y0 + 2, l_x1 - 2, l_y1 - 2, 14.0, 20, 20, 30, 160);
        }

        // 4. Right Dock Slot (Dock 2)
        let r_w = 44;
        let r_h = 200;
        let r_x0 = sw - 20 - r_w;
        let r_y0 = (sh - r_h) / 2;
        let r_x1 = r_x0 + r_w;
        let r_y1 = r_y0 + r_h;
        if target_dock == 2 {
            draw_aa_rrect(pixels, sw, sh, r_x0, r_y0, r_x1, r_y1, 16.0, 0, 113, 188, 220);
            draw_aa_rrect(pixels, sw, sh, r_x0 + 2, r_y0 + 2, r_x1 - 2, r_y1 - 2, 14.0, 10, 10, 16, 240);
        } else {
            draw_aa_rrect(pixels, sw, sh, r_x0, r_y0, r_x1, r_y1, 16.0, 255, 255, 255, 80);
            draw_aa_rrect(pixels, sw, sh, r_x0 + 2, r_y0 + 2, r_x1 - 2, r_y1 - 2, 14.0, 20, 20, 30, 160);
        }

        // Text Labels on Targets
        SetBkMode(hdc_mem, TRANSPARENT);
        SelectObject(hdc_mem, get_bold_overlay_font());

        SetTextColor(hdc_mem, if target_dock == 0 { colorref(0, 180, 255) } else { colorref(200, 200, 210) });
        let b_lbl = wide("Bottom Dock");
        TextOutW(hdc_mem, b_x0 + 44, b_y0 + 13, b_lbl.as_ptr(), b_lbl.len().saturating_sub(1) as i32);

        SetTextColor(hdc_mem, if target_dock == 1 { colorref(0, 180, 255) } else { colorref(200, 200, 210) });
        let l_lbl = wide("Left");
        TextOutW(hdc_mem, l_x0 + 8, l_y0 + 90, l_lbl.as_ptr(), l_lbl.len().saturating_sub(1) as i32);

        SetTextColor(hdc_mem, if target_dock == 2 { colorref(0, 180, 255) } else { colorref(200, 200, 210) });
        let r_lbl = wide("Right");
        TextOutW(hdc_mem, r_x0 + 4, r_y0 + 90, r_lbl.as_ptr(), r_lbl.len().saturating_sub(1) as i32);

        fix_gdi_alpha(pixels, total_pixels);

        let pt_dst = POINT { x: 0, y: 0 };
        let pt_src = POINT { x: 0, y: 0 };
        let size = POINT { x: sw, y: sh };
        let blend = BLENDFUNCTION {
            blend_op: AC_SRC_OVER,
            blend_flags: 0,
            source_constant_alpha: 255,
            alpha_format: AC_SRC_ALPHA,
        };

        UpdateLayeredWindow(
            PREVIEW_HWND,
            0,
            &pt_dst,
            &size,
            hdc_mem,
            &pt_src,
            0,
            &blend,
            ULW_ALPHA,
        );

        SelectObject(hdc_mem, old_bitmap);
        DeleteObject(h_bitmap);
        DeleteDC(hdc_mem);
        ReleaseDC(0, hdc_screen);
    }
}

// ─── Window Procedures ──────────────────────────────────────────────────────

unsafe extern "system" fn native_overlay_proc(
    hwnd: isize,
    msg: u32,
    w_param: usize,
    l_param: isize,
) -> isize {
    // Explicitly set normal arrow cursor to prevent any hourglass / loading wait cursor
    if msg == WM_SETCURSOR {
        SetCursor(LoadCursorW(0, IDC_ARROW));
        return 1;
    }
    DefWindowProcW(hwnd, msg, w_param, l_param)
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
            h_cursor: LoadCursorW(0, IDC_ARROW),
            hbr_background: 0,
            lpsz_menu_name: std::ptr::null(),
            lpsz_class_name: class_name.as_ptr(),
        };
        let _ = RegisterClassW(&class);
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

        // Create Fullscreen Drag Preview Window
        let preview_class_name = wide("QuickSTTDockPreviewOverlay");
        let preview_class = WNDCLASSW {
            style: 0,
            lpfn_wnd_proc: Some(native_overlay_proc),
            cb_cls_extra: 0,
            cb_wnd_extra: 0,
            h_instance: instance,
            h_icon: 0,
            h_cursor: LoadCursorW(0, IDC_ARROW),
            hbr_background: 0,
            lpsz_menu_name: std::ptr::null(),
            lpsz_class_name: preview_class_name.as_ptr(),
        };
        let _ = RegisterClassW(&preview_class);
        let preview_ex_style = (WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT) as u32;
        let sw = GetSystemMetrics(0);
        let sh = GetSystemMetrics(1);
        let prev_hwnd = CreateWindowExW(
            preview_ex_style,
            preview_class_name.as_ptr(),
            wide("Dock Preview").as_ptr(),
            WS_POPUP,
            0,
            0,
            sw,
            sh,
            0,
            0,
            instance,
            std::ptr::null(),
        );
        PREVIEW_HWND = prev_hwnd;
        ShowWindow(PREVIEW_HWND, SW_HIDE);

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
        eprintln!("[POPUP] Native Win32 overlay created (HWND={:#x}, PREVIEW_HWND={:#x})", OUR_HWND, PREVIEW_HWND);
        true
    }
}

// ─── Overlay Loop ───────────────────────────────────────────────────────────

fn run_native_overlay(
    state: Arc<Mutex<PopupState>>,
    _hotkey_rx: mpsc::Receiver<HotkeyEvent>,
) {
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
                if message.message == WM_SETCURSOR {
                    SetCursor(LoadCursorW(0, IDC_ARROW));
                }
                if message.message == WM_MOUSEMOVE && message.hwnd == OUR_HWND {
                    SetCursor(LoadCursorW(0, IDC_ARROW));
                    let y = ((message.l_param >> 16) & 0xFFFF) as i16 as i32;
                    let mut st = state.lock().unwrap();
                    if st.dragging {
                        let mut pt = POINT { x: 0, y: 0 };
                        GetCursorPos(&mut pt);
                        let sw = GetSystemMetrics(0);
                        let sh = GetSystemMetrics(1);
                        
                        let (w, h) = widget_size().unwrap_or((MORPH_PILL_W, MORPH_PILL_H));
                        let wx = (pt.x - st.drag_start_x).clamp(0, sw - w);
                        let wy = (pt.y - st.drag_start_y).clamp(0, sh - h);
                        SetWindowPos(
                            OUR_HWND,
                            HWND_TOPMOST,
                            wx,
                            wy,
                            w,
                            h,
                            SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW,
                        );

                        let d_left = pt.x;
                        let d_right = sw - pt.x;
                        let d_bottom = sh - pt.y;
                        
                        st.drag_preview_dock = if d_bottom <= d_left && d_bottom <= d_right {
                            0
                        } else if d_left < d_right {
                            1
                        } else {
                            2
                        };
                        draw_docking_preview_overlay(st.drag_preview_dock);
                    } else if st.state == OverlayState::Idle {
                        st.hover_active = true;
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
                    // Do NOT close context menu on mouse leave so user can comfortably select options!
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
                    if st.state == OverlayState::Idle {
                        st.dragging = true;
                        SetCapture(OUR_HWND);
                        let mut pt = POINT { x: 0, y: 0 };
                        GetCursorPos(&mut pt);
                        let mut w_rect = RECT::default();
                        GetWindowRect(OUR_HWND, &mut w_rect);
                        st.drag_start_x = (pt.x - w_rect.left).max(0);
                        st.drag_start_y = (pt.y - w_rect.top).max(0);
                        st.drag_preview_dock = st.dock_side as i32;
                        ShowWindow(PREVIEW_HWND, SW_SHOWNOACTIVATE);
                        draw_docking_preview_overlay(st.drag_preview_dock);
                    }
                }
                if message.message == WM_LBUTTONUP && message.hwnd == OUR_HWND {
                    let mut st = state.lock().unwrap();
                    if st.dragging {
                        ReleaseCapture();
                        ShowWindow(PREVIEW_HWND, SW_HIDE);
                        if st.drag_preview_dock >= 0 {
                            st.dock_side = st.drag_preview_dock as u8;
                            move_to_dock_position(st.dock_side);
                        }
                        st.dragging = false;
                    }
                }
                if message.message == WM_LBUTTONDOWN && message.hwnd == OUR_HWND {
                    let x = (message.l_param & 0xFFFF) as i16 as i32;
                    let y = ((message.l_param >> 16) & 0xFFFF) as i16 as i32;
                    let mut st = state.lock().unwrap();
                    if st.state == OverlayState::Idle {
                        if st.context_menu_open {
                            let click_idx = (y - MENU_PADDING) / MENU_ITEM_H;
                            match click_idx {
                                0 => {
                                    // Cycle languages
                                    st.selected_language = match st.selected_language.as_str() {
                                        "English (US)" => "Auto-detect".to_string(),
                                        "Auto-detect" => "Spanish".to_string(),
                                        "Spanish" => "French".to_string(),
                                        "French" => "German".to_string(),
                                        _ => "English (US)".to_string(),
                                    };
                                }
                                1 => {
                                    st.context_menu_open = false;
                                }
                                2 => {
                                    st.context_menu_open = false;
                                    st.state = OverlayState::Hidden;
                                    st.hidden_until = Some(Instant::now() + Duration::from_secs(3600));
                                }
                                3 => {
                                    st.context_menu_open = false;
                                    std::thread::spawn(|| {
                                        let _ = std::process::Command::new("QuickSTT_App.exe").spawn();
                                    });
                                }
                                4 => {
                                    st.context_menu_open = false;
                                    std::thread::spawn(|| {
                                        let _ = std::process::Command::new("explorer.exe").arg(".").spawn();
                                    });
                                }
                                5 => {
                                    st.context_menu_open = false;
                                    let text = st.captured_text.clone();
                                    if !text.trim().is_empty() {
                                        std::thread::spawn(move || typing::type_text(&text));
                                    }
                                }
                                6 => {
                                    st.context_menu_open = false;
                                }
                                _ => {
                                    st.context_menu_open = false;
                                }
                            }
                        } else if st.morph_progress > 0.05 {
                            // Check if click was on the circular Mic button
                            let (w, _h) = widget_size().unwrap_or((MORPH_PILL_W, MORPH_PILL_H));
                            let mic_cx = (w / 2) as f32;
                            let mic_cy = 46.0f32;
                            let dx = x as f32 - mic_cx;
                            let dy = y as f32 - mic_cy;
                            let is_mic_click = (dx * dx + dy * dy) <= (18.0 * 18.0) || y >= 32;

                            if is_mic_click {
                                // Mic button clicked -> start recording
                                drop(st);
                                let sent = send_popup_command(&tcp_writer, &tcp_connected, b"{\"cmd\":\"popup_start\"}\n");
                                if sent {
                                    let mut st = state.lock().unwrap();
                                    begin_recording_session(&mut st);
                                }
                            } else {
                                // Capsule body clicked/dragged -> Start Drag-and-Drop Docking Mode
                                st.dragging = true;
                                SetCapture(OUR_HWND);
                                let mut pt = POINT { x: 0, y: 0 };
                                GetCursorPos(&mut pt);
                                let mut w_rect = RECT::default();
                                GetWindowRect(OUR_HWND, &mut w_rect);
                                st.drag_start_x = (pt.x - w_rect.left).max(0);
                                st.drag_start_y = (pt.y - w_rect.top).max(0);
                                st.drag_preview_dock = st.dock_side as i32;
                                ShowWindow(PREVIEW_HWND, SW_SHOWNOACTIVATE);
                                draw_docking_preview_overlay(st.drag_preview_dock);
                            }
                        } else {
                            // Idle micro pill clicked -> Start Drag-and-Drop Docking Mode
                            st.dragging = true;
                            SetCapture(OUR_HWND);
                            let mut pt = POINT { x: 0, y: 0 };
                            GetCursorPos(&mut pt);
                            let mut w_rect = RECT::default();
                            GetWindowRect(OUR_HWND, &mut w_rect);
                            st.drag_start_x = (pt.x - w_rect.left).max(0);
                            st.drag_start_y = (pt.y - w_rect.top).max(0);
                            st.drag_preview_dock = st.dock_side as i32;
                            ShowWindow(PREVIEW_HWND, SW_SHOWNOACTIVATE);
                            draw_docking_preview_overlay(st.drag_preview_dock);
                        }
                    } else {
                        let (w, h) = widget_size().unwrap_or((OV_REST_W, OV_BASE_H));
                        let cancel = cancel_button_rect(w, h);
                        if point_in_rect(x, y, &cancel) {
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

        // Animation & State Update
        let (visible, size_key, dock_side) = {
            let mut st = state.lock().unwrap();

            // Cursor proximity detection for idle micro-pill hover morphing using direct window rect
            let mut cursor = POINT::default();
            unsafe {
                if GetCursorPos(&mut cursor) != 0 && OUR_HWND != 0 {
                    let mut w_rect = RECT::default();
                    let near = if GetWindowRect(OUR_HWND, &mut w_rect) != 0 {
                        let pad = 40;
                        cursor.x >= (w_rect.left - pad) && cursor.x <= (w_rect.right + pad) &&
                        cursor.y >= (w_rect.top - pad) && cursor.y <= (w_rect.bottom + pad)
                    } else {
                        false
                    };

                    if st.state == OverlayState::Idle {
                        if st.context_menu_open {
                            st.morph_target = 1.0;
                        } else {
                            st.morph_target = if near || st.hover_active { 1.0 } else { 0.0 };
                        }
                    }
                }
            }

            // Morph transition progress
            if (st.morph_target - st.morph_progress).abs() > 0.005 {
                let speed = 0.22;
                st.morph_progress += (st.morph_target - st.morph_progress) * speed;
            } else {
                st.morph_progress = st.morph_target;
            }

            // No-signal detection
            if st.state == OverlayState::Recording && !st.no_signal_shown {
                if st.last_voice_time.elapsed() > Duration::from_secs(NO_SIGNAL_DELAY_SECS) {
                    st.no_signal_shown = true;
                    st.no_signal_since = Some(Instant::now());
                }
            }
            if st.no_signal_shown {
                if let Some(since) = st.no_signal_since {
                    if since.elapsed() > Duration::from_secs(8) {
                        st.no_signal_shown = false;
                    }
                }
            }

            // Auto-dismiss Done state
            if st.state == OverlayState::Done {
                if let Some(until) = st.show_until {
                    if Instant::now() >= until {
                        hide_session(&mut st);
                        st.show_until = None;
                    }
                }
            }

            // Smooth audio waveform damping
            for i in 0..WAVE_BARS {
                let target = st.audio_levels.get(i).copied().unwrap_or(0.0);
                st.smoothed_levels[i] = st.smoothed_levels[i] * 0.6 + target * 0.4;
            }

            let vis = st.state != OverlayState::Hidden;
            let live = !st.live_committed.is_empty() || !st.live_tentative.is_empty();
            let key = (st.state, live);
            (vis, key, st.dock_side)
        };

        if visible {
            if !was_visible {
                move_to_dock_position(dock_side);
                was_visible = true;
            }
            let st = state.lock().unwrap();
            if size_key != last_size_key || st.morph_progress > 0.0 || st.context_menu_open {
                resize_overlay_to(&st);
                last_size_key = size_key;
            }
            draw_native_overlay(&st);
        } else if was_visible {
            unsafe {
                ShowWindow(OUR_HWND, SW_HIDE);
            }
            was_visible = false;
        }

        std::thread::sleep(Duration::from_millis(16)); // ~60 FPS
    }
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
        eprintln!("[POPUP] Connecting to 127.0.0.1:19876...");
        let stream = match TcpStream::connect("127.0.0.1:19876") {
            Ok(s) => {
                eprintln!("[POPUP] Connected to QuickSTT core!");
                s
            }
            Err(_) => {
                std::thread::sleep(Duration::from_millis(500));
                continue;
            }
        };
        let wc = stream.try_clone().unwrap();
        *tcp_writer.lock().unwrap() = Some(wc);
        tcp_connected.store(true, Ordering::SeqCst);

        let reader = BufReader::new(stream);
        for line in reader.lines().flatten() {
            let msg: serde_json::Value = match serde_json::from_str(&line) {
                Ok(v) => v,
                Err(_) => continue,
            };
            let event = msg.get("event").or_else(|| msg.get("type")).and_then(|v| v.as_str()).unwrap_or("");
            match event {
                "AUDIO_LEVEL" | "AUDIO_LEVELS" => {
                    let mut st = state.lock().unwrap();
                    if let Some(arr) = msg.get("levels").and_then(|v| v.as_array()) {
                        for (i, v) in arr.iter().enumerate().take(WAVE_BARS) {
                            st.audio_levels[i] = v.as_f64().unwrap_or(0.0) as f32;
                        }
                    } else if let Some(lvl) = msg.get("level").and_then(|v| v.as_f64()) {
                        let raw = (lvl as f32 / 100.0).clamp(0.0, 1.0);
                        st.audio_levels.remove(0);
                        st.audio_levels.push(raw);
                    }
                    if st.audio_levels.iter().any(|&l| l > 0.06) {
                        st.last_voice_time = Instant::now();
                        st.no_signal_shown = false;
                        st.no_signal_since = None;
                    }
                }
                "STREAM_TEXT" | "LIVE_TEXT" => {
                    let mut st = state.lock().unwrap();
                    if st.commit_sent || (st.state != OverlayState::Recording && st.state != OverlayState::Transcribing) {
                        continue;
                    }
                    st.streaming_mode = true;
                    if let Some(committed) = msg.get("committed").and_then(|v| v.as_str()) {
                        st.live_committed = committed.to_string();
                    }
                    if let Some(tentative) = msg.get("tentative").or_else(|| msg.get("partial")).and_then(|v| v.as_str()) {
                        st.live_tentative = tentative.to_string();
                    }
                }
                "FINAL_TEXT" => {
                    let text = msg.get("text").and_then(|v| v.as_str()).unwrap_or("").to_string();
                    eprintln!("[POPUP] FINAL_TEXT: '{}'", text);
                    let mut st = state.lock().unwrap();
                    st.final_text = text.clone();
                    st.commit_sent = true;
                    st.session_live = false;
                    let output_mode = st.output_mode;
                    let typed_len = st.live_typed_len;
                    if !text.trim().is_empty() {
                        st.captured_text = text.clone();
                        st.state = OverlayState::Done;
                        st.show_until = Some(Instant::now() + Duration::from_millis(900));
                        std::thread::spawn(move || {
                            if output_mode == 1 {
                                typing::copy_only(&text);
                            } else if typed_len > 0 && text.len() > typed_len {
                                typing::type_text(&text[typed_len..]);
                            } else if typed_len == 0 {
                                typing::type_text(&text);
                            }
                        });
                    } else {
                        hide_session(&mut st);
                    }
                }
                "CONFIG" => {
                    let mut st = state.lock().unwrap();
                    if let Some(m) = msg.get("mode").and_then(|v| v.as_u64()) {
                        st.activation_mode = m as u8;
                    }
                    if let Some(o) = msg.get("output").and_then(|v| v.as_u64()) {
                        st.output_mode = o as u8;
                    }
                    if let Some(on) = msg.get("always_on_pill").and_then(|v| v.as_bool()) {
                        st.always_on_pill = on;
                        if on && st.state == OverlayState::Hidden {
                            st.state = OverlayState::Idle;
                        } else if !on && st.state == OverlayState::Idle {
                            st.state = OverlayState::Hidden;
                        }
                    }
                }
                "STATE" => {
                    let state_text = msg.get("text").and_then(|v| v.as_str()).unwrap_or("");
                    if matches!(state_text.trim(), "Ready" | "Hidden" | "Paused" | "Model Missing") {
                        let mut st = state.lock().unwrap();
                        if st.state == OverlayState::Done || st.state == OverlayState::Transcribing {
                            hide_session(&mut st);
                        }
                    }
                }
                _ => {}
            }
        }
        tcp_connected.store(false, Ordering::SeqCst);
        *tcp_writer.lock().unwrap() = None;
        std::thread::sleep(Duration::from_millis(500));
    }
}

// ─── Main Entry Point ───────────────────────────────────────────────────────

fn main() {
    eprintln!("[POPUP] QuickSTT high-performance native Win32 overlay starting...");

    let state = Arc::new(Mutex::new(PopupState::default()));
    let tcp_writer: Arc<Mutex<Option<std::net::TcpStream>>> = Arc::new(Mutex::new(None));
    let tcp_connected = Arc::new(AtomicBool::new(false));

    let _ = CANCEL_TCP_WRITER.set(Arc::clone(&tcp_writer));
    let _ = CANCEL_TCP_CONNECTED.set(Arc::clone(&tcp_connected));

    // Keyboard Hook Thread
    let (event_tx, event_rx) = mpsc::channel::<HotkeyEvent>();
    let _hook_thread = std::thread::spawn(move || {
        hotkey::run_hotkey_loop(event_tx);
    });

    // Hotkey Event Handler Thread with 500ms trailing audio capture buffer
    {
        let state = Arc::clone(&state);
        let tcp_writer = Arc::clone(&tcp_writer);
        let tcp_connected = Arc::clone(&tcp_connected);
        std::thread::spawn(move || {
            while let Ok(event) = event_rx.recv() {
                match event {
                    HotkeyEvent::Press => {
                        eprintln!("[POPUP] Ctrl+Space PTT PRESS");
                        {
                            let mut st = state.lock().unwrap();
                            begin_recording_session(&mut st);
                        }
                        let _ = send_popup_command(&tcp_writer, &tcp_connected, b"{\"cmd\":\"popup_start\"}\n");
                    }
                    HotkeyEvent::Release => {
                        eprintln!("[POPUP] Ctrl+Space PTT RELEASE -> buffering 500ms trailing audio...");
                        {
                            let mut st = state.lock().unwrap();
                            if st.state == OverlayState::Recording {
                                st.state = OverlayState::Transcribing;
                                st.release_requested = true;
                                st.transcribing_since = Some(Instant::now());
                            }
                        }
                        // 500ms trailing audio capture drain so the last spoken words are never truncated
                        std::thread::sleep(Duration::from_millis(500));
                        let _ = send_popup_command(&tcp_writer, &tcp_connected, b"{\"cmd\":\"popup_stop\"}\n");
                    }
                }
            }
        });
    }

    // TCP Receiver Thread
    {
        let state = Arc::clone(&state);
        let tcp_writer = Arc::clone(&tcp_writer);
        let tcp_connected = Arc::clone(&tcp_connected);
        std::thread::spawn(move || {
            tcp_loop(state, tcp_writer, tcp_connected);
        });
    }

    // Run native paint and message pump loop
    let (dummy_tx, dummy_rx) = mpsc::channel::<HotkeyEvent>();
    drop(dummy_tx);
    run_native_overlay(state, dummy_rx);
}
