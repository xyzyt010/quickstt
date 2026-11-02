// Android speech-to-text: the engines (Cactus needle, whisper.cpp,
// transcribe.cpp), the model catalog (with Cactus Whistle 16M bundled inside
// the APK), and the on-device RAM onloading / offloading runtime manager.

use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

/// The engines that run on the phone.
pub const ENGINES: &[(&str, &str, &str)] = &[
    // id, display name, binary file name
    ("cactus", "Cactus needle", "needle"),
    ("whisper", "Whisper (whisper.cpp)", "whisper-cli"),
    ("transcribe", "transcribe.cpp", "transcribe-cli"),
];

/// The display name of an engine.
pub fn engine_name(id: &str) -> &'static str {
    ENGINES
        .iter()
        .find(|e| e.0 == id)
        .map(|e| e.1)
        .unwrap_or("unknown engine")
}

/// A size that a human can read.
pub fn human_size(bytes: u64) -> String {
    const MB: f64 = 1_048_576.0;
    let mb = bytes as f64 / MB;
    if mb >= 1.0 {
        format!("{mb:.1} MB")
    } else if bytes > 0 {
        format!("{:.0} kB", bytes as f64 / 1024.0)
    } else {
        "0 MB".to_string()
    }
}

/// Where an engine's binary is and whether it is there at all.
pub fn engine_info(id: &str) -> String {
    match ensure_engine(id) {
        Ok(engine) => {
            let size = std::fs::metadata(&engine.binary).map(|m| m.len()).unwrap_or(0);
            let via = if engine.launcher.is_some() {
                " (via system linker64)"
            } else {
                " (native exec)"
            };
            format!("{} · {}{via}", engine.binary.display(), human_size(size))
        }
        Err(e) => format!("not available ({e})"),
    }
}

/// Where a model's file is and whether it is there.
pub fn model_info(index: usize) -> String {
    let spec = model(index);
    match model_path(index).and_then(|p| std::fs::metadata(&p).ok()) {
        Some(meta) => format!("{} · {}", meta_path(index), human_size(meta.len())),
        None => format!("{} · not downloaded", spec.file),
    }
}

fn meta_path(index: usize) -> String {
    model_path(index)
        .map(|p| p.display().to_string())
        .unwrap_or_else(|| "no path".to_string())
}

/// Transcribe a WAV and report how long the engine took.
pub fn transcribe_timed(wav: &Path, index: usize) -> Result<(String, f32), String> {
    let started = Instant::now();
    let text = transcribe(wav, index)?;
    Ok((text, started.elapsed().as_secs_f32()))
}

fn engine_binary(id: &str) -> &'static str {
    ENGINES
        .iter()
        .find(|e| e.0 == id)
        .map(|e| e.2)
        .unwrap_or("needle")
}

fn engine_entry(id: &str) -> String {
    format!("lib{}.so", engine_binary(id))
}

/// A ready-to-run engine.
pub struct Engine {
    pub binary: PathBuf,
    pub launcher: Option<PathBuf>,
}

impl Engine {
    pub fn command(&self) -> Command {
        let mut command = match &self.launcher {
            Some(launcher) => {
                let mut cmd = Command::new(launcher);
                cmd.arg(&self.binary);
                cmd
            }
            None => Command::new(&self.binary),
        };
        if let Some(dir) = work_dir() {
            command.env("TMPDIR", &dir).env("HOME", &dir);
        }
        command.stdout(Stdio::piped());
        command.stderr(Stdio::piped());
        command
    }
}

/// A speech model that really runs on Android.
pub struct ModelSpec {
    pub name: &'static str,
    pub engine: &'static str,
    pub file: &'static str,
    pub url: &'static str,
    pub size: &'static str,
    pub min_bytes: u64,
    pub detail: &'static str,
    pub bundled: bool,
    pub lang_arg: Option<&'static str>,
}

/// The Android model catalog.
///
/// Index 0 (`DEFAULT_MODEL`) is **Cactus Whistle 16M** (`whistle.cact`), which
/// is bundled directly inside the APK and extracted automatically on launch so
/// the app works 100% offline out of the box without any download step.
pub const MODELS: &[ModelSpec] = &[
    ModelSpec {
        name: "Cactus Whistle 16M",
        engine: "cactus",
        file: "whistle.cact",
        url: "https://huggingface.co/Cactus-Compute/whistle/resolve/main/whistle.cact",
        size: "16.9 MB",
        min_bytes: 16_000_000,
        detail: "Built-in · 7 languages · ARM64 NEON dotprod · Cactus needle",
        bundled: true,
        lang_arg: Some("en"),
    },
    ModelSpec {
        name: "Whisper Tiny EN",
        engine: "whisper",
        file: "ggml-tiny.en-q5_1.bin",
        url: "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en-q5_1.bin",
        size: "32 MB",
        min_bytes: 30_000_000,
        detail: "English · Q5_1 quantized · whisper.cpp",
        bundled: false,
        lang_arg: Some("en"),
    },
    ModelSpec {
        name: "Moonshine Base EN",
        engine: "transcribe",
        file: "moonshine-base-Q8_0.gguf",
        url: "https://huggingface.co/handy-computer/moonshine-base-gguf/resolve/main/moonshine-base-Q8_0.gguf",
        size: "74 MB",
        min_bytes: 70_000_000,
        detail: "English · low-latency encoder-decoder · transcribe.cpp",
        bundled: false,
        lang_arg: None,
    },
    ModelSpec {
        name: "SenseVoice Small",
        engine: "transcribe",
        file: "SenseVoiceSmall-Q4_K_M.gguf",
        url: "https://huggingface.co/handy-computer/SenseVoiceSmall-gguf/resolve/main/SenseVoiceSmall-Q4_K_M.gguf",
        size: "139 MB",
        min_bytes: 130_000_000,
        detail: "5 languages · ultra-fast non-autoregressive · transcribe.cpp",
        bundled: false,
        lang_arg: None,
    },
    ModelSpec {
        name: "Canary 180M Flash",
        engine: "transcribe",
        file: "canary-180m-flash-Q5_K_M.gguf",
        url: "https://huggingface.co/handy-computer/canary-180m-flash-gguf/resolve/main/canary-180m-flash-Q5_K_M.gguf",
        size: "151 MB",
        min_bytes: 145_000_000,
        detail: "4 languages · punctuation & capitalization · transcribe.cpp",
        bundled: false,
        lang_arg: Some("en"),
    },
    ModelSpec {
        name: "Parakeet Unified EN 0.6B",
        engine: "transcribe",
        file: "parakeet-unified-en-0.6b-Q4_K_M.gguf",
        url: "https://huggingface.co/handy-computer/parakeet-unified-en-0.6b-gguf/resolve/main/parakeet-unified-en-0.6b-Q4_K_M.gguf",
        size: "455 MB",
        min_bytes: 440_000_000,
        detail: "English · FastConformer TDT 0.6B · transcribe.cpp",
        bundled: false,
        lang_arg: None,
    },
    ModelSpec {
        name: "Nemotron 3.5 ASR Streaming 0.6B",
        engine: "transcribe",
        file: "nemotron-3.5-asr-streaming-0.6b-Q4_K_M.gguf",
        url: "https://huggingface.co/handy-computer/nemotron-3.5-asr-streaming-0.6b-gguf/resolve/main/nemotron-3.5-asr-streaming-0.6b-Q4_K_M.gguf",
        size: "473 MB",
        min_bytes: 460_000_000,
        detail: "30 languages · streaming RNNT · transcribe.cpp",
        bundled: false,
        lang_arg: Some("en-US"),
    },
    ModelSpec {
        name: "Qwen3-ASR 0.6B",
        engine: "transcribe",
        file: "Qwen3-ASR-0.6B-Q4_K_M.gguf",
        url: "https://huggingface.co/handy-computer/Qwen3-ASR-0.6B-gguf/resolve/main/Qwen3-ASR-0.6B-Q4_K_M.gguf",
        size: "562 MB",
        min_bytes: 540_000_000,
        detail: "Multilingual · auto-detect · transcribe.cpp",
        bundled: false,
        lang_arg: None,
    },
];

/// Default model is 0 (Cactus Whistle 16M, inbuilt in the APK).
pub const DEFAULT_MODEL: usize = 0;

/// Get a model spec by index.
pub fn model(index: usize) -> &'static ModelSpec {
    MODELS.get(index).unwrap_or(&MODELS[DEFAULT_MODEL])
}

/// Writable app directory for models and temporary WAV buffers.
pub fn work_dir() -> Option<PathBuf> {
    let base = crate::platform::files_dir()?;
    let dir = base.join("quickstt");
    let _ = std::fs::create_dir_all(&dir);
    Some(dir)
}

/// Writable path for temporary WAV recording files.
///
/// Never uses `/data/local/tmp` on Android (which is permission-denied to apps).
pub fn wav_path(file_name: &str) -> PathBuf {
    if let Some(dir) = work_dir() {
        return dir.join(file_name);
    }
    std::env::temp_dir().join(file_name)
}

/// Extract any bundled models (such as `whistle.cact`) from the APK on startup.
pub fn ensure_bundled_models() {
    for (idx, spec) in MODELS.iter().enumerate() {
        if spec.bundled && !model_installed(idx) {
            let _ = crate::platform::extract_bundled_model(spec.file, spec.min_bytes);
        }
    }
}

/// Make sure the engine binary is unpacked and executable.
pub fn ensure_engine(id: &str) -> Result<Engine, String> {
    #[cfg(target_os = "android")]
    {
        let entry = engine_entry(id);
        if let Some(dir) = crate::platform::native_lib_dir() {
            let path = dir.join(&entry);
            if path.exists() {
                return Ok(Engine {
                    binary: path,
                    launcher: None,
                });
            }
        }
        if let Some(path) = crate::platform::stage_from_apk(&entry) {
            // If stage_from_apk found the file inside nativeLibraryDir, run it directly
            let in_native = crate::platform::native_lib_dir()
                .map(|nd| path.starts_with(&nd))
                .unwrap_or(false);
            if in_native {
                return Ok(Engine {
                    binary: path,
                    launcher: None,
                });
            }
            return Ok(Engine {
                binary: path,
                launcher: crate::platform::linker64(),
            });
        }
        Err(format!("the {id} engine is missing from the app"))
    }
    #[cfg(not(target_os = "android"))]
    {
        let _ = id;
        Err("the Android engines are only available on Android".into())
    }
}

/// Path of a model on disk.
pub fn model_path(index: usize) -> Option<PathBuf> {
    work_dir().map(|d| d.join("models").join(model(index).file))
}

/// True when the model is present on disk and meets the minimum byte size.
pub fn model_installed(index: usize) -> bool {
    let spec = model(index);
    if let Some(path) = model_path(index) {
        if let Ok(meta) = std::fs::metadata(&path) {
            if meta.len() >= spec.min_bytes {
                return true;
            }
        }
    }
    false
}

/// Actual size on disk in bytes (0 if not installed).
pub fn model_disk_bytes(index: usize) -> u64 {
    model_path(index)
        .and_then(|p| std::fs::metadata(p).ok())
        .map(|m| m.len())
        .unwrap_or(0)
}

/// Delete a downloaded model from disk (and offload it if currently in RAM).
pub fn uninstall_model(index: usize) -> Result<(), String> {
    if loaded_model_index() == Some(index) {
        let _ = offload_model();
    }
    if let Some(path) = model_path(index) {
        if path.exists() {
            std::fs::remove_file(&path).map_err(|e| format!("could not remove model file: {e}"))?;
            return Ok(());
        }
    }
    Err("model is not installed on disk".into())
}

/// Remember the model the user picked.
pub fn save_selected(index: usize) {
    if let Some(dir) = work_dir() {
        let _ = std::fs::write(dir.join("selected_model"), index.to_string());
    }
}

/// The model the user picked (defaults to index 0 = inbuilt Cactus Whistle 16M).
pub fn selected() -> usize {
    let saved = work_dir()
        .and_then(|d| std::fs::read_to_string(d.join("selected_model")).ok())
        .and_then(|s| s.trim().parse::<usize>().ok())
        .filter(|i| *i < MODELS.len());
    if let Some(idx) = saved {
        if model_installed(idx) {
            return idx;
        }
    }
    // Prefer the inbuilt model (index 0) or any installed model
    for i in 0..MODELS.len() {
        if model_installed(i) {
            return i;
        }
    }
    DEFAULT_MODEL
}

// ══════════════════════════════════════════════════════════════════════════════
//  MODEL ONLOADING & OFFLOADING RUNTIME MANAGER
// ══════════════════════════════════════════════════════════════════════════════

struct WorkerHandle {
    child: Child,
    stdin: ChildStdin,
    stdout: BufReader<ChildStdout>,
}

impl Drop for WorkerHandle {
    fn drop(&mut self) {
        // Ask the worker to release the model before taking the process down.
        let _ = writeln!(self.stdin, "{{\"action\":\"quit\"}}");
        let _ = self.stdin.flush();
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

struct ResidentModel {
    index: usize,
    ram_bytes: u64,
    load_ms: u32,
    last_used: Instant,
    /// Persistent `transcribe-cli --worker` session keeping GGUF/GGML tensors in RAM.
    ///
    /// This lives behind its own mutex so a long transcription does not hold
    /// `RESIDENT` — the UI polls the resident status every second and must
    /// never block on engine I/O.
    worker: Option<Arc<Mutex<WorkerHandle>>>,
    /// Pre-warmed model bytes kept hot in RAM for instant one-shot engines (`needle`/`whisper`).
    pinned_weights: Option<Vec<u8>>,
}

static RESIDENT: Mutex<Option<ResidentModel>> = Mutex::new(None);

/// Currently onloaded model index, if any.
pub fn loaded_model_index() -> Option<usize> {
    RESIDENT.lock().ok().and_then(|g| g.as_ref().map(|r| r.index))
}

/// Details of the currently onloaded model: `(index, ram_bytes, load_ms, idle_secs)`.
pub fn loaded_model_status() -> Option<(usize, u64, u32, u64)> {
    let guard = RESIDENT.lock().ok()?;
    let res = guard.as_ref()?;
    Some((
        res.index,
        res.ram_bytes,
        res.load_ms,
        res.last_used.elapsed().as_secs(),
    ))
}

/// Onload a model into RAM so transcription starts with zero cold-load latency.
pub fn onload_model(index: usize) -> Result<String, String> {
    let spec = model(index);
    if !model_installed(index) {
        if spec.bundled && crate::platform::extract_bundled_model(spec.file, spec.min_bytes) {
            // Extracted bundled model on the fly
        } else {
            return Err(format!("{} is not downloaded yet", spec.name));
        }
    }

    // If already loaded, just touch timestamp and return status
    if let Ok(mut guard) = RESIDENT.lock() {
        if let Some(existing) = guard.as_mut() {
            if existing.index == index {
                let alive = existing
                    .worker
                    .as_mut()
                    .map(|w| match w.lock() {
                        Ok(mut w) => matches!(w.child.try_wait(), Ok(None)),
                        Err(_) => false,
                    })
                    .unwrap_or(true);
                if alive {
                    existing.last_used = Instant::now();
                    return Ok(format!(
                        "{} is onloaded in RAM ({} · {} ms)",
                        spec.name,
                        human_size(existing.ram_bytes),
                        existing.load_ms
                    ));
                }
            }
        }
    }

    // Offload any previous model first
    let _ = offload_model();

    let model_file = model_path(index).ok_or("no model path available")?;
    let disk_bytes = std::fs::metadata(&model_file)
        .map(|m| m.len())
        .unwrap_or(spec.min_bytes);
    let started = Instant::now();

    #[cfg(target_os = "android")]
    {
        let engine = ensure_engine(spec.engine)?;
        if spec.engine == "transcribe" {
            // Spawn persistent `transcribe-cli --worker` and load the GGUF into RAM
            match spawn_transcribe_worker(&engine, &model_file) {
                Ok((worker, load_ms)) => {
                    let elapsed_ms = if load_ms > 0 {
                        load_ms
                    } else {
                        started.elapsed().as_millis() as u32
                    };
                    if let Ok(mut guard) = RESIDENT.lock() {
                        *guard = Some(ResidentModel {
                            index,
                            ram_bytes: disk_bytes,
                            load_ms: elapsed_ms,
                            last_used: Instant::now(),
                            worker: Some(Arc::new(Mutex::new(worker))),
                            pinned_weights: None,
                        });
                    }
                    return Ok(format!(
                        "Onloaded {} into RAM ({} · {} ms)",
                        spec.name,
                        human_size(disk_bytes),
                        elapsed_ms
                    ));
                }
                Err(e) => {
                    crate::platform::log(&format!(
                        "worker preload failed for {}: {e}",
                        spec.name
                    ));
                    // Never pin a multi-hundred-MB GGUF as a fallback — the
                    // one-shot engine still works, but the phone could OOM.
                    return Err(format!(
                        "{} could not be kept in RAM ({e}); one-shot transcription still works",
                        spec.name
                    ));
                }
            }
        }

        // For Cactus needle / Whisper (or fallback): pin the model weights in RAM
        // so the kernel page cache is 100% hot and verify the engine binary.
        let weights = std::fs::read(&model_file)
            .map_err(|e| format!("could not read {} into RAM: {e}", spec.name))?;
        let ram_bytes = weights.len() as u64;
        let elapsed_ms = started.elapsed().as_millis().max(1) as u32;
        if let Ok(mut guard) = RESIDENT.lock() {
            *guard = Some(ResidentModel {
                index,
                ram_bytes,
                load_ms: elapsed_ms,
                last_used: Instant::now(),
                worker: None,
                pinned_weights: Some(weights),
            });
        }
        Ok(format!(
            "Onloaded {} into RAM ({} · {} ms)",
            spec.name,
            human_size(ram_bytes),
            elapsed_ms
        ))
    }

    #[cfg(not(target_os = "android"))]
    {
        let elapsed_ms = started.elapsed().as_millis().max(1) as u32;
        if let Ok(mut guard) = RESIDENT.lock() {
            *guard = Some(ResidentModel {
                index,
                ram_bytes: disk_bytes,
                load_ms: elapsed_ms,
                last_used: Instant::now(),
                worker: None,
                pinned_weights: None,
            });
        }
        Ok(format!(
            "Onloaded {} into RAM ({})",
            spec.name,
            human_size(disk_bytes)
        ))
    }
}

/// Offload the currently resident model from RAM to free device memory.
pub fn offload_model() -> Result<String, String> {
    let mut guard = RESIDENT.lock().map_err(|_| "lock poisoned".to_string())?;
    let Some(mut resident) = guard.take() else {
        return Ok("No model was loaded in RAM".to_string());
    };
    let name = model(resident.index).name;
    let freed = resident.ram_bytes;
    if let Some(worker) = resident.worker.take() {
        // If a transcription currently owns the worker, dropping our handle
        // lets that call finish; WorkerHandle::drop shuts it down afterwards.
        if let Ok(mut worker) = worker.try_lock() {
            let _ = writeln!(worker.stdin, "{{\"action\":\"quit\"}}");
            let _ = worker.stdin.flush();
        }
        drop(worker);
    }
    drop(resident.pinned_weights.take());
    Ok(format!(
        "Offloaded {name} — freed {} of RAM",
        human_size(freed)
    ))
}

/// Automatically offload the resident model if it has been idle longer than `max_idle_secs`.
pub fn check_idle_offload(max_idle_secs: u64) -> Option<String> {
    let should_offload = {
        let guard = RESIDENT.lock().ok()?;
        let res = guard.as_ref()?;
        res.last_used.elapsed() >= Duration::from_secs(max_idle_secs)
    };
    if should_offload {
        offload_model().ok()
    } else {
        None
    }
}

#[cfg(target_os = "android")]
fn spawn_transcribe_worker(engine: &Engine, model_file: &Path) -> Result<(WorkerHandle, u32), String> {
    let args: Vec<std::ffi::OsString> = vec![
        "--worker".into(),
        "-q".into(),
        "--threads".into(),
        "4".into(),
        "--backend".into(),
        "cpu".into(),
    ];
    let mut child = spawn_engine_process(engine, &args, true)?;
    let mut stdin = child.stdin.take().ok_or("worker stdin unavailable")?;
    let stdout = child.stdout.take().ok_or("worker stdout unavailable")?;
    let mut reader = BufReader::new(stdout);

    let escaped_path = model_file.to_string_lossy().replace('\\', "\\\\").replace('"', "\\\"");
    writeln!(stdin, "{{\"action\":\"load\",\"model_path\":\"{escaped_path}\"}}")
        .map_err(|e| format!("failed to send load command: {e}"))?;
    stdin
        .flush()
        .map_err(|e| format!("failed to flush load command: {e}"))?;

    let mut line = String::new();
    reader
        .read_line(&mut line)
        .map_err(|e| format!("worker did not respond to load: {e}"))?;
    if !line.contains("\"status\":\"ok\"") {
        let _ = child.kill();
        let _ = child.wait();
        let err = json_field(&line, "error").unwrap_or_else(|| line.trim().to_string());
        return Err(err);
    }
    let load_ms = json_number(&line, "load_ms").unwrap_or(0.0) as u32;
    Ok((
        WorkerHandle {
            child,
            stdin,
            stdout: reader,
        },
        load_ms,
    ))
}

#[cfg(target_os = "android")]
fn spawn_engine_process(
    engine: &Engine,
    args: &[std::ffi::OsString],
    piped_stdin: bool,
) -> Result<Child, String> {
    let start = |launcher: Option<&Path>| -> std::io::Result<Child> {
        let mut command = match launcher {
            Some(launcher) => {
                let mut cmd = Command::new(launcher);
                cmd.arg(&engine.binary);
                cmd
            }
            None => Command::new(&engine.binary),
        };
        command.args(args);
        if let Some(dir) = work_dir() {
            command.env("TMPDIR", &dir).env("HOME", &dir);
        }
        if piped_stdin {
            command.stdin(Stdio::piped());
        }
        command.stdout(Stdio::piped());
        command.stderr(Stdio::piped());
        command.spawn()
    };

    // 1. Try the preferred launch mode (direct if in nativeLibraryDir, or via engine.launcher)
    match start(engine.launcher.as_deref()) {
        Ok(child) => Ok(child),
        Err(first) => {
            // 2. If direct exec failed, fall back to /system/bin/linker64
            if engine.launcher.is_none() {
                if let Some(linker) = crate::platform::linker64() {
                    if let Ok(child) = start(Some(&linker)) {
                        return Ok(child);
                    }
                }
            } else {
                // Or if launcher failed, try direct exec
                if let Ok(child) = start(None) {
                    return Ok(child);
                }
            }
            Err(format!("could not start engine {}: {first}", engine.binary.display()))
        }
    }
}

/// Download a model, reporting progress (done, total) in bytes.
#[cfg(target_os = "android")]
pub fn download_model<F: FnMut(u64, u64) + 'static>(
    index: usize,
    mut progress: F,
) -> Result<(), String> {
    let spec = model(index);
    if crate::platform::extract_bundled_model(spec.file, spec.min_bytes) {
        progress(spec.min_bytes, spec.min_bytes);
        return Ok(());
    }
    let path = model_path(index).ok_or("no writable app directory")?;
    crate::platform::download_file_with_progress(spec.url, &path, spec.min_bytes, progress)
}

#[cfg(not(target_os = "android"))]
pub fn download_model<F: FnMut(u64, u64)>(_index: usize, _progress: F) -> Result<(), String> {
    Err("model downloads are only available on Android".into())
}

/// Transcribe a 16 kHz mono WAV with the selected model.
///
/// If the model is onloaded in the persistent worker, it transcribes directly
/// against the warm RAM session; otherwise it onloads / runs the engine.
pub fn transcribe(wav: &Path, index: usize) -> Result<String, String> {
    let spec = model(index);
    if !model_installed(index) {
        if spec.bundled && crate::platform::extract_bundled_model(spec.file, spec.min_bytes) {
            // Extracted bundled model
        } else {
            return Err(format!("{} is not downloaded yet", spec.name));
        }
    }

    // 1. Check if this model is already warm in our persistent transcribe-cli worker
    #[cfg(target_os = "android")]
    if spec.engine == "transcribe" {
        if loaded_model_index() != Some(index) {
            let _ = onload_model(index);
        }
        let worker = {
            let mut guard = RESIDENT.lock().ok();
            if let Some(Some(res)) = guard.as_deref_mut() {
                if res.index == index {
                    res.last_used = Instant::now();
                    res.worker.as_ref().cloned()
                } else {
                    None
                }
            } else {
                None
            }
        };
        // Run the worker I/O without holding RESIDENT: the UI polls the
        // resident status on the event-loop thread while this runs.
        let worker_attempt = worker.and_then(|worker| {
            worker
                .lock()
                .ok()
                .map(|mut worker| transcribe_via_worker(&mut *worker, wav, spec.lang_arg))
        });
        if let Some(Ok(text)) = worker_attempt {
            return Ok(text);
        }
    }

    // Touch or onload the model so the UI reflects that it is active in RAM
    if loaded_model_index() != Some(index) {
        let _ = onload_model(index);
    } else if let Ok(mut guard) = RESIDENT.lock() {
        if let Some(res) = guard.as_mut() {
            res.last_used = Instant::now();
        }
    }

    let engine = ensure_engine(spec.engine)?;
    let model_file = model_path(index).ok_or("no model downloaded")?;

    let mut args: Vec<std::ffi::OsString> = Vec::new();
    match spec.engine {
        "cactus" => {
            // Cactus needle: --model <file.cact> --audio <wav> [--audio-language en] --threads 4
            args.push("--model".into());
            args.push(model_file.into_os_string());
            args.push("--audio".into());
            args.push(wav.as_os_str().to_owned());
            if let Some(lang) = spec.lang_arg {
                args.push("--audio-language".into());
                args.push(lang.into());
            }
            args.push("--threads".into());
            args.push("4".into());
        }
        "transcribe" => {
            args.push("-m".into());
            args.push(model_file.into_os_string());
            if let Some(lang) = spec.lang_arg {
                args.push("--language".into());
                args.push(lang.into());
            }
            args.push("--threads".into());
            args.push("4".into());
            args.push("--backend".into());
            args.push("cpu".into());
            args.push("--timestamps".into());
            args.push("none".into());
            args.push("-q".into());
            args.push(wav.as_os_str().to_owned());
        }
        _ => {
            // whisper.cpp: -m <model> -f <wav> -l en --no-timestamps --suppress-nst -nth 0.85 -t 4
            args.push("-m".into());
            args.push(model_file.into_os_string());
            args.push("-f".into());
            args.push(wav.as_os_str().to_owned());
            args.push("-l".into());
            args.push(spec.lang_arg.unwrap_or("en").into());
            args.push("--no-timestamps".into());
            args.push("--suppress-nst".into());
            args.push("-nth".into());
            args.push("0.85".into());
            args.push("-t".into());
            args.push("4".into());
        }
    }

    #[cfg(target_os = "android")]
    let mut child = spawn_engine_process(&engine, &args, false)?;

    #[cfg(not(target_os = "android"))]
    let mut child = engine
        .command()
        .args(&args)
        .spawn()
        .map_err(|e| format!("could not run the engine: {e}"))?;

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

    let deadline = Instant::now() + Duration::from_secs(120);
    let status = loop {
        match child.try_wait() {
            Ok(Some(status)) => break Some(status),
            Ok(None) => {
                if Instant::now() > deadline {
                    let _ = child.kill();
                    let _ = child.wait();
                    break None;
                }
                std::thread::sleep(Duration::from_millis(30));
            }
            Err(e) => return Err(format!("the engine could not be waited for: {e}")),
        }
    };
    let stdout = out_handle.join().unwrap_or_default();
    let stderr = err_handle.join().unwrap_or_default();

    let Some(status) = status else {
        return Err("the engine timed out after 120s".into());
    };
    if !status.success() {
        let err = stderr.trim();
        return Err(if err.is_empty() {
            format!("the engine exited with {status}")
        } else {
            format!("the engine failed: {err}")
        });
    }
    if spec.engine == "cactus" {
        for line in stdout.lines().rev() {
            let line = line.trim();
            if line.starts_with('{') && line.contains("\"text\"") {
                if let Some(text) = json_text(line) {
                    return Ok(clean_transcript(&text));
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

#[cfg(target_os = "android")]
fn transcribe_via_worker(
    worker: &mut WorkerHandle,
    wav: &Path,
    lang: Option<&str>,
) -> Result<String, String> {
    let wav_str = wav.to_string_lossy().replace('\\', "\\\\").replace('"', "\\\"");
    let lang_str = lang.unwrap_or("auto");
    writeln!(
        worker.stdin,
        "{{\"action\":\"transcribe\",\"audio_path\":\"{wav_str}\",\"language\":\"{lang_str}\"}}"
    )
    .map_err(|e| e.to_string())?;
    worker.stdin.flush().map_err(|e| e.to_string())?;

    let mut line = String::new();
    worker
        .stdout
        .read_line(&mut line)
        .map_err(|e| e.to_string())?;
    if line.is_empty() {
        return Err("worker closed pipe".into());
    }
    if !line.contains("\"status\":\"ok\"") {
        let err = json_field(&line, "error").unwrap_or_else(|| line.trim().to_string());
        return Err(err);
    }
    let raw = json_text(&line).unwrap_or_default();
    Ok(clean_transcript_output(&raw))
}

/// Clean output from `transcribe-cli`.
pub fn clean_transcript_output(raw: &str) -> String {
    // 1. If explicit `text:` lines exist, extract only those!
    let mut explicit_text: Vec<String> = Vec::new();
    for line in raw.lines() {
        let trimmed = line.trim();
        let low = trimmed.to_lowercase();
        if low.starts_with("text:") {
            let val = trimmed[5..].trim();
            if !val.is_empty() && !val.eq_ignore_ascii_case("(empty)") {
                explicit_text.push(val.to_string());
            }
        }
    }
    let joined = if !explicit_text.is_empty() {
        explicit_text.join(" ")
    } else {
        let mut lines: Vec<String> = Vec::new();
        for line in raw.lines() {
            let line = line.trim();
            if line.is_empty() || line.eq_ignore_ascii_case("(empty)") {
                continue;
            }
            let low = line.to_lowercase();
            if low.starts_with("audio:")
                || low.starts_with("text:")
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
                || low.starts_with("detected-language:")
                || low.starts_with("segments:")
                || low.starts_with("words:")
                || low.starts_with("tokens:")
                || low.starts_with("realtime:")
                || low.starts_with("load:")
                || low.starts_with("mel:")
                || low.starts_with("encode:")
                || low.starts_with("decode:")
                || low.starts_with("note:")
                || low.starts_with("[info]")
                || low.starts_with("[warn]")
                || low.starts_with("[error]")
                || low.starts_with("[debug]")
                || (low.starts_with('[') && low.contains("->"))
                || low.starts_with('{')
            {
                continue;
            }
            lines.push(line.to_string());
        }
        lines.join(" ")
    };

    let cleaned = joined
        .replace("<unk>", "")
        .replace("<pad>", "")
        .replace("<s>", "")
        .replace("</s>", "")
        .replace("<|endoftext|>", "")
        .replace("<lang-en-US>", "")
        .replace("<en-US>", "");
    clean_transcript(&cleaned)
}

/// Pull a string field out of a flat JSON object.
fn json_field(line: &str, field: &str) -> Option<String> {
    let key = format!("\"{field}\"");
    let start = line.find(&key)? + key.len();
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

fn json_text(line: &str) -> Option<String> {
    json_field(line, "text")
}

#[cfg(target_os = "android")]
fn json_number(line: &str, field: &str) -> Option<f64> {
    let key = format!("\"{field}\"");
    let start = line.find(&key)? + key.len();
    let rest = &line[start..];
    let colon = rest.find(':')? + 1;
    let rest = rest[colon..].trim_start();
    let num_str: String = rest
        .chars()
        .take_while(|c| c.is_ascii_digit() || *c == '.' || *c == '-')
        .collect();
    num_str.parse().ok()
}

/// Drop whisper blank-audio markers and bracketed/parenthesised noise lines.
fn clean_transcript(raw: &str) -> String {
    raw.lines()
        .map(|l| l.trim())
        .filter(|l| !l.is_empty() && !l.eq_ignore_ascii_case("(empty)"))
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
        .split_whitespace()
        .collect::<Vec<_>>()
        .join(" ")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn transcript_drops_the_noise_markers() {
        let raw = "\n[BLANK_AUDIO]\n  hello there  \n(inaudible)\nhow are you\n";
        assert_eq!(clean_transcript(raw), "hello there how are you");
    }

    #[test]
    fn transcribe_cli_output_extracts_only_text() {
        let sample = "audio: test.wav\n  samples: 32000\nmodel: m.gguf -> ok\nrun: ok\ntext: Hello world.\ndetected-language: en\nsegments: 1\n  [   0.00 ->    2.00] Hello world.\n  realtime: 15x\n";
        assert_eq!(clean_transcript_output(sample), "Hello world.");
        let empty = "run: ok\ntext: (empty)\n";
        assert_eq!(clean_transcript_output(empty), "");
    }

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

    #[test]
    fn model_catalog_is_real_and_needle_is_default_inbuilt() {
        assert!(!MODELS.is_empty());
        assert_eq!(MODELS[DEFAULT_MODEL].engine, "cactus");
        assert_eq!(MODELS[DEFAULT_MODEL].file, "whistle.cact");
        assert!(MODELS[DEFAULT_MODEL].bundled);

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
            let mut reader = response.into_reader();
            let mut head = [0u8; 4];
            let n = std::io::Read::read(&mut reader, &mut head).unwrap_or(0);
            assert!(n > 0, "{} delivered no data", spec.name);
            let magic = &head[..n];
            let known = magic == b"ggml"
                || magic == b"lmgg"
                || magic == b"GGUF"
                || magic == b"\x84\x2a\xe1\x05";
            assert!(
                known,
                "{} does not start with a model header ({:?})",
                spec.name,
                String::from_utf8_lossy(magic)
            );
        }
    }

    #[test]
    fn the_shipped_engines_know_our_options() {
        let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("prebuilt");
        let cases: &[(&str, &[&str])] = &[
            (
                "whisper-cli-arm64-v8a",
                &[
                    "--model",
                    "--file",
                    "--language",
                    "--no-timestamps",
                    "--suppress-nst",
                    "--no-speech-thold",
                    "--threads",
                ],
            ),
            (
                "needle-arm64-v8a",
                &["--model", "--audio", "--audio-", "language", "--threads"],
            ),
            (
                "transcribe-cli-arm64-v8a",
                &["--model", "--language", "--threads", "--backend", "--timestamps"],
            ),
        ];
        for (file, options) in cases {
            let path = dir.join(file);
            let bytes = std::fs::read(&path)
                .unwrap_or_else(|e| panic!("{} cannot be read: {e}", path.display()));
            for option in *options {
                let needle = option.as_bytes();
                let found = bytes.windows(needle.len()).any(|w| w == needle);
                assert!(
                    found,
                    "{} does not know the option {option} — the transcription would fail",
                    file
                );
            }
        }

        // `--worker` is short enough for the compiler to compare inline, so
        // the literal is not guaranteed to survive in the binary. The worker
        // protocol's own error strings are: if they are present, worker_main()
        // was compiled into the shipped engine and onload/offload can work.
        let worker = std::fs::read(dir.join("transcribe-cli-arm64-v8a"))
            .expect("transcribe-cli cannot be read");
        for marker in ["missing model_path", "unknown action"] {
            assert!(
                worker.windows(marker.len()).any(|w| w == marker.as_bytes()),
                "transcribe-cli-arm64-v8a has no worker mode — onload/offload would fail"
            );
        }
    }

    #[test]
    fn the_shipped_engines_are_android_binaries() {
        let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("prebuilt");
        for file in ["whisper-cli-arm64-v8a", "needle-arm64-v8a", "transcribe-cli-arm64-v8a"] {
            let path = dir.join(file);
            let bytes = std::fs::read(&path)
                .unwrap_or_else(|e| panic!("{} cannot be read: {e}", path.display()));
            assert_eq!(&bytes[0..4], b"\x7fELF", "{} is not an ELF file", file);
            assert_eq!(bytes[4], 2, "{} is not 64-bit", file);
            assert_eq!(bytes[18], 0xB7, "{} is not an ARM64 program", file);
            let text = String::from_utf8_lossy(&bytes);
            assert!(
                text.contains("/system/bin/linker64"),
                "{} does not use the Android linker",
                file
            );
        }
    }
}
