//! Cross-platform settings management
//!
//! Windows: `HKEY_CURRENT_USER\Software\QuickSTT\Config` (legacy QSettingsCompat)
//! Linux/macOS: `~/.config/QuickSTT/config.toml` (XDG) with TOML serialization
//! Qt's QSettings writes both REG_SZ and REG_DWORD for booleans — Windows helpers handle both.

use crate::config::*;
use crate::error::QuickSttResult;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::path::PathBuf;

// ── Windows Registry imports (Windows only) ──
#[cfg(target_os = "windows")]
use windows::core::PCWSTR;
#[cfg(target_os = "windows")]
use windows::Win32::System::Registry::*;

/// Application settings
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Settings {
    pub selected_model: String,
    pub widget_models: Vec<String>,
    pub cloud_widget_models: Vec<String>,
    pub favorite_models: Vec<String>,
    pub wake_words: Vec<String>,
    pub close_words: Vec<String>,
    pub wake_engine: String,
    pub porcupine_access_key: String,
    pub recording_dir: String,
    pub auto_offload: bool,
    pub offload_seconds: u32,
    pub auto_model_load: bool,
    pub startup_enabled: bool,
    pub startup_background: bool,
    pub special_commands: bool,
    pub haptics: bool,
    pub sound: bool,
    pub widget_flexible: bool,
    pub first_launch: bool,
    pub setup_completed: bool,
    pub pill_width: u32,
    pub pill_height: u32,
    pub pill_radius: u32,
    pub active_opacity: u32,
    pub icon_size: u32,
    pub tray_icon_size: u32,
    pub txt_opacity: u32,
    pub txt_size: u32,
    pub r_color: u32,
    pub o_color: u32,
    pub show_waveform: bool,
    pub waveform_sensitivity: u32,
    pub smart_life_account_mode: String,
    pub smart_life_endpoint: String,
    pub smart_life_access_id: String,
    pub smart_life_access_key: String,
    pub smart_life_developer_uid: String,
    pub smart_life_username: String,
    pub smart_life_password: String,
    pub smart_life_country_code: String,
    pub smart_life_schema: String,
    pub ha_url: String,
    pub ha_token: String,
    pub android_tv_auto_scan: bool,
    #[serde(default = "default_always_on_pill")]
    pub always_on_pill: bool,
    #[serde(default = "default_ctrl_space_enabled")]
    pub ctrl_space_enabled: bool,
    #[serde(default = "default_ctrl_space_mode")]
    pub ctrl_space_mode: u32,
    #[serde(default = "default_ctrl_space_output")]
    pub ctrl_space_output: u32,
    #[serde(default = "default_on_command_transcription")]
    pub on_command_transcription: bool,
    #[serde(default = "default_wake_word_mode")]
    pub wake_word_mode: String,
    /// extra unknown keys preserved for forward compat
    #[serde(default, skip_serializing_if = "HashMap::is_empty")]
    pub extra: HashMap<String, String>,
}

fn default_always_on_pill() -> bool { DEFAULT_ALWAYS_ON_PILL }
fn default_ctrl_space_enabled() -> bool { DEFAULT_CTRL_SPACE_ENABLED }
fn default_ctrl_space_mode() -> u32 { DEFAULT_CTRL_SPACE_MODE }
fn default_ctrl_space_output() -> u32 { DEFAULT_CTRL_SPACE_OUTPUT }
fn default_on_command_transcription() -> bool { DEFAULT_ON_COMMAND_TRANSCRIPTION }
fn default_wake_word_mode() -> String { DEFAULT_WAKE_WORD_MODE.to_string() }

impl Default for Settings {
    fn default() -> Self {
        Self {
            selected_model: String::new(),
            widget_models: vec![],
            cloud_widget_models: vec![],
            favorite_models: vec![],
            wake_words: DEFAULT_WAKE_WORDS.iter().map(|s| s.to_string()).collect(),
            close_words: DEFAULT_CLOSE_WORDS.iter().map(|s| s.to_string()).collect(),
            wake_engine: DEFAULT_WAKE_ENGINE.to_string(),
            porcupine_access_key: String::new(),
            recording_dir: String::new(),
            auto_offload: DEFAULT_AUTO_OFFLOAD,
            offload_seconds: DEFAULT_OFFLOAD_DELAY_SECS,
            auto_model_load: DEFAULT_AUTO_MODEL_LOAD,
            startup_enabled: DEFAULT_STARTUP_ENABLED,
            startup_background: DEFAULT_STARTUP_BACKGROUND,
            special_commands: DEFAULT_SPECIAL_COMMANDS,
            haptics: DEFAULT_HAPTICS,
            sound: DEFAULT_SOUND,
            widget_flexible: DEFAULT_WIDGET_FLEXIBLE,
            first_launch: DEFAULT_FIRST_LAUNCH,
            setup_completed: DEFAULT_SETUP_COMPLETED,
            pill_width: DEFAULT_PILL_WIDTH,
            pill_height: DEFAULT_PILL_HEIGHT,
            pill_radius: DEFAULT_PILL_RADIUS,
            active_opacity: DEFAULT_ACTIVE_OPACITY,
            icon_size: DEFAULT_ICON_SIZE,
            tray_icon_size: DEFAULT_TRAY_ICON_SIZE,
            txt_opacity: DEFAULT_TEXT_OPACITY,
            txt_size: DEFAULT_TEXT_SIZE,
            r_color: DEFAULT_R_COLOR,
            o_color: DEFAULT_O_COLOR,
            show_waveform: DEFAULT_SHOW_WAVEFORM,
            waveform_sensitivity: DEFAULT_WAVEFORM_SENSITIVITY,
            smart_life_account_mode: DEFAULT_SMART_LIFE_ACCOUNT_MODE.to_string(),
            smart_life_endpoint: DEFAULT_SMART_LIFE_ENDPOINT.to_string(),
            smart_life_access_id: String::new(),
            smart_life_access_key: String::new(),
            smart_life_developer_uid: String::new(),
            smart_life_username: String::new(),
            smart_life_password: String::new(),
            smart_life_country_code: DEFAULT_SMART_LIFE_COUNTRY_CODE.to_string(),
            smart_life_schema: DEFAULT_SMART_LIFE_SCHEMA.to_string(),
            ha_url: String::new(),
            ha_token: String::new(),
            android_tv_auto_scan: DEFAULT_ANDROID_TV_AUTO_SCAN,
            always_on_pill: DEFAULT_ALWAYS_ON_PILL,
            ctrl_space_enabled: DEFAULT_CTRL_SPACE_ENABLED,
            ctrl_space_mode: DEFAULT_CTRL_SPACE_MODE,
            ctrl_space_output: DEFAULT_CTRL_SPACE_OUTPUT,
            on_command_transcription: DEFAULT_ON_COMMAND_TRANSCRIPTION,
            wake_word_mode: DEFAULT_WAKE_WORD_MODE.to_string(),
            extra: HashMap::new(),
        }
    }
}

impl Settings {
    /// Config file path — XDG on Linux, APPDATA on Windows
    pub fn config_file_path() -> PathBuf {
        #[cfg(target_os = "windows")]
        {
            if let Ok(appdata) = std::env::var("APPDATA") {
                return PathBuf::from(appdata).join("QuickSTT").join("config.toml");
            }
            Self::data_root().join("config.toml")
        }
        #[cfg(not(target_os = "windows"))]
        {
            if let Some(dir) = dirs::config_dir() {
                return dir.join("QuickSTT").join("config.toml");
            }
            if let Ok(home) = std::env::var("HOME") {
                return PathBuf::from(home).join(".config").join("QuickSTT").join("config.toml");
            }
            PathBuf::from("config.toml")
        }
    }

    /// Load settings — Registry on Windows, TOML file on Linux/macOS
    pub fn load() -> QuickSttResult<Self> {
        #[cfg(target_os = "windows")]
        {
            Self::load_windows()
        }
        #[cfg(not(target_os = "windows"))]
        {
            Self::load_unix()
        }
    }

    #[cfg(target_os = "windows")]
    fn load_windows() -> QuickSttResult<Self> {
        let mut settings = Self::default();
        let key = match open_settings_key() {
            Ok(k) => k,
            Err(_) => {
                // Fallback: try TOML if Registry unavailable (e.g. Wine)
                if let Ok(toml_settings) = Self::load_unix() {
                    // Use TOML values only if non-default to allow migration
                    return Ok(toml_settings);
                }
                return Ok(settings);
            }
        };

        settings.selected_model = read_string(key, "selectedModel")
            .or_else(|| read_string(key, "selected_model"))
            .unwrap_or_default();
        settings.widget_models = read_multi_string(key, "widgetModels").unwrap_or_default();
        if settings.widget_models.len() == 1
            && settings.widget_models[0].trim().eq_ignore_ascii_case("Vosk Small En")
        {
            settings.widget_models.clear();
        }
        settings.cloud_widget_models =
            read_multi_string(key, "cloudWidgetModels").unwrap_or_default();
        settings.favorite_models = read_multi_string(key, "favoriteModels").unwrap_or_default();

        settings.wake_words = read_multi_string(key, "wakeWords")
            .unwrap_or_else(|| DEFAULT_WAKE_WORDS.iter().map(|s| s.to_string()).collect());
        settings.close_words = read_multi_string(key, "closeWords")
            .unwrap_or_else(|| DEFAULT_CLOSE_WORDS.iter().map(|s| s.to_string()).collect());
        settings.wake_engine =
            read_string(key, "wakeEngine").unwrap_or_else(|| DEFAULT_WAKE_ENGINE.to_string());
        settings.porcupine_access_key = read_string(key, "porcupineAccessKey").unwrap_or_default();

        settings.recording_dir = read_string(key, "recordingDir").unwrap_or_default();
        settings.auto_offload = read_bool(key, "autoOffload").unwrap_or(DEFAULT_AUTO_OFFLOAD);
        settings.offload_seconds = read_dword(key, "offloadSeconds")
            .or_else(|| read_dword(key, "offloadMinutes").map(|v| v * 60))
            .unwrap_or(DEFAULT_OFFLOAD_DELAY_SECS);

        settings.auto_model_load =
            read_bool(key, "autoModelLoad").unwrap_or(DEFAULT_AUTO_MODEL_LOAD);
        settings.startup_enabled =
            read_bool(key, "startupEnabled").unwrap_or(DEFAULT_STARTUP_ENABLED);
        settings.startup_background =
            read_bool(key, "startupBackground").unwrap_or(DEFAULT_STARTUP_BACKGROUND);
        settings.special_commands =
            read_bool(key, "specialCommands").unwrap_or(DEFAULT_SPECIAL_COMMANDS);
        settings.haptics = read_bool(key, "haptics").unwrap_or(DEFAULT_HAPTICS);
        settings.sound = read_bool(key, "sound").unwrap_or(DEFAULT_SOUND);
        settings.widget_flexible =
            read_bool(key, "widgetFlexible").unwrap_or(DEFAULT_WIDGET_FLEXIBLE);
        settings.first_launch = read_bool(key, "firstLaunch").unwrap_or(DEFAULT_FIRST_LAUNCH);
        settings.setup_completed =
            read_bool(key, "setupCompleted").unwrap_or(DEFAULT_SETUP_COMPLETED);

        settings.pill_width = read_dword(key, "pillWidth").unwrap_or(DEFAULT_PILL_WIDTH);
        settings.pill_height = read_dword(key, "pillHeight").unwrap_or(DEFAULT_PILL_HEIGHT);
        settings.pill_radius = read_dword(key, "pillRadius").unwrap_or(DEFAULT_PILL_RADIUS);
        settings.active_opacity = read_dword(key, "activeOpacity")
            .or_else(|| read_dword(key, "opacity"))
            .unwrap_or(DEFAULT_ACTIVE_OPACITY);
        settings.icon_size = read_dword(key, "iconSize").unwrap_or(DEFAULT_ICON_SIZE);
        settings.tray_icon_size = read_dword(key, "trayIconSize").unwrap_or(DEFAULT_TRAY_ICON_SIZE);
        settings.txt_opacity = read_dword(key, "txtOpacity").unwrap_or(DEFAULT_TEXT_OPACITY);
        settings.txt_size = read_dword(key, "txtSize").unwrap_or(DEFAULT_TEXT_SIZE);
        settings.r_color = read_dword(key, "r").unwrap_or(DEFAULT_R_COLOR);
        settings.o_color = read_dword(key, "o").unwrap_or(DEFAULT_O_COLOR);

        settings.show_waveform = read_bool(key, "showWaveform").unwrap_or(DEFAULT_SHOW_WAVEFORM);
        settings.waveform_sensitivity =
            read_dword(key, "waveformSensitivity").unwrap_or(DEFAULT_WAVEFORM_SENSITIVITY);

        settings.smart_life_account_mode = read_string(key, "smartLifeAccountMode")
            .unwrap_or_else(|| DEFAULT_SMART_LIFE_ACCOUNT_MODE.to_string());
        settings.smart_life_endpoint = read_string(key, "smartLifeEndpoint")
            .unwrap_or_else(|| DEFAULT_SMART_LIFE_ENDPOINT.to_string());
        settings.smart_life_access_id = read_string(key, "smartLifeAccessId").unwrap_or_default();
        settings.smart_life_access_key = read_string(key, "smartLifeAccessKey").unwrap_or_default();
        settings.smart_life_developer_uid =
            read_string(key, "smartLifeDeveloperUid").unwrap_or_default();
        settings.smart_life_username = read_string(key, "smartLifeUsername").unwrap_or_default();
        settings.smart_life_password = read_string(key, "smartLifePassword").unwrap_or_default();
        settings.smart_life_country_code = read_string(key, "smartLifeCountryCode")
            .unwrap_or_else(|| DEFAULT_SMART_LIFE_COUNTRY_CODE.to_string());
        settings.smart_life_schema = read_string(key, "smartLifeSchema")
            .unwrap_or_else(|| DEFAULT_SMART_LIFE_SCHEMA.to_string());

        settings.ha_url = read_string(key, "haUrl").unwrap_or_default();
        settings.ha_token = read_string(key, "haToken").unwrap_or_default();

        settings.android_tv_auto_scan =
            read_bool(key, "androidTvAutoScan").unwrap_or(DEFAULT_ANDROID_TV_AUTO_SCAN);

        // Rust GUI parity with C++ Qt: always-on pill + Ctrl+Space
        settings.always_on_pill =
            read_bool(key, "alwaysOnPill").unwrap_or(DEFAULT_ALWAYS_ON_PILL);
        settings.ctrl_space_enabled =
            read_bool(key, "ctrlSpaceEnabled").unwrap_or(DEFAULT_CTRL_SPACE_ENABLED);
        settings.ctrl_space_mode =
            read_dword(key, "ctrlSpaceMode").unwrap_or(DEFAULT_CTRL_SPACE_MODE);
        settings.ctrl_space_output =
            read_dword(key, "ctrlSpaceOutput").unwrap_or(DEFAULT_CTRL_SPACE_OUTPUT);
        settings.on_command_transcription =
            read_bool(key, "onCommandTranscription").unwrap_or(DEFAULT_ON_COMMAND_TRANSCRIPTION);
        settings.wake_word_mode =
            read_string(key, "wakeWordMode").unwrap_or_else(|| DEFAULT_WAKE_WORD_MODE.to_string());

        // Migrate legacy extra keys if Registry had them as strings
        Self::migrate_extra(&mut settings);

        unsafe {
            let _ = RegCloseKey(key);
        }
        Ok(settings)
    }

    #[cfg(not(target_os = "windows"))]
    fn load_unix() -> QuickSttResult<Self> {
        Self::load_from_toml()
    }

    #[cfg(target_os = "windows")]
    fn load_unix() -> QuickSttResult<Self> {
        Self::load_from_toml()
    }

    fn load_from_toml() -> QuickSttResult<Self> {
        let path = Self::config_file_path();
        if !path.exists() {
            return Ok(Self::default());
        }
        let content = std::fs::read_to_string(&path)
            .map_err(|e| crate::error::QuickSttError::SettingsError(e.to_string()))?;
        let mut settings: Self = toml::from_str(&content)
            .map_err(|e| crate::error::QuickSttError::SettingsError(e.to_string()))?;
        Self::migrate_extra(&mut settings);
        Ok(settings)
    }

    fn migrate_extra(settings: &mut Self) {
        // Pull legacy keys that were previously stored in `extra` (old Rust builds)
        // into their new typed fields so TOML round-trips correctly.
        let mut migrated = false;
        if let Some(v) = settings.extra.remove("alwaysOnPill") {
            settings.always_on_pill = v.eq_ignore_ascii_case("true") || v == "1";
            migrated = true;
        }
        if let Some(v) = settings.extra.remove("ctrlSpaceEnabled") {
            settings.ctrl_space_enabled = v.eq_ignore_ascii_case("true") || v == "1";
            migrated = true;
        }
        if let Some(v) = settings.extra.remove("ctrlSpaceMode") {
            if let Ok(n) = v.parse::<u32>() {
                settings.ctrl_space_mode = n;
                migrated = true;
            }
        }
        if let Some(v) = settings.extra.remove("ctrlSpaceOutput") {
            if let Ok(n) = v.parse::<u32>() {
                settings.ctrl_space_output = n;
                migrated = true;
            }
        }
        if let Some(v) = settings.extra.remove("onCommandTranscription") {
            settings.on_command_transcription = v.eq_ignore_ascii_case("true") || v == "1";
            migrated = true;
        }
        if let Some(v) = settings.extra.remove("wakeWordMode") {
            settings.wake_word_mode = v.clone();
            migrated = true;
        }
        // Also handle snake_case variants written by some helpers
        for (k, target) in [
            ("always_on_pill", "alwaysOnPill"),
            ("ctrl_space_enabled", "ctrlSpaceEnabled"),
            ("on_command_transcription", "onCommandTranscription"),
        ] {
            if let Some(v) = settings.extra.remove(k) {
                let _ = target; // satisfy unused
                migrated = true;
                match k {
                    "always_on_pill" => settings.always_on_pill = v.eq_ignore_ascii_case("true") || v == "1",
                    "ctrl_space_enabled" => settings.ctrl_space_enabled = v.eq_ignore_ascii_case("true") || v == "1",
                    "on_command_transcription" => settings.on_command_transcription = v.eq_ignore_ascii_case("true") || v == "1",
                    _ => {}
                }
            }
        }
        if migrated {
            let _ = settings.save_to_toml();
        }
    }

    fn save_to_toml(&self) -> QuickSttResult<()> {
        let path = Self::config_file_path();
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent)
                .map_err(|e| crate::error::QuickSttError::SettingsError(e.to_string()))?;
        }
        let content = toml::to_string_pretty(self)
            .map_err(|e| crate::error::QuickSttError::SettingsError(e.to_string()))?;
        std::fs::write(&path, content)
            .map_err(|e| crate::error::QuickSttError::SettingsError(e.to_string()))?;
        Ok(())
    }

    /// Save a single string setting
    pub fn save_string(key_name: &str, value: &str) -> QuickSttResult<()> {
        #[cfg(target_os = "windows")]
        {
            // Try Registry first; fallback to TOML
            if Self::save_string_windows(key_name, value).is_ok() {
                return Ok(());
            }
        }
        let mut s = Self::load_from_toml().unwrap_or_default();
        s.set_string_field(key_name, value);
        s.save_to_toml()
    }

    #[cfg(target_os = "windows")]
    fn save_string_windows(key_name: &str, value: &str) -> QuickSttResult<()> {
        let key = open_settings_key_writable()?;
        let wide: Vec<u16> = value.encode_utf16().chain(std::iter::once(0)).collect();
        let name = to_wide(key_name);
        unsafe {
            let data = std::slice::from_raw_parts(wide.as_ptr() as *const u8, wide.len() * 2);
            RegSetValueExW(key, PCWSTR(name.as_ptr()), 0, REG_SZ, Some(data))?;
            let _ = RegCloseKey(key);
        }
        Ok(())
    }

    /// Save a single DWORD setting
    pub fn save_dword(key_name: &str, value: u32) -> QuickSttResult<()> {
        #[cfg(target_os = "windows")]
        {
            if Self::save_dword_windows(key_name, value).is_ok() {
                return Ok(());
            }
        }
        let mut s = Self::load_from_toml().unwrap_or_default();
        s.set_dword_field(key_name, value);
        s.save_to_toml()
    }

    #[cfg(target_os = "windows")]
    fn save_dword_windows(key_name: &str, value: u32) -> QuickSttResult<()> {
        let key = open_settings_key_writable()?;
        let name = to_wide(key_name);
        unsafe {
            let data = std::slice::from_raw_parts(&value as *const u32 as *const u8, 4);
            RegSetValueExW(key, PCWSTR(name.as_ptr()), 0, REG_DWORD, Some(data))?;
            let _ = RegCloseKey(key);
        }
        Ok(())
    }

    /// Save a boolean setting (as string "true"/"false" for compat)
    pub fn save_bool(key_name: &str, value: bool) -> QuickSttResult<()> {
        Self::save_string(key_name, if value { "true" } else { "false" })
    }

    /// Save a multi-string setting
    pub fn save_multi_string(key_name: &str, values: &[String]) -> QuickSttResult<()> {
        #[cfg(target_os = "windows")]
        {
            if Self::save_multi_string_windows(key_name, values).is_ok() {
                return Ok(());
            }
        }
        let mut s = Self::load_from_toml().unwrap_or_default();
        s.set_multi_string_field(key_name, values);
        s.save_to_toml()
    }

    #[cfg(target_os = "windows")]
    fn save_multi_string_windows(key_name: &str, values: &[String]) -> QuickSttResult<()> {
        let key = open_settings_key_writable()?;
        let name = to_wide(key_name);
        let mut data: Vec<u16> = Vec::new();
        for v in values {
            data.extend(v.encode_utf16());
            data.push(0);
        }
        data.push(0);
        unsafe {
            let bytes = std::slice::from_raw_parts(data.as_ptr() as *const u8, data.len() * 2);
            RegSetValueExW(key, PCWSTR(name.as_ptr()), 0, REG_MULTI_SZ, Some(bytes))?;
            let _ = RegCloseKey(key);
        }
        Ok(())
    }

    // ── TOML helpers: map Qt/Registry key names to struct fields ──
    fn set_string_field(&mut self, key: &str, value: &str) {
        match key {
            "selectedModel" | "selected_model" => self.selected_model = value.to_string(),
            "wakeEngine" => self.wake_engine = value.to_string(),
            "porcupineAccessKey" => self.porcupine_access_key = value.to_string(),
            "recordingDir" => self.recording_dir = value.to_string(),
            "smartLifeAccountMode" => self.smart_life_account_mode = value.to_string(),
            "smartLifeEndpoint" => self.smart_life_endpoint = value.to_string(),
            "smartLifeAccessId" => self.smart_life_access_id = value.to_string(),
            "smartLifeAccessKey" => self.smart_life_access_key = value.to_string(),
            "smartLifeDeveloperUid" => self.smart_life_developer_uid = value.to_string(),
            "smartLifeUsername" => self.smart_life_username = value.to_string(),
            "smartLifePassword" => self.smart_life_password = value.to_string(),
            "smartLifeCountryCode" => self.smart_life_country_code = value.to_string(),
            "smartLifeSchema" => self.smart_life_schema = value.to_string(),
            "haUrl" => self.ha_url = value.to_string(),
            "haToken" => self.ha_token = value.to_string(),
            "alwaysOnPill" | "always_on_pill" => self.always_on_pill = value.eq_ignore_ascii_case("true") || value == "1",
            "ctrlSpaceEnabled" | "ctrl_space_enabled" => self.ctrl_space_enabled = value.eq_ignore_ascii_case("true") || value == "1",
            "onCommandTranscription" | "on_command_transcription" => self.on_command_transcription = value.eq_ignore_ascii_case("true") || value == "1",
            "wakeWordMode" | "wake_word_mode" => self.wake_word_mode = value.to_string(),
            _ => { self.extra.insert(key.to_string(), value.to_string()); }
        }
    }

    fn set_dword_field(&mut self, key: &str, value: u32) {
        match key {
            "pillWidth" => self.pill_width = value,
            "pillHeight" => self.pill_height = value,
            "pillRadius" => self.pill_radius = value,
            "activeOpacity" | "opacity" => self.active_opacity = value,
            "iconSize" => self.icon_size = value,
            "trayIconSize" => self.tray_icon_size = value,
            "txtOpacity" => self.txt_opacity = value,
            "txtSize" => self.txt_size = value,
            "r" => self.r_color = value,
            "o" => self.o_color = value,
            "offloadSeconds" => self.offload_seconds = value,
            "offloadMinutes" => self.offload_seconds = value * 60,
            "waveformSensitivity" => self.waveform_sensitivity = value,
            "ctrlSpaceMode" | "ctrl_space_mode" => self.ctrl_space_mode = value,
            "ctrlSpaceOutput" | "ctrl_space_output" => self.ctrl_space_output = value,
            _ => { self.extra.insert(key.to_string(), value.to_string()); }
        }
    }

    fn set_multi_string_field(&mut self, key: &str, values: &[String]) {
        match key {
            "widgetModels" => self.widget_models = values.to_vec(),
            "cloudWidgetModels" => self.cloud_widget_models = values.to_vec(),
            "favoriteModels" => self.favorite_models = values.to_vec(),
            "wakeWords" => self.wake_words = values.to_vec(),
            "closeWords" => self.close_words = values.to_vec(),
            _ => {
                // Store as comma-joined fallback
                self.extra.insert(key.to_string(), values.join(","));
            }
        }
    }

    /// Persist entire settings object — used by Settings UI
    pub fn save_all(&self) -> QuickSttResult<()> {
        #[cfg(target_os = "windows")]
        {
            // Field-by-field Registry write + TOML mirror for migration
            let key = open_settings_key_writable()?;
            let write_str = |name: &str, val: &str| -> QuickSttResult<()> {
                let wide: Vec<u16> = val.encode_utf16().chain(std::iter::once(0)).collect();
                let wname = to_wide(name);
                unsafe {
                    let data = std::slice::from_raw_parts(wide.as_ptr() as *const u8, wide.len() * 2);
                    RegSetValueExW(key, PCWSTR(wname.as_ptr()), 0, REG_SZ, Some(data))?;
                }
                Ok(())
            };
            let write_dword = |name: &str, val: u32| -> QuickSttResult<()> {
                let wname = to_wide(name);
                unsafe {
                    let data = std::slice::from_raw_parts(&val as *const u32 as *const u8, 4);
                    RegSetValueExW(key, PCWSTR(wname.as_ptr()), 0, REG_DWORD, Some(data))?;
                }
                Ok(())
            };
            let write_multi = |name: &str, vals: &[String]| -> QuickSttResult<()> {
                let wname = to_wide(name);
                let mut data: Vec<u16> = Vec::new();
                for v in vals { data.extend(v.encode_utf16()); data.push(0); }
                data.push(0);
                unsafe {
                    let bytes = std::slice::from_raw_parts(data.as_ptr() as *const u8, data.len()*2);
                    RegSetValueExW(key, PCWSTR(wname.as_ptr()), 0, REG_MULTI_SZ, Some(bytes))?;
                }
                Ok(())
            };
            let _ = write_str("selectedModel", &self.selected_model);
            let _ = write_multi("widgetModels", &self.widget_models);
            let _ = write_multi("cloudWidgetModels", &self.cloud_widget_models);
            let _ = write_multi("favoriteModels", &self.favorite_models);
            let _ = write_multi("wakeWords", &self.wake_words);
            let _ = write_multi("closeWords", &self.close_words);
            let _ = write_str("wakeEngine", &self.wake_engine);
            let _ = write_str("porcupineAccessKey", &self.porcupine_access_key);
            let _ = write_str("recordingDir", &self.recording_dir);
            let _ = write_str("autoOffload", if self.auto_offload { "true" } else { "false" });
            let _ = write_dword("offloadSeconds", self.offload_seconds);
            let _ = write_str("autoModelLoad", if self.auto_model_load { "true" } else { "false" });
            let _ = write_str("startupEnabled", if self.startup_enabled { "true" } else { "false" });
            let _ = write_str("startupBackground", if self.startup_background { "true" } else { "false" });
            let _ = write_str("specialCommands", if self.special_commands { "true" } else { "false" });
            let _ = write_str("haptics", if self.haptics { "true" } else { "false" });
            let _ = write_str("sound", if self.sound { "true" } else { "false" });
            let _ = write_str("widgetFlexible", if self.widget_flexible { "true" } else { "false" });
            let _ = write_str("firstLaunch", if self.first_launch { "true" } else { "false" });
            let _ = write_str("setupCompleted", if self.setup_completed { "true" } else { "false" });
            let _ = write_dword("pillWidth", self.pill_width);
            let _ = write_dword("pillHeight", self.pill_height);
            let _ = write_dword("pillRadius", self.pill_radius);
            let _ = write_dword("activeOpacity", self.active_opacity);
            let _ = write_dword("iconSize", self.icon_size);
            let _ = write_dword("trayIconSize", self.tray_icon_size);
            let _ = write_dword("txtOpacity", self.txt_opacity);
            let _ = write_dword("txtSize", self.txt_size);
            let _ = write_dword("r", self.r_color);
            let _ = write_dword("o", self.o_color);
            let _ = write_str("showWaveform", if self.show_waveform { "true" } else { "false" });
            let _ = write_dword("waveformSensitivity", self.waveform_sensitivity);
            let _ = write_str("smartLifeAccountMode", &self.smart_life_account_mode);
            let _ = write_str("smartLifeEndpoint", &self.smart_life_endpoint);
            let _ = write_str("smartLifeAccessId", &self.smart_life_access_id);
            let _ = write_str("smartLifeAccessKey", &self.smart_life_access_key);
            let _ = write_str("smartLifeDeveloperUid", &self.smart_life_developer_uid);
            let _ = write_str("smartLifeUsername", &self.smart_life_username);
            let _ = write_str("smartLifePassword", &self.smart_life_password);
            let _ = write_str("smartLifeCountryCode", &self.smart_life_country_code);
            let _ = write_str("smartLifeSchema", &self.smart_life_schema);
            let _ = write_str("haUrl", &self.ha_url);
            let _ = write_str("haToken", &self.ha_token);
            let _ = write_str("androidTvAutoScan", if self.android_tv_auto_scan { "true" } else { "false" });
            let _ = write_str("alwaysOnPill", if self.always_on_pill { "true" } else { "false" });
            let _ = write_str("ctrlSpaceEnabled", if self.ctrl_space_enabled { "true" } else { "false" });
            let _ = write_dword("ctrlSpaceMode", self.ctrl_space_mode);
            let _ = write_dword("ctrlSpaceOutput", self.ctrl_space_output);
            let _ = write_str("onCommandTranscription", if self.on_command_transcription { "true" } else { "false" });
            let _ = write_str("wakeWordMode", &self.wake_word_mode);
            unsafe { let _ = RegCloseKey(key); }
            let _ = self.save_to_toml();
            return Ok(());
        }
        #[cfg(not(target_os = "windows"))]
        {
            return self.save_to_toml();
        }
    }

    /// Get the data root directory
    pub fn data_root() -> PathBuf {
        if let Ok(exe_path) = std::env::current_exe() {
            if let Some(exe_dir) = exe_path.parent() {
                // Prefer side-by-side data dir if it exists (portable)
                let candidate = exe_dir.join("data");
                if candidate.exists() {
                    return exe_dir.to_path_buf();
                }
                // Also check exe_dir itself has models
                if exe_dir.join("models").exists() {
                    return exe_dir.to_path_buf();
                }
            }
        }
        #[cfg(target_os = "windows")]
        {
            if let Ok(appdata) = std::env::var("APPDATA") {
                return PathBuf::from(appdata).join("QuickSTT");
            }
        }
        #[cfg(not(target_os = "windows"))]
        {
            if let Some(data_dir) = dirs::data_dir() {
                return data_dir.join("QuickSTT");
            }
            if let Ok(home) = std::env::var("HOME") {
                return PathBuf::from(home).join(".local").join("share").join("QuickSTT");
            }
        }
        PathBuf::from(".")
    }

    /// Get the models root directory
    pub fn models_root() -> PathBuf {
        Self::data_root().join(DEFAULT_MODELS_DIR)
    }

    /// Get the recording directory
    pub fn recording_dir(&self) -> PathBuf {
        if self.recording_dir.is_empty() {
            Self::data_root().join(DEFAULT_REC_DIR)
        } else {
            PathBuf::from(&self.recording_dir)
        }
    }
}

// ── Windows Registry helpers (Windows only) ──
#[cfg(target_os = "windows")]
fn open_settings_key() -> QuickSttResult<HKEY> {
    let subkey = to_wide(REGISTRY_KEY);
    let mut hkey = HKEY::default();
    unsafe {
        RegOpenKeyExW(HKEY_CURRENT_USER, PCWSTR(subkey.as_ptr()), 0, KEY_READ, &mut hkey)?;
    }
    Ok(hkey)
}

#[cfg(target_os = "windows")]
fn open_settings_key_writable() -> QuickSttResult<HKEY> {
    let subkey = to_wide(REGISTRY_KEY);
    let mut hkey = HKEY::default();
    unsafe {
        RegCreateKeyExW(
            HKEY_CURRENT_USER, PCWSTR(subkey.as_ptr()), 0, None,
            REG_OPTION_NON_VOLATILE, KEY_WRITE, None, &mut hkey, None,
        )?;
    }
    Ok(hkey)
}

#[cfg(target_os = "windows")]
fn read_string(key: HKEY, name: &str) -> Option<String> {
    let wide_name = to_wide(name);
    let mut data_type = REG_VALUE_TYPE::default();
    let mut size = 0u32;
    unsafe {
        let status = RegQueryValueExW(key, PCWSTR(wide_name.as_ptr()), None, Some(&mut data_type), None, Some(&mut size));
        if status.is_err() || size == 0 { return None; }
        let mut buf = vec![0u8; size as usize];
        let status = RegQueryValueExW(key, PCWSTR(wide_name.as_ptr()), None, Some(&mut data_type), Some(buf.as_mut_ptr()), Some(&mut size));
        if status.is_err() { return None; }
        let wide_slice = std::slice::from_raw_parts(buf.as_ptr() as *const u16, (size / 2) as usize);
        let trimmed = if wide_slice.last() == Some(&0) { &wide_slice[..wide_slice.len()-1] } else { wide_slice };
        String::from_utf16(trimmed).ok()
    }
}

#[cfg(target_os = "windows")]
fn read_dword(key: HKEY, name: &str) -> Option<u32> {
    let wide_name = to_wide(name);
    let mut data_type = REG_VALUE_TYPE::default();
    let mut value = 0u32;
    let mut size = 4u32;
    unsafe {
        let status = RegQueryValueExW(key, PCWSTR(wide_name.as_ptr()), None, Some(&mut data_type), Some(&mut value as *mut u32 as *mut u8), Some(&mut size));
        if status.is_err() { return None; }
    }
    Some(value)
}

#[cfg(target_os = "windows")]
fn read_bool(key: HKEY, name: &str) -> Option<bool> {
    if let Some(s) = read_string(key, name) { return Some(s.eq_ignore_ascii_case("true") || s == "1"); }
    read_dword(key, name).map(|v| v != 0)
}

#[cfg(target_os = "windows")]
fn read_multi_string(key: HKEY, name: &str) -> Option<Vec<String>> {
    let wide_name = to_wide(name);
    let mut data_type = REG_VALUE_TYPE::default();
    let mut size = 0u32;
    unsafe {
        let status = RegQueryValueExW(key, PCWSTR(wide_name.as_ptr()), None, Some(&mut data_type), None, Some(&mut size));
        if status.is_err() || size == 0 { return None; }
        let mut buf = vec![0u8; size as usize];
        let status = RegQueryValueExW(key, PCWSTR(wide_name.as_ptr()), None, Some(&mut data_type), Some(buf.as_mut_ptr()), Some(&mut size));
        if status.is_err() { return None; }
        let wide_slice = std::slice::from_raw_parts(buf.as_ptr() as *const u16, (size / 2) as usize);
        let mut result = Vec::new();
        let mut current = Vec::new();
        for &ch in wide_slice {
            if ch == 0 {
                if current.is_empty() { break; }
                if let Ok(s) = String::from_utf16(&current) { result.push(s); }
                current.clear();
            } else { current.push(ch); }
        }
        if result.is_empty() { None } else { Some(result) }
    }
}

#[cfg(target_os = "windows")]
fn to_wide(s: &str) -> Vec<u16> { s.encode_utf16().chain(std::iter::once(0)).collect() }

#[cfg(test)]
mod tests {
    use super::*;
    #[test] fn test_settings_default() {
        let settings = Settings::default();
        assert_eq!(settings.wake_engine, DEFAULT_WAKE_ENGINE);
        assert_eq!(settings.wake_words, vec!["hey jarvis"]);
        assert_eq!(settings.pill_width, DEFAULT_PILL_WIDTH);
        assert_eq!(settings.pill_height, DEFAULT_PILL_HEIGHT);
        assert!(settings.first_launch);
        assert!(!settings.setup_completed);
    }
    #[test] fn test_data_root() {
        let root = Settings::data_root();
        assert!(root.is_absolute() || root == PathBuf::from("."));
    }
    #[test] fn test_models_root() {
        let models = Settings::models_root();
        assert!(models.ends_with("models"));
    }
    #[test] fn test_recording_dir_default() {
        let settings = Settings::default();
        let dir = settings.recording_dir();
        assert!(dir.ends_with("Recordings"));
    }
    #[test] fn test_recording_dir_custom() {
        let mut settings = Settings::default();
        settings.recording_dir = "/tmp/custom".to_string();
        let dir = settings.recording_dir();
        assert_eq!(dir, PathBuf::from("/tmp/custom"));
    }
    #[test] fn test_config_file_path() {
        let p = Settings::config_file_path();
        assert!(p.to_string_lossy().contains("QuickSTT"));
    }
    #[test] fn test_toml_roundtrip() {
        let mut s = Settings::default();
        s.selected_model = "Vosk Small En".into();
        s.pill_width = 400;
        let toml_str = toml::to_string(&s).unwrap();
        let back: Settings = toml::from_str(&toml_str).unwrap();
        assert_eq!(back.selected_model, "Vosk Small En");
        assert_eq!(back.pill_width, 400);
    }
}
