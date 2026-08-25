use crate::models::catalog::{EngineFamily, ModelDescriptor};
use anyhow::{Context, Result};
use once_cell::sync::Lazy;
use serde_json::json;
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};
use std::sync::Mutex;
use std::time::{SystemTime, UNIX_EPOCH};
use tracing::{info, warn};

pub struct SttEngineConfig {
    pub whisper_cli_path: Option<PathBuf>,
    pub sherpa_onnx_path: Option<PathBuf>,
    pub parakeet_engine_path: Option<PathBuf>,
    pub vosk_path: Option<PathBuf>,
    pub nemotron_path: Option<PathBuf>,
}

impl SttEngineConfig {
    pub fn detect() -> Self {
        let mut exe_dir = std::env::current_exe()
            .ok()
            .and_then(|p| p.parent().map(|p| p.to_path_buf()))
            .unwrap_or_default();

        if exe_dir.ends_with("debug") || exe_dir.ends_with("release") {
            if let Some(ws_root) = exe_dir.parent().and_then(|p| p.parent()) {
                let app_dir = ws_root.join("QuickSTT_App");
                if app_dir.exists() {
                    exe_dir = app_dir;
                }
            }
        }

        let models_root = crate::models::catalog::models_root();
        
        let exe_ext = if cfg!(target_os = "windows") { ".exe" } else { "" };
        let whisper_candidates = [
            models_root.join(format!("runtimes/whisper_cpp/cpu/whisper-cli{}", exe_ext)),
            models_root.join(format!("runtimes/whisper_cpp/whisper-cli{}", exe_ext)),
            exe_dir.join(format!("runtimes/whisper_cpp/cpu/whisper-cli{}", exe_ext)),
            PathBuf::from(format!("/usr/lib/quickstt/tools/whisper_cpp/whisper-cli{}", exe_ext)),
            exe_dir.join(format!("whisper-cli{}", exe_ext)),
        ];
        
        let sherpa_candidates = [
            models_root.join(format!("runtimes/sherpa_onnx/cpu/bin/sherpa-onnx-offline{}", exe_ext)),
            models_root.join(format!("runtimes/sherpa_onnx/sherpa-onnx-offline{}", exe_ext)),
            exe_dir.join(format!("runtimes/sherpa_onnx/cpu/bin/sherpa-onnx-offline{}", exe_ext)),
            PathBuf::from(format!("/usr/lib/quickstt/tools/sherpa_onnx/bin/sherpa-onnx-offline{}", exe_ext)),
            exe_dir.join(format!("sherpa-onnx-offline{}", exe_ext)),
        ];
        
        let parakeet_candidates = [
            exe_dir.join(format!("tools/parakeet/parakeet_engine{}", exe_ext)),
            models_root.join(format!("runtimes/parakeet/parakeet_engine{}", exe_ext)),
            PathBuf::from(format!("/usr/lib/quickstt/tools/parakeet/parakeet_engine{}", exe_ext)),
        ];

        // Vosk: libvosk.so is loaded by native stt_service; Rust-side helper binary (optional)
        let vosk_candidates = [
            exe_dir.join(format!("tools/vosk/vosk_transcriber{}", exe_ext)),
            models_root.join(format!("runtimes/vosk/vosk_transcriber{}", exe_ext)),
            models_root.join(format!("vosk/small_en_us_0.15{}", "")),
        ];
        // Nemotron streaming GGUF engine — built from tools/nemotron
        let nemotron_candidates = [
            exe_dir.join(format!("tools/nemotron/nemotron_engine{}", exe_ext)),
            exe_dir.join(format!("tools/nemotron/transcribe{}", exe_ext)),
            models_root.join(format!("runtimes/nemotron/nemotron_engine{}", exe_ext)),
            PathBuf::from(format!("/usr/lib/quickstt/tools/nemotron/nemotron_engine{}", exe_ext)),
            exe_dir.join(format!("nemotron_engine{}", exe_ext)),
        ];

        Self {
            whisper_cli_path: whisper_candidates.iter().find(|p| p.exists()).cloned(),
            sherpa_onnx_path: sherpa_candidates.iter().find(|p| p.exists()).cloned(),
            parakeet_engine_path: parakeet_candidates.iter().find(|p| p.exists()).cloned(),
            vosk_path: vosk_candidates.iter().find(|p| p.exists()).cloned(),
            nemotron_path: nemotron_candidates.iter().find(|p| p.exists()).cloned(),
        }
    }
}

pub fn transcribe(
    config: &SttEngineConfig,
    descriptor: &ModelDescriptor,
    wav_path: &Path,
) -> Result<String> {
    let models_root = crate::models::catalog::models_root();
    let model_dir = models_root.join(&descriptor.model_dir);
    let preprocessed = maybe_preprocess_audio(descriptor, wav_path)?;
    let wav_path = preprocessed.path.as_path();

    match descriptor.engine_family {
        EngineFamily::WhisperCpp => transcribe_whisper_cpp(config, &model_dir, wav_path),
        EngineFamily::ParakeetRust => transcribe_parakeet_rust(config, &model_dir, wav_path),
        EngineFamily::Vosk => transcribe_vosk(config, &model_dir, wav_path),
        EngineFamily::Nemotron => transcribe_nemotron(config, &model_dir, wav_path),
        EngineFamily::SherpaOnnx | EngineFamily::NemoTransducer | EngineFamily::NemoCTC => {
            transcribe_sherpa_onnx(config, &model_dir, wav_path, &descriptor.engine_family)
        }
    }
}

struct PreprocessedAudio {
    path: PathBuf,
    cleanup_dir: Option<PathBuf>,
}

impl Drop for PreprocessedAudio {
    fn drop(&mut self) {
        if let Some(dir) = self.cleanup_dir.take() {
            let _ = std::fs::remove_dir_all(dir);
        }
    }
}

fn maybe_preprocess_audio(
    descriptor: &ModelDescriptor,
    wav_path: &Path,
) -> Result<PreprocessedAudio> {
    if !should_run_deepfilter_for_model(descriptor) {
        return Ok(PreprocessedAudio {
            path: wav_path.to_path_buf(),
            cleanup_dir: None,
        });
    }

    let Some((root, exe, model)) = locate_deepfilter_assets() else {
        return Ok(PreprocessedAudio {
            path: wav_path.to_path_buf(),
            cleanup_dir: None,
        });
    };

    let timestamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis();
    let out_dir = std::env::temp_dir()
        .join("quickstt")
        .join(format!("deepfilter_{timestamp}"));
    std::fs::create_dir_all(&out_dir)?;

    info!(
        "DeepFilter preprocessing start: exe={:?} model={:?} audio={:?}",
        exe, model, wav_path
    );

    let output = Command::new(&exe)
        .current_dir(&root)
        .arg("-m")
        .arg(&model)
        .arg("-D")
        .arg("--pf")
        .arg("-a")
        .arg("18")
        .arg("-o")
        .arg(&out_dir)
        .arg(wav_path)
        .output();

    let output = match output {
        Ok(output) => output,
        Err(err) => {
            let _ = std::fs::remove_dir_all(&out_dir);
            warn!("DeepFilter unavailable; using raw audio: {}", err);
            return Ok(PreprocessedAudio {
                path: wav_path.to_path_buf(),
                cleanup_dir: None,
            });
        }
    };

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        let _ = std::fs::remove_dir_all(&out_dir);
        warn!("DeepFilter failed; using raw audio: {}", stderr);
        return Ok(PreprocessedAudio {
            path: wav_path.to_path_buf(),
            cleanup_dir: None,
        });
    }

    let Some(enhanced) = newest_wav_in_directory(&out_dir) else {
        let _ = std::fs::remove_dir_all(&out_dir);
        return Ok(PreprocessedAudio {
            path: wav_path.to_path_buf(),
            cleanup_dir: None,
        });
    };

    info!("DeepFilter ok enhanced={:?}", enhanced);
    Ok(PreprocessedAudio {
        path: enhanced,
        cleanup_dir: Some(out_dir),
    })
}

fn should_run_deepfilter_for_model(descriptor: &ModelDescriptor) -> bool {
    let override_value = std::env::var("QUICKSTT_DEEPFILTER_FRONTEND")
        .unwrap_or_default()
        .trim()
        .to_lowercase();
    if matches!(override_value.as_str(), "1" | "true" | "on" | "yes") {
        return true;
    }
    if matches!(override_value.as_str(), "0" | "false" | "off" | "no") {
        return false;
    }

    !matches!(
        descriptor.engine_family,
        EngineFamily::NemoTransducer | EngineFamily::NemoCTC | EngineFamily::Nemotron | EngineFamily::Vosk
    )
}

fn locate_deepfilter_assets() -> Option<(PathBuf, PathBuf, PathBuf)> {
    let exe_dir = std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|p| p.to_path_buf()))?;

    let workspace_root = exe_dir
        .parent()
        .and_then(|p| p.parent())
        .and_then(|p| p.parent())
        .map(|p| p.to_path_buf());

    let mut roots = vec![
        exe_dir.join("audio_preprocess"),
        exe_dir.join("data").join("audio_preprocess"),
        exe_dir
            .join("..")
            .join("third_party")
            .join("audio_preprocess"),
    ];
    if let Some(workspace_root) = workspace_root {
        roots.push(workspace_root.join("third_party").join("audio_preprocess"));
        roots.push(workspace_root.join("QuickSTT_App").join("audio_preprocess"));
    }

    let deepfilter_exe = if cfg!(target_os = "windows") { "deep-filter.exe" } else { "deep-filter" };
    for root in roots {
        let exe = root.join("deepfilter").join(deepfilter_exe);
        let model = root.join("deepfilter").join("DeepFilterNet3.tar.gz");
        if exe.exists() && model.exists() {
            return Some((root, exe, model));
        }
    }
    None
}

fn newest_wav_in_directory(dir: &Path) -> Option<PathBuf> {
    let mut newest: Option<(std::time::SystemTime, PathBuf)> = None;
    let entries = std::fs::read_dir(dir).ok()?;
    for entry in entries.flatten() {
        let path = entry.path();
        if path.extension().and_then(|s| s.to_str()) != Some("wav") {
            continue;
        }
        let modified = entry.metadata().ok()?.modified().ok().unwrap_or(UNIX_EPOCH);
        match &newest {
            Some((best_time, _)) if modified <= *best_time => {}
            _ => newest = Some((modified, path)),
        }
    }
    newest.map(|(_, path)| path)
}

fn parakeet_engine_candidates(exe_dir: &Path) -> Vec<PathBuf> {
    let ext = if cfg!(target_os = "windows") { ".exe" } else { "" };
    let mut out = vec![
        exe_dir.join(format!("tools/parakeet/parakeet_engine{}", ext)),
        exe_dir.join(format!("parakeet_engine{}", ext)),
        PathBuf::from(format!("/usr/lib/quickstt/tools/parakeet/parakeet_engine{}", ext)),
    ];

    if let Some(workspace) = exe_dir
        .parent()
        .and_then(|p| p.parent())
        .and_then(|p| p.parent())
    {
        out.push(
            workspace
                .join("QuickSTT_App")
                .join("tools")
                .join("parakeet")
                .join(format!("parakeet_engine{}", ext)),
        );
        out.push(
            workspace
                .join("parakeet_engine")
                .join("target")
                .join("release")
                .join(format!("parakeet_engine{}", ext)),
        );
    }

    out
}

struct ParakeetSession {
    child: Child,
    stdin: ChildStdin,
    stdout: BufReader<ChildStdout>,
    loaded_model_dir: Option<PathBuf>,
    exe_path: PathBuf,
}

static PARAKEET_SESSION: Lazy<Mutex<Option<ParakeetSession>>> = Lazy::new(|| Mutex::new(None));

fn transcribe_parakeet_rust(
    config: &SttEngineConfig,
    model_dir: &Path,
    wav_path: &Path,
) -> Result<String> {
    let exe = config
        .parakeet_engine_path
        .as_ref()
        .context("parakeet_engine not found")?;

    if !model_dir.exists() {
        anyhow::bail!("Parakeet model directory not found: {:?}", model_dir);
    }

    let mut guard = PARAKEET_SESSION
        .lock()
        .map_err(|_| anyhow::anyhow!("Parakeet session lock poisoned"))?;

    let needs_spawn = guard.as_ref().map(|s| s.exe_path != *exe).unwrap_or(true);
    if needs_spawn {
        *guard = Some(spawn_parakeet_session(exe)?);
    }

    let session = guard
        .as_mut()
        .context("Parakeet session was not initialised")?;

    if session.loaded_model_dir.as_deref() != Some(model_dir) {
        let response = parakeet_request(
            session,
            json!({
                "action": "load",
                "model_path": model_dir.to_string_lossy(),
            }),
        )?;
        ensure_parakeet_ok(response, "load")?;
        session.loaded_model_dir = Some(model_dir.to_path_buf());
    }

    let response = parakeet_request(
        session,
        json!({
            "action": "transcribe",
            "audio_path": wav_path.to_string_lossy(),
        }),
    )?;
    let text = ensure_parakeet_ok(response, "transcribe")?;
    Ok(text.unwrap_or_default())
}

fn spawn_parakeet_session(exe: &Path) -> Result<ParakeetSession> {
    let workdir = exe.parent().unwrap_or(Path::new("."));
    info!("Starting Parakeet Rust engine: {:?}", exe);
    let mut child = Command::new(exe)
        .current_dir(workdir)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .context("Failed to start parakeet_engine")?;

    let stdin = child.stdin.take().context("Parakeet stdin unavailable")?;
    let stdout = child.stdout.take().context("Parakeet stdout unavailable")?;

    Ok(ParakeetSession {
        child,
        stdin,
        stdout: BufReader::new(stdout),
        loaded_model_dir: None,
        exe_path: exe.to_path_buf(),
    })
}

fn parakeet_request(
    session: &mut ParakeetSession,
    request: serde_json::Value,
) -> Result<serde_json::Value> {
    if let Ok(Some(status)) = session.child.try_wait() {
        anyhow::bail!("Parakeet engine exited early: {}", status);
    }

    let line = serde_json::to_string(&request)?;
    session.stdin.write_all(line.as_bytes())?;
    session.stdin.write_all(b"\n")?;
    session.stdin.flush()?;

    let mut response = String::new();
    let read = session.stdout.read_line(&mut response)?;
    if read == 0 {
        anyhow::bail!("Parakeet engine closed stdout");
    }
    serde_json::from_str(response.trim()).context("Invalid Parakeet JSON response")
}

fn ensure_parakeet_ok(response: serde_json::Value, action: &str) -> Result<Option<String>> {
    let status = response
        .get("status")
        .and_then(|v| v.as_str())
        .unwrap_or("error");
    if status == "ok" {
        return Ok(response
            .get("text")
            .and_then(|v| v.as_str())
            .map(|s| s.trim().to_string()));
    }

    let error = response
        .get("error")
        .and_then(|v| v.as_str())
        .unwrap_or("unknown Parakeet error");
    anyhow::bail!("Parakeet {} failed: {}", action, error)
}

fn transcribe_whisper_cpp(
    config: &SttEngineConfig,
    model_dir: &Path,
    wav_path: &Path,
) -> Result<String> {
    let cli = config
        .whisper_cli_path
        .as_ref()
        .context("whisper-cli not found")?;

    let model_file = find_ggml_file(model_dir)?;

    info!(
        "Whisper CLI: {:?} model: {:?} wav: {:?}",
        cli, model_file, wav_path
    );

    let output = Command::new(cli)
        .arg("-m")
        .arg(&model_file)
        .arg("-f")
        .arg(wav_path)
        .arg("-l")
        .arg("en")
        .arg("--no-timestamps")
        .arg("-t")
        .arg("4")
        .output()
        .context("Failed to run whisper-cli")?;

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        warn!("whisper-cli error: {}", stderr);
        anyhow::bail!("whisper-cli failed: {}", stderr);
    }

    let text = String::from_utf8_lossy(&output.stdout)
        .lines()
        .filter(|l| !l.trim().is_empty())
        .map(|l| l.trim())
        .collect::<Vec<_>>()
        .join(" ");

    Ok(text)
}

fn transcribe_sherpa_onnx(
    config: &SttEngineConfig,
    model_dir: &Path,
    wav_path: &Path,
    family: &EngineFamily,
) -> Result<String> {
    let cli = config
        .sherpa_onnx_path
        .as_ref()
        .context("sherpa-onnx-offline not found")?;

    let mut cmd = Command::new(cli);

    match family {
        EngineFamily::NemoTransducer => {
            let encoder = model_dir.join("encoder.int8.onnx");
            let decoder = model_dir.join("decoder.int8.onnx");
            let joiner = model_dir.join("joiner.int8.onnx");
            let tokens = model_dir.join("tokens.txt");
            cmd.arg("--transducer-encoder")
                .arg(&encoder)
                .arg("--transducer-decoder")
                .arg(&decoder)
                .arg("--transducer-joiner")
                .arg(&joiner)
                .arg("--tokens")
                .arg(&tokens);
        }
        EngineFamily::NemoCTC => {
            let model = model_dir.join("model.int8.onnx");
            let tokens = model_dir.join("tokens.txt");
            cmd.arg("--nemo-ctc-model")
                .arg(&model)
                .arg("--tokens")
                .arg(&tokens);
        }
        EngineFamily::SherpaOnnx => {
            let encoder = model_dir.join("encoder_model.ort");
            let decoder = model_dir.join("decoder_model_merged.ort");
            let tokens = model_dir.join("tokens.txt");
            cmd.arg("--moonshine-preprocessor")
                .arg(&encoder)
                .arg("--moonshine-encoder")
                .arg(&encoder)
                .arg("--moonshine-uncached-decoder")
                .arg(&decoder)
                .arg("--moonshine-cached-decoder")
                .arg(&decoder)
                .arg("--tokens")
                .arg(&tokens);
        }
        _ => {}
    }

    cmd.arg(wav_path);

    info!("Sherpa ONNX: {:?}", cmd);

    let output = cmd
        .output()
        .context("Failed to run sherpa-onnx-offline")?;

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        warn!("sherpa-onnx error: {}", stderr);
        anyhow::bail!("sherpa-onnx failed: {}", stderr);
    }

    let text = String::from_utf8_lossy(&output.stdout)
        .lines()
        .filter(|l| !l.trim().is_empty() && !l.starts_with("Duration"))
        .map(|l| l.trim())
        .collect::<Vec<_>>()
        .join(" ");

    Ok(text)
}

fn transcribe_vosk(
    config: &SttEngineConfig,
    model_dir: &Path,
    wav_path: &Path,
) -> Result<String> {
    // 1. Optional helper binary (Windows layout)
    if let Some(cli) = &config.vosk_path {
        if cli.is_file() {
            info!("Vosk CLI: {:?} model: {:?} wav: {:?}", cli, model_dir, wav_path);
            let output = Command::new(cli)
                .arg("-m").arg(model_dir)
                .arg("-i").arg(wav_path)
                .output()
                .context("Failed to run vosk_transcriber")?;
            if output.status.success() {
                let text = String::from_utf8_lossy(&output.stdout).trim().to_string();
                if !text.is_empty() { return Ok(text); }
            } else {
                warn!("vosk_transcriber failed: {}", String::from_utf8_lossy(&output.stderr));
            }
        }
    }
    // 2. Native libvosk FFI (Linux primary path; libvosk.so downloaded by
    //    Settings → Models into runtimes/vosk/ or shipped in tools/vosk/).
    transcribe_vosk_native(model_dir, wav_path)
}

#[cfg(target_os = "linux")]
fn vosk_library_candidates() -> Vec<PathBuf> {
    let models_root = crate::models::catalog::models_root();
    let exe_dir = std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|p| p.to_path_buf()))
        .unwrap_or_default();
    vec![
        models_root.join("runtimes/vosk/libvosk.so"),
        exe_dir.join("tools/vosk/libvosk.so"),
        PathBuf::from("/usr/lib/quickstt/tools/vosk/libvosk.so"),
        exe_dir.join("libvosk.so"),
        PathBuf::from("libvosk.so"),
    ]
}

#[cfg(target_os = "windows")]
fn vosk_library_candidates() -> Vec<PathBuf> {
    let models_root = crate::models::catalog::models_root();
    let exe_dir = std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|p| p.to_path_buf()))
        .unwrap_or_default();
    vec![
        models_root.join("runtimes/vosk/libvosk.dll"),
        exe_dir.join("tools/vosk/vosk.dll"),
        exe_dir.join("vosk.dll"),
    ]
}

/// Minimal Vosk C-API binding driven through `libloading` so no build-time
/// link dependency exists. Reads a 16 kHz mono WAV and returns the final
/// recognition JSON's "text" field.
fn transcribe_vosk_native(model_dir: &Path, wav_path: &Path) -> Result<String> {
    use libloading::{Library, Symbol};

    let lib_path = vosk_library_candidates()
        .into_iter()
        .find(|p| p.exists())
        .context(
            "libvosk not found — install the Vosk Small EN (50M) model from Settings → Models \
             (the runtime library is downloaded alongside it)",
        )?;
    unsafe {
        let lib = Library::new(&lib_path)
            .with_context(|| format!("Failed to load {:?}", lib_path))?;

        type ModelNew = unsafe extern "C" fn(*const std::ffi::c_char) -> *mut core::ffi::c_void;
        type RecNew = unsafe extern "C" fn(
            *mut core::ffi::c_void,
            f32,
        ) -> *mut core::ffi::c_void;
        type AcceptWaveform = unsafe extern "C" fn(
            *mut core::ffi::c_void,
            *const i16,
            i32,
        ) -> i32;
        type FinalResult =
            unsafe extern "C" fn(*mut core::ffi::c_void) -> *const std::ffi::c_char;
        type FreeRecognizer = unsafe extern "C" fn(*mut core::ffi::c_void);
        type FreeModel = unsafe extern "C" fn(*mut core::ffi::c_void);

        let model_new: Symbol<ModelNew> = lib.get(b"VoskModelNew")?;
        let rec_new: Symbol<RecNew> = lib.get(b"VoskRecognizerNew")?;
        let accept: Symbol<AcceptWaveform> = lib.get(b"VoskRecognizerAcceptWaveform")?;
        let final_result: Symbol<FinalResult> = lib.get(b"VoskRecognizerFinalResult")?;
        let rec_free: Symbol<FreeRecognizer> = lib.get(b"VoskRecognizerFree")?;
        let model_free: Symbol<FreeModel> = lib.get(b"VoskModelFree")?;

        let c_model = std::ffi::CString::new(model_dir.to_string_lossy().as_bytes())?;
        let model_ptr = model_new(c_model.as_ptr());
        if model_ptr.is_null() {
            anyhow::bail!("VoskModelNew failed for {:?}", model_dir);
        }
        let rec_ptr = rec_new(model_ptr, 16000.0);
        if rec_ptr.is_null() {
            model_free(model_ptr);
            anyhow::bail!("VoskRecognizerNew failed");
        }

        let mut reader = hound::WavReader::open(wav_path)
            .with_context(|| format!("Failed to open wav {:?}", wav_path))?;
        let mut chunk = Vec::with_capacity(3200);
        let result_json: String = {
            for sample in reader.samples::<i16>() {
                let s = sample.unwrap_or(0);
                chunk.push(s);
                if chunk.len() >= 3200 {
                    accept(rec_ptr, chunk.as_ptr(), chunk.len() as i32);
                    chunk.clear();
                }
            }
            if !chunk.is_empty() {
                accept(rec_ptr, chunk.as_ptr(), chunk.len() as i32);
            }
            let res = final_result(rec_ptr);
            if res.is_null() {
                String::new()
            } else {
                std::ffi::CStr::from_ptr(res).to_string_lossy().into_owned()
            }
        };

        rec_free(rec_ptr);
        model_free(model_ptr);

        let parsed: serde_json::Value = serde_json::from_str(result_json.trim())
            .unwrap_or(serde_json::json!({}));
        Ok(parsed
            .get("text")
            .and_then(|v| v.as_str())
            .unwrap_or("")
            .trim()
            .to_string())
    }
}

#[cfg(not(any(target_os = "linux", target_os = "windows")))]
fn transcribe_vosk_native(_model_dir: &Path, _wav_path: &Path) -> Result<String> {
    anyhow::bail!("Native Vosk transcription unsupported on this platform")
}

fn transcribe_nemotron(
    config: &SttEngineConfig,
    model_dir: &Path,
    wav_path: &Path,
) -> Result<String> {
    // Nemotron 3.5 ASR Streaming 0.6B GGUF — uses Handy transcribe library (GGML) + VAD.
    // On Linux the engine is tools/nemotron/nemotron_engine or transcribe binary.
    let cli = config.nemotron_path.as_ref().context("nemotron_engine not found. Build tools/nemotron on Linux (cmake -DGGML_BACKEND=ON) to produce nemotron_engine")?;
    // Nemotron expects: nemotron_engine --model <gguf> --audio <wav> --vad
    // Find GGUF file in model_dir
    let gguf = find_gguf_file(model_dir)?;
    info!("Nemotron: {:?} model: {:?} wav: {:?}", cli, gguf, wav_path);
    let output = Command::new(cli)
        .arg("--model").arg(&gguf)
        .arg("--audio").arg(wav_path)
        .arg("--vad")
        .output()
        .context("Failed to run nemotron_engine")?;
    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        warn!("nemotron_engine error: {}", stderr);
        anyhow::bail!("nemotron_engine failed: {}", stderr);
    }
    let text = String::from_utf8_lossy(&output.stdout).trim().to_string();
    Ok(text)
}

fn find_gguf_file(model_dir: &Path) -> Result<PathBuf> {
    if let Ok(entries) = std::fs::read_dir(model_dir) {
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().to_string();
            if name.ends_with(".gguf") {
                return Ok(entry.path());
            }
        }
    }
    anyhow::bail!("No *.gguf model file found in {:?}", model_dir)
}

fn find_ggml_file(model_dir: &Path) -> Result<PathBuf> {
    if let Ok(entries) = std::fs::read_dir(model_dir) {
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().to_string();
            if name.starts_with("ggml-") && name.ends_with(".bin") {
                return Ok(entry.path());
            }
        }
    }
    anyhow::bail!("No ggml-*.bin model file found in {:?}", model_dir)
}

pub fn compact_working_set() {
    #[cfg(target_os = "windows")]
    {
        use windows::Win32::System::Threading::{GetCurrentProcess, SetProcessWorkingSetSize};
        unsafe {
            let _ = SetProcessWorkingSetSize(GetCurrentProcess(), usize::MAX, usize::MAX);
        }
    }
    #[cfg(target_os = "linux")]
    {
        // Hint kernel to reclaim — malloc_trim + madvise via libc
        unsafe { libc_madvise_hint(); }
    }
}

#[cfg(target_os = "linux")]
unsafe fn libc_madvise_hint() {
    // Best-effort: call malloc_trim(0) via libc if available
    extern "C" { fn malloc_trim(pad: usize) -> i32; }
    let _ = malloc_trim(0);
}
