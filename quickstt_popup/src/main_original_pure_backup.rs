//! QuickSTT Popup — Hardware-accelerated overlay powered by Slint framework
//! Hold Ctrl+Space = show + record, Release = stop + type + hide.

mod hotkey;
mod typing;

use std::ffi::c_void;
use std::io::{BufRead, BufReader, Write};
use std::net::TcpStream;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use hotkey::HotkeyEvent;
use serde_json::Value;

slint::include_modules!();

// Win32 API for cursor tracking, monitor geometry, and window positioning
#[link(name = "user32")]
extern "system" {
    fn GetCursorPos(lpPoint: *mut POINT) -> i32;
    fn GetForegroundWindow() -> isize;
    fn MonitorFromPoint(pt: POINT, dwFlags: u32) -> isize;
    fn MonitorFromWindow(hWnd: isize, dwFlags: u32) -> isize;
    fn GetMonitorInfoW(hMonitor: isize, lpmi: *mut MONITORINFO) -> i32;
    fn SetWindowPos(
        hWnd: isize,
        hWndInsertAfter: isize,
        x: i32,
        y: i32,
        cx: i32,
        cy: i32,
        uFlags: u32,
    ) -> i32;
    fn GetWindowLongPtrW(hWnd: isize, nIndex: i32) -> isize;
    fn SetWindowLongPtrW(hWnd: isize, nIndex: i32, dwNewLong: isize) -> isize;
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct POINT {
    x: i32,
    y: i32,
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
struct MONITORINFO {
    cb_size: u32,
    rc_monitor: RECT,
    rc_work: RECT,
    dw_flags: u32,
}

const MONITOR_DEFAULTTONEAREST: u32 = 2;
const GWL_EXSTYLE: i32 = -20;
const WS_EX_TOOLWINDOW: isize = 0x00000080;
const WS_EX_TOPMOST: isize = 0x00000008;
const WS_EX_NOACTIVATE: isize = 0x08000000;
const HWND_TOPMOST: isize = -1;
const SWP_NOACTIVATE: u32 = 0x0010;
const SWP_SHOWWINDOW: u32 = 0x0040;

const TCP_PORT: u16 = 19876;
const WAVE_BARS: usize = 8;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum OverlayState {
    Hidden = 0,
    Idle = 1,
    Recording = 2,
    Transcribing = 3,
    Done = 4,
}

struct PopupState {
    state: OverlayState,
    activation_mode: u8,
    output_mode: u8,
    audio_levels: [f32; WAVE_BARS],
    live_committed: String,
    live_tentative: String,
    live_typed_len: usize,
    captured_text: String,
    final_text: String,
    last_transcript: String,
    streaming_mode: bool,
    session_live: bool,
    session_started: Option<Instant>,
    commit_sent: bool,
    release_requested: bool,
    show_until: Option<Instant>,
    transcribing_since: Option<Instant>,
    always_on_pill: bool,
    dock_side: u8, // 0=bottom, 1=left, 2=right
    morph_progress: f32,
    morph_target: f32,
    hover_active: bool,
}

impl Default for PopupState {
    fn default() -> Self {
        Self {
            state: OverlayState::Idle,
            activation_mode: 0,
            output_mode: 0,
            audio_levels: [0.0; WAVE_BARS],
            live_committed: String::new(),
            live_tentative: String::new(),
            live_typed_len: 0,
            captured_text: String::new(),
            final_text: String::new(),
            last_transcript: String::new(),
            streaming_mode: false,
            session_live: false,
            session_started: None,
            commit_sent: false,
            release_requested: false,
            show_until: None,
            transcribing_since: None,
            always_on_pill: true,
            dock_side: 0,
            morph_progress: 0.0,
            morph_target: 0.0,
            hover_active: false,
        }
    }
}

static TCP_WRITER: std::sync::OnceLock<Arc<Mutex<Option<TcpStream>>>> = std::sync::OnceLock::new();

fn send_tcp(cmd: &str) {
    if let Some(writer_arc) = TCP_WRITER.get() {
        if let Ok(mut lock) = writer_arc.lock() {
            if let Some(ref mut stream) = *lock {
                let _ = writeln!(stream, "{}", cmd);
                let _ = stream.flush();
            }
        }
    }
}

fn commit_text(text: String, output_mode: u8, live_typed_len: usize) {
    let trimmed = text.trim().to_string();
    if trimmed.is_empty() {
        return;
    }
    eprintln!("[POPUP] Committing transcript: '{}' (mode={})", trimmed, output_mode);
    std::thread::spawn(move || {
        typing::commit_transcript(&trimmed, output_mode, live_typed_len);
    });
}

fn get_work_area() -> RECT {
    unsafe {
        let foreground = GetForegroundWindow();
        let mut cursor = POINT::default();
        let monitor = if foreground != 0 {
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

        if monitor != 0 && GetMonitorInfoW(monitor, &mut info) != 0 {
            info.rc_work
        } else {
            RECT { left: 0, top: 0, right: 1920, bottom: 1080 }
        }
    }
}

fn main() {
    eprintln!("[POPUP] QuickSTT popup starting (Slint high-performance engine)...");

    let popup = PopupOverlay::new().unwrap();
    let popup_handle = popup.as_weak();

    let state = Arc::new(Mutex::new(PopupState::default()));
    let tcp_writer: Arc<Mutex<Option<TcpStream>>> = Arc::new(Mutex::new(None));
    let _ = TCP_WRITER.set(Arc::clone(&tcp_writer));
    let tcp_connected = Arc::new(AtomicBool::new(false));

    // ── Setup Slint Callbacks ──
    {
        let _st = Arc::clone(&state);
        popup.on_mic_clicked(move || {
            send_tcp("POPUP_START");
        });
    }

    {
        let st = Arc::clone(&state);
        popup.on_cancel_clicked(move || {
            let mut s = st.lock().unwrap();
            s.state = if s.always_on_pill { OverlayState::Idle } else { OverlayState::Hidden };
            s.session_live = false;
            s.commit_sent = true;
            send_tcp("POPUP_CANCEL");
        });
    }

    {
        let st = Arc::clone(&state);
        popup.on_hover_changed(move |hovered| {
            let mut s = st.lock().unwrap();
            s.hover_active = hovered;
            s.morph_target = if hovered { 1.0 } else { 0.0 };
        });
    }

    // ── Keyboard Hook Thread (Ctrl+Space PTT) ──
    let (event_tx, event_rx) = mpsc::channel::<HotkeyEvent>();
    let _hook_thread = std::thread::spawn(move || {
        hotkey::install_hook(event_tx);
    });

    // ── Hotkey Event Handler Thread ──
    {
        let state = Arc::clone(&state);
        let _tcp_conn = Arc::clone(&tcp_connected);
        std::thread::spawn(move || {
            while let Ok(event) = event_rx.recv() {
                match event {
                    HotkeyEvent::Press => {
                        eprintln!("[POPUP] Ctrl+Space PTT PRESS");
                        {
                            let mut st = state.lock().unwrap();
                            st.state = OverlayState::Recording;
                            st.session_live = true;
                            st.session_started = Some(Instant::now());
                            st.commit_sent = false;
                            st.release_requested = false;
                            st.live_committed.clear();
                            st.live_tentative.clear();
                            st.captured_text.clear();
                            st.final_text.clear();
                            st.live_typed_len = 0;
                        }
                        send_tcp("POPUP_START");
                    }
                    HotkeyEvent::Release => {
                        eprintln!("[POPUP] Ctrl+Space PTT RELEASE");
                        {
                            let mut st = state.lock().unwrap();
                            if st.state == OverlayState::Recording {
                                st.state = OverlayState::Transcribing;
                                st.release_requested = true;
                                st.transcribing_since = Some(Instant::now());
                            }
                        }
                        send_tcp("POPUP_STOP");
                    }
                }
            }
        });
    }

    // ── TCP Receiver Thread ──
    {
        let state = Arc::clone(&state);
        let tcp_writer = Arc::clone(&tcp_writer);
        let tcp_connected = Arc::clone(&tcp_connected);
        std::thread::spawn(move || {
            loop {
                eprintln!("[POPUP] Connecting to QuickSTT core (port {})...", TCP_PORT);
                let stream = match TcpStream::connect(format!("127.0.0.1:{}", TCP_PORT)) {
                    Ok(s) => s,
                    Err(_) => {
                        std::thread::sleep(Duration::from_millis(500));
                        continue;
                    }
                };
                eprintln!("[POPUP] Connected!");
                tcp_connected.store(true, Ordering::SeqCst);
                *tcp_writer.lock().unwrap() = Some(stream.try_clone().unwrap());

                let reader = BufReader::new(stream);
                for line in reader.lines().flatten() {
                    let msg: Value = match serde_json::from_str(&line) {
                        Ok(v) => v,
                        Err(_) => continue,
                    };
                    let msg_type = msg.get("type").and_then(|v| v.as_str()).unwrap_or("");
                    match msg_type {
                        "AUDIO_LEVELS" => {
                            if let Some(arr) = msg.get("levels").and_then(|v| v.as_array()) {
                                let mut st = state.lock().unwrap();
                                for (i, v) in arr.iter().enumerate().take(WAVE_BARS) {
                                    st.audio_levels[i] = v.as_f64().unwrap_or(0.0) as f32;
                                }
                            }
                        }
                        "FINAL_TEXT" => {
                            let text = msg.get("text").and_then(|v| v.as_str()).unwrap_or("").to_string();
                            eprintln!("[POPUP] FINAL_TEXT received: '{}'", text);
                            let commit = {
                                let mut st = state.lock().unwrap();
                                st.final_text = text.clone();
                                st.commit_sent = true;
                                st.session_live = false;
                                let typed_len = st.live_typed_len;
                                if !text.trim().is_empty() {
                                    st.last_transcript = text.clone();
                                    st.state = OverlayState::Done;
                                    st.show_until = Some(Instant::now() + Duration::from_millis(800));
                                    Some((text, st.output_mode, typed_len))
                                } else {
                                    st.state = if st.always_on_pill { OverlayState::Idle } else { OverlayState::Hidden };
                                    None
                                }
                            };
                            if let Some((t, mode, typed)) = commit {
                                commit_text(t, mode, typed);
                            }
                        }
                        "LIVE_TEXT" => {
                            let committed = msg.get("committed").and_then(|v| v.as_str()).unwrap_or("").to_string();
                            let tentative = msg.get("tentative").or_else(|| msg.get("partial")).and_then(|v| v.as_str()).unwrap_or("").to_string();
                            let mut delta_type = None;
                            {
                                let mut st = state.lock().unwrap();
                                if st.commit_sent || (st.state != OverlayState::Recording && st.state != OverlayState::Transcribing) {
                                    continue;
                                }
                                st.streaming_mode = true;
                                if !committed.is_empty() && committed != st.live_committed {
                                    if committed.starts_with(&st.live_committed) {
                                        let delta = committed[st.live_committed.len()..].to_string();
                                        if !delta.trim().is_empty() {
                                            delta_type = Some(delta);
                                        }
                                    }
                                    st.live_committed = committed;
                                    st.live_typed_len = st.live_committed.len();
                                }
                                st.live_tentative = tentative;
                            }
                            if let Some(text) = delta_type {
                                std::thread::spawn(move || typing::type_text_live(&text));
                            }
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
                        }
                        "STATE" => {
                            let state_text = msg.get("text").and_then(|v| v.as_str()).unwrap_or("");
                            if matches!(state_text.trim(), "Ready" | "Hidden" | "Paused" | "Model Missing") {
                                let mut st = state.lock().unwrap();
                                if st.state == OverlayState::Done || st.state == OverlayState::Transcribing {
                                    st.state = if st.always_on_pill { OverlayState::Idle } else { OverlayState::Hidden };
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
        });
    }

    // ── Slint UI Animation & Property Sync Loop (60 FPS) ──
    let timer = slint::Timer::default();
    {
        let state = Arc::clone(&state);
        let popup_handle = popup_handle.clone();
        let mut last_window_x = -9999;
        let mut last_window_y = -9999;
        let mut last_window_w = 0;
        let mut last_window_h = 0;

        timer.start(slint::TimerMode::Repeated, Duration::from_millis(16), move || {
            let Some(popup) = popup_handle.upgrade() else { return; };
            let (cur_state, dock_side, morph_progress, live_text, has_live, levels, done_deadline) = {
                let mut st = state.lock().unwrap();

                // Smooth morph progress interpolation
                let speed = 0.20;
                st.morph_progress += (st.morph_target - st.morph_progress) * speed;
                if (st.morph_target - st.morph_progress).abs() < 0.01 {
                    st.morph_progress = st.morph_target;
                }

                // Check cursor position for hover hit testing on idle micro pill
                let mut cursor = POINT::default();
                if unsafe { GetCursorPos(&mut cursor) } != 0 {
                    let work = get_work_area();
                    let hit = match st.dock_side {
                        1 => cursor.x >= work.left && cursor.x <= work.left + 220 && (cursor.y - ((work.bottom + work.top) / 2)).abs() < 40,
                        2 => cursor.x >= work.right - 220 && cursor.x <= work.right && (cursor.y - ((work.bottom + work.top) / 2)).abs() < 40,
                        _ => cursor.y >= work.bottom - 60 && cursor.y <= work.bottom && (cursor.x - ((work.left + work.right) / 2)).abs() < 120,
                    };
                    if st.state == OverlayState::Idle {
                        st.morph_target = if hit { 1.0 } else { 0.0 };
                    }
                }

                // Done timer auto-dismiss
                if st.state == OverlayState::Done {
                    if let Some(deadline) = st.show_until {
                        if Instant::now() > deadline {
                            st.state = if st.always_on_pill { OverlayState::Idle } else { OverlayState::Hidden };
                            st.show_until = None;
                        }
                    }
                }

                let mut display_text = st.live_committed.clone();
                if !st.live_tentative.is_empty() {
                    if !display_text.is_empty() && !display_text.ends_with(' ') {
                        display_text.push(' ');
                    }
                    display_text.push_str(&st.live_tentative);
                }
                let has_live = !display_text.trim().is_empty();

                (
                    st.state,
                    st.dock_side as i32,
                    st.morph_progress,
                    display_text,
                    has_live,
                    st.audio_levels,
                    st.show_until,
                )
            };

            let _ = done_deadline;
            // Update Slint properties
            popup.set_overlay_state(cur_state as i32);
            popup.set_dock_side(dock_side);
            popup.set_morph_progress(morph_progress);
            popup.set_live_text(slint::SharedString::from(live_text));
            popup.set_has_live_text(has_live);

            let model = slint::ModelRc::new(slint::VecModel::from(levels.to_vec()));
            popup.set_waveform_levels(model);

            // Compute dynamic Slint window dimensions & screen placement
            let (target_w, target_h) = match cur_state {
                OverlayState::Hidden => (1, 1),
                OverlayState::Idle => (200, 48),
                OverlayState::Recording => if has_live { (340, 110) } else { (280, 48) },
                OverlayState::Transcribing => (240, 48),
                OverlayState::Done => (160, 48),
            };

            let work = get_work_area();
            let gap = 6;
            let (target_x, target_y) = match dock_side {
                1 => (work.left + gap, work.top + ((work.bottom - work.top - target_h) / 2)),
                2 => (work.right - target_w - gap, work.top + ((work.bottom - work.top - target_h) / 2)),
                _ => (
                    work.left + ((work.right - work.left - target_w) / 2),
                    work.bottom - target_h - gap,
                ),
            };

            if target_w != last_window_w || target_h != last_window_h {
                popup.window().set_size(slint::PhysicalSize::new(target_w as u32, target_h as u32));
                last_window_w = target_w;
                last_window_h = target_h;
            }

            if target_x != last_window_x || target_y != last_window_y {
                popup.window().set_position(slint::PhysicalPosition::new(target_x, target_y));
                last_window_x = target_x;
                last_window_y = target_y;
            }
        });
    }

    popup.show().unwrap();
    slint::run_event_loop().unwrap();
}
