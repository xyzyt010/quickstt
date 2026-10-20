// Android speech-to-text: the whisper.cpp engine (aarch64) + the GGML model.
//
// The engine binary is built from the same whisper.cpp sources as the desktop
// build (portable x86-64 baseline) but cross-compiled for arm64-v8a with the
// Android NDK. It is embedded in the APK and written to the app's private
// directory on first run, then executed once per utterance.
//
// The model is NOT bundled: it is downloaded in-app (32 MB) from the same
// HuggingFace source the desktop app uses.

#[cfg(target_os = "android")]
use std::io::Read;
use std::path::{Path, PathBuf};

/// whisper-cli built for aarch64-linux-android (NDK 27, portable baseline).
#[cfg(target_os = "android")]
const WHISPER_CLI: &[u8] = include_bytes!("../prebuilt/whisper-cli-arm64-v8a");

#[cfg(target_os = "android")]
const MODEL_URL: &str =
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en-q5_1.bin";
const MODEL_FILE: &str = "ggml-tiny.en-q5_1.bin";
const MODEL_MIN_BYTES: u64 = 30_000_000;

/// Directory for the engine + models (the app's private files dir).
fn work_dir() -> Option<PathBuf> {
    let base = crate::platform::files_dir()?;
    let dir = base.join("quickstt");
    let _ = std::fs::create_dir_all(&dir);
    Some(dir)
}

/// Write the embedded engine binary to disk and make it executable.
#[cfg(target_os = "android")]
pub fn ensure_engine() -> Result<PathBuf, String> {
    let dir = work_dir().ok_or("no writable app directory")?;
    let path = dir.join("whisper-cli");
    let current = std::fs::metadata(&path).map(|m| m.len()).unwrap_or(0);
    if current == WHISPER_CLI.len() as u64 {
        return Ok(path);
    }
    std::fs::write(&path, WHISPER_CLI).map_err(|e| format!("could not unpack the engine: {e}"))?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let mut perms = std::fs::metadata(&path)
            .map_err(|e| e.to_string())?
            .permissions();
        perms.set_mode(0o755);
        std::fs::set_permissions(&path, perms).map_err(|e| e.to_string())?;
    }
    Ok(path)
}

#[cfg(not(target_os = "android"))]
pub fn ensure_engine() -> Result<PathBuf, String> {
    Err("the Android engine is only available on Android".into())
}

/// Path of the downloaded GGML model.
pub fn model_path() -> Option<PathBuf> {
    work_dir().map(|d| d.join("models").join(MODEL_FILE))
}

/// True when the model is already downloaded.
pub fn model_installed() -> bool {
    model_path()
        .and_then(|p| std::fs::metadata(p).ok())
        .map(|m| m.len() > MODEL_MIN_BYTES)
        .unwrap_or(false)
}

/// Download the model, reporting progress (done, total) in bytes.
#[cfg(target_os = "android")]
pub fn download_model<F: FnMut(u64, u64)>(mut progress: F) -> Result<(), String> {
    let path = model_path().ok_or("no writable app directory")?;
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }
    let part = path.with_extension("part");
    let resp = ureq::get(MODEL_URL)
        .call()
        .map_err(|e| format!("download failed: {e}"))?;
    let total: u64 = resp
        .header("Content-Length")
        .and_then(|v| v.parse().ok())
        .unwrap_or(0);
    let mut reader = resp.into_reader();
    let mut file = std::fs::File::create(&part).map_err(|e| e.to_string())?;
    let mut buf = vec![0u8; 256 * 1024];
    let mut done = 0u64;
    loop {
        let n = reader.read(&mut buf).map_err(|e| e.to_string())?;
        if n == 0 {
            break;
        }
        std::io::Write::write_all(&mut file, &buf[..n]).map_err(|e| e.to_string())?;
        done += n as u64;
        progress(done, total);
    }
    drop(file);
    std::fs::rename(&part, &path).map_err(|e| e.to_string())?;
    Ok(())
}

#[cfg(not(target_os = "android"))]
pub fn download_model<F: FnMut(u64, u64)>(_progress: F) -> Result<(), String> {
    Err("model downloads are only available on Android".into())
}

/// Transcribe a 16 kHz mono WAV with the bundled engine.
pub fn transcribe(wav: &Path) -> Result<String, String> {
    let cli = ensure_engine()?;
    let model = model_path().ok_or("no model downloaded")?;
    let output = std::process::Command::new(&cli)
        .arg("-m")
        .arg(&model)
        .arg("-f")
        .arg(wav)
        .arg("-l")
        .arg("en")
        .arg("--no-timestamps")
        .arg("--suppress-nst")
        .arg("-nth")
        .arg("0.85")
        .arg("-t")
        .arg("4")
        .output()
        .map_err(|e| format!("could not run the engine: {e}"))?;
    if !output.status.success() {
        let err = String::from_utf8_lossy(&output.stderr);
        return Err(format!("engine failed: {err}"));
    }
    let stdout = String::from_utf8_lossy(&output.stdout);
    Ok(clean_transcript(&stdout))
}

/// Same cleanup the desktop app applies: drop whisper's blank-audio markers
/// and bracketed/parenthesised noise lines.
fn clean_transcript(raw: &str) -> String {
    raw.lines()
        .map(|l| l.trim())
        .filter(|l| !l.is_empty())
        .filter(|l| {
            let low = l.to_lowercase();
            if low == "[blank audio]" || low == "[blank_audio]" || low == "[blank]" {
                return false;
            }
            if low.starts_with('[') && low.ends_with(']') {
                return false;
            }
            if low.starts_with('(') && low.ends_with(')') {
                return false;
            }
            true
        })
        .collect::<Vec<_>>()
        .join(" ")
        .trim()
        .to_string()
}
