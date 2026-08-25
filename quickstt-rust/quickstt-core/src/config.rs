//! Application configuration constants

/// Registry key path for QuickSTT settings
pub const REGISTRY_KEY: &str = r"Software\QuickSTT\Config";

/// Application name
pub const APP_NAME: &str = "QuickSTT";

/// Application version
pub const APP_VERSION: &str = "2.0.0-alpha.1";

/// STT service executable name (platform-specific extension)
#[cfg(target_os = "windows")]
pub const STT_SERVICE_EXE: &str = "stt_service.exe";
#[cfg(not(target_os = "windows"))]
pub const STT_SERVICE_EXE: &str = "stt_service";

/// Default recording directory name
pub const DEFAULT_REC_DIR: &str = "Recordings";

/// Default model directory name
pub const DEFAULT_MODELS_DIR: &str = "models";

/// Pipe protocol newline terminator
pub const PIPE_TERMINATOR: &str = "\n";

/// Health check interval (seconds)
pub const HEALTH_CHECK_INTERVAL_SECS: u64 = 5;

/// Auto-restart delay (seconds)
pub const AUTO_RESTART_DELAY_SECS: u64 = 2;

/// Default auto-offload delay (seconds)
pub const DEFAULT_OFFLOAD_DELAY_SECS: u32 = 15;

/// Default wake word engine label kept compatible with the legacy Qt app.
pub const DEFAULT_WAKE_ENGINE: &str = "OpenWakeWord (TFLite)";

/// Default wake words
pub const DEFAULT_WAKE_WORDS: &[&str] = &["hey jarvis"];

/// Default close words
pub const DEFAULT_CLOSE_WORDS: &[&str] = &["stop listening", "go to sleep"];

/// Default transcription mode (v2.0: always LOCAL, cloud removed)
pub const DEFAULT_TRANSCRIBE_MODE: &str = "LOCAL";

/// Default frontend segmentation mode
pub const DEFAULT_FRONTEND_SEGMENTATION: u8 = 0;

/// Default widget dimensions
pub const DEFAULT_PILL_WIDTH: u32 = 360;
pub const DEFAULT_PILL_HEIGHT: u32 = 50;
pub const DEFAULT_PILL_RADIUS: u32 = 25;

/// Default opacity values
pub const DEFAULT_ACTIVE_OPACITY: u32 = 100;
pub const DEFAULT_TEXT_OPACITY: u32 = 87;

/// Default icon sizes
pub const DEFAULT_ICON_SIZE: u32 = 30;
pub const DEFAULT_TRAY_ICON_SIZE: u32 = 32;

/// Default text size
pub const DEFAULT_TEXT_SIZE: u32 = 14;

/// Default color values (ARGB)
pub const DEFAULT_R_COLOR: u32 = 0xFF0078D7; // Blue
pub const DEFAULT_O_COLOR: u32 = 0xFFFFFFFF; // White

/// Default waveform settings
pub const DEFAULT_SHOW_WAVEFORM: bool = true;
pub const DEFAULT_WAVEFORM_SENSITIVITY: u32 = 50;

/// Default boolean settings
pub const DEFAULT_AUTO_OFFLOAD: bool = true;
pub const DEFAULT_AUTO_MODEL_LOAD: bool = true;
pub const DEFAULT_STARTUP_ENABLED: bool = false;
pub const DEFAULT_STARTUP_BACKGROUND: bool = false;
pub const DEFAULT_SPECIAL_COMMANDS: bool = true;
pub const DEFAULT_HAPTICS: bool = true;
pub const DEFAULT_SOUND: bool = true;
pub const DEFAULT_WIDGET_FLEXIBLE: bool = false;
pub const DEFAULT_FIRST_LAUNCH: bool = true;
pub const DEFAULT_SETUP_COMPLETED: bool = false;

// ──────────────────────────────────────────────────────────────────────
// LEGACY: The following constants were removed in v2.0.
// Smart Life / Tuya, Android TV, and Cloud STT providers are no longer
// part of the Rust architecture. The original C++ source files remain
// in the repository for reference.
// ──────────────────────────────────────────────────────────────────────

/// Local model backends (v2.0: only whisper-rs is active)
pub const LOCAL_BACKENDS: &[&str] = &["Whisper.cpp (whisper-rs)"];

/// Wake word engines (v2.0: only livekit-wakeword)
pub const WAKE_ENGINES: &[&str] = &["livekit-wakeword (ONNX)"];

/// Default Smart Life settings (legacy, preserved for registry compatibility)
pub const DEFAULT_SMART_LIFE_ACCOUNT_MODE: &str = "smart_home";
pub const DEFAULT_SMART_LIFE_ENDPOINT: &str = "https://openapi.tuyaus.com";
pub const DEFAULT_SMART_LIFE_COUNTRY_CODE: &str = "1";
pub const DEFAULT_SMART_LIFE_SCHEMA: &str = "tuyaSmart";

/// Default Android TV settings (legacy)
pub const DEFAULT_ANDROID_TV_AUTO_SCAN: bool = false;
