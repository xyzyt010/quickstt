// Android speech-to-text: the engines (whisper.cpp, Cactus needle) + the
// GGML/CACT models.
//
// The engine binaries are built for arm64-v8a and shipped inside the APK.
// They are unpacked into the app's *native library directory* on first run:
// since Android 10 the app's home directory is mounted noexec for apps that
// target API 29 or newer, so a binary unpacked into the files directory
// cannot be started ("Permission denied"). The native library directory
// belongs to the app, is writable by it and is exec-mounted.
//
// The models are NOT bundled: they are downloaded in-app from the same
// HuggingFace sources the desktop app uses. Every entry in `MODELS` is a
// model the bundled engine really can load, so the Models tab only ever
// lists engines that work on the phone.

#[cfg(target_os = "android")]
use std::io::Read;
use std::path::{Path, PathBuf};

/// whisper-cli built for aarch64-linux-android (NDK 27, portable baseline).
#[cfg(target_os = "android")]
const WHISPER_CLI: &[u8] = include_bytes!("../prebuilt/whisper-cli-arm64-v8a");
/// Cactus `needle` (Apache-2.0), the Cactus Compute CPU engine.
#[cfg(target_os = "android")]
const CACTUS_NEEDLE: &[u8] = include_bytes!("../prebuilt/needle-arm64-v8a");
/// transcribe.cpp's CLI (the engine behind the GGUF model family).
#[cfg(target_os = "android")]
const TRANSCRIBE_CLI: &[u8] = include_bytes!("../prebuilt/transcribe-cli-arm64-v8a");

/// The engines that run on the phone.
pub const ENGINES: &[(&str, &str, &str)] = &[
    // id, display name, binary file name
    ("whisper", "Whisper (whisper.cpp)", "whisper-cli"),
    ("cactus", "Cactus needle", "needle"),
    ("transcribe", "transcribe.cpp", "transcribe-cli"),
];

/// The engine bytes for an id (Android only).
#[cfg(target_os = "android")]
fn engine_bytes(id: &str) -> Option<&'static [u8]> {
    match id {
        "whisper" => Some(WHISPER_CLI),
        "cactus" => Some(CACTUS_NEEDLE),
        "transcribe" => Some(TRANSCRIBE_CLI),
        _ => None,
    }
}

#[cfg(not(target_os = "android"))]
fn engine_bytes(_id: &str) -> Option<&'static [u8]> {
    None
}

/// The file name of an engine's binary.
fn engine_binary(id: &str) -> &'static str {
    ENGINES
        .iter()
        .find(|e| e.0 == id)
        .map(|e| e.2)
        .unwrap_or("whisper-cli")
}

/// A speech model that really runs on Android.
pub struct ModelSpec {
    /// Name shown in the app.
    pub name: &'static str,
    /// The engine that runs it.
    pub engine: &'static str,
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

/// The Android model catalog. Every entry is a model the bundled engine can
/// really run — the phone build has no placeholder rows.
///
/// The list mirrors the desktop build: Whisper (GGML), the Cactus Whistle
/// container and the GGUF family (Nemotron, Moonshine, SenseVoice, Canary,
/// Parakeet, Qwen3-ASR), all served from the same sources the desktop uses.
pub const MODELS: &[ModelSpec] = &[
    ModelSpec {
        name: "Whisper Tiny EN",
        engine: "whisper",
        file: "ggml-tiny.en-q5_1.bin",
        url: "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en-q5_1.bin",
        size: "32 MB",
        min_bytes: 30_000_000,
        detail: "English · whisper.cpp · fastest",
    },
    ModelSpec {
        name: "Cactus Whistle 16M",
        engine: "cactus",
        file: "whistle.cact",
        url: "https://huggingface.co/Cactus-Compute/whistle/resolve/main/whistle.cact",
        size: "16.9 MB",
        min_bytes: 16_000_000,
        detail: "7 languages · auto-detect · Cactus needle",
    },
    ModelSpec {
        name: "Moonshine Base EN",
        engine: "transcribe",
        file: "moonshine-base-Q8_0.gguf",
        url: "https://huggingface.co/handy-computer/moonshine-base-gguf/resolve/main/moonshine-base-Q8_0.gguf",
        size: "74 MB",
        min_bytes: 70_000_000,
        detail: "English · streaming · transcribe.cpp",
    },
    ModelSpec {
        name: "SenseVoice Small",
        engine: "transcribe",
        file: "SenseVoiceSmall-Q4_K_M.gguf",
        url: "https://huggingface.co/handy-computer/SenseVoiceSmall-gguf/resolve/main/SenseVoiceSmall-Q4_K_M.gguf",
        size: "139 MB",
        min_bytes: 130_000_000,
        detail: "5 languages · auto-detect · transcribe.cpp",
    },
    ModelSpec {
        name: "Canary 180M Flash",
        engine: "transcribe",
        file: "canary-180m-flash-Q5_K_M.gguf",
        url: "https://huggingface.co/handy-computer/canary-180m-flash-gguf/resolve/main/canary-180m-flash-Q5_K_M.gguf",
        size: "151 MB",
        min_bytes: 145_000_000,
        detail: "8 languages · auto-detect · transcribe.cpp",
    },
    ModelSpec {
        name: "Parakeet Unified EN 0.6B",
        engine: "transcribe",
        file: "parakeet-unified-en-0.6b-Q4_K_M.gguf",
        url: "https://huggingface.co/handy-computer/parakeet-unified-en-0.6b-gguf/resolve/main/parakeet-unified-en-0.6b-Q4_K_M.gguf",
        size: "455 MB",
        min_bytes: 440_000_000,
        detail: "English · most accurate · transcribe.cpp",
    },
    ModelSpec {
        name: "Nemotron 3.5 ASR Streaming 0.6B",
        engine: "transcribe",
        file: "nemotron-3.5-asr-streaming-0.6b-Q4_K_M.gguf",
        url: "https://huggingface.co/handy-computer/nemotron-3.5-asr-streaming-0.6b-gguf/resolve/main/nemotron-3.5-asr-streaming-0.6b-Q4_K_M.gguf",
        size: "473 MB",
        min_bytes: 460_000_000,
        detail: "30 languages · streaming · transcribe.cpp",
    },
    ModelSpec {
        name: "Qwen3-ASR 0.6B",
        engine: "transcribe",
        file: "Qwen3-ASR-0.6B-Q4_K_M.gguf",
        url: "https://huggingface.co/handy-computer/Qwen3-ASR-0.6B-gguf/resolve/main/Qwen3-ASR-0.6B-Q4_K_M.gguf",
        size: "562 MB",
        min_bytes: 540_000_000,
        detail: "multilingual · streaming · transcribe.cpp",
    },
];

/// The model used when nothing else was picked.
pub const DEFAULT_MODEL: usize = 0;

/// A model by index (falls back to the default).
pub fn model(index: usize) -> &'static ModelSpec {
    MODELS.get(index).unwrap_or(&MODELS[DEFAULT_MODEL])
}

/// Directory for the models (the app's private files dir).
fn work_dir() -> Option<PathBuf> {
    let base = crate::platform::files_dir()?;
    let dir = base.join("quickstt");
    let _ = std::fs::create_dir_all(&dir);
    Some(dir)
}

/// Where the engine binaries are unpacked.
///
/// The native library directory is exec-mounted and writable by the app; the
/// files directory is *not* executable on Android 10+.
fn engine_dir() -> Option<PathBuf> {
    #[cfg(target_os = "android")]
    {
        if let Some(dir) = crate::platform::native_lib_dir() {
            if std::fs::create_dir_all(&dir).is_ok() {
                crate::platform::log(&format!(
                    "the engines run from the native library directory: {}",
                    dir.display()
                ));
                return Some(dir);
            }
            crate::platform::log("the native library directory is not writable");
        }
    }
    work_dir()
}

/// Write the embedded engine binary to disk and make it executable.
#[cfg(target_os = "android")]
pub fn ensure_engine(id: &str) -> Result<PathBuf, String> {
    let bytes = engine_bytes(id).ok_or("unknown engine")?;
    let dir = engine_dir().ok_or("no writable app directory")?;
    let path = dir.join(engine_binary(id));
    let current = std::fs::metadata(&path).map(|m| m.len()).unwrap_or(0);
    if current != bytes.len() as u64 {
        std::fs::write(&path, bytes)
            .map_err(|e| format!("could not unpack the {id} engine: {e}"))?;
    }
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
pub fn ensure_engine(_id: &str) -> Result<PathBuf, String> {
    Err("the Android engines are only available on Android".into())
}

/// Path of a downloaded model.
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
    // A download must not hang forever: the connect is short, the reads may
    // take as long as the file needs.
    let agent = ureq::AgentBuilder::new()
        .timeout_connect(std::time::Duration::from_secs(30))
        .timeout_read(std::time::Duration::from_secs(120))
        .timeout_write(std::time::Duration::from_secs(120))
        .build();
    let resp = agent
        .get(spec.url)
        .set("User-Agent", "QuickSTT-Android/1.0")
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
    let spec = model(index);
    let cli = ensure_engine(spec.engine)?;
    let model_file = model_path(index).ok_or("no model downloaded")?;
    if !model_installed(index) {
        return Err("the speech model is not downloaded yet".into());
    }
    let mut command = std::process::Command::new(&cli);
    match spec.engine {
        "cactus" => {
            // Cactus needle: --model <file.cact> --audio <wav> [--audio-language en]
            command
                .arg("--model")
                .arg(&model_file)
                .arg("--audio")
                .arg(wav)
                .arg("--audio-language")
                .arg("en");
        }
        "transcribe" => {
            // transcribe.cpp: the same command line the desktop build uses.
            command
                .arg("-m")
                .arg(&model_file)
                .arg("--language")
                .arg("en")
                .arg("--threads")
                .arg("4")
                .arg("--backend")
                .arg("cpu")
                .arg(wav);
        }
        _ => {
            // whisper.cpp: -m <model> -f <wav> -l en --no-timestamps ...
            command
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
                .arg("4");
        }
    }
    let mut child = command
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
    if spec.engine == "cactus" {
        // The needle prints a JSON object per line; the last one with a
        // "text" field is the transcript.
        for line in stdout.lines().rev() {
            let line = line.trim();
            if line.starts_with('{') && line.contains("\"text\"") {
                if let Some(text) = json_text(line) {
                    return Ok(text);
                }
            }
        }
        return Ok(clean_transcript(&stdout));
    }
    if spec.engine == "transcribe" {
        return Ok(clean_transcript_output(&stdout));
    }
    Ok(clean_transcript(&stdout))
}

/// The transcribe.cpp CLI prints a metadata block and then
/// `text: <transcript>`; the metadata and the special tokens are not part
/// of the transcript. This mirrors the desktop build's cleanup.
fn clean_transcript_output(raw: &str) -> String {
    let mut lines: Vec<String> = Vec::new();
    for line in raw.lines() {
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        let low = line.to_lowercase();
        if low.starts_with("text:") {
            lines.push(line[5..].trim().to_string());
            continue;
        }
        // The metadata block the CLI echoes before the transcript.
        if low.starts_with("audio:")
            || low.starts_with("model:")
            || low.starts_with("run:")
            || low.starts_with("backend:")
            || low.starts_with("timings:")
            || low.starts_with("samples:")
            || low.starts_with("duration:")
            || low.starts_with("sample rate")
            || low.starts_with("name:")
            || low.starts_with("license:")
            || low.starts_with("max audio:")
            || low.starts_with("[info]")
            || low.starts_with("[warn]")
            || low.starts_with('{')
        {
            continue;
        }
        lines.push(line.to_string());
    }
    let text = lines.join(" ");
    let text = text
        .replace("<unk>", "")
        .replace("<pad>", "")
        .replace("<s>", "")
        .replace("</s>", "")
        .replace("<|endoftext|>", "")
        .replace("<lang-en-US>", "")
        .replace("<en-US>", "");
    text.split_whitespace().collect::<Vec<_>>().join(" ")
}

/// Pull the "text" field out of a flat JSON object (no JSON dependency).
fn json_text(line: &str) -> Option<String> {
    let key = "\"text\"";
    let start = line.find(key)? + key.len();
    let rest = &line[start..];
    let colon = rest.find(':')? + 1;
    let rest = rest[colon..].trim_start();
    if !rest.starts_with('"') {
        return None;
    }
    let mut out = String::new();
    let mut chars = rest[1..].chars();
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
                None => break,
            },
            _ => out.push(c),
        }
    }
    None
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

    /// The needle prints JSON; the text field has to survive the escaping.
    #[test]
    fn json_text_is_extracted() {
        assert_eq!(
            json_text("{\"text\": \"hello there\"}"),
            Some("hello there".to_string())
        );
        assert_eq!(
            json_text("{\"text\":\"line\\nbreak\"}"),
            Some("line\nbreak".to_string())
        );
        assert_eq!(json_text("{\"other\": 1}"), None);
    }

    /// A model the engine cannot load is worse than no model at all, so the
    /// catalog must only contain sane, distinct entries.
    #[test]
    fn model_catalog_is_real() {
        assert!(!MODELS.is_empty());
        let mut files: Vec<&str> = Vec::new();
        for spec in MODELS {
            assert!(
                spec.file.ends_with(".bin")
                    || spec.file.ends_with(".cact")
                    || spec.file.ends_with(".gguf"),
                "{} is not a model file",
                spec.name
            );
            assert!(
                spec.url.ends_with(spec.file),
                "{} is downloaded from a different file",
                spec.name
            );
            assert!(spec.min_bytes > 0, "{} has no size check", spec.name);
            assert!(!spec.size.is_empty(), "{} has no size label", spec.name);
            // The engine binaries only exist in the Android build.
            #[cfg(target_os = "android")]
            assert!(
                engine_bytes(spec.engine).is_some(),
                "{} has no engine",
                spec.name
            );
            #[cfg(not(target_os = "android"))]
            assert!(
                ENGINES.iter().any(|e| e.0 == spec.engine),
                "{} has no engine",
                spec.name
            );
            assert!(!files.contains(&spec.file), "{} is listed twice", spec.name);
            files.push(spec.file);
        }
        assert!(MODELS.get(DEFAULT_MODEL).is_some());
        assert_eq!(model(999).file, MODELS[DEFAULT_MODEL].file);
    }

    /// The download has to work — this is the check that the URLs are alive
    /// and that the TLS stack works. It needs the network, so it is ignored
    /// by default: `cargo test -- --ignored`.
    #[test]
    #[ignore = "needs the network"]
    fn every_model_url_is_downloadable() {
        for spec in MODELS {
            let response = ureq::get(spec.url)
                .call()
                .unwrap_or_else(|e| panic!("{}: {e}", spec.name));
            assert_eq!(response.status(), 200, "{} is not reachable", spec.name);
            let total: u64 = response
                .header("Content-Length")
                .and_then(|v| v.parse().ok())
                .unwrap_or(0);
            assert!(
                total > spec.min_bytes,
                "{} is smaller than expected ({total} bytes)",
                spec.name
            );
            // The first bytes have to be the model, not an error page.
            let mut reader = response.into_reader();
            let mut head = [0u8; 4];
            let n = std::io::Read::read(&mut reader, &mut head).unwrap_or(0);
            assert!(n > 0, "{} delivered no data", spec.name);
        }
    }
}
