//! Disfluency cleanup (stillerman/fdt-disfluency-distilbert-66m-v5, int8 ONNX)
//! via the persistent `tools/disfluency/disfluency_worker.py` helper.
//!
//! The tagger is deletion-only (KEEP / DELETE / KEEP_STRIP_COMMA /
//! KEEP_CAPITALIZE per whitespace word): it strips fillers ("um", "uh"),
//! repeats and false starts from a transcript, and can never rephrase or
//! hallucinate. It runs on the NON-STREAMING utterance path only — after a
//! turn's text is final, before it reaches the transcript buffer / history /
//! paste — gated on the `clean_disfluencies` setting (dashboard toggle).
//! Streaming deltas (PartialText) are untouched: cleanup needs the whole
//! utterance for context.
//!
//! Runtime: Python `onnxruntime` (real ORT — the in-process ort is
//! tract-backed and cannot run the int8 QDQ graph) + `tokenizers`, CPU-only,
//! ~10-50ms per turn. The worker stays resident once loaded; `unload`
//! releases the 66MB session on idle offload. Model files (~66MB) live under
//! `<models>/disfluency/fdt-disfluency-distilbert-66m-v5/` and are fetched by
//! the downloader on first use — cleanup passes text through untouched until
//! they arrive.

use anyhow::{Context, Result};
use once_cell::sync::Lazy;
use serde_json::json;
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, ChildStdout, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicBool, Ordering};
use tracing::{info, warn};

/// Never flash a terminal window when spawning the helper.
#[cfg(target_os = "windows")]
fn no_window(cmd: &mut Command) {
    use std::os::windows::process::CommandExt;
    cmd.creation_flags(0x08000000); // CREATE_NO_WINDOW
}
#[cfg(not(target_os = "windows"))]
fn no_window(_cmd: &mut Command) {}

/// Model subdir (under the shared models root) + required files.
pub const DISFLUENCY_DIR: &str = "disfluency/fdt-disfluency-distilbert-66m-v5";
const REQUIRED_FILES: &[&str] = &["model_quantized.onnx", "tokenizer.json"];

/// Download file list for the downloader (repo path, local name, ~bytes).
pub const DISFLUENCY_FILES: &[(&str, &str, u64)] = &[
    (
        "onnx/model_quantized.onnx",
        "model_quantized.onnx",
        65_583_507,
    ),
    ("vocab.txt", "vocab.txt", 213_450),
    ("config.json", "config.json", 831),
    ("tokenizer.json", "tokenizer.json", 669_176),
];

pub fn model_dir() -> PathBuf {
    crate::models::catalog::models_root().join(DISFLUENCY_DIR)
}

/// True when the worker can run (script + ONNX + tokenizer present).
pub fn files_present() -> bool {
    let worker_ok = worker_candidates().iter().any(|p| p.is_file());
    worker_ok && REQUIRED_FILES.iter().all(|f| model_dir().join(f).is_file())
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

/// Clean one utterance transcript. NEVER fails the turn: any unavailability
/// (files missing, no Python, worker error) returns the original text, with
/// a one-time warn so the log doesn't spam per turn.
pub fn clean_text(text: &str) -> String {
    if text.trim().is_empty() {
        return text.to_string();
    }
    if !files_present() {
        if !WARNED_UNAVAILABLE.swap(true, Ordering::Relaxed) {
            warn!("Disfluency cleanup skipped (model files not ready — background download in progress?)");
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
            warn!("No Python found for disfluency cleanup (need onnxruntime + tokenizers)");
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
                warn!("Disfluency worker spawn failed: {e:#}");
                return text.to_string();
            }
        }
    }
    let session = match guard.as_mut() {
        Some(s) => s,
        None => return text.to_string(),
    };
    if !session.loaded {
        let dir = model_dir().to_string_lossy().to_string();
        match request(session, json!({ "action": "load", "model_dir": dir }))
            .and_then(|r| ensure_text(r, "load").map(|_| ()))
        {
            Ok(()) => session.loaded = true,
            Err(e) => {
                warn!("Disfluency load failed: {e:#}");
                return text.to_string();
            }
        }
    }
    match request(session, json!({ "action": "clean", "text": text }))
        .and_then(|r| ensure_text(r, "clean"))
    {
        Ok(Some(cleaned)) if !cleaned.trim().is_empty() => {
            if cleaned.trim() != text.trim() {
                info!("Disfluency cleanup: {:?} -> {:?}", text.trim(), cleaned.trim());
            }
            cleaned
        }
        Ok(_) => text.to_string(),
        Err(e) => {
            warn!("Disfluency clean failed: {e:#}");
            text.to_string()
        }
    }
}

/// Idle offload: release the tagger session (the process stays for reload).
/// Safe when idle; next clean reloads on demand.
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
            info!("Disfluency tagger unloaded (idle offload)");
        }
        Err(e) => warn!("Disfluency unload failed: {e:#}"),
    }
}

type SharedState = Arc<Mutex<crate::orchestration::AppState>>;
static DL_STARTED: AtomicBool = AtomicBool::new(false);

/// Fetch the ~66MB tagger in the background (once per process). Cleanup
/// passes text through until the files land; no turn ever blocks on this.
pub fn ensure_download(state: SharedState) {
    if files_present() || DL_STARTED.swap(true, Ordering::Relaxed) {
        return;
    }
    std::thread::Builder::new()
        .name("dl-disfluency".to_string())
        .spawn(move || {
            if let Ok(mut s) = state.lock() {
                s.download_name = "Disfluency cleaner".to_string();
            }
            let dir = model_dir();
            let base = "https://huggingface.co/stillerman/fdt-disfluency-distilbert-66m-v5/resolve/main";
            let files = [
                ("onnx/model_quantized.onnx", "model_quantized.onnx"),
                ("vocab.txt", "vocab.txt"),
                ("config.json", "config.json"),
                ("tokenizer.json", "tokenizer.json"),
            ];
            for (i, (repo, name)) in files.iter().enumerate() {
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
                    warn!("Disfluency download failed for {name}: {e:#}");
                    let _ = std::fs::remove_file(&dest);
                    return;
                }
            }
            info!("Disfluency tagger downloaded (cleanup now active)");
            if let Ok(mut s) = state.lock() {
                s.is_downloading = false;
                s.download_speed_text = String::new();
            }
        })
        .ok();
}
