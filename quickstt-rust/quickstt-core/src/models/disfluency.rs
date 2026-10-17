//! Transcript cleanup via the persistent `tools/disfluency/disfluency_worker.py`
//! helper. Four engines, chosen by the dashboard (`disfluency_engine`):
//!
//! * 0 = FDT 66M (`stillerman/fdt-disfluency-distilbert-66m-v5`, int8 ONNX) —
//!   a deletion-only word tagger: strips fillers ("um", "uh"), repeats and
//!   false starts. It can never rephrase or hallucinate, and it cannot rewrite
//!   numbers/dates or resolve substitution repairs. ~15ms/turn.
//! * 1 = S1-mini INT4 (`superwhisper/s1-mini`, the Qwen3-0.6B ASR-transcript
//!   normalizer, via `elbruno/s1-mini-onnx` INT4 + `onnxruntime-genai`) —
//!   removes fillers AND resolves self-corrections to the landed-on value
//!   ("nine am no sorry ten thirty" -> "10:30"), and writes spoken
//!   numbers/dates/times in written form. Apache-2.0 + naming clause: the name
//!   "S1-mini" by "Superwhisper" must be kept wherever it is used. Slower
//!   (~seconds per turn) — kept selectable, no longer the only rewriting op.
//! * 2 = SmolLM2-360M-Instruct Q4_K_M (`bartowski/SmolLM2-360M-Instruct-GGUF`
//!   via llama.cpp, Apache-2.0) — same job family as S1-mini (fillers +
//!   self-corrections + digit normalizing) with a much smaller/faster model.
//! * 3 = Gemma-3-270m-it Q4_K_M (`bartowski/google_gemma-3-270m-it-GGUF` via
//!   llama.cpp) — Google's smallest instruction-tuned Gemma 3; same job
//!   family. NOTE: Gemma terms of use, not Apache — unlike SmolLM2.
//!
//! Engines 2/3 load one GGUF through llama.cpp (CPU, mmap) per turn exactly
//! like the others — warm page cache makes the load a fraction of a second
//! and generation is temperature-0 greedy, so output is repeatable.
//!
//! Cleanup runs on the NON-STREAMING utterance path only, as the final
//! post-processing phase of a turn (after the user releases Ctrl+Space or
//! stops and every STT segment has landed), before the text is buffered,
//! recorded in history and pasted. Streaming deltas (PartialText) are
//! untouched: cleanup needs the whole utterance for context.
//!
//! MEMORY POLICY (by design): the model is loaded, used for exactly one turn,
//! then dropped — `clean_text` sends `unload` immediately after cleaning, so no
//! cleanup model (66MB – 375MB) sits resident between turns. Only the small
//! Python interpreter stays warm, which makes the next turn's load a fraction
//! of a second. Model files live under `<models>/disfluency/<engine>/` and are
//! fetched by the downloader on first use; cleanup passes text through
//! untouched until they arrive.

use anyhow::{Context, Result};
use once_cell::sync::Lazy;
use serde_json::json;
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicBool, Ordering};
use tracing::{info, warn};

/// Cleanup engine selector (matches `settings.disfluency_engine`).
pub const ENGINE_FDT: u32 = 0;
pub const ENGINE_S1MINI: u32 = 1;
/// SmolLM2-360M-Instruct Q4_K_M via llama.cpp (Apache-2.0; bartowski GGUF).
pub const ENGINE_SMOLLM2: u32 = 2;
/// Gemma-3-270m-it Q4_K_M via llama.cpp (Gemma terms; bartowski GGUF).
pub const ENGINE_GEMMA3: u32 = 3;

/// Never flash a terminal window when spawning the helper.
#[cfg(target_os = "windows")]
fn no_window(cmd: &mut Command) {
    use std::os::windows::process::CommandExt;
    cmd.creation_flags(0x08000000); // CREATE_NO_WINDOW
}
#[cfg(not(target_os = "windows"))]
fn no_window(_cmd: &mut Command) {}

/// Model subdirs (under the shared models root) + required files.
pub const FDT_DIR: &str = "disfluency/fdt-disfluency-distilbert-66m-v5";
pub const S1MINI_DIR: &str = "disfluency/s1-mini-int4";
pub const SMOLLM2_DIR: &str = "disfluency/smollm2-360m-instruct-q4km";
pub const GEMMA3_DIR: &str = "disfluency/gemma-3-270m-it-q4km";
const FDT_REQUIRED: &[&str] = &["model_quantized.onnx", "tokenizer.json"];
const S1_REQUIRED: &[&str] = &[
    "model.onnx",
    "model.onnx.data",
    "genai_config.json",
    "tokenizer.json",
];
const SMOLLM2_REQUIRED: &[&str] = &["SmolLM2-360M-Instruct-Q4_K_M.gguf"];
const GEMMA3_REQUIRED: &[&str] = &["google_gemma-3-270m-it-Q4_K_M.gguf"];

/// Download file lists for the downloader (repo path, local name, ~bytes).
pub const FDT_FILES: &[(&str, &str, u64)] = &[
    (
        "onnx/model_quantized.onnx",
        "model_quantized.onnx",
        65_583_507,
    ),
    ("vocab.txt", "vocab.txt", 213_450),
    ("config.json", "config.json", 831),
    ("tokenizer.json", "tokenizer.json", 669_176),
];
/// `elbruno/s1-mini-onnx` `int4/` (ORT-genai layout). Weights NOT bundled.
pub const S1MINI_FILES: &[(&str, &str, u64)] = &[
    ("int4/model.onnx", "model.onnx", 299_040),
    ("int4/model.onnx.data", "model.onnx.data", 393_740_288),
    ("int4/genai_config.json", "genai_config.json", 1_571),
    ("int4/tokenizer.json", "tokenizer.json", 11_422_650),
    ("int4/tokenizer_config.json", "tokenizer_config.json", 723),
    ("int4/chat_template.jinja", "chat_template.jinja", 4_256),
];

/// `bartowski/SmolLM2-360M-Instruct-GGUF` Q4_K_M — one GGUF, 270_590_880 B.
pub const SMOLLM2_FILES: &[(&str, &str, u64)] = &[(
    "SmolLM2-360M-Instruct-Q4_K_M.gguf",
    "SmolLM2-360M-Instruct-Q4_K_M.gguf",
    270_590_880,
)];
/// `bartowski/google_gemma-3-270m-it-GGUF` Q4_K_M — one GGUF, 253_115_168 B.
pub const GEMMA3_FILES: &[(&str, &str, u64)] = &[(
    "google_gemma-3-270m-it-Q4_K_M.gguf",
    "google_gemma-3-270m-it-Q4_K_M.gguf",
    253_115_168,
)];

/// HuggingFace repo each engine's files come from.
pub fn repo_for(engine: u32) -> &'static str {
    match engine {
        ENGINE_S1MINI => "elbruno/s1-mini-onnx",
        ENGINE_SMOLLM2 => "bartowski/SmolLM2-360M-Instruct-GGUF",
        ENGINE_GEMMA3 => "bartowski/google_gemma-3-270m-it-GGUF",
        _ => "stillerman/fdt-disfluency-distilbert-66m-v5",
    }
}

/// Worker-side backend name for an engine id.
pub fn backend_name(engine: u32) -> &'static str {
    match engine {
        ENGINE_S1MINI => "s1mini",
        ENGINE_SMOLLM2 => "smollm2",
        ENGINE_GEMMA3 => "gemma3",
        _ => "fdt",
    }
}

/// Human label for the dashboard combo box.
pub fn engine_label(engine: u32) -> &'static str {
    match engine {
        ENGINE_S1MINI => {
            "S1-mini INT4 (0.6B) - self-corrections + numbers (375MB, slow)"
        }
        ENGINE_SMOLLM2 => {
            "SmolLM2 360M Q4 - self-corrections + numbers (271MB, fast)"
        }
        ENGINE_GEMMA3 => {
            "Gemma 3 270M Q4 - self-corrections + numbers (253MB, fast)"
        }
        _ => "FDT 66M tagger - fillers + repeats only (66MB)",
    }
}

/// Approximate download size in MB (shown in the dashboard).
pub fn engine_size_mb(engine: u32) -> u32 {
    match engine {
        ENGINE_S1MINI => 375,
        ENGINE_SMOLLM2 => 271,
        ENGINE_GEMMA3 => 253,
        _ => 66,
    }
}

pub fn model_dir(engine: u32) -> PathBuf {
    crate::models::catalog::models_root().join(match engine {
        ENGINE_S1MINI => S1MINI_DIR,
        ENGINE_SMOLLM2 => SMOLLM2_DIR,
        ENGINE_GEMMA3 => GEMMA3_DIR,
        _ => FDT_DIR,
    })
}

/// True when the worker script + every file this engine needs are on disk.
pub fn files_present(engine: u32) -> bool {
    let worker_ok = worker_candidates().iter().any(|p| p.is_file());
    let required = match engine {
        ENGINE_S1MINI => S1_REQUIRED,
        ENGINE_SMOLLM2 => SMOLLM2_REQUIRED,
        ENGINE_GEMMA3 => GEMMA3_REQUIRED,
        _ => FDT_REQUIRED,
    };
    worker_ok && required.iter().all(|f| model_dir(engine).join(f).is_file())
}

fn worker_candidates() -> Vec<PathBuf> {
    let mut v = Vec::new();
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            v.push(dir.join("tools/disfluency/disfluency_worker.py"));
        }
    }
    #[cfg(target_os = "linux")]
    v.push(PathBuf::from(
        "/usr/lib/quickstt/tools/disfluency/disfluency_worker.py",
    ));
    // Dev-run fallback: repo checkout layout.
    if let Ok(exe) = std::env::current_exe() {
        let mut d = exe.parent().map(|p| p.to_path_buf()).unwrap_or_default();
        for _ in 0..4 {
            let cand =
                d.join("quickstt-rust").join("tools/disfluency/disfluency_worker.py");
            v.push(cand);
            if !d.pop() {
                break;
            }
        }
    }
    v
}

fn find_python() -> Option<PathBuf> {
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

struct DisfluencySession {
    child: Child,
    stdin: ChildStdin,
    stdout: BufReader<ChildStdout>,
    loaded: bool,
    /// (worker backend name, model dir) currently resident, if any.
    loaded_for: Option<(String, String)>,
    worker_path: PathBuf,
    python: PathBuf,
}

static SESSION: Lazy<Mutex<Option<DisfluencySession>>> =
    Lazy::new(|| Mutex::new(None));
static WARNED_UNAVAILABLE: AtomicBool = AtomicBool::new(false);

fn spawn_session(python: &Path, worker: &Path) -> Result<DisfluencySession> {
    let workdir = worker.parent().unwrap_or(Path::new("."));
    info!("Starting disfluency worker: {:?} {:?}", python, worker);
    let err_log = std::env::temp_dir().join("quickstt-disfluency-stderr.log");
    let err_log = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&err_log)
        .map(Stdio::from)
        .unwrap_or_else(|_| Stdio::null());
    let mut cmd = Command::new(python);
    no_window(&mut cmd);
    let mut child = cmd
        .arg(worker)
        .current_dir(workdir)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(err_log)
        .spawn()
        .context("Failed to start disfluency_worker.py")?;
    let stdin = child.stdin.take().context("worker stdin unavailable")?;
    let stdout = child.stdout.take().context("worker stdout unavailable")?;
    Ok(DisfluencySession {
        child,
        stdin,
        stdout: BufReader::new(stdout),
        loaded: false,
        loaded_for: None,
        worker_path: worker.to_path_buf(),
        python: python.to_path_buf(),
    })
}

fn request(session: &mut DisfluencySession, req: serde_json::Value) -> Result<serde_json::Value> {
    if let Ok(Some(status)) = session.child.try_wait() {
        anyhow::bail!("disfluency worker exited early: {}", status);
    }
    let line = serde_json::to_string(&req)?;
    session.stdin.write_all(line.as_bytes())?;
    session.stdin.write_all(b"\n")?;
    session.stdin.flush()?;
    let mut response = String::new();
    if session.stdout.read_line(&mut response)? == 0 {
        anyhow::bail!("disfluency worker closed stdout");
    }
    serde_json::from_str(response.trim()).context("Invalid worker JSON response")
}

fn ensure_text(resp: serde_json::Value, action: &str) -> Result<Option<String>> {
    if resp.get("status").and_then(|v| v.as_str()).unwrap_or("error") == "ok" {
        return Ok(resp
            .get("text")
            .and_then(|v| v.as_str())
            .map(|s| s.to_string()));
    }
    let err = resp
        .get("error")
        .and_then(|v| v.as_str())
        .unwrap_or("unknown worker error");
    anyhow::bail!("disfluency {} failed: {}", action, err)
}

/// Clean one finished turn's transcript with the selected engine. NEVER fails
/// the turn: any unavailability (files missing, no Python, worker error)
/// returns the original text, with a one-time warn so the log doesn't spam.
///
/// The model is unloaded again before returning — cleanup holds its weights
/// only for the few hundred milliseconds it actually needs.
pub fn clean_text_with(engine: u32, text: &str) -> String {
    if text.trim().is_empty() {
        return text.to_string();
    }
    if !files_present(engine) {
        if !WARNED_UNAVAILABLE.swap(true, Ordering::Relaxed) {
            warn!("Transcript cleanup skipped ({} files not ready — background download in progress?)", backend_name(engine));
        }
        return text.to_string();
    }
    let worker = match worker_candidates().into_iter().find(|p| p.is_file()) {
        Some(p) => p,
        None => {
            warn!("disfluency_worker.py not found (expected next to the exe under tools/disfluency)");
            return text.to_string();
        }
    };
    let python = match find_python() {
        Some(p) => p,
        None => {
            warn!("No Python found for transcript cleanup (need onnxruntime + tokenizers / onnxruntime-genai)");
            return text.to_string();
        }
    };
    let mut guard = match SESSION.lock() {
        Ok(g) => g,
        Err(_) => return text.to_string(),
    };
    let dead = guard
        .as_mut()
        .map(|s| matches!(s.child.try_wait(), Ok(Some(_))))
        .unwrap_or(true);
    let changed = guard
        .as_ref()
        .map(|s| s.worker_path != worker || s.python != python)
        .unwrap_or(true);
    if dead || changed {
        match spawn_session(&python, &worker) {
            Ok(s) => *guard = Some(s),
            Err(e) => {
                warn!("Cleanup worker spawn failed: {e:#}");
                return text.to_string();
            }
        }
    }
    let session = match guard.as_mut() {
        Some(s) => s,
        None => return text.to_string(),
    };
    let backend = backend_name(engine);
    let dir = model_dir(engine).to_string_lossy().to_string();
    if !session.loaded
        || session.loaded_for.as_ref() != Some(&(backend.to_string(), dir.clone()))
    {
        match request(
            session,
            json!({ "action": "load", "backend": backend, "model_dir": dir }),
        )
        .and_then(|r| ensure_text(r, "load").map(|_| ()))
        {
            Ok(()) => {
                session.loaded = true;
                session.loaded_for = Some((backend.to_string(), dir.clone()));
            }
            Err(e) => {
                warn!("Cleanup model load failed ({backend}): {e:#}");
                session.loaded = false;
                session.loaded_for = None;
                return text.to_string();
            }
        }
    }
    let started = std::time::Instant::now();
    let outcome = request(
        session,
        json!({ "action": "clean", "backend": backend, "model_dir": dir, "text": text }),
    )
    .and_then(|r| ensure_text(r, "clean"));
    // Instant offload: the model exists only for this one cleanup, so drop it
    // before the paste happens — no cleanup weights linger between turns.
    if session.loaded {
        match request(session, json!({ "action": "unload" })) {
            Ok(_) => {
                session.loaded = false;
                session.loaded_for = None;
            }
            Err(e) => warn!("Cleanup unload failed: {e:#}"),
        }
    }
    match outcome {
        Ok(Some(cleaned)) if !cleaned.trim().is_empty() => {
            if cleaned.trim() != text.trim() {
                info!(
                    "Cleanup [{}] took {}ms: {:?} -> {:?}",
                    backend,
                    started.elapsed().as_millis(),
                    text.trim(),
                    cleaned.trim()
                );
            }
            cleaned
        }
        Ok(_) => text.to_string(),
        Err(e) => {
            warn!("Cleanup clean failed ({backend}): {e:#}");
            text.to_string()
        }
    }
}

/// Default-engine shorthand (FDT 66M) for callers without a setting handy.
pub fn clean_text(text: &str) -> String {
    clean_text_with(ENGINE_FDT, text)
}

/// Release any loaded cleanup model (belt-and-braces; `clean_text_with`
/// already unloads after every turn).
pub fn disfluency_unload() {
    let mut guard = match SESSION.lock() {
        Ok(g) => g,
        Err(_) => return,
    };
    let session = match guard.as_mut() {
        Some(s) => s,
        None => return,
    };
    if !session.loaded {
        return;
    }
    if matches!(session.child.try_wait(), Ok(Some(_))) {
        *guard = None;
        return;
    }
    match request(session, json!({ "action": "unload" })) {
        Ok(_) => {
            session.loaded = false;
            session.loaded_for = None;
            info!("Cleanup model unloaded");
        }
        Err(e) => warn!("Cleanup unload failed: {e:#}"),
    }
}

type SharedState = Arc<Mutex<crate::orchestration::AppState>>;
static DL_STARTED: [AtomicBool; 4] = [
    AtomicBool::new(false),
    AtomicBool::new(false),
    AtomicBool::new(false),
    AtomicBool::new(false),
];

/// Fetch an engine's weights in the background (once per engine per process).
/// Cleanup passes text through until the files land; no turn ever blocks.
pub fn ensure_download(state: SharedState, engine: u32) {
    let idx = match engine {
        ENGINE_S1MINI => 1,
        ENGINE_SMOLLM2 => 2,
        ENGINE_GEMMA3 => 3,
        _ => 0,
    };
    if files_present(engine) || DL_STARTED[idx].swap(true, Ordering::Relaxed) {
        return;
    }
    std::thread::Builder::new()
        .name("dl-cleanup".to_string())
        .spawn(move || {
            let label = match engine {
                ENGINE_S1MINI => "S1-mini normalizer".to_string(),
                ENGINE_SMOLLM2 => "SmolLM2 normalizer".to_string(),
                ENGINE_GEMMA3 => "Gemma 3 normalizer".to_string(),
                _ => "Disfluency tagger".to_string(),
            };
            if let Ok(mut s) = state.lock() {
                s.download_name = label.clone();
            }
            let dir = model_dir(engine);
            let base = format!(
                "https://huggingface.co/{}/resolve/main",
                repo_for(engine)
            );
            let files: &[(&str, &str, u64)] = match engine {
                ENGINE_S1MINI => S1MINI_FILES,
                ENGINE_SMOLLM2 => SMOLLM2_FILES,
                ENGINE_GEMMA3 => GEMMA3_FILES,
                _ => FDT_FILES,
            };
            for (i, (repo, name, _bytes)) in files.iter().enumerate() {
                let url = format!("{base}/{repo}");
                let dest = dir.join(name);
                if dest.is_file() {
                    continue;
                }
                let dl = crate::models::downloader::FileDl {
                    label: name,
                    index: i,
                    count: files.len(),
                };
                if let Err(e) =
                    crate::models::downloader::download_file(&url, &dest, &state, &dl)
                {
                    warn!("Cleanup download failed for {name}: {e:#}");
                    let _ = std::fs::remove_file(&dest);
                    if let Ok(mut s) = state.lock() {
                        s.is_downloading = false;
                        s.download_speed_text = String::new();
                    }
                    return;
                }
            }
            info!("{label} downloaded (cleanup now active)");
            if let Ok(mut s) = state.lock() {
                s.is_downloading = false;
                s.download_speed_text = String::new();
            }
        })
        .ok();
}
