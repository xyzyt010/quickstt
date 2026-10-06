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

/// Never flash a terminal window when spawning transcription helpers.
#[cfg(target_os = "windows")]
fn no_window(cmd: &mut Command) {
    use std::os::windows::process::CommandExt;
    cmd.creation_flags(0x08000000); // CREATE_NO_WINDOW
}
#[cfg(not(target_os = "windows"))]
fn no_window(_cmd: &mut Command) {}

#[derive(Clone)]
pub struct SttEngineConfig {
    pub whisper_cli_path: Option<PathBuf>,
    pub sherpa_onnx_path: Option<PathBuf>,
    pub parakeet_engine_path: Option<PathBuf>,
    pub vosk_path: Option<PathBuf>,
    pub nemotron_path: Option<PathBuf>,
    /// Persistent Photon worker script (tools/photon/photon_worker.py).
    pub photon_worker_path: Option<PathBuf>,
    /// Python interpreter that has `moondream` installed (probed once).
    pub photon_python: Option<PathBuf>,
}

/// Probe well-known Python launchers; returns the first that runs.
/// (Windows: `python` from the Store/python.org install, `py` launcher.)
#[cfg(target_os = "windows")]
fn probe_python() -> Option<PathBuf> {
    for name in ["python", "python3", "py"] {
        let mut cmd = Command::new(name);
        no_window(&mut cmd);
        if cmd
            .arg("--version")
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
            .map(|s| s.success())
            .unwrap_or(false)
        {
            return Some(PathBuf::from(name));
        }
    }
    None
}

#[cfg(not(target_os = "windows"))]
fn probe_python() -> Option<PathBuf> {
    for name in ["python3", "python"] {
        let mut cmd = Command::new(name);
        if cmd
            .arg("--version")
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
            .map(|s| s.success())
            .unwrap_or(false)
        {
            return Some(PathBuf::from(name));
        }
    }
    None
}

impl SttEngineConfig {
    pub fn detect() -> Self {        let mut exe_dir = std::env::current_exe()
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
        // Nemotron 3.5 streaming: prefer the persistent Handy worker
        // (tools/nemotron/nemotron_engine + transcribe.dll + ggml) — it is
        // the exact Handy stack, stable on CPU/iGPU, and speaks the same
        // JSON load/transcribe protocol as parakeet_engine. The standalone
        // transcribe-cli static build crashes on this CPU family for the
        // Nemotron RNNT decoder (access violation after weight promotion),
        // so it is only a fallback when no worker is present.
        // Q4_K_M is the working CPU quant (Q8_0 decodes to all-<unk> in the
        // shipped GGUF); the worker resolves the file inside model_dir.
        let nemotron_candidates = [
            exe_dir.join(format!("tools/nemotron/nemotron_engine{}", exe_ext)),
            PathBuf::from(format!("/usr/lib/quickstt/tools/nemotron/nemotron_engine{}", exe_ext)),
            exe_dir.join(format!("nemotron_engine{}", exe_ext)),
            exe_dir.join(format!("tools/nemotron/transcribe{}", exe_ext)),
            models_root.join(format!("runtimes/nemotron/transcribe-cli{}", exe_ext)),
            exe_dir.join(format!("tools/nemotron/transcribe-cli{}", exe_ext)),
            exe_dir.join(format!("transcribe-cli{}", exe_ext)),
            exe_dir.join(format!("../third_party/transcribe.cpp/build/bin/Release/transcribe-cli{}", exe_ext)),
            exe_dir.join(format!("../../third_party/transcribe.cpp/build/bin/Release/transcribe-cli{}", exe_ext)),
            PathBuf::from("/usr/lib/quickstt/tools/nemotron/transcribe-cli"),
        ];

        // Photon worker script (Moondream Parakeet). Deployed layout keeps it
        // next to the exe (tools/photon/); dev runs fall back to the
        // workspace tree (target/{debug,release} -> quickstt-rust).
        let mut photon_worker_candidates = vec![
            exe_dir.join("tools/photon/photon_worker.py"),
            PathBuf::from("/usr/lib/quickstt/tools/photon/photon_worker.py"),
        ];
        if exe_dir.ends_with("debug") || exe_dir.ends_with("release") {
            if let Some(quickstt_rust) = exe_dir.parent().and_then(|p| p.parent()) {
                photon_worker_candidates.push(
                    quickstt_rust.join("tools/photon/photon_worker.py"),
                );
            }
        }
        let photon_worker_candidates = photon_worker_candidates;

        Self {
            whisper_cli_path: whisper_candidates.iter().find(|p| p.exists()).cloned(),
            sherpa_onnx_path: sherpa_candidates.iter().find(|p| p.exists()).cloned(),
            parakeet_engine_path: parakeet_candidates.iter().find(|p| p.exists()).cloned(),
            vosk_path: vosk_candidates.iter().find(|p| p.exists()).cloned(),
            nemotron_path: nemotron_candidates.iter().find(|p| p.exists()).cloned(),
            photon_worker_path: photon_worker_candidates
                .iter()
                .find(|p| p.exists())
                .cloned(),
            photon_python: probe_python(),
        }
    }
}

/// Process-wide cached engine paths. Filesystem probing runs once instead
/// of on every utterance — saves latency on the PTT release flush path.
pub fn cached_config() -> SttEngineConfig {
    static CACHE: Lazy<SttEngineConfig> = Lazy::new(SttEngineConfig::detect);
    CACHE.clone()
}

pub fn transcribe(
    config: &SttEngineConfig,
    descriptor: &ModelDescriptor,
    wav_path: &Path,
) -> Result<String> {
    transcribe_with_language(config, descriptor, wav_path, "Auto")
}

/// Transcribe with a UI language name ("Auto", "English", ...). Only
/// multilingual whisper.cpp checkpoints act on it; English-only engines
/// (Parakeet, Vosk, Nemotron, Moonshine) transcribe English regardless.
pub fn transcribe_with_language(
    config: &SttEngineConfig,
    descriptor: &ModelDescriptor,
    wav_path: &Path,
    language: &str,
) -> Result<String> {
    let models_root = crate::models::catalog::models_root();
    let mut model_dir = models_root.join(&descriptor.model_dir);
    if !model_dir.exists() {
        if descriptor.engine_family == EngineFamily::Nemotron {
            let alt = models_root.join("nemotron/nemotron-3.5-asr-streaming-0.6b");
            if alt.exists() {
                model_dir = alt;
            }
        } else if descriptor.engine_family == EngineFamily::Vosk {
            let alt = models_root.join("vosk-model-small-en-us-0.15");
            if alt.exists() {
                model_dir = alt;
            }
        } else if descriptor.engine_family == EngineFamily::SherpaOnnx {
            for cand in [
                "moonshine_v2/base_en",
                "moonshine_v2/tiny_en",
                "moonshine/streaming_medium_245m",
                "sherpa_onnx/moonshine",
            ] {
                let alt = models_root.join(cand);
                if alt.exists() {
                    model_dir = alt;
                    break;
                }
            }
        }
    }
    let preprocessed = maybe_preprocess_audio(descriptor, wav_path)?;
    let wav_path = preprocessed.path.as_path();

    match descriptor.engine_family {
        EngineFamily::WhisperCpp => {
            transcribe_whisper_cpp(config, &model_dir, wav_path, descriptor, language)
        }
        EngineFamily::ParakeetRust => transcribe_parakeet_rust(config, &model_dir, wav_path),
        EngineFamily::Vosk => transcribe_vosk(config, &model_dir, wav_path),
        EngineFamily::Nemotron => transcribe_nemotron(config, &model_dir, wav_path, language),
        EngineFamily::Canary => transcribe_canary(config, &model_dir, wav_path, language),
        EngineFamily::Photon => transcribe_photon(config, descriptor, wav_path),
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

    let output = {
        let mut c = Command::new(&exe);
        no_window(&mut c);
        c.current_dir(&root)
            .arg("-m")
            .arg(&model)
            .arg("-D")
            .arg("--pf")
            .arg("-a")
            .arg("18")
            .arg("-o")
            .arg(&out_dir)
            .arg(wav_path)
            .output()
    };

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
        EngineFamily::NemoTransducer
            | EngineFamily::NemoCTC
            | EngineFamily::Nemotron
            | EngineFamily::Canary
            | EngineFamily::Vosk
            | EngineFamily::Photon
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

#[allow(dead_code)]
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

/// PID of the live Parakeet helper (if any), for the UI watchdog: a hung
/// engine (infinite "Transcribing…") is killed by PID so the turn fails
/// gracefully instead of wedging the widget forever.
pub fn parakeet_child_pid() -> Option<u32> {
    PARAKEET_SESSION
        .lock()
        .ok()
        .and_then(|g| g.as_ref().map(|s| s.child.id()))
}

/// Idle offload: release the helper's model from RAM (the process stays
/// alive for fast reload). Safe to call when idle — never yanks mid-turn
/// (callers only invoke it settled). Next transcribe reloads on demand via
/// the loaded_model_dir check in transcribe_parakeet_rust.
pub fn parakeet_unload() {
    let mut guard = match PARAKEET_SESSION.lock() {
        Ok(g) => g,
        Err(_) => return,
    };
    let session = match guard.as_mut() {
        Some(s) => s,
        None => return,
    };
    if session.loaded_model_dir.is_none() {
        return;
    }
    // Dead child: drop the session; a later transcribe respawns cleanly.
    if matches!(session.child.try_wait(), Ok(Some(_))) {
        *guard = None;
        return;
    }
    match parakeet_request(session, json!({ "action": "unload" })) {
        Ok(resp) => match ensure_parakeet_ok(resp, "unload") {
            Ok(_) => {
                session.loaded_model_dir = None;
                info!("Parakeet model unloaded (idle offload)");
            }
            Err(e) => warn!("Parakeet unload rejected: {e:#}"),
        },
        Err(e) => warn!("Parakeet unload failed: {e:#}"),
    }
}

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

    // (Re)spawn when the binary changed OR the previous helper died (a
    // watchdog kill after a hang, a crash, ...). Without the dead check a
    // killed session would fail every future turn with "exited early".
    let dead = guard
        .as_mut()
        .map(|s| matches!(s.child.try_wait(), Ok(Some(_))))
        .unwrap_or(true);
    let needs_spawn =
        dead || guard.as_ref().map(|s| s.exe_path != *exe).unwrap_or(true);
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

/// Locate a bundled onnxruntime shared library for ort/load-dynamic.
fn locate_onnxruntime_dylib(engine_dir: &Path) -> Option<PathBuf> {
    let models_root = crate::models::catalog::models_root();
    let names: &[&str] = if cfg!(target_os = "windows") {
        &["onnxruntime.dll"]
    } else {
        &["libonnxruntime.so", "libonnxruntime.so.1.19.2"]
    };
    let mut dirs = vec![
        engine_dir.to_path_buf(),
        engine_dir.join("../lib"),
        PathBuf::from("/usr/lib/quickstt/tools/parakeet"),
        models_root.join("runtimes/parakeet"),
    ];
    if let Some(exe_dir) = std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|p| p.to_path_buf()))
    {
        dirs.push(exe_dir.join("tools/parakeet"));
        dirs.push(exe_dir);
    }
    for dir in dirs {
        for name in names {
            let p = dir.join(name);
            if p.exists() {
                return Some(p);
            }
        }
    }
    None
}

fn spawn_parakeet_session(exe: &Path) -> Result<ParakeetSession> {
    let workdir = exe.parent().unwrap_or(Path::new("."));
    info!("Starting Parakeet Rust engine: {:?}", exe);
    let mut cmd = Command::new(exe);
    // Never flash a terminal when starting transcription (Windows console).
    #[cfg(target_os = "windows")]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x08000000); // CREATE_NO_WINDOW
    }
    // ort/load-dynamic: point the engine at our bundled onnxruntime when present.
    if let Some(dylib) = locate_onnxruntime_dylib(workdir) {
        info!("ORT_DYLIB_PATH={:?}", dylib);
        cmd.env("ORT_DYLIB_PATH", &dylib);
    }
    // Engine stderr goes to a log file (never null): a hung/failed helper
    // used to be completely silent, which wedged turns with no diagnosis.
    let err_log = std::env::temp_dir()
        .join("quickstt-parakeet-stderr.log");
    let err_log = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&err_log)
        .map(Stdio::from)
        .unwrap_or_else(|_| Stdio::null());
    let mut child = cmd
        .current_dir(workdir)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(err_log)
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

// ── Moondream Photon (parakeet-redux / parakeet-ultra) ──
// Persistent `photon_worker.py` helper: same JSON load/transcribe protocol
// as parakeet_engine (load carries the HF repo id + optional ternary ISA;
// Photon only accepts registered ids, never local paths). The worker keeps
// one Photon context open, so Ultra's minutes-long first load is paid once
// per model, not per utterance. Needs Python 3.10+ with moondream>=2.4.

struct PhotonSession {
    child: Child,
    stdin: ChildStdin,
    stdout: BufReader<ChildStdout>,
    loaded_repo: Option<String>,
    worker_path: PathBuf,
    python: PathBuf,
}

static PHOTON_SESSION: Lazy<Mutex<Option<PhotonSession>>> = Lazy::new(|| Mutex::new(None));

fn transcribe_photon(
    config: &SttEngineConfig,
    descriptor: &ModelDescriptor,
    wav_path: &Path,
) -> Result<String> {
    let worker = config
        .photon_worker_path
        .as_ref()
        .context("photon_worker.py not found (expected next to the exe under tools/photon)")?;
    let python = config.photon_python.as_ref().context(
        "no Python with `moondream` found (install Python 3.10+ and run: pip install \"moondream>=2.4\")",
    )?;
    let repo_id = crate::models::catalog::photon_repo_id(&descriptor.model_dir)
        .context(format!("No Photon repo registered for {}", descriptor.name))?;

    let mut guard = PHOTON_SESSION
        .lock()
        .map_err(|_| anyhow::anyhow!("Photon session lock poisoned"))?;

    // (Re)spawn when the helper changed OR the previous worker died. Without
    // the dead check a killed session would fail every future turn.
    let dead = guard
        .as_mut()
        .map(|s| matches!(s.child.try_wait(), Ok(Some(_))))
        .unwrap_or(true);
    let changed = guard
        .as_ref()
        .map(|s| s.worker_path != *worker || s.python != *python)
        .unwrap_or(true);
    if dead || changed {
        *guard = Some(spawn_photon_session(python, worker)?);
    }

    let session = guard
        .as_mut()
        .context("Photon session was not initialised")?;

    if session.loaded_repo.as_deref() != Some(repo_id) {
        // Ternary checkpoints (Redux) need the ISA override on Windows, where
        // kestrel's CPU auto-detection reports scalar. Ultra is full precision
        // and ignores it.
        let isa = if repo_id.contains("redux") {
            Some("avxvnni")
        } else {
            None
        };
        let mut load = serde_json::Map::new();
        load.insert("action".into(), json!("load"));
        load.insert("repo_id".into(), json!(repo_id));
        if let Some(isa) = isa {
            load.insert("isa".into(), json!(isa));
        }
        let response = photon_request(session, serde_json::Value::Object(load))?;
        ensure_photon_ok(response, "load")?;
        session.loaded_repo = Some(repo_id.to_string());
    }

    let response = photon_request(
        session,
        json!({
            "action": "transcribe",
            "audio_path": wav_path.to_string_lossy(),
        }),
    )?;
    let text = ensure_photon_ok(response, "transcribe")?;
    Ok(text.unwrap_or_default())
}

fn spawn_photon_session(python: &Path, worker: &Path) -> Result<PhotonSession> {
    let workdir = worker.parent().unwrap_or(Path::new("."));
    info!("Starting Photon worker: {:?} {:?}", python, worker);
    let mut cmd = Command::new(python);
    no_window(&mut cmd);
    // Worker stderr goes to a log file (never null): a hung/failed helper
    // used to be completely silent, which wedged turns with no diagnosis.
    let err_log = std::env::temp_dir().join("quickstt-photon-stderr.log");
    let err_log = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&err_log)
        .map(Stdio::from)
        .unwrap_or_else(|_| Stdio::null());
    let mut child = cmd
        .arg(worker)
        .current_dir(workdir)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(err_log)
        .spawn()
        .context("Failed to start photon_worker.py")?;

    let stdin = child.stdin.take().context("Photon stdin unavailable")?;
    let stdout = child.stdout.take().context("Photon stdout unavailable")?;

    Ok(PhotonSession {
        child,
        stdin,
        stdout: BufReader::new(stdout),
        loaded_repo: None,
        worker_path: worker.to_path_buf(),
        python: python.to_path_buf(),
    })
}

fn photon_request(
    session: &mut PhotonSession,
    request: serde_json::Value,
) -> Result<serde_json::Value> {
    if let Ok(Some(status)) = session.child.try_wait() {
        anyhow::bail!("Photon worker exited early: {}", status);
    }

    let line = serde_json::to_string(&request)?;
    session.stdin.write_all(line.as_bytes())?;
    session.stdin.write_all(b"\n")?;
    session.stdin.flush()?;

    let mut response = String::new();
    let read = session.stdout.read_line(&mut response)?;
    if read == 0 {
        anyhow::bail!("Photon worker closed stdout");
    }
    serde_json::from_str(response.trim()).context("Invalid Photon JSON response")
}

fn ensure_photon_ok(response: serde_json::Value, action: &str) -> Result<Option<String>> {
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
        .unwrap_or("unknown Photon error");
    anyhow::bail!("Photon {} failed: {}", action, error)
}

/// PID of the live Photon helper (if any), for the UI watchdog: a hung
/// engine (infinite "Transcribing…") is killed by PID so the turn fails
/// gracefully instead of wedging the widget forever.
pub fn photon_child_pid() -> Option<u32> {
    PHOTON_SESSION
        .lock()
        .ok()
        .and_then(|g| g.as_ref().map(|s| s.child.id()))
}

/// Idle offload: release the helper's model from RAM (the process stays
/// alive for fast reload). Safe to call when idle — never yanks mid-turn
/// (callers only invoke it settled). Next transcribe reloads on demand via
/// the loaded_repo check in transcribe_photon.
pub fn photon_unload() {
    let mut guard = match PHOTON_SESSION.lock() {
        Ok(g) => g,
        Err(_) => return,
    };
    let session = match guard.as_mut() {
        Some(s) => s,
        None => return,
    };
    if session.loaded_repo.is_none() {
        return;
    }
    // Dead child: drop the session; a later transcribe respawns cleanly.
    if matches!(session.child.try_wait(), Ok(Some(_))) {
        *guard = None;
        return;
    }
    match photon_request(session, json!({ "action": "unload" })) {
        Ok(resp) => match ensure_photon_ok(resp, "unload") {
            Ok(_) => {
                session.loaded_repo = None;
                info!("Photon model unloaded (idle offload)");
            }
            Err(e) => warn!("Photon unload rejected: {e:#}"),
        },
        Err(e) => warn!("Photon unload failed: {e:#}"),
    }
}

fn transcribe_whisper_cpp(
    config: &SttEngineConfig,
    model_dir: &Path,
    wav_path: &Path,
    descriptor: &ModelDescriptor,
    language: &str,
) -> Result<String> {
    let cli = config
        .whisper_cli_path
        .as_ref()
        .context("whisper-cli not found")?;

    let model_file = find_ggml_file(model_dir)?;

    // Resolve the -l flag: Auto omits it (whisper auto-detects); EN-only
    // checkpoints always use "en" since their weights lack other languages.
    let english_only = descriptor.name.contains(" EN ") || descriptor.name.ends_with(" EN");
    let lang_code: Option<&str> = if english_only || language.eq_ignore_ascii_case("English") {
        Some("en")
    } else {
        crate::models::catalog::whisper_code(language)
    };
    if !english_only && !language.eq_ignore_ascii_case("Auto") && lang_code.is_none() {
        warn!("Language '{}' has no whisper code — auto-detecting", language);
    }

    info!(
        "Whisper CLI: {:?} model: {:?} wav: {:?} lang: {:?}",
        cli, model_file, wav_path, lang_code,
    );

    let mut cmd = Command::new(cli);
    no_window(&mut cmd);
    cmd.arg("-m")
        .arg(&model_file)
        .arg("-f")
        .arg(wav_path);
    if let Some(code) = lang_code {
        cmd.arg("-l").arg(code);
    }
    let output = cmd
        .arg("--no-timestamps")
        .arg("--suppress-nst")
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
        .map(|l| l.trim())
        .filter(|l| {
            if l.is_empty() {
                return false;
            }
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
        .join(" ");

    let trimmed = text.trim();
    let low = trimmed.to_lowercase();
    if low == "[blank audio]" || low == "[blank_audio]" || low == "[blank]" {
        return Ok(String::new());
    }

    Ok(trimmed.to_string())
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

    // sherpa-onnx uses `--opt=value` syntax (space-separated `--opt value`
    // is rejected as "Invalid option"). CPU-optimised: 4 threads (physical
    // cores on most laptops, avoids oversubscription on PTT flush), cpu
    // provider, explicit model-type so the model loads once.
    let num_threads = std::thread::available_parallelism()
        .map(|n| n.get().clamp(2, 8) as u32)
        .unwrap_or(4)
        .min(4);

    let mut cmd = Command::new(cli);
    no_window(&mut cmd);

    match family {
        EngineFamily::NemoTransducer => {
            let encoder = model_dir.join("encoder.int8.onnx");
            let decoder = model_dir.join("decoder.int8.onnx");
            let joiner = model_dir.join("joiner.int8.onnx");
            let tokens = model_dir.join("tokens.txt");
            cmd.arg(format!("--transducer-encoder={}", encoder.display()))
                .arg(format!("--transducer-decoder={}", decoder.display()))
                .arg(format!("--transducer-joiner={}", joiner.display()))
                .arg(format!("--tokens={}", tokens.display()));
        }
        EngineFamily::NemoCTC => {
            let model = model_dir.join("model.int8.onnx");
            let tokens = model_dir.join("tokens.txt");
            cmd.arg(format!("--nemo-ctc-model={}", model.display()))
                .arg(format!("--tokens={}", tokens.display()));
        }
        EngineFamily::SherpaOnnx => {
            let encoder = model_dir.join("encoder_model.ort");
            let decoder_merged = model_dir.join("decoder_model_merged.ort");
            let decoder_cached = model_dir.join("cached_decoder_model.ort");
            let decoder_uncached = model_dir.join("uncached_decoder_model.ort");
            let tokens = model_dir.join("tokens.txt");
            let preprocessor = model_dir.join("preprocess.ort");

            if decoder_merged.exists() {
                cmd.arg(format!("--moonshine-encoder={}", encoder.display()))
                    .arg(format!(
                        "--moonshine-merged-decoder={}",
                        decoder_merged.display()
                    ))
                    .arg(format!("--tokens={}", tokens.display()));
                if preprocessor.exists() {
                    cmd.arg(format!(
                        "--moonshine-preprocessor={}",
                        preprocessor.display()
                    ));
                }
            } else {
                if preprocessor.exists() {
                    cmd.arg(format!(
                        "--moonshine-preprocessor={}",
                        preprocessor.display()
                    ));
                }
                cmd.arg(format!("--moonshine-encoder={}", encoder.display()))
                    .arg(format!(
                        "--moonshine-uncached-decoder={}",
                        decoder_uncached.display()
                    ))
                    .arg(format!(
                        "--moonshine-cached-decoder={}",
                        decoder_cached.display()
                    ))
                    .arg(format!("--tokens={}", tokens.display()));
            }
        }
        _ => {}
    }

    cmd.arg(format!("--num-threads={num_threads}"))
        .arg("--provider=cpu");
    // Explicit model-type avoids loading the model twice (sherpa optimisation).
    // Moonshine has no dedicated type — omit it (unknown values just double-load).
    if matches!(family, EngineFamily::NemoTransducer) {
        cmd.arg("--model-type=transducer");
    } else if matches!(family, EngineFamily::NemoCTC) {
        cmd.arg("--model-type=nemo_ctc");
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

    let stdout_str = String::from_utf8_lossy(&output.stdout);
    let clean_moonshine_str = |raw: &str| -> String {
        let mut s = raw.trim();
        if let Some(idx) = s.find(". ") {
            let prefix = s[..idx].trim();
            if !prefix.is_empty() && prefix.chars().all(|c| c.is_ascii_digit()) {
                s = &s[idx + 2..];
            }
        } else if let Some(idx) = s.find(": ") {
            let prefix = s[..idx].trim();
            if !prefix.is_empty() && prefix.chars().all(|c| c.is_ascii_digit()) {
                s = &s[idx + 2..];
            }
        }
        s.trim().to_string()
    };

    for line in stdout_str.lines().rev() {
        let tr = line.trim();
        if tr.starts_with('{') && tr.ends_with('}') {
            if let Ok(v) = serde_json::from_str::<serde_json::Value>(tr) {
                if let Some(t) = v.get("text").and_then(|s| s.as_str()) {
                    let cleaned = clean_moonshine_str(t);
                    return Ok(cleaned);
                }
            }
        }
    }

    let text = stdout_str
        .lines()
        .filter(|l| {
            let tr = l.trim();
            !tr.is_empty()
                && !tr.starts_with("Duration")
                && !tr.starts_with("OfflineRecognizerConfig")
                && !tr.starts_with("Creating ")
                && !tr.starts_with("Started at")
                && !tr.starts_with("Done!")
                && !tr.starts_with("Elapsed")
                && !tr.starts_with("Audio duration")
                && !tr.starts_with("Real-time factor")
        })
        .map(|l| clean_moonshine_str(l))
        .filter(|l| !l.is_empty())
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
            let mut vc = Command::new(cli);
            no_window(&mut vc);
            let output = vc
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
        models_root.join("runtimes/vosk/vosk.dll"),
        exe_dir.join("tools/vosk/libvosk.dll"),
        exe_dir.join("tools/vosk/vosk.dll"),
        exe_dir.join("libvosk.dll"),
        exe_dir.join("vosk.dll"),
        PathBuf::from("libvosk.dll"),
        PathBuf::from("vosk.dll"),
        exe_dir.join("../vosk_api/vosk-win64-0.3.42/libvosk.dll"),
        exe_dir.join("../../vosk_api/vosk-win64-0.3.42/libvosk.dll"),
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
    // Windows: libvosk.dll depends on MinGW runtimes (libgcc_s_seh-1.dll,
    // libstdc++-6.dll, libwinpthread-1.dll) shipped alongside it in
    // runtimes/vosk/. The loader searches only the app dir + PATH by default,
    // so add the DLL's own dir to the search path first — otherwise LoadLibrary
    // fails with "module not found" even though libvosk.dll exists.
    #[cfg(target_os = "windows")]
    {
        if let Some(dir) = lib_path.parent() {
            use std::os::windows::ffi::OsStrExt;
            use windows::core::PCWSTR;
            use windows::Win32::System::LibraryLoader::SetDllDirectoryW;
            let wide: Vec<u16> = dir
                .as_os_str()
                .encode_wide()
                .chain(std::iter::once(0))
                .collect();
            unsafe {
                let _ = SetDllDirectoryW(PCWSTR(wide.as_ptr()));
            }
            // Pre-load the MinGW deps explicitly so a PATH without them still works.
            for dep in ["libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll"] {
                let p = dir.join(dep);
                if p.exists() {
                    unsafe {
                        let _ = Library::new(&p);
                    }
                }
            }
        }
    }
    unsafe {
        let lib = Library::new(&lib_path)
            .with_context(|| format!("Failed to load {:?} (missing MinGW deps? check runtimes/vosk/ holds libgcc/libstdc++/libwinpthread alongside libvosk.dll)", lib_path))?;

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

        let model_new: Symbol<ModelNew> = lib.get(b"vosk_model_new")?;
        let rec_new: Symbol<RecNew> = lib.get(b"vosk_recognizer_new")?;
        let accept: Symbol<AcceptWaveform> = lib.get(b"vosk_recognizer_accept_waveform_s")?;
        let final_result: Symbol<FinalResult> = lib.get(b"vosk_recognizer_final_result")?;
        let rec_free: Symbol<FreeRecognizer> = lib.get(b"vosk_recognizer_free")?;
        let model_free: Symbol<FreeModel> = lib.get(b"vosk_model_free")?;

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

static NEMOTRON_SESSION: Lazy<Mutex<Option<ParakeetSession>>> = Lazy::new(|| Mutex::new(None));

pub fn nemotron_unload() {
    let mut guard = match NEMOTRON_SESSION.lock() {
        Ok(g) => g,
        Err(_) => return,
    };
    let session = match guard.as_mut() {
        Some(s) => s,
        None => return,
    };
    if session.loaded_model_dir.is_none() {
        return;
    }
    if matches!(session.child.try_wait(), Ok(Some(_))) {
        *guard = None;
        return;
    }
    match parakeet_request(session, json!({ "action": "unload" })) {
        Ok(resp) => match ensure_parakeet_ok(resp, "unload") {
            Ok(_) => {
                session.loaded_model_dir = None;
                info!("Nemotron model unloaded (idle offload)");
            }
            Err(e) => warn!("Nemotron unload rejected: {e:#}"),
        },
        Err(e) => warn!("Nemotron unload failed: {e:#}"),
    }
}

fn transcribe_nemotron(
    config: &SttEngineConfig,
    model_dir: &Path,
    wav_path: &Path,
    language: &str,
) -> Result<String> {
    let cli = config.nemotron_path.as_ref().context(
        "Nemotron engine not found — install the Nemotron 3.5 ASR Streaming 0.6B model \
         from Settings → Models (the transcribe.cpp CLI ships with the app)",
    )?;
    let stem = cli.file_stem().and_then(|f| f.to_str()).unwrap_or("");
    let is_tcpp_cli = stem == "transcribe-cli" || stem == "transcribe";

    let lang_code = if language.is_empty()
        || language.eq_ignore_ascii_case("Auto")
        || language.eq_ignore_ascii_case("English")
    {
        "en-US"
    } else {
        language
    };

    // Q4_K_M is the working CPU quant: Q8_0 decodes to all-<unk> in the
    // shipped GGUF (verified 2026-09: Q4 gives clean JFK transcript, Q8 gives
    // 490x <unk> on CPU and Vulkan). Q4 is also 34% smaller (473MB vs 716MB)
    // and faster on CPU — the efficient native choice.
    let prefer_q4_gguf = |dir: &Path| -> Option<PathBuf> {
        for name in [
            "nemotron-3.5-asr-streaming-0.6b-Q4_K_M.gguf",
            "nemotron-3.5-asr-streaming-0.6b-Q5_K_M.gguf",
            "nemotron-3.5-asr-streaming-0.6b-Q6_K.gguf",
            "nemotron-3.5-asr-streaming-0.6b-Q8_0.gguf",
            "nemotron-3.5-asr-streaming-0.6b-F16.gguf",
        ] {
            let p = dir.join(name);
            if p.exists() {
                return Some(p);
            }
        }
        None
    };

    if is_tcpp_cli {
        let gguf = prefer_q4_gguf(model_dir)
            .or_else(|| find_gguf_file(model_dir).ok())
            .context("No *.gguf model file found")?;
        info!("Nemotron via transcribe-cli {:?} gguf={:?}", cli, gguf);
        let mut nc = Command::new(cli);
        no_window(&mut nc);
        // CPU-optimised: 4 threads max (RNNT joint is memory-bound, more
        // threads hurt), cpu backend (Vulkan iGPU gives same <unk> as CPU
        // for broken Q8, and CPU is deterministic), correct locale.
        let output = nc
            .arg("-m").arg(&gguf)
            .arg("--language").arg(lang_code)
            .arg("--threads").arg("4")
            .arg("--backend").arg("cpu")
            .arg(wav_path)
            .output()
            .context("Failed to run transcribe-cli")?;
        if !output.status.success() {
            let stderr = String::from_utf8_lossy(&output.stderr);
            warn!("transcribe-cli error: {}", stderr);
            anyhow::bail!("transcribe-cli failed: {}", stderr);
        }
        let text = String::from_utf8_lossy(&output.stdout);
        let clean = clean_nemotron_text(&text);
        // CLI fallback still crashes on some CPUs for Nemotron RNNT — if it
        // returns empty/<unk>-only, surface it as empty (VAD gate drops it)
        // rather than crashing the turn.
        return Ok(clean);
    }

    // Windows helper binary: persistent JSON-RPC worker (Handy stack).
    if !model_dir.exists() {
        anyhow::bail!("Nemotron model directory not found: {:?}", model_dir);
    }

    let mut guard = NEMOTRON_SESSION
        .lock()
        .map_err(|_| anyhow::anyhow!("Nemotron session lock poisoned"))?;

    let dead = guard
        .as_mut()
        .map(|s| matches!(s.child.try_wait(), Ok(Some(_))))
        .unwrap_or(true);
    let needs_spawn =
        dead || guard.as_ref().map(|s| s.exe_path != *cli).unwrap_or(true);
    if needs_spawn {
        *guard = Some(spawn_parakeet_session(cli)?);
    }

    let session = guard
        .as_mut()
        .context("Nemotron session was not initialised")?;

    // Pass the explicit Q4 file path (not the dir) so the worker's
    // resolveGgufPath picks the working quant even when a broken Q8 sits
    // alongside it. Falls back to the dir for older installs.
    let load_path: PathBuf = prefer_q4_gguf(model_dir)
        .unwrap_or_else(|| model_dir.to_path_buf());
    let load_key = load_path.clone();
    if session.loaded_model_dir.as_deref() != Some(model_dir) {
        let response = parakeet_request(
            session,
            json!({
                "action": "load",
                "model_path": load_path.to_string_lossy(),
                "language": lang_code,
            }),
        )?;
        ensure_parakeet_ok(response, "load")?;
        session.loaded_model_dir = Some(model_dir.to_path_buf());
        // Remember the resolved file for diagnostics (session key stays dir).
        let _ = load_key;
    }

    let response = parakeet_request(
        session,
        json!({
            "action": "transcribe",
            "audio_path": wav_path.to_string_lossy(),
            "language": lang_code,
        }),
    )?;
    let text = ensure_parakeet_ok(response, "transcribe")?.unwrap_or_default();
    Ok(clean_nemotron_text(&text))
}

static CANARY_SESSION: Lazy<Mutex<Option<ParakeetSession>>> = Lazy::new(|| Mutex::new(None));

/// Idle offload for Canary (same async pattern as Parakeet/Nemotron).
pub fn canary_unload() {
    let mut guard = match CANARY_SESSION.lock() {
        Ok(g) => g,
        Err(_) => return,
    };
    let session = match guard.as_mut() {
        Some(s) => s,
        None => return,
    };
    if session.loaded_model_dir.is_none() {
        return;
    }
    if matches!(session.child.try_wait(), Ok(Some(_))) {
        *guard = None;
        return;
    }
    match parakeet_request(session, json!({ "action": "unload" })) {
        Ok(resp) => match ensure_parakeet_ok(resp, "unload") {
            Ok(_) => {
                session.loaded_model_dir = None;
                info!("Canary model unloaded (idle offload)");
            }
            Err(e) => warn!("Canary unload rejected: {e:#}"),
        },
        Err(e) => warn!("Canary unload failed: {e:#}"),
    }
}

/// Canary 180M Flash via the Handy transcribe.cpp worker (same JSON protocol
/// as Nemotron): GGUF + language + CPU threads. CPU default Q5_K_M per spec;
/// Q4_K_M low-memory fallback. Language maps to canary locales (en/de/fr/es);
/// anything else falls back to en.
fn transcribe_canary(
    config: &SttEngineConfig,
    model_dir: &Path,
    wav_path: &Path,
    language: &str,
) -> Result<String> {
    let cli = config.nemotron_path.as_ref().context(
        "Canary engine not found — the Handy transcribe worker ships with the app \
         (tools/nemotron/nemotron_engine)",
    )?;
    let stem = cli.file_stem().and_then(|f| f.to_str()).unwrap_or("");
    // The worker binary handles every GGUF family; a bare transcribe-cli would
    // need per-family flags — route CLI-named binaries through the same worker
    // spawn (they speak the same protocol when built from tools/nemotron).
    let _ = stem;
    let lang_code = if language.eq_ignore_ascii_case("German") {
        "de"
    } else if language.eq_ignore_ascii_case("French") {
        "fr"
    } else if language.eq_ignore_ascii_case("Spanish") {
        "es"
    } else {
        "en"
    };
    if !model_dir.exists() {
        anyhow::bail!("Canary model directory not found: {:?}", model_dir);
    }
    let load_path: PathBuf =
        find_canary_gguf(model_dir).unwrap_or_else(|_| model_dir.to_path_buf());
    let mut guard = CANARY_SESSION
        .lock()
        .map_err(|_| anyhow::anyhow!("Canary session lock poisoned"))?;
    let dead = guard
        .as_mut()
        .map(|s| matches!(s.child.try_wait(), Ok(Some(_))))
        .unwrap_or(true);
    let needs_spawn =
        dead || guard.as_ref().map(|s| s.exe_path != *cli).unwrap_or(true);
    if needs_spawn {
        *guard = Some(spawn_parakeet_session(cli)?);
    }
    let session = guard
        .as_mut()
        .context("Canary session was not initialised")?;
    if session.loaded_model_dir.as_deref() != Some(model_dir) {
        let response = parakeet_request(
            session,
            json!({
                "action": "load",
                "model_path": load_path.to_string_lossy(),
                "language": lang_code,
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
            "language": lang_code,
        }),
    )?;
    let text = ensure_parakeet_ok(response, "transcribe")?.unwrap_or_default();
    Ok(clean_nemotron_text(&text))
}

fn find_canary_gguf(model_dir: &Path) -> Result<PathBuf> {
    for prefer in [
        "canary-180m-flash-Q5_K_M.gguf",
        "canary-180m-flash-Q4_K_M.gguf",
        "canary-180m-flash-Q8_0.gguf",
        "canary-180m-flash-F16.gguf",
    ] {
        let p = model_dir.join(prefer);
        if p.exists() {
            return Ok(p);
        }
    }
    if let Ok(entries) = std::fs::read_dir(model_dir) {
        for entry in entries.flatten() {
            if entry.file_name().to_string_lossy().ends_with(".gguf") {
                return Ok(entry.path());
            }
        }
    }
    anyhow::bail!("No canary *.gguf found in {:?}", model_dir)
}

fn find_gguf_file(model_dir: &Path) -> Result<PathBuf> {
    // Prefer the working CPU quant first (Q4_K_M verified clean; Q8_0 in the
    // shipped batch decodes to all-<unk>), then any .gguf.
    for prefer in [
        "nemotron-3.5-asr-streaming-0.6b-Q4_K_M.gguf",
        "nemotron-3.5-asr-streaming-0.6b-Q5_K_M.gguf",
        "nemotron-3.5-asr-streaming-0.6b-Q6_K.gguf",
        "nemotron-3.5-asr-streaming-0.6b-Q8_0.gguf",
    ] {
        let p = model_dir.join(prefer);
        if p.exists() {
            return Ok(p);
        }
    }
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

/// Strip Handy control tokens; an all-<unk> decode (broken quant) becomes
/// empty so the VAD/hallucination gate drops it instead of pasting garbage.
fn clean_nemotron_text(raw: &str) -> String {    let without_tokens = raw
        .replace("<unk>", "")
        .replace("<pad>", "")
        .replace("<s>", "")
        .replace("</s>", "")
        .replace("<|endoftext|>", "")
        .replace("<lang-en-US>", "")
        .replace("<en-US>", "");
    without_tokens
        .lines()
        .map(|l| l.trim())
        .filter(|l| {
            if l.is_empty() {
                return false;
            }
            let low = l.to_lowercase();
            // transcribe-cli echoes run metadata — never part of the transcript.
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
            {
                return false;
            }
            true
        })
        .collect::<Vec<_>>()
        .join(" ")
        .trim()
        .to_string()
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

