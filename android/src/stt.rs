// Android speech-to-text: the whisper.cpp engine (aarch64) + the GGML models.
//
// The engine binary is built from the same whisper.cpp sources as the desktop
// build (portable x86-64 baseline) but cross-compiled for arm64-v8a with the
// Android NDK. It is embedded in the APK and written to the app's private
// directory on first run, then executed once per utterance.
//
// The models are NOT bundled: they are downloaded in-app from the same
// HuggingFace source the desktop app uses. Every entry in `MODELS` is a
// whisper.cpp GGML file that the bundled engine really can load, so the
// Models tab only ever lists engines that work on the phone.

#[cfg(target_os = "android")]
use std::io::Read;
use std::path::{Path, PathBuf};

/// whisper-cli built for aarch64-linux-android (NDK 27, portable baseline).
#[cfg(target_os = "android")]
const WHISPER_CLI: &[u8] = include_bytes!("../prebuilt/whisper-cli-arm64-v8a");

/// A speech model that really runs on Android.
pub struct ModelSpec {
    /// Name shown in the app.
    pub name: &'static str,
    /// File name inside the model directory.
    pub file: &'static str,
    /// Where it is downloaded from.
    pub url: &'static str,
    /// Human readable size.
    pub size: &'static str,
    /// A download smaller than this is broken (a truncated file).
    pub min_bytes: u64,
    /// One line explaining the trade-off.
    pub detail: &'static str,
}

/// The Android model catalog. All three are English-only whisper.cpp models —
/// the phone build has no multilingual engine, so the app never claims
/// auto-detection or languages it cannot do.
pub const MODELS: &[ModelSpec] = &[
    ModelSpec {
        name: "Whisper Tiny EN",
        file: "ggml-tiny.en-q5_1.bin",
        url: "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en-q5_1.bin",
        size: "32 MB",
        min_bytes: 30_000_000,
        detail: "English · fastest · least battery",
    },
    ModelSpec {
        name: "Whisper Base EN",
        file: "ggml-base.en-q5_1.bin",
        url: "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en-q5_1.bin",
        size: "57 MB",
        min_bytes: 55_000_000,
        detail: "English · balanced accuracy and speed",
    },
    ModelSpec {
        name: "Whisper Small EN",
        file: "ggml-small.en-q5_1.bin",
        url: "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.en-q5_1.bin",
        size: "181 MB",
        min_bytes: 180_000_000,
        detail: "English · most accurate · slowest",
    },
];

/// The model used when nothing else was picked.
pub const DEFAULT_MODEL: usize = 0;

/// A model by index (falls back to the default).
pub fn model(index: usize) -> &'static ModelSpec {
    MODELS.get(index).unwrap_or(&MODELS[DEFAULT_MODEL])
}

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

/// Path of a downloaded GGML model.
pub fn model_path(index: usize) -> Option<PathBuf> {
    work_dir().map(|d| d.join("models").join(model(index).file))
}

/// True when the model is already downloaded.
pub fn model_installed(index: usize) -> bool {
    let spec = model(index);
    model_path(index)
        .and_then(|p| std::fs::metadata(p).ok())
        .map(|m| m.len() > spec.min_bytes)
        .unwrap_or(false)
}

/// Remember the model the user picked (the app restarts with it).
pub fn save_selected(index: usize) {
    if let Some(dir) = work_dir() {
        let _ = std::fs::write(dir.join("selected_model"), index.to_string());
    }
}

/// The model the user picked (defaults to the fastest one).
pub fn selected() -> usize {
    work_dir()
        .and_then(|d| std::fs::read_to_string(d.join("selected_model")).ok())
        .and_then(|s| s.trim().parse::<usize>().ok())
        .filter(|i| *i < MODELS.len())
        .unwrap_or(DEFAULT_MODEL)
}

/// Download a model, reporting progress (done, total) in bytes.
#[cfg(target_os = "android")]
pub fn download_model<F: FnMut(u64, u64)>(index: usize, mut progress: F) -> Result<(), String> {
    let spec = model(index);
    let path = model_path(index).ok_or("no writable app directory")?;
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }
    let part = path.with_extension("part");
    let resp = ureq::get(spec.url)
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
    // A truncated download is worse than none: the engine would fail on it.
    let written = std::fs::metadata(&part).map(|m| m.len()).unwrap_or(0);
    if written < spec.min_bytes {
        let _ = std::fs::remove_file(&part);
        return Err(format!(
            "the download was incomplete ({written} of {total} bytes)"
        ));
    }
    std::fs::rename(&part, &path).map_err(|e| e.to_string())?;
    Ok(())
}

#[cfg(not(target_os = "android"))]
pub fn download_model<F: FnMut(u64, u64)>(_index: usize, _progress: F) -> Result<(), String> {
    Err("model downloads are only available on Android".into())
}

/// Transcribe a 16 kHz mono WAV with the bundled engine.
///
/// The engine gets a hard timeout: a model that hangs must not freeze the
/// app, and the user always gets an answer (or a clear error).
pub fn transcribe(wav: &Path, index: usize) -> Result<String, String> {
    let cli = ensure_engine()?;
    let model_file = model_path(index).ok_or("no model downloaded")?;
    if !model_installed(index) {
        return Err("the speech model is not downloaded yet".into());
    }
    let mut child = std::process::Command::new(&cli)
        .arg("-m")
        .arg(&model_file)
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
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .map_err(|e| format!("could not run the engine: {e}"))?;

    // Read both pipes on their own threads so a full pipe cannot deadlock
    // the engine, then wait with a deadline.
    let mut stdout = child.stdout.take();
    let mut stderr = child.stderr.take();
    let out_handle = std::thread::spawn(move || {
        let mut buf = String::new();
        if let Some(mut s) = stdout.take() {
            use std::io::Read as _;
            let _ = s.read_to_string(&mut buf);
        }
        buf
    });
    let err_handle = std::thread::spawn(move || {
        let mut buf = String::new();
        if let Some(mut s) = stderr.take() {
            use std::io::Read as _;
            let _ = s.read_to_string(&mut buf);
        }
        buf
    });

    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(120);
    let status = loop {
        match child.try_wait() {
            Ok(Some(status)) => break Some(status),
            Ok(None) => {
                if std::time::Instant::now() > deadline {
                    let _ = child.kill();
                    let _ = child.wait();
                    break None;
                }
                std::thread::sleep(std::time::Duration::from_millis(50));
            }
            Err(e) => return Err(format!("the engine could not be waited for: {e}")),
        }
    };
    let stdout = out_handle.join().unwrap_or_default();
    let stderr = err_handle.join().unwrap_or_default();

    let Some(status) = status else {
        return Err("the engine did not finish in time".into());
    };
    if !status.success() {
        let err = stderr.trim();
        return Err(if err.is_empty() {
            format!("the engine failed ({status})")
        } else {
            format!("the engine failed: {err}")
        });
    }
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

#[cfg(test)]
mod tests {
    use super::*;

    /// The transcript is what ends up in the other app, so the noise lines
    /// whisper adds for silence have to be gone.
    #[test]
    fn transcript_drops_the_noise_markers() {
        let raw = "\n[BLANK_AUDIO]\n  hello there  \n(inaudible)\nhow are you\n";
        assert_eq!(clean_transcript(raw), "hello there how are you");
    }

    /// A model the engine cannot load is worse than no model at all, so the
    /// catalog must only contain sane, distinct entries.
    #[test]
    fn model_catalog_is_real() {
        assert!(!MODELS.is_empty());
        let mut files: Vec<&str> = Vec::new();
        for spec in MODELS {
            assert!(spec.file.ends_with(".bin"), "{} is not a ggml file", spec.name);
            assert!(
                spec.url.ends_with(spec.file),
                "{} is downloaded from a different file",
                spec.name
            );
            assert!(spec.min_bytes > 0, "{} has no size check", spec.name);
            assert!(!spec.size.is_empty(), "{} has no size label", spec.name);
            assert!(!files.contains(&spec.file), "{} is listed twice", spec.name);
            files.push(spec.file);
        }
        // The default model is the fastest one and it must exist.
        assert!(MODELS.get(DEFAULT_MODEL).is_some());
        assert_eq!(model(999).file, MODELS[DEFAULT_MODEL].file);
    }
}
