//! Persistent per-turn transcription history (dashboard History section).
//!
//! Every transcribed turn lands here as `{text, audio}`: the transcript plus
//! a copy of the utterance wav, so the dashboard can show text, play the
//! audio back, and export/delete per turn. Capped at [`HISTORY_CAP`] turns
//! (oldest pruned with their audio). Ghost singletons ("you", "yeah") are
//! never recorded — history holds deliberate dictation only. Storage is a
//! `history.json` index + one wav per turn under the platform config dir
//! (`%APPDATA%/QuickSTT/history` on Windows, `~/.config/QuickSTT/history`
//! on Linux), so history survives restarts and updates.

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

/// Maximum retained turns (oldest pruned first, audio deleted with them).
pub const HISTORY_CAP: usize = 100;

const HISTORY_FILE: &str = "history.json";

/// One transcribed turn.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct HistoryItem {
    /// Unique id (`"<epoch_ms>"`, suffixed on collision).
    pub id: String,
    /// Creation time (UTC epoch millis).
    pub created_ms: u64,
    /// Transcript text (trimmed).
    pub text: String,
    /// Wav filename inside the history dir (`""` when no audio survived).
    pub audio_file: String,
    /// Audio length in millis (0 when unknown / no audio).
    pub duration_ms: u64,
}

/// Current UTC epoch millis.
pub fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}

/// History directory (created on demand).
pub fn history_dir() -> PathBuf {
    let base = dirs::config_dir().unwrap_or_else(std::env::temp_dir);
    let dir = base.join("QuickSTT").join("history");
    std::fs::create_dir_all(&dir).ok();
    dir
}

fn index_path() -> PathBuf {
    history_dir().join(HISTORY_FILE)
}

/// Load persisted history (newest first). Corrupt/missing index → empty.
pub fn load() -> Vec<HistoryItem> {
    let path = index_path();
    let data = match std::fs::read(&path) {
        Ok(d) => d,
        Err(_) => return Vec::new(),
    };
    serde_json::from_slice(&data).unwrap_or_default()
}

/// Persist the index (audio files are managed by push/delete).
pub fn save(items: &[HistoryItem]) {
    let path = index_path();
    if let Ok(data) = serde_json::to_vec(items) {
        let _ = std::fs::write(path, data);
    }
}

/// Full wav path for an item (`None` when the turn has no audio).
pub fn audio_path(item: &HistoryItem) -> Option<PathBuf> {
    if item.audio_file.is_empty() {
        return None;
    }
    let p = history_dir().join(&item.audio_file);
    if p.is_file() { Some(p) } else { None }
}

/// Wav duration in millis (0 on any read failure).
pub fn wav_duration_ms(path: &Path) -> u64 {
    let reader = match hound::WavReader::open(path) {
        Ok(r) => r,
        Err(_) => return 0,
    };
    let spec = reader.spec();
    if spec.sample_rate == 0 {
        return 0;
    }
    let frames = reader.duration();
    let channels = spec.channels.max(1) as u32;
    frames as u64 * 1000 / spec.sample_rate as u64 / channels as u64
}

/// Record a transcribed turn (newest first). Copies the wav into the history
/// dir, prunes past [`HISTORY_CAP`] (deleting pruned audio), saves the index.
/// Returns true when the turn was recorded.
pub fn push_turn(
    items: &mut Vec<HistoryItem>,
    text: String,
    wav: Option<&Path>,
) -> bool {
    let text = text.trim().to_string();
    if text.is_empty() {
        return false;
    }
    let created = now_ms();
    let dir = history_dir();
    // Unique id + filename (same-ms bursts get suffixed).
    let mut id = created.to_string();
    let mut n = 1u32;
    while items.iter().any(|i| i.id == id) || dir.join(format!("{id}.wav")).is_file() {
        n += 1;
        id = format!("{created}-{n}");
    }
    let (audio_file, duration_ms) = match wav {
        Some(src) if src.is_file() => {
            let name = format!("{id}.wav");
            match std::fs::copy(src, dir.join(&name)) {
                Ok(_) => {
                    let dur = wav_duration_ms(&dir.join(&name));
                    (name, dur)
                }
                Err(_) => (String::new(), 0),
            }
        }
        _ => (String::new(), 0),
    };
    items.insert(
        0,
        HistoryItem {
            id,
            created_ms: created,
            text,
            audio_file,
            duration_ms,
        },
    );
    // Prune oldest past the cap (audio goes with them).
    while items.len() > HISTORY_CAP {
        if let Some(old) = items.pop() {
            if !old.audio_file.is_empty() {
                let _ = std::fs::remove_file(dir.join(&old.audio_file));
            }
        }
    }
    save(items);
    true
}

/// Delete one turn (audio file included). Returns true when found.
pub fn delete_turn(items: &mut Vec<HistoryItem>, id: &str) -> bool {
    let Some(pos) = items.iter().position(|i| i.id == id) else {
        return false;
    };
    let old = items.remove(pos);
    if !old.audio_file.is_empty() {
        let _ = std::fs::remove_file(history_dir().join(&old.audio_file));
    }
    save(items);
    true
}

/// Relative age label (`now_ms` = display time): "just now" / "5m ago" /
/// "3h ago" / "12 Oct" (UTC civil date past 24h — midnight-edge TZ skew at
/// most, no tz database needed).
pub fn display_label(created_ms: u64, now_ms: u64) -> String {
    let age_s = now_ms.saturating_sub(created_ms) / 1000;
    if age_s < 60 {
        return "just now".to_string();
    }
    if age_s < 3600 {
        return format!("{}m ago", age_s / 60);
    }
    if age_s < 86400 {
        return format!("{}h ago", age_s / 3600);
    }
    let (y, m, d) = civil_from_days((created_ms / 86_400_000) as i64);
    let _ = y;
    const MON: [&str; 12] = [
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    ];
    format!("{} {}", d, MON[(m as usize - 1).min(11)])
}

/// `m:ss` duration label.
pub fn fmt_dur(ms: u64) -> String {
    format!("{}:{:02}", ms / 60_000, (ms / 1000) % 60)
}

/// Howard Hinnant's civil-from-days (days since 1970-01-01 → y/m/d).
fn civil_from_days(z: i64) -> (i64, u32, u32) {
    let z = z + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = (z - era * 146_097) as u64;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe as i64 + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = (if mp < 10 { mp + 3 } else { mp - 9 }) as u32;
    (if m <= 2 { y + 1 } else { y }, m, d)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn history_cap_prunes_oldest() {
        // push_turn persists the index: back up any real history file and
        // restore it after, so unit tests never clobber user data.
        let path = index_path();
        let backup = path
            .is_file()
            .then(|| std::fs::read(&path).ok())
            .flatten();
        let mut items = Vec::new();
        // Bypass fs copy with no-audio turns (still exercises cap + save).
        for i in 0..HISTORY_CAP + 5 {
            assert!(push_turn(&mut items, format!("turn {i}"), None));
        }
        assert_eq!(items.len(), HISTORY_CAP);
        assert_eq!(items[0].text, format!("turn {}", HISTORY_CAP + 4));
        let first_id = items[0].id.clone();
        assert!(delete_turn(&mut items, &first_id));
        assert_eq!(items.len(), HISTORY_CAP - 1);
        assert!(!delete_turn(&mut items, "missing"));
        assert!(!push_turn(&mut items, "   ".to_string(), None));
        match backup {
            Some(data) => {
                let _ = std::fs::write(&path, data);
            }
            None => {
                let _ = std::fs::remove_file(&path);
            }
        }
    }

    #[test]
    fn history_labels_sane() {
        assert_eq!(display_label(1000, 30_000), "just now");
        assert_eq!(display_label(0, 5 * 60_000), "5m ago");
        assert_eq!(display_label(0, 3 * 3_600_000), "3h ago");
        assert_eq!(fmt_dur(0), "0:00");
        assert_eq!(fmt_dur(65_000), "1:05");
        assert_eq!(fmt_dur(600_000), "10:00");
    }
}
