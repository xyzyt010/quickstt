use egui::Color32;

// Pill widget
pub const PILL_BG: Color32 = Color32::from_rgb(0x1A, 0x1A, 0x1A);
pub const PILL_WIDTH: f32 = 360.0;
pub const PILL_HEIGHT: f32 = 50.0;
pub const PILL_RADIUS: f32 = 25.0;
pub const PILL_OPACITY: f32 = 1.0;
pub const ICON_SIZE: f32 = 30.0;

// Dot states
pub const DOT_IDLE: Color32 = Color32::from_rgb(0x44, 0x44, 0x44);
pub const DOT_LISTENING: Color32 = Color32::from_rgb(0x55, 0x00, 0x00);
pub const DOT_RECORDING_ON: Color32 = Color32::from_rgb(0xFF, 0x22, 0x22);
pub const DOT_RECORDING_OFF: Color32 = Color32::from_rgb(0x88, 0x00, 0x00);
pub const DOT_SIZE: f32 = 10.0;

// Mic icon color
pub const ICON_COLOR: Color32 = Color32::from_rgb(0xE3, 0xE3, 0xE3);

// Combo/dropdown
pub const COMBO_BG: Color32 = Color32::from_rgb(0x33, 0x33, 0x33);
pub const COMBO_TEXT: Color32 = Color32::from_rgb(0xDD, 0xDD, 0xDD);
pub const COMBO_HEIGHT: f32 = 24.0;

// Status label
pub const STATUS_COLOR: Color32 = Color32::from_rgb(0x88, 0x88, 0x88);

// Close button
pub const CLOSE_NORMAL: Color32 = Color32::from_rgb(0x88, 0x88, 0x88);
pub const CLOSE_HOVER: Color32 = Color32::from_rgb(0xFF, 0x44, 0x44);
pub const CLOSE_SIZE: f32 = 18.0;

// Collapse button
pub const COLLAPSE_COLOR: Color32 = Color32::from_rgb(0xCC, 0xCC, 0xCC);
pub const COLLAPSE_SIZE: f32 = 26.0;

// Waveform
pub const WAVE_COLOR: Color32 = Color32::from_rgb(0x88, 0x88, 0x88);
pub const WAVE_RECORDING: Color32 = Color32::from_rgb(0xFF, 0x22, 0x22);
pub const WAVE_FPS_MS: u64 = 16;
pub const WAVE_DELAY_MS: u64 = 45;

// TextBoard
pub const TB_TITLE_HEIGHT: f32 = 28.0;
pub const TB_BG: Color32 = Color32::from_rgba_premultiplied(20, 20, 20, 240);
pub const TB_TITLE_BG: Color32 = Color32::from_rgb(0x1E, 0x1E, 0x1E);
pub const TB_TEXT_COLOR: Color32 = Color32::WHITE;
pub const TB_TITLE_COLOR: Color32 = Color32::from_rgb(0xCC, 0xCC, 0xCC);
pub const TB_DEFAULT_WIDTH: f32 = 420.0;
pub const TB_DEFAULT_HEIGHT: f32 = 180.0;
pub const TB_OPACITY: f32 = 0.87;
pub const TB_TEXT_SIZE: f32 = 14.0;

// Dashboard
pub const DASH_BG: Color32 = Color32::from_rgb(0x12, 0x12, 0x12);
pub const DASH_PANEL: Color32 = Color32::from_rgb(0x1E, 0x1E, 0x1E);
pub const DASH_BORDER: Color32 = Color32::from_rgb(0x33, 0x33, 0x33);
pub const DASH_TEXT: Color32 = Color32::from_rgb(0xE0, 0xE0, 0xE0);
pub const DASH_ACCENT: Color32 = Color32::from_rgb(0x00, 0xAA, 0xFF);
pub const DASH_TAB_BG: Color32 = Color32::from_rgb(0x1E, 0x1E, 0x1E);
pub const DASH_TAB_SELECTED: Color32 = Color32::from_rgb(0x33, 0x33, 0x33);
pub const DASH_TAB_TEXT: Color32 = Color32::from_rgb(0xBB, 0xBB, 0xBB);
pub const DASH_WIDTH: f32 = 700.0;
pub const DASH_HEIGHT: f32 = 650.0;

// Button styles
pub const BTN_BG: Color32 = Color32::from_rgb(0x1E, 0x1E, 0x1E);
pub const BTN_HOVER: Color32 = Color32::from_rgb(0x33, 0x33, 0x33);
pub const BTN_PRESSED: Color32 = Color32::from_rgb(0x44, 0x44, 0x44);
pub const BTN_BORDER: Color32 = Color32::from_rgb(0x33, 0x33, 0x33);

// Slider accent
pub const SLIDER_ACCENT: Color32 = Color32::from_rgb(0x00, 0xAA, 0xFF);

// Auto-offload
pub const OFFLOAD_DEFAULT_SECS: u32 = 15;
pub const HEALTH_CHECK_MS: u64 = 5000;
pub const RAM_COMPACT_MS: u64 = 60000;

// Blink interval (MP3 recording)
pub const BLINK_MS: u64 = 500;

// Margins / layout
pub const LEFT_MARGIN_MIN: f32 = 9.0;
pub const DOT_GAP: f32 = 6.0;
pub const MIC_GAP: f32 = 2.0;
pub const COMBO_GAP: f32 = 4.0;
pub const RIGHT_MARGIN: f32 = 12.0;

// App icon (blue mic)
pub const APP_ICON_BLUE: Color32 = Color32::from_rgb(0x00, 0x71, 0xBC);
pub const FALLBACK_ICON_BG: Color32 = Color32::from_rgb(0x1A, 0x1A, 0x1A);
pub const FALLBACK_ICON_BORDER: Color32 = Color32::from_rgb(0x00, 0xAA, 0xFF);
