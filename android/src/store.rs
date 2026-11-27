//! Durable on-disk state: dictation history + user settings.
//!
//! Why hand-rolled JSON: the Android crate deliberately has no serde
//! dependency (the APK stays lean and the host tests stay offline). The
//! format written here is therefore small, fixed and fully owned by this
//! module — one JSON object per line (JSONL) for history, one JSON object
//! for settings, both escaped/parsed by the helpers below and round-trip
//! tested. Every write is atomic: write `<file>.tmp`, fsync, rename over
//! the target, so a force-stop mid-write can never leave a torn file.

use std::path::{Path, PathBuf};

pub const HISTORY_VERSION: u32 = 1;
pub const HISTORY_CAP: usize = 200;
pub const DEFAULT_OFFLOAD_IDLE_SECS: u64 = 30;
pub const MIN_OFFLOAD_IDLE_SECS: u64 = 5;
pub const MAX_OFFLOAD_IDLE_SECS: u64 = 3600;

/// One dictated turn, as persisted in history.
#[derive(Clone, Debug, PartialEq)]
pub struct HistoryRecord {
    /// Stable id (epoch millis at creation; unique enough per device).
    pub id: u64,
    pub text: String,
    pub ts_ms: u64,
    pub seconds: i32,
    pub model: String,
    /// Retained WAV for this turn (relative file name), if kept.
    pub audio: Option<String>,
}

impl HistoryRecord {
    pub fn to_json(&self) -> String {
        let audio = match &self.audio {
            Some(a) => format!("\"{}\"", escape(a)),
            None => "null".to_string(),
        };
        format!(
            "{{\"v\":{HISTORY_VERSION},\"id\":{},\"ts\":{},\"seconds\":{},\"model\":\"{}\",\"audio\":{},\"text\":\"{}\"}}",
            self.id,
            self.ts_ms,
            self.seconds,
            escape(&self.model),
            audio,
            escape(&self.text)
        )
    }

    pub fn from_json(line: &str) -> Option<Self> {
        let line = line.trim();
        if !line.starts_with('{') {
            return None;
        }
        Some(Self {
            id: json_u64(line, "id")?,
            ts_ms: json_u64(line, "ts")?,
            seconds: json_u64(line, "seconds").map(|v| v as i32).unwrap_or(0),
            model: json_string(line, "model").unwrap_or_default(),
            audio: json_string(line, "audio"),
            text: json_string(line, "text")?,
        })
    }
}

/// Versioned JSONL history store. Newest record first.
pub struct HistoryStore {
    path: PathBuf,
    pub records: Vec<HistoryRecord>,
}

impl HistoryStore {
    pub fn load(dir: &Path) -> Self {
        let path = dir.join("history.jsonl");
        let mut records = Vec::new();
        if let Ok(body) = std::fs::read_to_string(&path) {
            for line in body.lines() {
                if let Some(rec) = HistoryRecord::from_json(line) {
                    records.push(rec);
                }
            }
        }
        records.truncate(HISTORY_CAP);
        Self { path, records }
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    /// Insert a turn at the front. Returns audio file names evicted by the
    /// cap so the caller can delete them from disk.
    pub fn add(&mut self, rec: HistoryRecord) -> Vec<String> {
        self.records.insert(0, rec);
        let mut evicted = Vec::new();
        while self.records.len() > HISTORY_CAP {
            if let Some(old) = self.records.pop() {
                if let Some(a) = old.audio {
                    evicted.push(a);
                }
            }
        }
        evicted
    }

    /// Remove one record by id. Returns its audio file name, if any.
    pub fn delete(&mut self, id: u64) -> Option<String> {
        let pos = self.records.iter().position(|r| r.id == id)?;
        self.records.remove(pos).audio
    }

    /// Remove everything. Returns all retained audio file names.
    pub fn clear(&mut self) -> Vec<String> {
        let audio = self.records.drain(..).filter_map(|r| r.audio).collect();
        audio
    }

    pub fn save(&self) -> std::io::Result<()> {
        let mut body = String::new();
        for rec in &self.records {
            body.push_str(&rec.to_json());
            body.push('\n');
        }
        atomic_write(&self.path, body.as_bytes())
    }
}

/// Every persisted user setting. Loaded on start, saved on every change,
/// reapplied to the live atomics by the caller.
#[derive(Clone, Debug, PartialEq)]
pub struct Settings {
    pub auto_offload: bool,
    pub offload_idle_secs: u64,
    pub live_mode: bool,
    pub history_enabled: bool,
    pub pill_on_focus: bool,
    pub haptics: bool,
    pub selected_model: usize,
    pub language: String,
    /// Selected API provider id ("" = on-device model).
    pub api_provider: String,
    /// Selected API model id ("" = on-device model).
    pub api_model: String,
    pub onboarding_done: bool,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            auto_offload: true,
            offload_idle_secs: DEFAULT_OFFLOAD_IDLE_SECS,
            live_mode: true,
            history_enabled: true,
            pill_on_focus: true,
            haptics: true,
            selected_model: 0,
            language: "en".to_string(),
            api_provider: String::new(),
            api_model: String::new(),
            onboarding_done: false,
        }
    }
}

impl Settings {
    pub fn load(dir: &Path) -> Self {
        let path = dir.join("settings.json");
        let Ok(body) = std::fs::read_to_string(path) else {
            return Self::default();
        };
        let mut s = Self::default();
        if let Some(v) = json_bool(&body, "auto_offload") {
            s.auto_offload = v;
        }
        if let Some(v) = json_u64(&body, "offload_idle_secs") {
            s.offload_idle_secs = v.clamp(MIN_OFFLOAD_IDLE_SECS, MAX_OFFLOAD_IDLE_SECS);
        }
        if let Some(v) = json_bool(&body, "live_mode") {
            s.live_mode = v;
        }
        if let Some(v) = json_bool(&body, "history_enabled") {
            s.history_enabled = v;
        }
        if let Some(v) = json_bool(&body, "pill_on_focus") {
            s.pill_on_focus = v;
        }
        if let Some(v) = json_bool(&body, "haptics") {
            s.haptics = v;
        }
        if let Some(v) = json_u64(&body, "selected_model") {
            s.selected_model = v as usize;
        }
        if let Some(v) = json_string(&body, "language") {
            if !v.is_empty() {
                s.language = v;
            }
        }
        if let Some(v) = json_string(&body, "api_provider") {
            s.api_provider = v;
        }
        if let Some(v) = json_string(&body, "api_model") {
            s.api_model = v;
        }
        if let Some(v) = json_bool(&body, "onboarding_done") {
            s.onboarding_done = v;
        }
        s
    }

    pub fn save(&self, dir: &Path) -> std::io::Result<()> {
        let body = format!(
            "{{\"v\":1,\"auto_offload\":{},\"offload_idle_secs\":{},\"live_mode\":{},\"history_enabled\":{},\"pill_on_focus\":{},\"haptics\":{},\"selected_model\":{},\"language\":\"{}\",\"api_provider\":\"{}\",\"api_model\":\"{}\",\"onboarding_done\":{}}}",
            self.auto_offload,
            self.offload_idle_secs.clamp(MIN_OFFLOAD_IDLE_SECS, MAX_OFFLOAD_IDLE_SECS),
            self.live_mode,
            self.history_enabled,
            self.pill_on_focus,
            self.haptics,
            self.selected_model,
            escape(&self.language),
            escape(&self.api_provider),
            escape(&self.api_model),
            self.onboarding_done
        );
        atomic_write(&dir.join("settings.json"), body.as_bytes())
    }

    /// True when dictation should go to a cloud API instead of a local model.
    pub fn using_api(&self) -> bool {
        !self.api_provider.is_empty() && !self.api_model.is_empty()
    }
}

/// Write `bytes` to `path` atomically (tmp file + rename in same dir).
pub fn atomic_write(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let tmp = path.with_extension("tmp");
    {
        use std::io::Write;
        let mut f = std::fs::File::create(&tmp)?;
        f.write_all(bytes)?;
        f.sync_all()?;
    }
    std::fs::rename(&tmp, path)
}

pub fn escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out
}

/// Extract a string field from our flat JSON objects. Handles escapes.
pub fn json_string(body: &str, field: &str) -> Option<String> {
    let key = format!("\"{field}\"");
    let at = body.find(&key)? + key.len();
    let rest = body[at..].trim_start();
    let rest = rest.strip_prefix(':')?.trim_start();
    if rest.starts_with("null") {
        return None;
    }
    let rest = rest.strip_prefix('"')?;
    let mut out = String::new();
    let mut chars = rest.chars();
    while let Some(c) = chars.next() {
        match c {
            '"' => return Some(out),
            '\\' => match chars.next() {
                Some('n') => out.push('\n'),
                Some('t') => out.push('\t'),
                Some('r') => out.push('\r'),
                Some('u') => {
                    let hex: String = chars.by_ref().take(4).collect();
                    if let Ok(code) = u32::from_str_radix(&hex, 16) {
                        if let Some(c) = char::from_u32(code) {
                            out.push(c);
                        }
                    }
                }
                Some(other) => out.push(other),
                None => return None,
            },
            c => out.push(c),
        }
    }
    None
}

pub fn json_u64(body: &str, field: &str) -> Option<u64> {
    let key = format!("\"{field}\"");
    let at = body.find(&key)? + key.len();
    let rest = body[at..].trim_start().strip_prefix(':')?.trim_start();
    let digits: String = rest.chars().take_while(|c| c.is_ascii_digit()).collect();
    digits.parse().ok()
}

pub fn json_bool(body: &str, field: &str) -> Option<bool> {
    let key = format!("\"{field}\"");
    let at = body.find(&key)? + key.len();
    let rest = body[at..].trim_start().strip_prefix(':')?.trim_start();
    if rest.starts_with("true") {
        Some(true)
    } else if rest.starts_with("false") {
        Some(false)
    } else {
        None
    }
}

/// Human "when" label for a history row (relative, no clock dependency in UI).
pub fn when_label(ts_ms: u64, now_ms: u64) -> String {
    let age = now_ms.saturating_sub(ts_ms);
    if age < 60_000 {
        "just now".to_string()
    } else if age < 3_600_000 {
        format!("{}m ago", age / 60_000)
    } else if age < 86_400_000 {
        format!("{}h ago", age / 3_600_000)
    } else {
        format!("{}d ago", age / 86_400_000)
    }
}

pub fn now_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rec(id: u64, text: &str) -> HistoryRecord {
        HistoryRecord {
            id,
            text: text.to_string(),
            ts_ms: id,
            seconds: 3,
            model: "Cactus Whistle 16M".to_string(),
            audio: Some(format!("turn-{id}.wav")),
        }
    }

    #[test]
    fn history_json_roundtrips_escapes() {
        let r = rec(42, "he said \"hi\"\nnew line \\ tab\t ünïcode");
        let back = HistoryRecord::from_json(&r.to_json()).expect("parse");
        assert_eq!(back, r);
        assert_eq!(HistoryRecord::from_json("not json"), None);
        assert_eq!(HistoryRecord::from_json("{\"id\":1}"), None);
    }

    #[test]
    fn history_store_survives_reload_and_caps() {
        let dir = std::env::temp_dir().join(format!("qstt-hist-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let mut store = HistoryStore::load(&dir);
        for i in 0..(HISTORY_CAP as u64 + 5) {
            store.add(rec(1000 + i, "turn"));
        }
        assert_eq!(store.records.len(), HISTORY_CAP);
        store.save().unwrap();
        assert!(!dir.join("history.tmp").exists(), "tmp must be renamed away");
        let reloaded = HistoryStore::load(&dir);
        assert_eq!(reloaded.records.len(), HISTORY_CAP);
        assert_eq!(reloaded.records[0].id, 1000 + HISTORY_CAP as u64 + 4);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn history_delete_and_clear_return_audio() {
        let dir = std::env::temp_dir().join(format!("qstt-hist2-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let mut store = HistoryStore::load(&dir);
        store.add(rec(1, "a"));
        store.add(rec(2, "b"));
        assert_eq!(store.delete(1), Some("turn-1.wav".to_string()));
        assert_eq!(store.delete(99), None);
        assert_eq!(store.clear(), vec!["turn-2.wav".to_string()]);
        assert!(store.records.is_empty());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn settings_roundtrip_and_clamp() {
        let dir = std::env::temp_dir().join(format!("qstt-set-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let mut s = Settings {
            offload_idle_secs: 999_999,
            api_provider: "groq".into(),
            api_model: "whisper-large-v3-turbo".into(),
            ..Settings::default()
        };
        s.save(&dir).unwrap();
        let back = Settings::load(&dir);
        assert_eq!(back.offload_idle_secs, MAX_OFFLOAD_IDLE_SECS);
        assert!(back.using_api());
        assert!(back.history_enabled);
        s.api_model.clear();
        assert!(!s.using_api());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn when_labels_are_relative() {
        assert_eq!(when_label(1_000, 2_000), "just now");
        assert_eq!(when_label(0, 5 * 60_000), "5m ago");
        assert_eq!(when_label(0, 3 * 3_600_000), "3h ago");
        assert_eq!(when_label(0, 2 * 86_400_000), "2d ago");
    }
}
