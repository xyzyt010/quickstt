#![cfg_attr(target_os = "windows", windows_subsystem = "windows")]

use eframe::egui;
use egui::{
    pos2, vec2, Color32, CursorIcon, FontId, Id, Pos2, Rect, Rounding, Sense, Stroke, Vec2,
    ViewportBuilder, ViewportCommand, ViewportId,
};
use global_hotkey::{
    hotkey::{Code, HotKey, Modifiers},
    GlobalHotKeyEvent, GlobalHotKeyManager,
};
use quickstt_core::orchestration::{
    AppMode, AppOrchestrator, AppState, ModelEntry, OrchestratorCommand,
};
use quickstt_gui::{icons::IconSet, theme, waveform};
use std::sync::OnceLock;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use tokio::sync::mpsc;
use tracing::{info, warn};
use tray_icon::{MouseButtonState, TrayIcon, TrayIconBuilder, TrayIconEvent};
use muda::{Menu, MenuEvent, MenuId, MenuItem, PredefinedMenuItem};

mod widget_platform;

/// Fixed tray-menu item ids (stable across the GTK tray thread and main thread).
const TRAY_ID_DASH: &str = "quickstt-dash";
const TRAY_ID_SHOW: &str = "quickstt-show";
const TRAY_ID_HIDE: &str = "quickstt-hide";
const TRAY_ID_QUIT: &str = "quickstt-quit";

/// TCP port used for single-instance IPC. The first instance binds a listener
/// on 127.0.0.1 and later invocations (`quickstt --show` etc.) forward the
/// command here and exit.
const IPC_PORT: u16 = 47631;

#[derive(Debug, Clone, Copy, PartialEq)]
enum CliCommand {
    Show,
    Hide,
    Toggle,
    Dashboard,
    Quit,
    None,
}

fn parse_cli() -> (CliCommand, bool) {
    let mut cmd = CliCommand::None;
    let mut background = false;
    for arg in std::env::args().skip(1) {
        match arg.as_str() {
            "--show" => cmd = CliCommand::Show,
            "--hide" => cmd = CliCommand::Hide,
            "--toggle" => cmd = CliCommand::Toggle,
            "--dashboard" => cmd = CliCommand::Dashboard,
            "--quit" => cmd = CliCommand::Quit,
            "--background" | "-b" => background = true,
            _ => {}
        }
    }
    (cmd, background)
}

/// Try to forward a CLI command to an already-running instance. Returns true
/// when another instance accepted the command.
fn forward_to_running_instance(cmd: CliCommand) -> bool {
    use std::io::Write;
    let payload = match cmd {
        CliCommand::Show => "SHOW\n",
        CliCommand::Hide => "HIDE\n",
        CliCommand::Toggle => "TOGGLE\n",
        CliCommand::Dashboard => "DASH\n",
        CliCommand::Quit => "QUIT\n",
        CliCommand::None => return false,
    };
    if cmd == CliCommand::None {
        return false;
    }
    match std::net::TcpStream::connect_timeout(
        &std::net::SocketAddr::from(([127, 0, 0, 1], IPC_PORT)),
        Duration::from_millis(400),
    ) {
        Ok(mut stream) => {
            let _ = stream.write_all(payload.as_bytes());
            let _ = stream.flush();
            info!("Forwarded {:?} to running instance", cmd);
            true
        }
        Err(_) => false,
    }
}

/// Spawn the single-instance listener. Commands received are applied directly
/// to the shared state / dash so the GUI picks them up on the next frame.
fn spawn_ipc_listener(state: Arc<Mutex<AppState>>, dash: Arc<Mutex<DashboardState>>) {
    std::thread::Builder::new()
        .name("ipc-listener".to_string())
        .spawn(move || {
            let addr = std::net::SocketAddr::from(([127, 0, 0, 1], IPC_PORT));
            let Ok(listener) = std::net::TcpListener::bind(addr) else {
                warn!("IPC port {} busy — another instance owns it", IPC_PORT);
                return;
            };
            info!("IPC listener on {}", addr);
            use std::io::Read;
            for stream in listener.incoming() {
                let Ok(mut stream) = stream else { continue };
                let mut buf = [0u8; 32];
                let n = stream.read(&mut buf).unwrap_or(0);
                let msg = String::from_utf8_lossy(&buf[..n]).trim().to_string();
                match msg.as_str() {
                    "SHOW" => {
                        if let Ok(mut s) = state.lock() {
                            s.widget_visible = true;
                            if s.status_message.eq_ignore_ascii_case("Hidden") {
                                s.status_message = "Ready".into();
                            }
                        }
                    }
                    "HIDE" => {
                        if let Ok(mut s) = state.lock() {
                            s.widget_visible = false;
                            s.status_message = "Hidden".into();
                        }
                    }
                    "TOGGLE" => {
                        if let Ok(mut s) = state.lock() {
                            s.widget_visible = !s.widget_visible;
                        }
                    }
                    "DASH" => {
                        if let Ok(mut d) = dash.lock() {
                            d.visible = true;
                        }
                    }
                    "QUIT" => {
                        info!("Quit requested via IPC");
                        std::process::exit(0);
                    }
                    _ => {}
                }
            }
        })
        .expect("spawn ipc listener");
}

fn load_icon_data() -> Option<egui::IconData> {
    // Primary: render the canonical app-logo SVG (same artwork as the pill).
    if let Some((rgba, width, height)) =
        quickstt_gui::icons::render_app_icon_rgba(256)
    {
        return Some(egui::IconData {
            rgba,
            width,
            height,
        });
    }
    // Fallback: prebuilt ICO.
    let ico_bytes = include_bytes!("../../assets/icon_app.ico");
    if let Ok(img) = image::load_from_memory_with_format(ico_bytes, image::ImageFormat::Ico) {
        let rgba = img.to_rgba8();
        let (width, height) = rgba.dimensions();
        Some(egui::IconData {
            rgba: rgba.into_raw(),
            width,
            height,
        })
    } else {
        None
    }
}

static ICON_DATA: OnceLock<Option<egui::IconData>> = OnceLock::new();

fn get_icon_data() -> Option<&'static egui::IconData> {
    ICON_DATA.get_or_init(|| load_icon_data()).as_ref()
}

// --- Windows DXGI GPU detection and registry helpers ---
#[cfg(target_os = "windows")]
use windows::{
    Win32::Foundation::{HWND, POINT},
    Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory1, DXGI_ADAPTER_DESC1},
    Win32::UI::WindowsAndMessaging::{GetCursorPos, GetSystemMetrics, SM_CXSCREEN, SM_CYSCREEN},
};

#[cfg(target_os = "windows")]
use raw_window_handle::HasWindowHandle;

#[derive(Clone, Debug)]
#[allow(dead_code)]
struct ComputeTargetInfo {
    id: String,
    display_name: String,
    vendor_name: String,
    backend_label: String,
    dedicated_vram_mb: u32,
    shared_memory_mb: u32,
    system_memory_mb: u32,
    integrated: bool,
    is_cpu_fallback: bool,
    local_acceleration_detected: bool,
}

#[allow(dead_code)]
fn detect_compute_targets() -> Vec<ComputeTargetInfo> {
    let mut targets = Vec::new();

    #[cfg(target_os = "linux")]
    {
        // Linux: try lspci + /proc
        if let Ok(out) = std::process::Command::new("lspci").arg("-nn").output() {
            let text = String::from_utf8_lossy(&out.stdout).to_lowercase();
            if text.contains("nvidia") {
                targets.push(ComputeTargetInfo { id: "gpu_nvidia".into(), display_name: "NVIDIA GPU".into(), vendor_name: "NVIDIA".into(), backend_label: "CUDA".into(), dedicated_vram_mb: 0, shared_memory_mb: 0, system_memory_mb: 16384, integrated: false, is_cpu_fallback: false, local_acceleration_detected: true });
            } else if text.contains("amd") || text.contains("advanced micro") {
                targets.push(ComputeTargetInfo { id: "gpu_amd".into(), display_name: "AMD GPU".into(), vendor_name: "AMD".into(), backend_label: "Vulkan GPU".into(), dedicated_vram_mb: 0, shared_memory_mb: 0, system_memory_mb: 16384, integrated: false, is_cpu_fallback: false, local_acceleration_detected: true });
            } else if text.contains("intel") && (text.contains("vga") || text.contains("display")) {
                targets.push(ComputeTargetInfo { id: "gpu_intel".into(), display_name: "Intel GPU".into(), vendor_name: "Intel".into(), backend_label: "OpenVINO GPU".into(), dedicated_vram_mb: 0, shared_memory_mb: 0, system_memory_mb: 16384, integrated: true, is_cpu_fallback: false, local_acceleration_detected: true });
            }
        }
    }

    #[cfg(target_os = "windows")]
    unsafe {
        if let Ok(factory) = CreateDXGIFactory1::<IDXGIFactory1>() {
            let mut index = 0;
            while let Ok(adapter) = factory.EnumAdapters1(index) {
                let mut desc: DXGI_ADAPTER_DESC1 = std::mem::zeroed();
                if adapter.GetDesc1(&mut desc).is_ok() {
                    let is_software = (desc.Flags & 1) != 0; // DXGI_ADAPTER_FLAG_SOFTWARE = 1
                    if !is_software {
                        let display_name = String::from_utf16_lossy(&desc.Description);
                        let display_name = display_name.trim_matches('\0').trim().to_string();
                        let id = format!(
                            "gpu_{:x}_{:x}",
                            desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart
                        );
                        let dedicated_vram_mb = (desc.DedicatedVideoMemory / (1024 * 1024)) as u32;
                        let shared_memory_mb = (desc.SharedSystemMemory / (1024 * 1024)) as u32;
                        let system_memory_mb = (desc.DedicatedSystemMemory / (1024 * 1024)) as u32;
                        let integrated = dedicated_vram_mb == 0;

                        let (vendor_name, backend_label, local_acceleration_detected) = match desc
                            .VendorId
                        {
                            0x10DE => ("NVIDIA".to_string(), "CUDA".to_string(), true),
                            0x8086 => ("Intel".to_string(), "OpenVINO GPU".to_string(), true),
                            0x1002 | 0x1022 => ("AMD".to_string(), "Vulkan GPU".to_string(), true),
                            v => (
                                format!("Vendor 0x{:04X}", v),
                                "Detection only".to_string(),
                                false,
                            ),
                        };

                        targets.push(ComputeTargetInfo {
                            id,
                            display_name,
                            vendor_name,
                            backend_label,
                            dedicated_vram_mb,
                            shared_memory_mb,
                            system_memory_mb,
                            integrated,
                            is_cpu_fallback: false,
                            local_acceleration_detected,
                        });
                    }
                }
                index += 1;
            }
        }
    }

    let system_memory_mb = 16384;

    targets.push(ComputeTargetInfo {
        id: "cpu".to_string(),
        display_name: "CPU Fallback".to_string(),
        vendor_name: "System CPU".to_string(),
        backend_label: "Native Vosk CPU".to_string(),
        dedicated_vram_mb: 0,
        shared_memory_mb: 0,
        system_memory_mb,
        integrated: true,
        is_cpu_fallback: true,
        local_acceleration_detected: false,
    });

    targets
}

#[allow(dead_code)]
fn get_cursor_position() -> Pos2 {
    #[cfg(target_os = "windows")]
    unsafe {
        let mut pt = windows::Win32::Foundation::POINT::default();
        if windows::Win32::UI::WindowsAndMessaging::GetCursorPos(&mut pt).is_ok() {
            return pos2(pt.x as f32, pt.y as f32);
        }
    }
    #[cfg(target_os = "linux")]
    {
        // Try X11 query via winit cursor position cache; fallback to center
        // Actual cursor is tracked via egui input hover_pos in render loop
    }
    pos2(100.0, 100.0)
}

fn get_screen_size() -> (f32, f32) {
    #[cfg(target_os = "windows")]
    unsafe {
        let cx = windows::Win32::UI::WindowsAndMessaging::GetSystemMetrics(windows::Win32::UI::WindowsAndMessaging::SM_CXSCREEN);
        let cy = windows::Win32::UI::WindowsAndMessaging::GetSystemMetrics(windows::Win32::UI::WindowsAndMessaging::SM_CYSCREEN);
        (cx as f32, cy as f32)
    }
    #[cfg(target_os = "linux")]
    {
        // Parse `xrandr --current` for the primary mode, e.g. "1920x1080*+".
        if let Ok(out) = std::process::Command::new("xrandr").arg("--current").output() {
            let text = String::from_utf8_lossy(&out.stdout);
            for line in text.lines() {
                if !line.contains('*') {
                    continue;
                }
                // Line looks like: "   1920x1080     60.02*+ ..."
                for token in line.split_whitespace() {
                    let dims: Vec<&str> = token.split('x').collect();
                    if dims.len() == 2 {
                        let w = dims[0].trim().parse::<f32>().ok();
                        let h = dims[1]
                            .trim_end_matches(|c: char| !c.is_ascii_digit())
                            .parse::<f32>()
                            .ok();
                        if let (Some(w), Some(h)) = (w, h) {
                            return (w, h);
                        }
                    }
                }
            }
        }
        (1920.0, 1080.0)
    }
    #[cfg(not(any(target_os = "windows", target_os = "linux")))]
    (1920.0, 1080.0)
}

fn deliver_transcription_output(delta: &str, output_mode: u32) {
    if delta.trim().is_empty() { return; }
    let text = format!("{} ", delta.trim());
    let esc = text.replace('\'', "'\\''");
    match output_mode {
        1 => {
            let _ = std::process::Command::new("sh").arg("-c")
                .arg(format!("printf '{}' | wl-copy 2>/dev/null || printf '{}' | xclip -selection clipboard 2>/dev/null || printf '{}' | xsel --clipboard 2>/dev/null || true", esc, esc, esc))
                .spawn();
        }
        2 => {} // none
        _ => {
            let cmd = if std::process::Command::new("which").arg("wtype").output().map(|o| o.status.success()).unwrap_or(false) {
                format!("wtype '{}' 2>/dev/null &", esc)
            } else if std::process::Command::new("which").arg("ydotool").output().map(|o| o.status.success()).unwrap_or(false) {
                format!("ydotool type '{}' 2>/dev/null &", esc)
            } else {
                format!("xdotool type --clearmodifiers --delay 0 '{}' 2>/dev/null &", esc)
            };
            let _ = std::process::Command::new("sh").arg("-c").arg(cmd).spawn();
        }
    }
}

#[allow(dead_code)]
fn read_registry_bool(key_name: &str, default: bool) -> bool {
    #[cfg(target_os = "windows")]
    unsafe {
        use windows::core::PCWSTR;
        use windows::Win32::System::Registry::{
            RegCloseKey, RegOpenKeyExW, RegQueryValueExW, HKEY_CURRENT_USER, KEY_READ,
        };
        let subkey: Vec<u16> = "Software\\QuickSTT\\Config"
            .encode_utf16()
            .chain(std::iter::once(0))
            .collect();
        let mut hkey = windows::Win32::System::Registry::HKEY::default();
        if RegOpenKeyExW(
            HKEY_CURRENT_USER,
            PCWSTR(subkey.as_ptr()),
            0,
            KEY_READ,
            &mut hkey,
        )
        .is_ok()
        {
            let wide_name: Vec<u16> = key_name.encode_utf16().chain(std::iter::once(0)).collect();
            let mut data_type = windows::Win32::System::Registry::REG_VALUE_TYPE::default();
            let mut val = 0u32;
            let mut size = 4u32;
            let res = RegQueryValueExW(
                hkey,
                PCWSTR(wide_name.as_ptr()),
                None,
                Some(&mut data_type),
                Some(&mut val as *mut u32 as *mut u8),
                Some(&mut size),
            );
            let _ = RegCloseKey(hkey);
            if res.is_ok() {
                return val != 0;
            }
        }
    }
    #[cfg(not(target_os = "windows"))]
    {
        // Linux: read from XDG TOML via Settings extra map
        if let Ok(settings) = quickstt_core::settings::Settings::load() {
            if let Some(v) = settings.extra.get(key_name) {
                return v.eq_ignore_ascii_case("true") || v == "1";
            }
            // Check known bool fields
            match key_name {
                "onCommandTranscription" => return false, // default
                _ => {}
            }
        }
    }
    default
}

// ── Shared GUI and dashboard state ──

#[derive(Clone)]
struct DashboardState {
    visible: bool,
    tab: usize,
    pill_radius: f32,
    opacity_pct: f32,
    icon_size: f32,
    tb_opacity_pct: f32,
    tb_text_size: f32,
    offload_enabled: bool,
    offload_secs: u32,
    show_waveform: bool,
    flexible: bool,

    // On-Command transcription state
    on_command_transcription: bool,
    on_command_active: bool,
    on_command_pos: Pos2,
}

/// Create a DashboardState from the loaded Settings
impl From<&quickstt_core::settings::Settings> for DashboardState {
    fn from(s: &quickstt_core::settings::Settings) -> Self {
        Self {
            visible: false,
            tab: 0,
            pill_radius: s.pill_radius as f32,
            opacity_pct: s.active_opacity as f32,
            icon_size: s.icon_size as f32,
            tb_opacity_pct: s.txt_opacity as f32,
            tb_text_size: s.txt_size as f32,
            offload_enabled: s.auto_offload,
            offload_secs: s.offload_seconds,
            show_waveform: s.show_waveform,
            flexible: s.widget_flexible,
            on_command_transcription: read_registry_bool("onCommandTranscription", false),
            on_command_active: false,
            on_command_pos: pos2(0.0, 0.0),
        }
    }
}

// ── Main Application ──

struct QuickSttApp {
    state: Arc<Mutex<AppState>>,
    tx_cmd: mpsc::Sender<OrchestratorCommand>,

    // Pill UI properties
    pill_w: f32,
    pill_h: f32,
    is_listening: bool,
    is_recording: bool,
    blink_state: bool,
    blink_timer: Instant,
    status_text: String,

    // Models dropdown selections
    model_names: Vec<String>,
    selected_model_idx: usize,
    model_popup_open: bool,
    model_popup_pos: Pos2,
    model_popup_width: f32,

    // Waveform rendering
    waveform: waveform::WaveformState,
    wave_timer: Instant,

    // TextBoard
    tb_visible: bool,
    tb_attached: bool,
    tb_restore_on_show: bool,
    last_sent_height: f32,
    last_sent_w: f32,
    transcript: String,
    partial_text: String,

    // Dashboard State
    dash: Arc<Mutex<DashboardState>>,

    // Auto-offload mechanism
    offload_timer: Option<Instant>,
    model_offloaded: bool,

    // Icon handling
    icons: Option<IconSet>,
    last_icon_size: u32,

    // Resize / edge detection state (mirrors C++ m_isResizing / m_resizeEdge)
    widget_flexible: bool,
    is_resizing: bool,
    resize_edge: u8, // 0=none, 1=left, 2=right, 3=top, 4=bottom, 5=topleft, 6=topright, 7=bottomleft, 8=bottomright
    resize_anchor_pos: Pos2,
    resize_anchor_size: Vec2,
    resize_anchor_topleft: Pos2,

    // System integrations
    _hotkey_manager: GlobalHotKeyManager,
    _tray_icon: Option<TrayIcon>,
    initialized: bool,
    positioned: bool,
    last_widget_visible: bool,
    #[cfg(target_os = "windows")]
    widget_hwnd: Option<isize>,

    menu_dash_id: MenuId,
    menu_show_id: MenuId,
    menu_hide_id: MenuId,
    menu_quit_id: MenuId,

    // Hotkey event identifiers (polled in update loop)
    id_hotkey_super_toggle: u32,
    id_hotkey_toggle_listen: u32,
    id_hotkey_show_widget: u32,
    id_hotkey_on_cmd: u32,

    // Background wakeword service handle (None if feature disabled,
    // service failed to initialise, or no models were found).
    wakeword_handle: Option<quickstt_core::wakeword_service::WakewordHandle>,
}

fn create_tray_icon_image() -> tray_icon::Icon {
    // Primary: the canonical app-logo SVG rendered at tray resolution.
    if let Some((rgba, width, height)) = quickstt_gui::icons::render_app_icon_rgba(64) {
        if let Ok(icon) = tray_icon::Icon::from_rgba(rgba, width, height) {
            return icon;
        }
    }
    // Fallback: prebuilt ICO.
    let ico_bytes = include_bytes!("../../assets/icon_app.ico");
    if let Ok(img) = image::load_from_memory_with_format(ico_bytes, image::ImageFormat::Ico) {
        let rgba = img.to_rgba8();
        let (width, height) = rgba.dimensions();
        if let Ok(icon) = tray_icon::Icon::from_rgba(rgba.into_raw(), width, height) {
            return icon;
        }
    }
    // programmatic fallback
    let s = 32u32;
    let mut rgba = vec![0u8; (s * s * 4) as usize];
    let c = s as f32 / 2.0;
    let r = c - 1.0;
    for y in 0..s {
        for x in 0..s {
            let dx = x as f32 - c;
            let dy = y as f32 - c;
            let dist = (dx * dx + dy * dy).sqrt();
            let idx = ((y * s + x) * 4) as usize;
            if dist <= r {
                rgba[idx] = 0x1A;
                rgba[idx + 1] = 0x1A;
                rgba[idx + 2] = 0x1A;
                rgba[idx + 3] = 0xFF;
                let nx = dx / r;
                let ny = dy / r;
                if (nx.abs() < 0.22 && ny > -0.50 && ny < 0.05)
                    || (nx.abs() > 0.15 && nx.abs() < 0.30 && ny > 0.0 && ny < 0.22)
                    || (nx.abs() <= 0.30 && ny >= 0.18 && ny < 0.26)
                    || (nx.abs() < 0.06 && ny >= 0.22 && ny < 0.48)
                    || (nx.abs() < 0.20 && ny >= 0.44 && ny < 0.52)
                {
                    rgba[idx] = 0x00;
                    rgba[idx + 1] = 0x71;
                    rgba[idx + 2] = 0xBC;
                }
            }
            if dist > r - 2.5 && dist <= r {
                rgba[idx] = 0x00;
                rgba[idx + 1] = 0xAA;
                rgba[idx + 2] = 0xFF;
                rgba[idx + 3] = 0xFF;
            }
        }
    }
    tray_icon::Icon::from_rgba(rgba, s, s).expect("icon")
}

fn draw_combo_arrow(painter: &egui::Painter, center: Pos2, color: Color32) {
    let stroke = Stroke::new(1.8, color);
    painter.line_segment(
        [
            pos2(center.x - 3.8, center.y - 1.1),
            pos2(center.x, center.y + 2.4),
        ],
        stroke,
    );
    painter.line_segment(
        [
            pos2(center.x, center.y + 2.4),
            pos2(center.x + 3.8, center.y - 1.1),
        ],
        stroke,
    );
}

fn elide_text_to_width(
    painter: &egui::Painter,
    text: &str,
    font: FontId,
    max_width: f32,
) -> String {
    if text.is_empty() || max_width <= 0.0 {
        return String::new();
    }

    let full_width = painter
        .layout_no_wrap(text.to_owned(), font.clone(), Color32::WHITE)
        .size()
        .x;
    if full_width <= max_width {
        return text.to_owned();
    }

    let chars: Vec<char> = text.chars().collect();
    if chars.is_empty() {
        return String::new();
    }

    let ellipsis = "…";
    let mut low = 0usize;
    let mut high = chars.len();
    let mut best = ellipsis.to_string();

    while low < high {
        let mid = (low + high) / 2;
        let candidate: String = chars[..mid].iter().collect::<String>() + ellipsis;
        let width = painter
            .layout_no_wrap(candidate.clone(), font.clone(), Color32::WHITE)
            .size()
            .x;
        if width <= max_width {
            best = candidate;
            low = mid + 1;
        } else {
            high = mid;
        }
    }

    best
}

fn draw_collapse_chevron(painter: &egui::Painter, center: Pos2, angle_deg: f32, color: Color32) {
    let s: f32 = 5.0;
    let stroke = Stroke::new(2.0, color);

    // Convert angle to radians
    let rad = angle_deg.to_radians();
    let cos_a = rad.cos();
    let sin_a = rad.sin();

    // Rotate point helper
    let rotate = |x: f32, y: f32| {
        pos2(
            center.x + x * cos_a - y * sin_a,
            center.y + x * sin_a + y * cos_a,
        )
    };

    // C++ lines: (-s, s/2) -> (0, -s/2) -> (s, s/2)
    let p1 = rotate(-s, s / 2.0);
    let p2 = rotate(0.0, -s / 2.0);
    let p3 = rotate(s, s / 2.0);

    painter.line_segment([p1, p2], stroke);
    painter.line_segment([p2, p3], stroke);
}

fn tray_menu_button(ui: &mut egui::Ui, text: &str) -> egui::Response {
    ui.add_sized(
        [ui.available_width(), 28.0],
        egui::Button::new(egui::RichText::new(text).color(Color32::WHITE).size(13.0))
            .fill(Color32::BLACK)
            .stroke(Stroke::NONE)
            .rounding(Rounding::ZERO),
    )
}

impl QuickSttApp {
    fn new(
        _cc: &eframe::CreationContext<'_>,
        state: Arc<Mutex<AppState>>,
        tx_cmd: mpsc::Sender<OrchestratorCommand>,
        wakeword_handle: Option<quickstt_core::wakeword_service::WakewordHandle>,
        dash: Arc<Mutex<DashboardState>>,
        start_hidden: bool,
    ) -> Self {
    let hotkey_manager = GlobalHotKeyManager::new().unwrap();
    // Primary toggle: Super+Space (matches the Windows build's Win+Space).
    let hotkey_super_space = HotKey::new(Some(Modifiers::SUPER), Code::Space);
    let id_super_toggle = hotkey_super_space.id();
    if let Err(e) = hotkey_manager.register(hotkey_super_space) {
        warn!("Super+Space hotkey unavailable: {e}");
    }
    let ctrl_enabled = state
        .lock()
        .map(|s| s.settings.ctrl_space_enabled)
        .unwrap_or(true);
    let (id_toggle, id_on_cmd) = if ctrl_enabled {
        let hotkey_toggle = HotKey::new(Some(Modifiers::CONTROL | Modifiers::SHIFT), Code::Space);
        let id_toggle = hotkey_toggle.id();
        if let Err(e) = hotkey_manager.register(hotkey_toggle) {
            warn!("Ctrl+Shift+Space hotkey unavailable: {e}");
        }
        let hotkey_on_cmd = HotKey::new(Some(Modifiers::CONTROL), Code::Space);
        let id_on_cmd = hotkey_on_cmd.id();
        if let Err(e) = hotkey_manager.register(hotkey_on_cmd) {
            warn!("Ctrl+Space hotkey unavailable (IBus may hold it — disable IBus Ctrl+Space): {e}");
        }
        (id_toggle, id_on_cmd)
    } else {
        warn!("Ctrl+Space hotkeys disabled by settings (ctrl_space_enabled=false)");
        // Still need IDs for polling match, but they will never fire.
        (0, 0)
    };
    let hotkey_show_widget =
        HotKey::new(Some(Modifiers::CONTROL | Modifiers::SHIFT), Code::KeyH);
    let id_show_widget_hotkey = hotkey_show_widget.id();
    if let Err(e) = hotkey_manager.register(hotkey_show_widget) {
        warn!("Ctrl+Shift+H hotkey unavailable: {e}");
    }

    let icon = create_tray_icon_image();

    let mut tray: Option<TrayIcon> = None;

    #[cfg(target_os = "linux")]
    {
        // Linux: tray-icon/muda need a GTK main loop AND their types are
        // !Send (Rc-based), so the entire menu + tray construction happens
        // inside the GTK thread. Events are matched by fixed string IDs.
        std::thread::Builder::new()
            .name("tray-gtk".to_string())
            .spawn(move || {
                if gtk::init().is_err() {
                    warn!("gtk init failed — system tray unavailable");
                    return;
                }
                let dash_item = MenuItem::with_id(TRAY_ID_DASH, "Dashboard", true, None);
                let show_item = MenuItem::with_id(TRAY_ID_SHOW, "Show", true, None);
                let hide_item = MenuItem::with_id(TRAY_ID_HIDE, "Hide", true, None);
                let quit_item = MenuItem::with_id(TRAY_ID_QUIT, "Quit", true, None);
                let mut menu = Menu::new();
                if let Err(e) = menu.append_items(&[
                    &dash_item,
                    &show_item,
                    &hide_item,
                    &PredefinedMenuItem::separator(),
                    &quit_item,
                ]) {
                    warn!("Tray menu build failed: {e}");
                    return;
                }
                match TrayIconBuilder::new()
                    .with_tooltip("QuickSTT")
                    .with_icon(icon)
                    .with_menu(Box::new(menu))
                    .build()
                {
                    Ok(tray) => {
                        info!("System tray initialised");
                        Box::leak(Box::new(tray));
                        gtk::main();
                    }
                    Err(e) => warn!("Tray build failed: {e}"),
                }
            })
            .expect("spawn tray thread");
    }
    #[cfg(not(target_os = "linux"))]
    {
        let dash_item = MenuItem::with_id(TRAY_ID_DASH, "Dashboard", true, None);
        let show_item = MenuItem::with_id(TRAY_ID_SHOW, "Show", true, None);
        let hide_item = MenuItem::with_id(TRAY_ID_HIDE, "Hide", true, None);
        let quit_item = MenuItem::with_id(TRAY_ID_QUIT, "Quit", true, None);
        let mut menu = Menu::new();
        menu.append_items(&[
            &dash_item,
            &show_item,
            &hide_item,
            &PredefinedMenuItem::separator(),
            &quit_item,
        ])
        .unwrap();
        tray = TrayIconBuilder::new()
            .with_tooltip("QuickSTT")
            .with_icon(icon)
            .with_menu(Box::new(menu))
            .build()
            .ok();
    }

        let names = {
            let s = state.lock().unwrap();
            s.model_entries.iter().map(|m| m.name.clone()).collect()
        };

        let (widget_flexible_init, initial_pill_w, initial_pill_h, initial_s_from_settings) = {
            let s = state.lock().unwrap();
            let saved_w = s.settings.pill_width as f32;
            let saved_h = s.settings.pill_height as f32;
            (
                s.settings.widget_flexible,
                if (200.0..=720.0).contains(&saved_w) {
                    saved_w
                } else {
                    theme::PILL_WIDTH
                },
                if (40.0..=96.0).contains(&saved_h) {
                    saved_h
                } else {
                    theme::PILL_HEIGHT
                },
                s.settings.clone(),
            )
        };

        // Start hidden when launched with --background, "start minimized to tray",
        // or always-on pill disabled (Rust parity with C++ MicroPillOverlay).
        if start_hidden
            || initial_s_from_settings.startup_background
            || !initial_s_from_settings.always_on_pill
        {
            if let Ok(mut s) = state.lock() {
                s.widget_visible = false;
                s.status_message = "Hidden".into();
            }
        }

        // Set initial widget visibility from AppState
        let widget_visible_init = state.lock().unwrap().widget_visible;

        Self {
            state,
            tx_cmd,
            pill_w: initial_pill_w,
            pill_h: initial_pill_h,
            is_listening: false,
            is_recording: false,
            blink_state: false,
            blink_timer: Instant::now(),
            status_text: "Ready".to_string(),
            model_names: names,
            selected_model_idx: 0,
            model_popup_open: false,
            model_popup_pos: pos2(0.0, 0.0),
            model_popup_width: 180.0,
            waveform: waveform::WaveformState::new(),
            wave_timer: Instant::now(),
            tb_visible: false,
            tb_attached: false,
            tb_restore_on_show: false,
            last_sent_height: theme::PILL_HEIGHT,
            last_sent_w: theme::PILL_WIDTH,
            transcript: String::new(),
            partial_text: String::new(),
            dash,
            offload_timer: Some(Instant::now()),
            model_offloaded: false,
            icons: None,
            last_icon_size: 0,
            widget_flexible: widget_flexible_init,
            is_resizing: false,
            resize_edge: 0,
            resize_anchor_pos: pos2(0.0, 0.0),
            resize_anchor_size: vec2(0.0, 0.0),
            resize_anchor_topleft: pos2(0.0, 0.0),
            _hotkey_manager: hotkey_manager,
            _tray_icon: tray,
            initialized: false,
            positioned: false,
            last_widget_visible: widget_visible_init,
            #[cfg(target_os = "windows")]
            widget_hwnd: None,
            menu_dash_id: MenuId::new(TRAY_ID_DASH),
            menu_show_id: MenuId::new(TRAY_ID_SHOW),
            menu_hide_id: MenuId::new(TRAY_ID_HIDE),
            menu_quit_id: MenuId::new(TRAY_ID_QUIT),
            id_hotkey_super_toggle: id_super_toggle,
            id_hotkey_toggle_listen: id_toggle,
            id_hotkey_show_widget: id_show_widget_hotkey,
            id_hotkey_on_cmd: id_on_cmd,
            wakeword_handle,
        }
    }

    fn show_widget(&mut self, ctx: &egui::Context) {
        if let Ok(mut s) = self.state.lock() {
            s.widget_visible = true;
            if s.status_message.eq_ignore_ascii_case("Hidden") {
                s.status_message = "Ready".into();
            }
        }
        if self.tb_restore_on_show {
            self.tb_visible = true;
            self.tb_restore_on_show = false;
        }
        ctx.send_viewport_cmd(ViewportCommand::Visible(true));
        ctx.send_viewport_cmd(ViewportCommand::InnerSize(vec2(self.pill_w, self.pill_h)));
        #[cfg(target_os = "windows")]
        if let Some(hwnd) = self.widget_hwnd {
            unsafe {
                let hwnd = windows::Win32::Foundation::HWND(hwnd);
                widget_platform::configure_widget_window(hwnd);
                widget_platform::show_widget(hwnd);
            }
        }
        #[cfg(target_os = "linux")]
        {
            // Linux transparency/always-on-top is via ViewportBuilder flags — nothing extra
            unsafe { widget_platform::configure_widget_window(()); }
        }
        ctx.request_repaint();
    }

    fn hide_widget(&mut self, ctx: &egui::Context) {
        self.tb_restore_on_show = self.tb_visible;
        self.tb_visible = false;
        if let Ok(mut s) = self.state.lock() {
            s.widget_visible = false;
            s.status_message = "Hidden".into();
        }
        self.stop_listening();
        self.model_popup_open = false;
        ctx.send_viewport_cmd(ViewportCommand::Visible(false));
        #[cfg(target_os = "windows")]
        if let Some(hwnd) = self.widget_hwnd {
            unsafe {
                widget_platform::hide_widget(windows::Win32::Foundation::HWND(hwnd));
            }
        }
        #[cfg(target_os = "linux")]
        unsafe { widget_platform::hide_widget(()); }
        ctx.request_repaint_after(Duration::from_millis(100));
    }

    /// Drain pending tray-icon and global hotkey events each frame.
    /// Called from the top of `update()` so we don't need a dedicated polling
    /// thread.
    fn poll_tray_and_hotkeys(&mut self, ctx: &egui::Context) {
        // Tray Events
        while let Ok(event) = TrayIconEvent::receiver().try_recv() {
            match event {
                TrayIconEvent::Click {
                    button: tray_icon::MouseButton::Left,
                    button_state: MouseButtonState::Up,
                    ..
                } => {
                    let mut s = self.state.lock().unwrap();
                    s.widget_visible = !s.widget_visible;
                    ctx.request_repaint();
                }
                _ => {}
            }
        }

        // Native Tray Menu Events
        while let Ok(event) = MenuEvent::receiver().try_recv() {
            if event.id == self.menu_dash_id {
                self.dash.lock().unwrap().visible = true;
                ctx.request_repaint();
            } else if event.id == self.menu_show_id {
                self.state.lock().unwrap().widget_visible = true;
                ctx.request_repaint();
            } else if event.id == self.menu_hide_id {
                self.state.lock().unwrap().widget_visible = false;
                ctx.request_repaint();
            } else if event.id == self.menu_quit_id {
                ctx.send_viewport_cmd(ViewportCommand::Close);
            }
        }

        // ── Global hotkey events ──
        while let Ok(event) = GlobalHotKeyEvent::receiver().try_recv() {
            let on_command_enabled = self
                .dash
                .lock()
                .map(|d| d.on_command_transcription)
                .unwrap_or(false);

            if event.id == self.id_hotkey_super_toggle
                || event.id == self.id_hotkey_toggle_listen
            {
                // Toggle hotkeys: Super+Space (primary) / Ctrl+Shift+Space (legacy)
                if event.state == global_hotkey::HotKeyState::Pressed {
                    let is_listening = self
                        .state
                        .lock()
                        .map(|s| s.mode != AppMode::Idle)
                        .unwrap_or(false);
                    let cmd = if is_listening {
                        OrchestratorCommand::StopListening
                    } else {
                        OrchestratorCommand::StartListening
                    };
                    let _ = self.tx_cmd.try_send(cmd);
                    ctx.request_repaint();
                }
            } else if event.id == self.id_hotkey_show_widget {
                // Show/hide widget hotkey: Ctrl+Shift+H (press to toggle)
                if event.state == global_hotkey::HotKeyState::Pressed {
                    let visible = self.state.lock().map(|s| s.widget_visible).unwrap_or(false);
                    if visible {
                        self.hide_widget(ctx);
                    } else {
                        self.show_widget(ctx);
                    }
                }
            } else if event.id == self.id_hotkey_on_cmd {
                // On-Command hotkey: Ctrl+Space (hold to transcribe)
                if on_command_enabled {
                    if event.state == global_hotkey::HotKeyState::Pressed {
                        if let Ok(mut d) = self.dash.lock() {
                            d.on_command_active = true;
                            let (sw, sh) = get_screen_size();
                            d.on_command_pos = pos2((sw - 120.0) / 2.0, sh - 100.0);
                        }
                        let _ = self.tx_cmd.try_send(OrchestratorCommand::StartListening);
                    } else if event.state == global_hotkey::HotKeyState::Released {
                        if let Ok(mut d) = self.dash.lock() {
                            d.on_command_active = false;
                        }
                        let _ = self.tx_cmd.try_send(OrchestratorCommand::StopListening);
                    }
                }
                ctx.request_repaint();
            }
        }
    }

    fn sync_from_state(&mut self) {
        if let Ok(s) = self.state.lock() {
            self.is_listening = s.mode != AppMode::Idle;
            self.is_recording = s.mode == AppMode::Recording || s.mode == AppMode::Transcribing;

            if self.is_listening && s.audio_level > 0 {
                let level = s.audio_level as f32 / 100.0;
                self.waveform.push_level(level);
            }
            if !s.transcript_buffer.is_empty() && s.transcript_buffer != self.transcript {
                let new_text = s.transcript_buffer.clone();
                let delta = if new_text.starts_with(&self.transcript) {
                    new_text[self.transcript.len()..].trim().to_string()
                } else {
                    new_text.clone()
                };
                let output_mode = s.settings.ctrl_space_output;
                self.transcript = new_text;
                if !delta.is_empty() {
                    deliver_transcription_output(&delta, output_mode);
                }
            }
            if s.partial_result != self.partial_text {
                self.partial_text = s.partial_result.clone();
            }
            if !s.status_message.is_empty() {
                self.status_text = s.status_message.clone();
            }
            let names: Vec<String> = s.model_entries.iter().map(|m| m.name.clone()).collect();
            if names != self.model_names {
                self.model_names = names;
            }
        }
    }

    fn handle_events(&mut self, _ctx: &egui::Context) {
        let dash = self.dash.lock().unwrap().clone();
        let offload_delay = if dash.on_command_transcription {
            60
        } else {
            dash.offload_secs
        };
        if dash.offload_enabled && !self.is_listening && !self.model_offloaded {
            if let Some(timer) = self.offload_timer {
                if timer.elapsed() >= Duration::from_secs(offload_delay as u64) {
                    self.model_offloaded = true;
                    self.status_text = "Offloaded".to_string();
                    self.offload_timer = None;
                    let _ = self.tx_cmd.try_send(OrchestratorCommand::OffloadModel);
                }
            }
        }

        if self.is_recording && self.blink_timer.elapsed() >= Duration::from_millis(theme::BLINK_MS)
        {
            self.blink_state = !self.blink_state;
            self.blink_timer = Instant::now();
        }

        if self.is_listening
            && self.wave_timer.elapsed() >= Duration::from_millis(theme::WAVE_FPS_MS)
        {
            let audio_level = self.state.lock().map(|s| s.audio_level).unwrap_or(0);
            if audio_level > 0 {
                let level = audio_level as f32 / 100.0;
                self.waveform.push_level(level);
            }
            self.waveform.tick();
            self.wave_timer = Instant::now();
        }
    }

    fn start_listening(&mut self) {
        self.is_listening = true;
        self.model_offloaded = false;
        self.offload_timer = None;
        self.waveform.start();
        let _ = self.tx_cmd.try_send(OrchestratorCommand::StartListening);
    }

    fn stop_listening(&mut self) {
        self.is_listening = false;
        self.is_recording = false;
        self.waveform.stop();
        self.offload_timer = Some(Instant::now());
        self.status_text = "Ready".to_string();
        let _ = self.tx_cmd.try_send(OrchestratorCommand::StopListening);
    }

    fn ensure_icons(&mut self, ctx: &egui::Context) {
        let target_size = self.dash.lock().unwrap().icon_size as u32;
        if self.icons.is_none() || self.last_icon_size != target_size {
            self.icons = Some(IconSet::load(ctx, target_size.max(16)));
            self.last_icon_size = target_size;
        }
    }


    fn render_model_dropdown(&mut self, ctx: &egui::Context) {
        if !self.model_popup_open {
            return;
        }

        let popup_width = self.model_popup_width;
        let popup_pos = self.model_popup_pos;
        let entries_count = match self.state.lock() {
            Ok(s) => s.model_entries.len(),
            Err(_) => 0,
        };
        let calculated_height = if entries_count == 0 {
            40.0
        } else {
            (entries_count as f32 * 23.0 + 8.0).clamp(40.0, 224.0)
        };
        let popup_height = calculated_height;
        let mut close_popup = false;

        let builder = ViewportBuilder::default()
            .with_title("QuickSTT Models")
            .with_inner_size([popup_width, popup_height])
            .with_position(popup_pos)
            .with_decorations(false)
            .with_resizable(false)
            .with_transparent(true)
            .with_always_on_top()
            .with_taskbar(false)
            .with_active(true);

        ctx.show_viewport_immediate(
            ViewportId::from_hash_of("quickstt_model_dropdown"),
            builder,
            |ctx, _class| {
                if ctx.input(|i| i.viewport().close_requested() || i.key_pressed(egui::Key::Escape))
                {
                    close_popup = true;
                }

                let mut visuals = ctx.style().visuals.clone();
                visuals.window_fill = Color32::from_rgb(0x1E, 0x1E, 0x1E);
                visuals.panel_fill = Color32::from_rgb(0x1E, 0x1E, 0x1E);
                visuals.override_text_color = Some(Color32::from_rgb(0xEE, 0xEE, 0xEE));
                visuals.widgets.inactive.bg_fill = Color32::from_rgb(0x1E, 0x1E, 0x1E);
                visuals.widgets.hovered.bg_fill = Color32::from_rgb(0x2A, 0x2A, 0x2A);
                visuals.widgets.active.bg_fill = Color32::from_rgb(0x33, 0x33, 0x33);
                visuals.selection.bg_fill = Color32::from_rgb(0x00, 0x71, 0xBC);
                visuals.popup_shadow = egui::epaint::Shadow::NONE;
                ctx.set_visuals(visuals);

                let frame = egui::Frame::none()
                    .fill(Color32::from_rgb(0x1E, 0x1E, 0x1E))
                    .stroke(Stroke::new(1.0, Color32::from_rgb(0x44, 0x44, 0x44)))
                    .inner_margin(egui::Margin::symmetric(2.0, 2.0));

                egui::CentralPanel::default().frame(frame).show(ctx, |ui| {
                    ui.set_min_width(popup_width);
                    ui.spacing_mut().item_spacing = vec2(0.0, 1.0);

                    let entries = match self.state.lock() {
                        Ok(s) => s.model_entries.clone(),
                        Err(_) => Vec::new(),
                    };

                    if entries.is_empty() {
                        ui.add_space(8.0);
                        ui.horizontal(|ui| {
                            ui.add_space(8.0);
                            ui.label(
                                egui::RichText::new("No models available")
                                    .color(Color32::from_rgb(0x99, 0x99, 0x99))
                                    .italics()
                                    .size(12.0),
                            );
                        });
                        return;
                    }

                    let mut installed: Vec<(usize, &ModelEntry)> =
                        Vec::with_capacity(entries.len());
                    let mut not_installed: Vec<(usize, &ModelEntry)> =
                        Vec::with_capacity(entries.len());
                    for (i, entry) in entries.iter().enumerate() {
                        if entry.installed {
                            installed.push((i, entry));
                        } else {
                            not_installed.push((i, entry));
                        }
                    }

                    egui::ScrollArea::vertical()
                        .max_height(220.0)
                        .auto_shrink([false, false])
                        .show(ui, |ui| {
                            for group in [&installed, &not_installed] {
                                for (real_idx, entry) in group.iter() {
                                    let selected = *real_idx == self.selected_model_idx;
                                    let row_font = FontId::proportional(12.0);
                                    let row_label = elide_text_to_width(
                                        ui.painter(),
                                        &entry.name,
                                        row_font.clone(),
                                        (popup_width - 30.0).max(20.0),
                                    );
                                    let text_color = if !entry.installed {
                                        Color32::from_rgb(0x88, 0x88, 0x88)
                                    } else if selected {
                                        Color32::WHITE
                                    } else {
                                        Color32::from_rgb(0xDD, 0xDD, 0xDD)
                                    };
                                    let response = ui.add_sized(
                                        [popup_width - 6.0, 22.0],
                                        egui::Button::new(
                                            egui::RichText::new(row_label)
                                                .color(text_color)
                                                .size(12.0),
                                        )
                                        .frame(false)
                                        .fill(
                                            if selected {
                                                Color32::from_rgb(0x33, 0x33, 0x33)
                                            } else {
                                                Color32::TRANSPARENT
                                            },
                                        ),
                                    );
                                    if response.clicked() {
                                        self.selected_model_idx = *real_idx;
                                        let cmd = if entry.installed {
                                            OrchestratorCommand::SelectModel(*real_idx)
                                        } else {
                                            // Picking a not-installed model kicks off
                                            // its background download.
                                            OrchestratorCommand::DownloadModel(*real_idx)
                                        };
                                        let _ = self.tx_cmd.try_send(cmd);
                                        close_popup = true;
                                    }
                                }
                            }
                        });
                });
            },
        );

        if close_popup {
            self.model_popup_open = false;
            ctx.send_viewport_cmd_to(
                ViewportId::from_hash_of("quickstt_model_dropdown"),
                ViewportCommand::Close,
            );
        } else {
            ctx.request_repaint_after(Duration::from_millis(16));
        }
    }

    fn render_pill(&mut self, ui: &mut egui::Ui) {
        let dash = self.dash.lock().unwrap().clone();
        let pill_r = dash.pill_radius;
        let icon_size = dash.icon_size;
        let show_waveform = dash.show_waveform;
        // Sync the live resize setting from the dashboard so changes take effect
        // immediately without restart (matches C++ `m_widgetFlexible`).
        self.widget_flexible = dash.flexible;
        drop(dash);

        let full_rect = ui.max_rect();
        let pill_rect = Rect::from_min_size(full_rect.min, vec2(self.pill_w, self.pill_h));

        // ── Edge-resize handling (mirrors C++ PillWidget mouse handlers) ──
        // When `widget_flexible` is on, hovering on the widget edges changes
        // the cursor and dragging resizes the pill. To keep the implementation
        // simple in egui we just detect proximity to edges anywhere on the pill
        // and, if the user pressed-and-drags, run a resize.
        if self.widget_flexible {
            let margin = 10.0;
            let pointer_opt = ui.ctx().input(|i| i.pointer.hover_pos());
            if let Some(pos) = pointer_opt {
                let on_left = pos.x <= pill_rect.left() + margin;
                let on_right = pos.x >= pill_rect.right() - margin;
                let on_top = pos.y <= pill_rect.top() + margin;
                let on_bottom = pos.y >= pill_rect.bottom() - margin;

                // Determine which edge the user is hovering (priority to corners).
                let edge = match (on_left, on_right, on_top, on_bottom) {
                    (true, _, true, _) => 5, // topleft
                    (_, true, true, _) => 6, // topright
                    (true, _, _, true) => 7, // bottomleft
                    (_, true, _, true) => 8, // bottomright
                    (true, _, _, _) => 1,    // left
                    (_, true, _, _) => 2,    // right
                    (_, _, true, _) => 3,    // top
                    (_, _, _, true) => 4,    // bottom
                    _ => 0,
                };

                // Cursor feedback — show resize cursors on hover, when not yet resizing.
                if !self.is_resizing && edge != 0 {
                    let cursor = match edge {
                        1 | 2 => CursorIcon::ResizeHorizontal,
                        3 | 4 => CursorIcon::ResizeVertical,
                        5 | 8 => CursorIcon::ResizeNwSe,
                        6 | 7 => CursorIcon::ResizeNeSw,
                        _ => CursorIcon::Default,
                    };
                    ui.ctx().set_cursor_icon(cursor);
                }

                // Primary button just started pressing in the pill → possible resize begin.
                if ui.ctx().input(|i| i.pointer.primary_pressed())
                    && edge != 0
                    && pill_rect.contains(pos)
                {
                    self.is_resizing = true;
                    self.resize_edge = edge;
                    self.resize_anchor_pos = pos;
                    self.resize_anchor_size = vec2(self.pill_w, self.pill_h);
                    self.resize_anchor_topleft = pill_rect.min;
                }

                // While resizing, mutate pill_w/pill_h based on cursor delta.
                if self.is_resizing {
                    let delta = pos - self.resize_anchor_pos;
                    let mut new_w = self.resize_anchor_size.x;
                    let mut new_h = self.resize_anchor_size.y;
                    let mut new_left = self.resize_anchor_topleft.x;
                    let mut new_top = self.resize_anchor_topleft.y;

                    match self.resize_edge {
                        1 => {
                            new_w = (self.resize_anchor_size.x - delta.x).max(200.0);
                            new_left =
                                self.resize_anchor_topleft.x + (self.resize_anchor_size.x - new_w);
                        }
                        2 => {
                            new_w = (self.resize_anchor_size.x + delta.x).max(200.0);
                        }
                        3 => {
                            new_h = (self.resize_anchor_size.y - delta.y).max(40.0);
                            new_top =
                                self.resize_anchor_topleft.y + (self.resize_anchor_size.y - new_h);
                        }
                        4 => {
                            new_h = (self.resize_anchor_size.y + delta.y).max(40.0);
                        }
                        5 => {
                            new_w = (self.resize_anchor_size.x - delta.x).max(200.0);
                            new_h = (self.resize_anchor_size.y - delta.y).max(40.0);
                            new_left =
                                self.resize_anchor_topleft.x + (self.resize_anchor_size.x - new_w);
                            new_top =
                                self.resize_anchor_topleft.y + (self.resize_anchor_size.y - new_h);
                        }
                        6 => {
                            new_w = (self.resize_anchor_size.x + delta.x).max(200.0);
                            new_h = (self.resize_anchor_size.y - delta.y).max(40.0);
                            new_top =
                                self.resize_anchor_topleft.y + (self.resize_anchor_size.y - new_h);
                        }
                        7 => {
                            new_w = (self.resize_anchor_size.x - delta.x).max(200.0);
                            new_h = (self.resize_anchor_size.y + delta.y).max(40.0);
                            new_left =
                                self.resize_anchor_topleft.x + (self.resize_anchor_size.x - new_w);
                        }
                        8 => {
                            new_w = (self.resize_anchor_size.x + delta.x).max(200.0);
                            new_h = (self.resize_anchor_size.y + delta.y).max(40.0);
                        }
                        _ => {}
                    }

                    if new_w != self.pill_w || new_h != self.pill_h {
                        self.pill_w = new_w;
                        self.pill_h = new_h;
                        // Move the viewport so the anchor corner stays fixed when
                        // pulling on the left/top edges — keeps the resize feeling
                        // natural.
                        let new_topleft = pos2(new_left, new_top);
                        ui.ctx()
                            .send_viewport_cmd(ViewportCommand::OuterPosition(new_topleft));
                    }
                }

                // Primary button released → finalize and persist.
                if self.is_resizing && ui.ctx().input(|i| i.pointer.primary_released()) {
                    self.is_resizing = false;
                    self.resize_edge = 0;
                    let _ = quickstt_core::settings::Settings::save_dword(
                        "pillWidth",
                        self.pill_w as u32,
                    );
                    let _ = quickstt_core::settings::Settings::save_dword(
                        "pillHeight",
                        self.pill_h as u32,
                    );
                }
            }
        }

        let left_margin = (self.pill_w / 22.0).clamp(9.0, 12.0);
        let dot_gap = (self.pill_w / 55.0).clamp(5.0, 8.0);
        let mic_gap = (self.pill_w / 120.0).clamp(1.0, 3.0);
        let combo_gap = (self.pill_w / 90.0).clamp(3.0, 5.0);
        let close_size: f32 = 18.0;
        let collapse_hit: f32 = 22.0;
        let right_margin: f32 = 12.0;
        let cy = pill_rect.center().y;
        let mut x = pill_rect.left() + left_margin;

        // Drag pill background (must be allocated BEFORE buttons so buttons
        // "win" interactions in their regions — last widget in an area wins).
        let bg_resp = ui.interact(pill_rect, Id::new("pill_drag"), Sense::drag());

        // Red Dot (double click quits app)
        let dot_center = pos2(x + theme::DOT_SIZE / 2.0, cy);
        let dot_resp = ui.allocate_rect(
            Rect::from_center_size(dot_center, vec2(theme::DOT_SIZE + 4.0, self.pill_h)),
            Sense::click(),
        );
        x += theme::DOT_SIZE + dot_gap;

        // Mic Click Button
        let mic_w = icon_size + 6.0;
        let mic_rect = Rect::from_min_size(pos2(x, pill_rect.top()), vec2(mic_w, self.pill_h));
        let mic_center = pos2(x + mic_w / 2.0, cy);
        let mic_resp = ui.allocate_rect(mic_rect, Sense::click());
        x += mic_w + mic_gap;

        // ComboBox dropdown for model selector
        let combo_w = (self.pill_w / 2.0 - 20.0).clamp(120.0, 220.0);
        let combo_rect = Rect::from_min_size(
            pos2(x, cy - theme::COMBO_HEIGHT / 2.0),
            vec2(combo_w, theme::COMBO_HEIGHT),
        );
        x += combo_w + combo_gap;

        // Close (hides window)
        let close_x = pill_rect.right() - close_size - right_margin;
        let close_rect = Rect::from_center_size(
            pos2(close_x + close_size / 2.0, cy),
            vec2(close_size, close_size),
        );
        let close_resp = ui.allocate_rect(close_rect, Sense::click());

        // Collapse (toggles TextBoard)
        let collapse_x = close_x - collapse_hit - 4.0;
        let collapse_rect = Rect::from_center_size(
            pos2(collapse_x + collapse_hit / 2.0, cy),
            vec2(collapse_hit, collapse_hit),
        );
        let collapse_resp = ui.allocate_rect(collapse_rect, Sense::click());

        // Status or Waveform area
        let status_right = collapse_x - 6.0;
        let status_w = (status_right - x).max(10.0);
        let status_rect =
            Rect::from_min_size(pos2(x, pill_rect.top()), vec2(status_w, self.pill_h));

        // Paint everything
        let painter = ui.painter().clone();

        // Pill background — draw a clean rounded rectangle without external
        // shadow or highlights to avoid clipping artifacts on transparent windows.
        // The rounded corners use the user-configurable pill_radius for smooth,
        // configurable curves.
        painter.rect_filled(pill_rect, Rounding::same(pill_r), theme::PILL_BG);

        // 2. Red Dot
        let dot_color = if self.is_recording {
            if self.blink_state {
                theme::DOT_RECORDING_ON
            } else {
                theme::DOT_RECORDING_OFF
            }
        } else if self.is_listening {
            theme::DOT_LISTENING
        } else {
            theme::DOT_IDLE
        };
        painter.circle_filled(dot_center, theme::DOT_SIZE / 2.0, dot_color);

        // 3. Mic Icon (SVG)
        // C++ used the active SVG while idle and the inactive/slashed SVG
        // while the app is actively listening/transcribing.
        if let Some(ref icons) = self.icons {
            let tex = if self.is_listening {
                &icons.mic_inactive
            } else {
                &icons.mic_active
            };
            let tex_size = vec2(icon_size, icon_size);
            let tex_rect = Rect::from_center_size(mic_center, tex_size);
            painter.image(
                tex.id(),
                tex_rect,
                Rect::from_min_max(pos2(0.0, 0.0), pos2(1.0, 1.0)),
                Color32::WHITE,
            );
        }

        // 4. Model ComboBox Styling
        painter.rect_filled(combo_rect, Rounding::same(12.0), theme::COMBO_BG);
        let (selected_model_name, selected_model_installed) = {
            let s = self.state.lock().ok();
            match s
                .as_ref()
                .and_then(|s| s.model_entries.get(self.selected_model_idx).cloned())
            {
                Some(e) => (e.name, e.installed),
                None => (
                    self.model_names
                        .get(self.selected_model_idx)
                        .cloned()
                        .unwrap_or_else(|| "No model".into()),
                    true,
                ),
            }
        };
        let model_font = FontId::proportional(12.0);
        let model_text = elide_text_to_width(
            &painter,
            &selected_model_name,
            model_font.clone(),
            (combo_rect.width() - 28.0).max(20.0),
        );
        let model_color = if selected_model_installed {
            theme::COMBO_TEXT
        } else {
            Color32::from_rgb(0x99, 0x99, 0x99)
        };
        painter.text(
            pos2(combo_rect.left() + 10.0, combo_rect.center().y),
            egui::Align2::LEFT_CENTER,
            &model_text,
            model_font,
            model_color,
        );
        draw_combo_arrow(
            &painter,
            pos2(combo_rect.right() - 13.0, combo_rect.center().y),
            Color32::WHITE,
        );

        // 5. Waveform or Status
        if self.is_listening && show_waveform {
            let wave_pad_x = (self.pill_h / 30.0).max(1.0);
            let wave_pad_y = (self.pill_h / 24.0).max(1.0);
            let wave_rect = Rect::from_min_size(
                pos2(
                    status_rect.left() + wave_pad_x,
                    pill_rect.top() + wave_pad_y,
                ),
                vec2(
                    (status_w - wave_pad_x * 2.0).max(10.0),
                    (self.pill_h - wave_pad_y * 2.0).max(10.0),
                ),
            );
            self.waveform.update_history_limit(wave_rect.width());
            let wave_color = if self.is_recording {
                theme::WAVE_RECORDING
            } else {
                theme::WAVE_COLOR
            };
            waveform::draw_waveform(&painter, wave_rect, &self.waveform, wave_color);
        } else {
            let status_font = FontId::proportional(10.0);
            let status_text = elide_text_to_width(
                &painter,
                &self.status_text,
                status_font.clone(),
                (status_w - 8.0).max(0.0),
            );
            painter.text(
                pos2(status_rect.left() + 4.0, cy),
                egui::Align2::LEFT_CENTER,
                &status_text,
                status_font,
                theme::STATUS_COLOR,
            );
        }

        // Collapse chevron rotation animation
        let target_angle = if self.tb_visible { 0.0 } else { 180.0 };
        let collapse_angle =
            ui.ctx()
                .animate_value_with_time(Id::new("collapse_rotation"), target_angle, 0.22);

        // 6. Collapse chevron
        draw_collapse_chevron(
            &painter,
            collapse_rect.center(),
            collapse_angle,
            theme::COLLAPSE_COLOR,
        );

        // 7. Close button (✕) manually drawn using lines to prevent rendering as square
        let close_color = if close_resp.hovered() {
            theme::CLOSE_HOVER
        } else {
            theme::CLOSE_NORMAL
        };
        let stroke = Stroke::new(2.0, close_color);
        let s = close_size / 2.0 - 4.0; // half-size of cross
        let c = close_rect.center();
        painter.line_segment([pos2(c.x - s, c.y - s), pos2(c.x + s, c.y + s)], stroke);
        painter.line_segment([pos2(c.x + s, c.y - s), pos2(c.x - s, c.y + s)], stroke);

        // Interactions
        if dot_resp.double_clicked() {
            self.dash.lock().unwrap().visible = true;
            ui.ctx().request_repaint();
        }

        if mic_resp.clicked() {
            if self.is_listening {
                self.stop_listening();
            } else {
                self.start_listening();
            }
        }
        if mic_resp.hovered() {
            ui.ctx().set_cursor_icon(CursorIcon::PointingHand);
        }

        let combo_resp = ui.allocate_rect(combo_rect, Sense::click());
        if combo_resp.clicked() {
            let origin = ui.ctx().input(|i| {
                i.viewport()
                    .outer_rect
                    .or(i.viewport().inner_rect)
                    .map(|r| r.min)
                    .unwrap_or(pos2(0.0, 0.0))
            });
            self.model_popup_open = !self.model_popup_open;
            self.model_popup_width = combo_w.max(180.0);
            self.model_popup_pos = pos2(
                origin.x + combo_rect.left(),
                origin.y + combo_rect.bottom() + 2.0,
            );
        }
        if combo_resp.hovered() {
            ui.ctx().set_cursor_icon(CursorIcon::PointingHand);
        }

        if close_resp.clicked() {
            self.hide_widget(ui.ctx());
        }

        if collapse_resp.clicked() {
            self.tb_visible = !self.tb_visible;
            ui.ctx().request_repaint();
        }
        if collapse_resp.hovered() {
            ui.ctx().set_cursor_icon(CursorIcon::PointingHand);
        }

        if bg_resp.drag_started() && !self.is_resizing {
            #[cfg(target_os = "windows")]
            if let Some(hwnd) = self.widget_hwnd {
                unsafe {
                    widget_platform::begin_widget_drag(windows::Win32::Foundation::HWND(hwnd));
                }
            } else {
                ui.ctx().send_viewport_cmd(ViewportCommand::StartDrag);
            }
            #[cfg(not(target_os = "windows"))]
            ui.ctx().send_viewport_cmd(ViewportCommand::StartDrag);
        }

        // Open dashboard on right click
        if bg_resp.secondary_clicked() {
            self.dash.lock().unwrap().visible = true;
            ui.ctx().request_repaint();
        }

        // Offload reload check
        if pill_rect.contains(
            ui.ctx()
                .input(|i| i.pointer.hover_pos().unwrap_or_default()),
        ) {
            if self.model_offloaded {
                self.model_offloaded = false;
                self.status_text = "Reloading...".to_string();
                let _ = self.tx_cmd.try_send(OrchestratorCommand::ReloadModel);
            }
        }
    }
}

impl eframe::App for QuickSttApp {
    fn update(&mut self, ctx: &egui::Context, frame: &mut eframe::Frame) {
        // Drain tray-icon, menu, and global-hotkey events first so any widget-
        // visibility / listening changes they apply are picked up by the rest
        // of this frame.
        self.poll_tray_and_hotkeys(ctx);

        #[cfg(target_os = "windows")]
        if let Ok(window_handle) = frame.window_handle() {
            if let raw_window_handle::RawWindowHandle::Win32(handle) = window_handle.as_raw() {
                let hwnd = handle.hwnd.get();
                if self.widget_hwnd != Some(hwnd) {
                    self.widget_hwnd = Some(hwnd);
                    unsafe {
                        widget_platform::configure_widget_window(HWND(hwnd));
                    }
                }
            }
        }

        if ctx.input(|i| i.viewport().close_requested()) {
            ctx.send_viewport_cmd(ViewportCommand::CancelClose);
            self.hide_widget(ctx);
        }

        if !self.initialized {
            let mut visuals = ctx.style().visuals.clone();
            visuals.panel_fill = Color32::TRANSPARENT;
            visuals.window_fill = Color32::TRANSPARENT;
            visuals.widgets.inactive.bg_fill = theme::BTN_BG;
            visuals.widgets.inactive.weak_bg_fill = theme::BTN_BG;
            visuals.widgets.inactive.bg_stroke = Stroke::new(1.0, theme::BTN_BORDER);
            visuals.widgets.hovered.bg_fill = theme::BTN_HOVER;
            visuals.widgets.active.bg_fill = theme::BTN_PRESSED;
            visuals.selection.bg_fill = theme::DASH_ACCENT;
            visuals.override_text_color = Some(theme::DASH_TEXT);
            visuals.popup_shadow = egui::epaint::Shadow::NONE;
            ctx.set_visuals(visuals);
            self.initialized = true;
        }

        // Position the pill bottom-center above the taskbar once, on the very
        // first frame (mirrors where the C++ widget parks itself by default).
        if !self.positioned {
            let (sw, sh) = get_screen_size();
            let px = ((sw - self.pill_w) / 2.0).max(0.0);
            let py = (sh - self.pill_h - 72.0).max(0.0);
            ctx.send_viewport_cmd(ViewportCommand::OuterPosition(pos2(px, py)));
            self.positioned = true;
        }

        self.ensure_icons(ctx);
        self.sync_from_state();
        self.handle_events(ctx);

        // Sync main window visibility (If On-Command Transcription is active, main widget is kept hidden)
        let on_command_active = self.dash.lock().unwrap().on_command_active;
        let mut widget_visible = self.state.lock().unwrap().widget_visible;
        if on_command_active {
            widget_visible = false;
        }

        let target_window_h = self.pill_h;
        let target_window_w = self.pill_w;

        // While resizing we keep updating the viewport's inner size so the
        // OS-level window follows the user's drag in real time.
        let size_delta = (target_window_h - self.last_sent_height).abs() > 0.5
            || (target_window_w - self.last_sent_w).abs() > 0.5;
        if size_delta || widget_visible != self.last_widget_visible {
            ctx.send_viewport_cmd(ViewportCommand::InnerSize(vec2(
                target_window_w,
                target_window_h,
            )));
            ctx.send_viewport_cmd(ViewportCommand::Visible(widget_visible));
            self.last_sent_height = target_window_h;
            self.last_sent_w = target_window_w;
            self.last_widget_visible = widget_visible;

            // Toggle background wakeword per wake_word_mode (Off/Always On/On with Widget) + widget visibility.
            if !on_command_active {
                if let Some(handle) = &self.wakeword_handle {
                    let mode = self.state.lock().map(|s| s.settings.wake_word_mode.clone()).unwrap_or_else(|_| "Off".into());
                    match mode.as_str() {
                        "Always On" => handle.start(),
                        "On with Widget" => {
                            if widget_visible { handle.stop(); } else { handle.start(); }
                        }
                        _ => handle.stop(), // Off
                    }
                }
            }
        }

        let panel_frame = egui::Frame::none().fill(Color32::TRANSPARENT);
        egui::CentralPanel::default()
            .frame(panel_frame)
            .show(ctx, |ui| {
                if widget_visible {
                    self.render_pill(ui);
                } else {
                    ctx.request_repaint_after(Duration::from_millis(100));
                }
            });

        self.render_model_dropdown(ctx);

        // 1. Dashboard Viewport
        let dash_visible = self.dash.lock().unwrap().visible;
        if dash_visible {
            let dash_state = self.dash.clone();
            let app_state = self.state.clone();
            let tx = self.tx_cmd.clone();
            let wakeword_handle_clone = self.wakeword_handle.clone();

            let mut builder = ViewportBuilder::default()
                .with_title("QuickSTT Advanced Dashboard")
                .with_inner_size([theme::DASH_WIDTH, theme::DASH_HEIGHT])
                .with_min_inner_size([600.0, 400.0])
                .with_decorations(true)
                .with_taskbar(true)
                .with_always_on_top();
            let (screen_w, screen_h) = get_screen_size();
            let dash_pos = pos2(
                ((screen_w - theme::DASH_WIDTH).max(0.0)) / 2.0,
                ((screen_h - theme::DASH_HEIGHT).max(0.0)) / 2.0,
            );
            builder = builder.with_position(dash_pos);
            if let Some(icon) = get_icon_data() {
                let icon_arc: std::sync::Arc<egui::IconData> = std::sync::Arc::new(icon.clone());
                builder = builder.with_icon(icon_arc);
            }

            ctx.show_viewport_immediate(
                ViewportId::from_hash_of("quickstt_dashboard"),
                builder,
                move |ctx, _class| {
                    ctx.send_viewport_cmd(ViewportCommand::Focus);
                    if ctx.input(|i: &egui::InputState| i.viewport().close_requested()) {
                        dash_state.lock().unwrap().visible = false;
                    }
                    render_dashboard(ctx, &dash_state, &app_state, &tx, &wakeword_handle_clone);
                },
            );
        }

        // 2. TextBoard Viewport. Attached mode is still a separate frameless
        // tool window, matching the C++ widget instead of expanding the pill.
        if self.tb_visible {
            let app_state = self.state.clone();
            let dash_state = self.dash.clone();
            let root_rect = ctx.input(|i| {
                i.viewport()
                    .outer_rect
                    .or(i.viewport().inner_rect)
                    .unwrap_or_else(|| {
                        Rect::from_min_size(pos2(100.0, 100.0), vec2(self.pill_w, self.pill_h))
                    })
            });
            let tb_width = if self.tb_attached {
                self.pill_w.max(200.0)
            } else {
                theme::TB_DEFAULT_WIDTH
            };

            let mut builder = ViewportBuilder::default()
                .with_title("QuickSTT TextBoard")
                .with_inner_size([tb_width, theme::TB_DEFAULT_HEIGHT])
                .with_decorations(false)
                .with_transparent(true)
                .with_always_on_top()
                .with_taskbar(false);
            if self.tb_attached {
                builder = builder
                    .with_position(pos2(root_rect.left(), root_rect.top() + self.pill_h))
                    .with_resizable(false);
            }
            if let Some(icon) = get_icon_data() {
                let icon_arc: std::sync::Arc<egui::IconData> = std::sync::Arc::new(icon.clone());
                builder = builder.with_icon(icon_arc);
            }

            ctx.show_viewport_immediate(
                ViewportId::from_hash_of("quickstt_textboard"),
                builder,
                |ctx, _class| {
                    render_textboard(ctx, &app_state, &dash_state, &mut self.tb_attached);
                },
            );
        }

        // 3. On-Command Transcription Circular Pill Viewport
        if on_command_active {
            let dash_state = self.dash.clone();
            let pos = self.dash.lock().unwrap().on_command_pos;

            let mut builder = ViewportBuilder::default()
                .with_title("QuickSTT Command Widget")
                .with_inner_size([120.0, 40.0])
                .with_decorations(false)
                .with_transparent(true)
                .with_always_on_top()
                .with_position(pos);
            if let Some(icon) = get_icon_data() {
                let icon_arc: std::sync::Arc<egui::IconData> = std::sync::Arc::new(icon.clone());
                builder = builder.with_icon(icon_arc);
            }

            ctx.show_viewport_immediate(
                ViewportId::from_hash_of("quickstt_on_command"),
                builder,
                |ctx, _class| {
                    render_on_command_pill(ctx, &dash_state, &self.waveform);
                },
            );
        }



        if self.is_listening {
            ctx.request_repaint_after(Duration::from_millis(theme::WAVE_FPS_MS));
        } else {
            ctx.request_repaint_after(Duration::from_millis(100));
        }
    }
}

// ── Startup Utils ──

fn apply_startup_setting(enabled: bool, background: bool) {
    #[cfg(target_os = "windows")]
    unsafe {
        use windows::core::PCWSTR;
        use windows::Win32::System::Registry::{
            RegCloseKey, RegCreateKeyExW, RegDeleteValueW, RegSetValueExW, HKEY_CURRENT_USER,
            KEY_WRITE, REG_OPTION_NON_VOLATILE, REG_SZ,
        };

        let subkey: Vec<u16> = "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
            .encode_utf16()
            .chain(std::iter::once(0))
            .collect();
        let mut hkey = windows::Win32::System::Registry::HKEY::default();
        if RegCreateKeyExW(
            HKEY_CURRENT_USER,
            PCWSTR(subkey.as_ptr()),
            0,
            None,
            REG_OPTION_NON_VOLATILE,
            KEY_WRITE,
            None,
            &mut hkey,
            None,
        )
        .is_ok()
        {
            let name: Vec<u16> = "QuickSTT"
                .encode_utf16()
                .chain(std::iter::once(0))
                .collect();
            if enabled {
                if let Ok(exe_path) = std::env::current_exe() {
                    let mut cmd = format!("\"{}\"", exe_path.to_string_lossy().replace('/', "\\"));
                    if background {
                        cmd.push_str(" --background");
                    }
                    let wide_cmd: Vec<u16> = cmd.encode_utf16().chain(std::iter::once(0)).collect();
                    let _ = RegSetValueExW(
                        hkey,
                        PCWSTR(name.as_ptr()),
                        0,
                        REG_SZ,
                        Some(std::slice::from_raw_parts(
                            wide_cmd.as_ptr() as *const u8,
                            wide_cmd.len() * 2,
                        )),
                    );
                }
            } else {
                let _ = RegDeleteValueW(hkey, PCWSTR(name.as_ptr()));
            }
            let _ = RegCloseKey(hkey);
        }
    }
}

// ── Dashboard Rendering ──

fn render_dashboard(
    ctx: &egui::Context,
    dash: &Arc<Mutex<DashboardState>>,
    state: &Arc<Mutex<AppState>>,
    tx: &mpsc::Sender<OrchestratorCommand>,
    wakeword_handle: &Option<quickstt_core::wakeword_service::WakewordHandle>,
) {
    egui::CentralPanel::default()
        .frame(
            egui::Frame::none()
                .fill(theme::DASH_BG)
                .inner_margin(egui::Margin::same(12.0)),
        )
        .show(ctx, |ui| {
            ui.style_mut().visuals.override_text_color = Some(theme::DASH_TEXT);
            ui.style_mut().visuals.widgets.inactive.bg_fill = theme::DASH_PANEL;
            ui.style_mut().visuals.selection.bg_fill = theme::DASH_ACCENT;

            // Tab bar: Models, Style, General, Updates, Wakeword
            let tab_names = ["Models", "Style", "General", "Updates", "Wakeword"];
            ui.horizontal(|ui| {
                let mut d = dash.lock().unwrap();
                for (i, name) in tab_names.iter().enumerate() {
                    let selected = d.tab == i;
                    let text = egui::RichText::new(*name).size(13.0).color(if selected {
                        Color32::WHITE
                    } else {
                        theme::DASH_TAB_TEXT
                    });
                    if ui.selectable_label(selected, text).clicked() {
                        d.tab = i;
                    }
                }
            });

            ui.separator();
            ui.add_space(8.0);

            let tab = dash.lock().unwrap().tab;
            egui::ScrollArea::vertical().show(ui, |ui| match tab {
                0 => render_models_tab(ui, state, tx),
                1 => render_style_tab(ui, dash, state),
                2 => render_general_tab(ui, dash, state),
                3 => render_updates_tab(ui),
                4 => render_wakeword_tab(ui, dash, state, tx, wakeword_handle),
                _ => {}
            });
        });
}

// ── Models Tab ──

fn render_models_tab(
    ui: &mut egui::Ui,
    state: &Arc<Mutex<AppState>>,
    tx: &mpsc::Sender<OrchestratorCommand>,
) {
    ui.label(
        egui::RichText::new(
            "Local models stay on-device. Choose which models appear in the widget dropdown.",
        )
        .size(12.0)
        .color(Color32::from_rgb(0xAA, 0xBB, 0xCC)),
    );
    ui.add_space(8.0);

    // ── Search row ──
    ui.horizontal(|ui| {
        let mut search_text = ui.data_mut(|d| {
            d.get_temp_mut_or_default::<String>(ui.id().with("model_search"))
                .clone()
        });
        ui.label("Search:");
        if ui
            .add(egui::TextEdit::singleline(&mut search_text).hint_text("Search model library..."))
            .changed()
        {
            ui.data_mut(|d| d.insert_temp(ui.id().with("model_search"), search_text.clone()));
        }
    });
    ui.add_space(8.0);

    let (entries, selected_idx, settings_snapshot) = {
        let s = state.lock().unwrap();
        (
            s.model_entries.clone(),
            s.selected_model,
            s.settings.clone(),
        )
    };

    // ── Quick stats header ──
    let installed_count = entries.iter().filter(|e| e.installed).count();
    let total_count = entries.len();
    ui.group(|ui| {
        ui.horizontal(|ui| {
            ui.label(
                egui::RichText::new(format!(
                    "\u{1F4BE} {} installed / {} available",
                    installed_count, total_count
                ))
                .strong()
                .size(13.0),
            );
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                ui.label(
                    egui::RichText::new(format!(
                        "Selected: {}",
                        if let Some(e) = entries.get(selected_idx) {
                            e.name.clone()
                        } else {
                            "None".into()
                        }
                    ))
                    .italics()
                    .size(12.0)
                    .color(theme::DASH_ACCENT),
                );
            });
        });
    });
    ui.add_space(8.0);

    ui.group(|ui| {
        ui.label(
            egui::RichText::new("Local Model Catalog")
                .strong()
                .size(13.0),
        );
        ui.add_space(4.0);
        ui.label(
            egui::RichText::new(
                "Radio a row to make it the active model. The widget dropdown follows.",
            )
            .size(11.0)
            .color(Color32::from_rgb(0x88, 0x99, 0xAA)),
        );
        ui.add_space(6.0);

        egui::ScrollArea::vertical()
            .max_height(220.0)
            .auto_shrink([false, false])
            .show(ui, |ui| {
                for (i, entry) in entries.iter().enumerate() {
                    ui.horizontal(|ui| {
                        let is_sel = i == selected_idx;
                        if ui.radio(is_sel, "").clicked() {
                            let _ = tx.try_send(OrchestratorCommand::SelectModel(i));
                        }

                        // Installed badge: filled circle for installed,
                        // open circle otherwise. Color code mirrors status.
                        let (status_glyph, status_color) = if entry.installed {
                            ("\u{25CF}", Color32::from_rgb(0x00, 0xCC, 0x00))
                        } else {
                            ("\u{25CB}", Color32::from_rgb(0x99, 0x99, 0x99))
                        };
                        ui.label(
                            egui::RichText::new(status_glyph)
                                .size(11.0)
                                .color(status_color),
                        );

                        let suffix = if entry.installed {
                            "Installed"
                        } else {
                            "Not installed"
                        };
                        let name_color = if entry.installed {
                            Color32::from_rgb(0xE0, 0xE0, 0xE0)
                        } else {
                            Color32::from_rgb(0x99, 0x99, 0x99)
                        };
                        ui.label(
                            egui::RichText::new(format!("{}", entry.name))
                                .color(name_color)
                                .size(12.0),
                        );
                        ui.label(
                            egui::RichText::new(format!("{}MB", entry.size_mb))
                                .color(Color32::from_rgb(0x77, 0x88, 0x99))
                                .size(11.0),
                        );
                        ui.label(
                            egui::RichText::new(format!("\u{2022} {}", suffix))
                                .italics()
                                .color(status_color)
                                .size(11.0),
                        );

                        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                            if entry.installed {
                                if ui
                                    .add(
                                        egui::Button::new(
                                            egui::RichText::new("Uninstall")
                                                .size(11.0)
                                                .color(Color32::from_rgb(0xFF, 0xAA, 0xAA)),
                                        )
                                        .frame(true)
                                        .rounding(Rounding::same(4.0))
                                        .min_size(vec2(72.0, 22.0)),
                                    )
                                    .clicked()
                                {
                                    let root = quickstt_core::models::catalog::models_root();
                                    let all = quickstt_core::models::catalog::all_descriptors();
                                    if let Some(desc) =
                                        all.iter().find(|d| d.name == entry.name)
                                    {
                                        let dir = root.join(&desc.model_dir);
                                        let _ = std::fs::remove_dir_all(&dir);
                                        // Also drop the downloaded vosk runtime when
                                        // uninstalling the Vosk model.
                                        if desc.engine_family
                                            == quickstt_core::models::catalog::EngineFamily::Vosk
                                        {
                                            let _ = std::fs::remove_dir_all(
                                                root.join("runtimes/vosk"),
                                            );
                                        }
                                        if let Ok(mut s) = state.lock() {
                                            s.status_message =
                                                format!("Removed: {}", desc.name);
                                            for e in s.model_entries.iter_mut() {
                                                if e.name == desc.name {
                                                    e.installed = false;
                                                }
                                            }
                                        }
                                    }
                                }
                            } else if ui
                                .add(
                                    egui::Button::new(
                                        egui::RichText::new("Download")
                                            .size(11.0)
                                            .color(Color32::WHITE),
                                    )
                                    .frame(true)
                                    .fill(Color32::from_rgb(0x00, 0xAA, 0xFF))
                                    .rounding(Rounding::same(4.0))
                                    .min_size(vec2(72.0, 22.0)),
                                )
                                .clicked()
                            {
                                let _ =
                                    tx.try_send(OrchestratorCommand::DownloadModel(i));
                            }
                        });
                    });
                }
            });
    });

    ui.add_space(10.0);

    // ── Widget Model Selection summary (mirrors C++ widgetModels list) ──
    ui.group(|ui| {
        ui.label(
            egui::RichText::new("Widget Dropdown Models")
                .strong()
                .size(13.0),
        );
        ui.add_space(4.0);
        ui.label(
            egui::RichText::new(
                "These local + cloud models appear in the floating widget's combobox.",
            )
            .size(11.0)
            .color(Color32::from_rgb(0x88, 0x99, 0xAA)),
        );
        ui.add_space(6.0);

        if settings_snapshot.widget_models.is_empty()
            && settings_snapshot.cloud_widget_models.is_empty()
        {
            ui.label(
                egui::RichText::new("No widget models configured yet. Add some above.")
                    .italics()
                    .size(11.0)
                    .color(Color32::from_rgb(0x99, 0x99, 0x99)),
            );
        } else {
            let mut chip_index = 0;
            for name in settings_snapshot.widget_models.iter() {
                chip_index += 1;
                draw_chip(ui, name, "local", chip_index);
            }
            for name in settings_snapshot.cloud_widget_models.iter() {
                chip_index += 1;
                draw_chip(ui, name, "cloud", chip_index);
            }
            for name in settings_snapshot.favorite_models.iter() {
                chip_index += 1;
                draw_chip(ui, name, "fav", chip_index);
            }
        }
    });

    ui.add_space(10.0);
    ui.group(|ui| {
        ui.label(egui::RichText::new("Runtime Backend").strong().size(13.0));
        ui.add_space(6.0);
        ui.horizontal(|ui| {
            ui.label("Active Runtime:");
            egui::ComboBox::from_id_salt("runtime_backend_combo")
                .selected_text("CPU ONNX Runtime")
                .show_ui(ui, |ui| {
                    let _ = ui.selectable_label(true, "CPU ONNX Runtime");
                    let _ = ui.selectable_label(false, "CPU whisper.cpp Subprocess");
                    let _ = ui.selectable_label(false, "sherpa-onnx DirectML GPU");
                });
        });
        ui.small("This model does not use an optional sherpa-onnx backend runtime.");
    });
}

fn draw_chip(ui: &mut egui::Ui, name: &str, kind: &str, idx: usize) {
    let (fill, stroke) = match kind {
        "cloud" => (
            Color32::from_rgb(0x14, 0x52, 0x7C),
            Color32::from_rgb(0x1E, 0x88, 0xE5),
        ),
        "fav" => (
            Color32::from_rgb(0x70, 0x42, 0x10),
            Color32::from_rgb(0xFF, 0xB3, 0x00),
        ),
        _ => (
            Color32::from_rgb(0x1A, 0x4D, 0x33),
            Color32::from_rgb(0x00, 0xCC, 0x66),
        ),
    };
    let frame = egui::Frame::none()
        .fill(fill)
        .stroke(Stroke::new(1.0, stroke))
        .rounding(Rounding::same(10.0))
        .inner_margin(egui::Margin::symmetric(8.0, 3.0));
    frame.show(ui, |ui| {
        ui.horizontal(|ui| {
            ui.label(
                egui::RichText::new(format!("[{}]", idx))
                    .color(Color32::from_rgb(0xCC, 0xCC, 0xCC))
                    .size(10.0),
            );
            ui.add_space(4.0);
            ui.label(
                egui::RichText::new(name)
                    .color(Color32::from_rgb(0xEE, 0xEE, 0xEE))
                    .size(11.0),
            );
            ui.add_space(4.0);
            let kind_label = match kind {
                "cloud" => "(cloud)",
                "fav" => "(favorite)",
                _ => "(local)",
            };
            ui.label(
                egui::RichText::new(kind_label)
                    .color(Color32::from_rgb(0xBB, 0xBB, 0xBB))
                    .italics()
                    .size(10.0),
            );
            ui.add_space(6.0);
            ui.label(
                egui::RichText::new("\u{2715}")
                    .color(Color32::from_rgb(0xAA, 0xAA, 0xAA))
                    .size(10.0),
            );
        });
    });
    ui.add_space(4.0);
}

// ── Style Tab ──

fn render_style_tab(
    ui: &mut egui::Ui,
    dash: &Arc<Mutex<DashboardState>>,
    state: &Arc<Mutex<AppState>>,
) {
    let mut d = dash.lock().unwrap();
    let mut s = state.lock().unwrap();

    ui.group(|ui| {
        ui.label(
            egui::RichText::new("Widget Corner Roundness & Size")
                .strong()
                .size(13.0),
        );
        ui.add_space(6.0);

        ui.horizontal(|ui| {
            ui.label("Corner Radius:");
            if ui
                .add(egui::Slider::new(&mut d.pill_radius, 5.0..=30.0).suffix("px"))
                .changed()
            {
                let _ = quickstt_core::settings::Settings::save_dword(
                    "pillRadius",
                    d.pill_radius as u32,
                );
                s.settings.pill_radius = d.pill_radius as u32;
            }
        });
        ui.horizontal(|ui| {
            ui.label("Opacity:");
            if ui
                .add(egui::Slider::new(&mut d.opacity_pct, 20.0..=100.0).suffix("%"))
                .changed()
            {
                let _ = quickstt_core::settings::Settings::save_dword(
                    "activeOpacity",
                    d.opacity_pct as u32,
                );
                s.settings.active_opacity = d.opacity_pct as u32;
            }
        });
        ui.horizontal(|ui| {
            ui.label("Icon Size:");
            if ui
                .add(egui::Slider::new(&mut d.icon_size, 16.0..=48.0).suffix("px"))
                .changed()
            {
                let _ =
                    quickstt_core::settings::Settings::save_dword("iconSize", d.icon_size as u32);
                s.settings.icon_size = d.icon_size as u32;
            }
        });
        if ui
            .checkbox(&mut d.flexible, "Allow manual resizing of Pill Widget")
            .changed()
        {
            let _ = quickstt_core::settings::Settings::save_bool("widgetFlexible", d.flexible);
            s.settings.widget_flexible = d.flexible;
        }
    });

    ui.add_space(12.0);
    ui.group(|ui| {
        ui.label(egui::RichText::new("TextBoard Stylers").strong().size(13.0));
        ui.add_space(6.0);

        ui.horizontal(|ui| {
            ui.label("Text Box Opacity:");
            if ui
                .add(egui::Slider::new(&mut d.tb_opacity_pct, 20.0..=100.0).suffix("%"))
                .changed()
            {
                let _ = quickstt_core::settings::Settings::save_dword(
                    "txtOpacity",
                    d.tb_opacity_pct as u32,
                );
                s.settings.txt_opacity = d.tb_opacity_pct as u32;
            }
        });
        ui.horizontal(|ui| {
            ui.label("Text Box Font Size:");
            if ui
                .add(egui::Slider::new(&mut d.tb_text_size, 8.0..=36.0).suffix("px"))
                .changed()
            {
                let _ =
                    quickstt_core::settings::Settings::save_dword("txtSize", d.tb_text_size as u32);
                s.settings.txt_size = d.tb_text_size as u32;
            }
        });
    });

    ui.add_space(12.0);
    ui.group(|ui| {
        ui.label(
            egui::RichText::new("Waveform Visualizer")
                .strong()
                .size(13.0),
        );
        ui.add_space(6.0);
        if ui
            .checkbox(
                &mut d.show_waveform,
                "Show Waveform Animation (when listening)",
            )
            .changed()
        {
            let _ = quickstt_core::settings::Settings::save_bool("showWaveform", d.show_waveform);
            s.settings.show_waveform = d.show_waveform;
        }
    });
}

// ── General Tab ──

fn render_general_tab(
    ui: &mut egui::Ui,
    _dash: &Arc<Mutex<DashboardState>>,
    state: &Arc<Mutex<AppState>>,
) {
    let mut s = state.lock().unwrap();

    ui.group(|ui| {
        ui.label(
            egui::RichText::new("Application Boot Settings")
                .strong()
                .size(13.0),
        );
        ui.add_space(6.0);

        if ui
            .checkbox(&mut s.settings.startup_enabled, "Run on Startup")
            .changed()
        {
            let _ = quickstt_core::settings::Settings::save_bool(
                "startupEnabled",
                s.settings.startup_enabled,
            );
            apply_startup_setting(s.settings.startup_enabled, s.settings.startup_background);
        }
        if ui
            .checkbox(
                &mut s.settings.startup_background,
                "Start minimized to tray on launch",
            )
            .changed()
        {
            let _ = quickstt_core::settings::Settings::save_bool(
                "startupBackground",
                s.settings.startup_background,
            );
            apply_startup_setting(s.settings.startup_enabled, s.settings.startup_background);
        }
        if ui
            .checkbox(
                &mut s.settings.special_commands,
                "Enable special single-word keyboard commands",
            )
            .changed()
        {
            let _ = quickstt_core::settings::Settings::save_bool(
                "specialCommands",
                s.settings.special_commands,
            );
        }
    });

    ui.add_space(12.0);
    ui.group(|ui| {
        ui.label(egui::RichText::new("Recording Options").strong().size(13.0));
        ui.add_space(6.0);
        ui.label(format!("Save directory: {}", s.settings.recording_dir));
    });

    ui.add_space(12.0);
    ui.group(|ui| {
        ui.label(egui::RichText::new("Memory Management").strong().size(13.0));
        ui.add_space(6.0);
        if ui
            .checkbox(
                &mut s.settings.auto_offload,
                "Auto-offload STT model when widget is closed",
            )
            .changed()
        {
            let _ = quickstt_core::settings::Settings::save_bool(
                "autoOffload",
                s.settings.auto_offload,
            );
        }
        ui.horizontal(|ui| {
            ui.label("Offload delay:");
            let mut secs = s.settings.offload_seconds;
            if ui
                .add(egui::Slider::new(&mut secs, 5..=300).suffix("s"))
                .changed()
            {
                s.settings.offload_seconds = secs;
                let _ = quickstt_core::settings::Settings::save_dword("offloadSeconds", secs);
            }
        });
    });

    ui.add_space(12.0);
    ui.group(|ui| {
        ui.label(egui::RichText::new("Wakeword Activation Mode").strong().size(13.0));
        ui.add_space(6.0);
        egui::ComboBox::from_label("Wakeword Mode")
            .selected_text(&s.settings.wake_word_mode)
            .show_ui(ui, |ui| {
                for mode in ["Off", "Always On", "On with Widget"] {
                    if ui.selectable_value(&mut s.settings.wake_word_mode, mode.to_string(), mode).changed() {
                        let _ = quickstt_core::settings::Settings::save_string("wakeWordMode", mode);
                    }
                }
            });
    });

    ui.add_space(12.0);
    ui.group(|ui| {
        ui.label(egui::RichText::new("Quick Transcription (Ctrl+Space)").strong().size(13.0));
        ui.add_space(6.0);
        if ui.checkbox(&mut s.settings.ctrl_space_enabled, "Enable Ctrl+Space quick transcription overlay").changed() {
            let _ = quickstt_core::settings::Settings::save_bool("ctrlSpaceEnabled", s.settings.ctrl_space_enabled);
        }
        if ui.checkbox(&mut s.settings.always_on_pill, "Always-on floating micro pill (docked on desktop)").changed() {
            let _ = quickstt_core::settings::Settings::save_bool("alwaysOnPill", s.settings.always_on_pill);
        }
        egui::ComboBox::from_label("Activation mode")
            .selected_text(if s.settings.ctrl_space_mode == 0 { "Push-to-Talk (hold)" } else { "Toggle (press)" })
            .show_ui(ui, |ui| {
                if ui.selectable_value(&mut s.settings.ctrl_space_mode, 0, "Push-to-Talk (hold)").changed() {
                    let _ = quickstt_core::settings::Settings::save_dword("ctrlSpaceMode", 0);
                }
                if ui.selectable_value(&mut s.settings.ctrl_space_mode, 1, "Toggle (press)").changed() {
                    let _ = quickstt_core::settings::Settings::save_dword("ctrlSpaceMode", 1);
                }
            });
        egui::ComboBox::from_label("Output")
            .selected_text(match s.settings.ctrl_space_output { 0 => "Type into active window", 1 => "Copy to clipboard", _ => "None" })
            .show_ui(ui, |ui| {
                if ui.selectable_value(&mut s.settings.ctrl_space_output, 0, "Type into active window").changed() {
                    let _ = quickstt_core::settings::Settings::save_dword("ctrlSpaceOutput", 0);
                }
                if ui.selectable_value(&mut s.settings.ctrl_space_output, 1, "Copy to clipboard").changed() {
                    let _ = quickstt_core::settings::Settings::save_dword("ctrlSpaceOutput", 1);
                }
                if ui.selectable_value(&mut s.settings.ctrl_space_output, 2, "None (show only)").changed() {
                    let _ = quickstt_core::settings::Settings::save_dword("ctrlSpaceOutput", 2);
                }
            });
        if ui.checkbox(&mut s.settings.on_command_transcription, "On-Command Transcription (legacy Ctrl+Space hold)").changed() {
            let _ = quickstt_core::settings::Settings::save_bool("onCommandTranscription", s.settings.on_command_transcription);
        }
    });
}

// ── Updates Tab ──

fn render_updates_tab(ui: &mut egui::Ui) {
    ui.label(
        egui::RichText::new(
            "QuickSTT updates include performance improvements and new model support.",
        )
        .size(12.0),
    );
    ui.add_space(8.0);

    ui.group(|ui| {
        ui.label(egui::RichText::new("Software Version").strong().size(13.0));
        ui.add_space(4.0);
        ui.label(format!(
            "Current Version: v{}",
            quickstt_core::config::APP_VERSION
        ));
        ui.colored_label(Color32::from_rgb(0x00, 0xCC, 0x00), "System is up-to-date");
    });

    ui.add_space(12.0);
    ui.group(|ui| {
        ui.label(
            egui::RichText::new("Update Server Addresses")
                .strong()
                .size(13.0),
        );
        ui.add_space(4.0);
        ui.label("• https://github.com/xyzyt010/quickstt/releases");
        ui.label("• http://127.0.0.1:5000 (local dev)");
        ui.label("• http://localhost:5000 (local dev)");
        ui.hyperlink_to("Check for updates on GitHub →", "https://github.com/xyzyt010/quickstt/releases");
    });
}

// ── Wakeword Tab ──

fn render_wakeword_tab(
    ui: &mut egui::Ui,
    dash: &Arc<Mutex<DashboardState>>,
    state: &Arc<Mutex<AppState>>,
    _tx: &mpsc::Sender<OrchestratorCommand>,
    wakeword_handle: &Option<quickstt_core::wakeword_service::WakewordHandle>,
) {
    let s = state.lock().unwrap();

    ui.group(|ui| {
        ui.label(
            egui::RichText::new("Wakeword Detection Engine")
                .strong()
                .size(13.0),
        );
        ui.add_space(6.0);
        ui.horizontal(|ui| {
            ui.label("Current Engine:");
            ui.colored_label(Color32::from_rgb(0x00, 0xAA, 0xFF), &s.settings.wake_engine);
        });
        ui.horizontal(|ui| {
            ui.label("Background Mode:");
            match wakeword_handle {
                None => {
                    ui.colored_label(
                        Color32::from_rgb(0xFF, 0xAA, 0x00),
                        "Disabled (no models / init failed)",
                    );
                }
                Some(h) => {
                    let active = h.is_active();
                    let (color, label) = if active {
                        (
                            Color32::from_rgb(0x00, 0xCC, 0x00),
                            "Listening for wakeword...",
                        )
                    } else {
                        (Color32::from_rgb(0x88, 0x88, 0x88), "Idle (widget visible)")
                    };
                    ui.colored_label(color, label);
                }
            }
        });
        ui.add_space(8.0);

        let mut d = dash.lock().unwrap();
        if ui
            .checkbox(
                &mut d.on_command_transcription,
                "On-Command Transcription (Hold Ctrl+Spacebar to Transcribe)",
            )
            .changed()
        {
            let _ = quickstt_core::settings::Settings::save_bool(
                "onCommandTranscription",
                d.on_command_transcription,
            );
        }
    });

    ui.add_space(12.0);
    ui.group(|ui| {
        ui.label(egui::RichText::new("Active Wake Words").strong().size(13.0));
        ui.add_space(6.0);
        for phrase in &s.discovered_wakewords {
            ui.horizontal(|ui| {
                ui.colored_label(Color32::from_rgb(0x00, 0xCC, 0x00), "\u{25CF}");
                ui.label(phrase);
            });
        }
        if s.discovered_wakewords.is_empty() {
            ui.colored_label(
                Color32::from_rgb(0xFF, 0xAA, 0x00),
                "No wakeword models loaded.",
            );
        }
    });

    ui.add_space(12.0);
    ui.group(|ui| {
        ui.label(
            egui::RichText::new("Active Close Words")
                .strong()
                .size(13.0),
        );
        ui.add_space(6.0);
        for word in &s.settings.close_words {
            ui.label(format!("• \"{}\"", word));
        }
    });
}

// ── TextBoard Viewport ──

fn render_textboard(
    ctx: &egui::Context,
    state: &Arc<Mutex<AppState>>,
    dash: &Arc<Mutex<DashboardState>>,
    tb_attached: &mut bool,
) {
    let frame = egui::Frame::none()
        .fill(theme::TB_BG)
        .inner_margin(egui::Margin::same(0.0));

    egui::CentralPanel::default().frame(frame).show(ctx, |ui| {
        let header = {
            let s = state.lock().unwrap();
            if s.status_message.is_empty() {
                "QuickSTT TextBoard".to_string()
            } else {
                s.status_message.clone()
            }
        };

        egui::Frame::none()
            .fill(theme::TB_TITLE_BG)
            .inner_margin(egui::Margin::symmetric(8.0, 0.0))
            .show(ui, |ui| {
                ui.set_height(theme::TB_TITLE_HEIGHT);
                ui.horizontal(|ui| {
                    ui.label(
                        egui::RichText::new(header)
                            .size(11.0)
                            .color(theme::TB_TITLE_COLOR),
                    );
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        let label = if *tb_attached {
                            "\u{26D3}"
                        } else {
                            "\u{1F517}"
                        };
                        let tip = if *tb_attached {
                            "Detach from widget"
                        } else {
                            "Attach to widget"
                        };
                        if ui
                            .add_sized(
                                [26.0, 24.0],
                                egui::Button::new(label)
                                    .fill(Color32::TRANSPARENT)
                                    .stroke(Stroke::NONE),
                            )
                            .on_hover_text(tip)
                            .clicked()
                        {
                            *tb_attached = !*tb_attached;
                        }
                    });
                });
            });

        egui::Frame::none()
            .fill(theme::TB_BG)
            .inner_margin(egui::Margin::symmetric(6.0, 6.0))
            .show(ui, |ui| {
                let (transcript, partial_text) = {
                    let s = state.lock().unwrap();
                    (s.transcript_buffer.clone(), s.partial_result.clone())
                };

                let display_text = if !partial_text.is_empty() {
                    format!("{}\n{}", transcript, partial_text)
                } else if transcript.is_empty() {
                    "Transcript will appear here...".to_string()
                } else {
                    transcript
                };

                let text_color = if display_text.starts_with("Transcript will") {
                    Color32::from_rgb(0x66, 0x66, 0x66)
                } else {
                    theme::TB_TEXT_COLOR
                };

                let tb_text_size = dash.lock().unwrap().tb_text_size;
                egui::ScrollArea::vertical().show(ui, |ui| {
                    ui.add(egui::Label::new(
                        egui::RichText::new(display_text)
                            .monospace()
                            .size(tb_text_size)
                            .color(text_color),
                    ));
                });
            });
    });
}

// ── On-Command Circular Pill Viewport ──

fn render_on_command_pill(
    ctx: &egui::Context,
    _dash: &Arc<Mutex<DashboardState>>,
    waveform: &waveform::WaveformState,
) {
    let frame = egui::Frame::none()
        .fill(Color32::from_rgb(0x1A, 0x1A, 0x1A))
        .stroke(Stroke::new(1.0, Color32::from_rgb(0x33, 0x33, 0x33)))
        .inner_margin(egui::Margin::symmetric(12.0, 6.0));

    egui::CentralPanel::default().frame(frame).show(ctx, |ui| {
        let full_rect = ui.max_rect();
        let rounded = Rounding::same(20.0);
        let painter = ui.painter();

        // Render background circular pill shape
        painter.rect_filled(full_rect, rounded, Color32::from_rgb(0x1A, 0x1A, 0x1A));

        // Draw the waveforms inside
        let wave_rect = full_rect.shrink2(vec2(8.0, 4.0));
        waveform::draw_waveform(painter, wave_rect, waveform, theme::WAVE_RECORDING);
    });
}

// ── Entry Point ──

fn main() -> Result<(), eframe::Error> {
    tracing_subscriber::fmt::init();
    info!("Starting QuickSTT v2.0...");

    // ── Single-instance + CLI handling ──
    let (cli_cmd, cli_background) = parse_cli();
    if forward_to_running_instance(cli_cmd) {
        return Ok(());
    }

    let rt = tokio::runtime::Runtime::new().unwrap();
    let (orchestrator, rx_cmd) = AppOrchestrator::new().expect("Failed to init orchestrator");
    let state = orchestrator.get_state();
    let tx_cmd = orchestrator.get_command_sender();

    // Dashboard state is created up-front so the IPC listener can toggle it.
    let initial_settings = state.lock().unwrap().settings.clone();
    let dash = Arc::new(Mutex::new(DashboardState::from(&initial_settings)));
    spawn_ipc_listener(state.clone(), dash.clone());

    // The audio control thread is spawned inside `AppOrchestrator::new()` so
    // the cpal stream + segmenter live on their own dedicated OS thread. The
    // command loop only needs the sender to dispatch Open/Close signals.
    #[cfg(feature = "audio-capture")]
    let audio_control_tx: std::sync::mpsc::Sender<
        quickstt_core::orchestration::AudioControlCommand,
    > = orchestrator.audio_control_tx_clone();
    #[cfg(feature = "audio-capture")]
    let audio_tx = orchestrator.audio_tx_clone();

    let cmd_state = state.clone();
    rt.spawn(async move {
        #[cfg(feature = "audio-capture")]
        {
            AppOrchestrator::run_command_loop(cmd_state, rx_cmd, audio_control_tx, audio_tx).await;
        }
        #[cfg(not(feature = "audio-capture"))]
        {
            AppOrchestrator::run_command_loop(cmd_state, rx_cmd).await;
        }
    });

    // ── Background wakeword detector ──
    // Loads ONNX once at startup, then listens in the background whenever the
    // floating widget is hidden (i.e. the user has closed it to the system
    // tray). On a positive detection we re-show the widget and start listening.
    let wakeword_handle: Option<quickstt_core::wakeword_service::WakewordHandle> = {
        #[cfg(all(feature = "wakeword", feature = "audio-capture"))]
        {
            use quickstt_core::wakeword_loader;
            use quickstt_core::wakeword_service::spawn_background_service;

            let models_dir = wakeword_loader::default_models_dir();
            match spawn_background_service(&models_dir, tx_cmd.clone()) {
                Some(h) => {
                    info!("Background wakeword service initialised");
                    Some(h)
                }
                None => {
                    warn!("Background wakeword service not available");
                    None
                }
            }
        }
        #[cfg(not(all(feature = "wakeword", feature = "audio-capture")))]
        None
    };

    let mut viewport = ViewportBuilder::default()
        .with_inner_size([theme::PILL_WIDTH, theme::PILL_HEIGHT])
        .with_min_inner_size([200.0, 40.0])
        .with_decorations(false)
        .with_transparent(true)
        .with_always_on_top()
        .with_resizable(false)
        .with_taskbar(false);

    if let Some(icon) = get_icon_data() {
        let icon_arc: std::sync::Arc<egui::IconData> = std::sync::Arc::new(icon.clone());
        viewport = viewport.with_icon(icon_arc);
    }

    let options = eframe::NativeOptions {
        viewport,
        vsync: true, // Fix 100% GPU usage
        ..Default::default()
    };

    eframe::run_native(
        "QuickSTT",
        options,
        Box::new(move |cc| {
            Ok(Box::new(QuickSttApp::new(
                cc,
                state,
                tx_cmd,
                wakeword_handle,
                dash,
                cli_background,
            )))
        }),
    )
}
