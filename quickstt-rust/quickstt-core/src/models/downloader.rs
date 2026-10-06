//! Background model downloader.
//!
//! Downloads and installs STT models + runtime engines into
//! `~/.local/share/QuickSTT/models/` (XDG). Progress is published through
//! `AppState::status_message` so the GUI shows it in the pill/dashboard.
//!
//! URLs verified 2026-08:
//! - Vosk small en-us 0.15: https://alphacephei.com/vosk/models/vosk-model-small-en-us-0.15.zip (~40MB)
//! - libvosk.so (vosk-api v0.3.45 linux x86_64): https://github.com/alphacep/vosk-api/releases/download/v0.3.45/vosk-linux-x86_64-0.3.45.zip
//! - Parakeet TDT 0.6B v3 INT8 ONNX (transcribe-rs/Handy layout):
//!   https://huggingface.co/KasuleTrevor/parakeet-tdt-0.6b-v3-onnx-int8/resolve/main/{encoder-model.int8.onnx,encoder-model.int8.onnx.data,decoder_joint-model.int8.onnx,decoder_joint-model.int8.onnx.data,nemo128.onnx,vocab.txt}

use crate::models::catalog::{self, EngineFamily, ModelDescriptor};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use tracing::{info, warn};

type SharedState = Arc<Mutex<crate::orchestration::AppState>>;

/// Entry point used by the orchestrator: spawn a background thread that
/// downloads `desc` and refreshes installed flags when done.
pub fn spawn_download(desc: ModelDescriptor, state: SharedState) {
    {
        // Reset byte counters up front so the dashboard shows a live
        // "0.0 / N MB • 0 B/s" row the moment a download starts.
        if let Ok(mut s) = state.lock() {
            s.download_name = desc.name.clone();
            s.download_file_label = "Starting…".into();
            s.download_received_bytes = 0;
            s.download_total_bytes = 0;
            s.download_speed_bps = 0.0;
            s.download_speed_text = "…".into();
            s.download_bytes_text = String::new();
        }
    }
    std::thread::Builder::new()
        .name(format!("dl-{}", desc.engine_family))
        .spawn(move || {
            let name = desc.name.clone();
            set_progress(&state, 0, format!("Downloading {}…", name));
            match install_model(&desc, &state) {
                Ok(()) => {
                    info!("Model installed: {}", name);
                    set_progress(&state, 100, format!("Installed: {}", name));
                    if let Ok(mut s) = state.lock() {
                        s.is_downloading = false;
                        s.download_speed_text = String::new();
                    }
                }
                Err(e) => {
                    warn!("Download failed for {}: {}", name, e);
                    set_progress(&state, 0, format!("Download failed: {}", e));
                    if let Ok(mut s) = state.lock() {
                        s.is_downloading = false;
                        s.download_speed_text = String::new();
                    }
                }
            }
            refresh_installed_flags(&state);
        })
        .expect("spawn download thread");
}

fn set_status(state: &SharedState, msg: String) {
    if let Ok(mut s) = state.lock() {
        s.status_message = msg.clone();
        s.download_status = msg;
    }
}

fn set_progress(state: &SharedState, pct: u8, msg: String) {
    if let Ok(mut s) = state.lock() {
        s.status_message = msg.clone();
        s.download_status = msg;
        s.download_progress = pct;
        s.is_downloading = pct < 100;
    }
}

/// Byte-accurate download progress with live throughput. Called throttled
/// from `download_file` (≈5Hz + on every percent step + on completion).
fn set_dl_progress(
    state: &SharedState,
    overall_pct: u8,
    msg: String,
    received: u64,
    total: u64,
    bps: f64,
    file_label: &str,
) {
    if let Ok(mut s) = state.lock() {
        s.status_message = msg.clone();
        s.download_status = msg;
        s.download_progress = overall_pct.min(100);
        s.is_downloading = overall_pct < 100;
        s.download_file_label = file_label.to_string();
        s.download_received_bytes = received;
        s.download_total_bytes = total;
        s.download_speed_bps = bps.max(0.0);
        s.download_speed_text = fmt_speed(bps);
        s.download_bytes_text = if total > 0 {
            format!("{} / {}", fmt_mb(received), fmt_mb(total))
        } else {
            fmt_mb(received)
        };
    }
}

/// Non-network stage of an install (extracting, …): keeps byte counters.
fn set_dl_stage(state: &SharedState, overall_pct: u8, msg: String, file_label: &str) {
    if let Ok(mut s) = state.lock() {
        s.status_message = msg.clone();
        s.download_status = msg;
        s.download_progress = overall_pct.min(100);
        s.is_downloading = overall_pct < 100;
        s.download_file_label = file_label.to_string();
        s.download_speed_text = String::new();
    }
}

pub fn fmt_mb(bytes: u64) -> String {
    const GB: f64 = 1024.0 * 1024.0 * 1024.0;
    const MB: f64 = 1024.0 * 1024.0;
    let b = bytes as f64;
    if b >= GB {
        format!("{:.1} GB", b / GB)
    } else {
        format!("{:.1} MB", b / MB)
    }
}

pub fn fmt_speed(bps: f64) -> String {
    const MB: f64 = 1024.0 * 1024.0;
    const KB: f64 = 1024.0;
    if bps >= MB {
        format!("{:.1} MB/s", bps / MB)
    } else if bps >= KB {
        format!("{:.0} KB/s", bps / KB)
    } else if bps > 0.0 {
        format!("{:.0} B/s", bps)
    } else {
        "…".to_string()
    }
}

fn refresh_installed_flags(state: &SharedState) {
    if let Ok(mut s) = state.lock() {
        let all = catalog::all_descriptors();
        for entry in s.model_entries.iter_mut() {
            if let Some(desc) = all.iter().find(|d| d.name == entry.name) {
                entry.installed = catalog::is_model_installed(desc);
            }
        }
    }
}

fn install_model(desc: &ModelDescriptor, state: &SharedState) -> anyhow::Result<()> {
    match desc.engine_family {
        EngineFamily::WhisperCpp => install_whisper(desc, state),
        EngineFamily::SherpaOnnx => install_moonshine(desc, state),
        EngineFamily::Vosk => install_vosk(desc, state),
        EngineFamily::ParakeetRust => install_parakeet(desc, state),
        EngineFamily::Nemotron => install_nemotron(desc, state),
        EngineFamily::Canary => install_canary(desc, state),
        EngineFamily::Photon => install_photon(desc, state),
        _ => anyhow::bail!("No download source registered for {}", desc.name),
    }
}

// ── HTTP helpers ──

fn http_get(url: &str) -> anyhow::Result<ureq::Response> {
    Ok(ureq::get(url)
        .timeout(std::time::Duration::from_secs(600))
        .call()?)
}

/// Which file of a (possibly multi-file) model install is downloading.
/// `index`/`count` drive the accurate overall percent across files.
pub struct FileDl<'a> {
    pub label: &'a str,
    pub index: usize,
    pub count: usize,
}

/// Stream a URL to a file with byte-accurate progress + live MB/s.
/// State writes are throttled (≈5Hz, every percent step, and completion).
fn download_file(url: &str, dest: &Path, state: &SharedState, file: &FileDl) -> anyhow::Result<()> {
    use std::time::{Duration, Instant};
    if let Some(parent) = dest.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let model = state
        .lock()
        .map(|s| s.download_name.clone())
        .unwrap_or_default();
    let resp = http_get(url)?;
    let total = resp
        .header("Content-Length")
        .and_then(|v| v.parse::<u64>().ok())
        .unwrap_or(0);
    // Pre-seed totals so the dashboard shows "0.0 / 473.0 MB" immediately.
    set_dl_progress(
        state,
        ((file.index as f64 / file.count.max(1) as f64) * 100.0) as u8,
        format!("Downloading {} — {}…", model, file.label),
        0,
        total,
        0.0,
        file.label,
    );
    let mut reader = resp.into_reader();
    let mut out = std::fs::File::create(dest)?;
    let mut buf = [0u8; 64 * 1024];
    let mut received: u64 = 0;
    let t0 = Instant::now();
    let mut last_emit = Instant::now() - Duration::from_secs(10);
    let mut last_bytes = 0u64;
    let mut last_pct = 255u8;
    let mut ema_bps = 0.0f64;
    loop {
        let n = reader.read(&mut buf)?;
        if n == 0 {
            break;
        }
        out.write_all(&buf[..n])?;
        received += n as u64;
        let file_frac = if total > 0 {
            (received as f64 / total as f64).min(1.0)
        } else {
            0.0
        };
        let overall =
            (((file.index as f64 + file_frac) / file.count.max(1) as f64) * 100.0) as u8;
        let now = Instant::now();
        let done = total > 0 && received >= total;
        if overall != last_pct || now.duration_since(last_emit) >= Duration::from_millis(200) || done {
            let dt = now.duration_since(last_emit).as_secs_f64().max(1e-3);
            let inst = (received - last_bytes) as f64 / dt;
            ema_bps = if ema_bps <= 0.0 {
                inst
            } else {
                0.35 * inst + 0.65 * ema_bps
            };
            // Blend in the whole-transfer average so the number stays honest
            // on bursty connections.
            let avg = received as f64 / t0.elapsed().as_secs_f64().max(1e-3);
            let bps = (0.7 * ema_bps + 0.3 * avg).max(0.0);
            let msg = if total > 0 {
                format!(
                    "Downloading {} — {} • {} / {} • {}",
                    model,
                    file.label,
                    fmt_mb(received),
                    fmt_mb(total),
                    fmt_speed(bps)
                )
            } else {
                format!(
                    "Downloading {} — {} • {} • {}",
                    model,
                    file.label,
                    fmt_mb(received),
                    fmt_speed(bps)
                )
            };
            set_dl_progress(state, overall.min(100), msg, received, total, bps, file.label);
            last_emit = now;
            last_bytes = received;
            last_pct = overall;
        }
    }
    // Final exact 100% of this file's share (servers sometimes under-report
    // Content-Length by a few bytes).
    let overall = (((file.index as f64 + 1.0) / file.count.max(1) as f64) * 100.0) as u8;
    let avg = received as f64 / t0.elapsed().as_secs_f64().max(1e-3);
    set_dl_progress(
        state,
        overall.min(100),
        format!(
            "Downloading {} — {} • {} • {}",
            model,
            file.label,
            fmt_mb(received),
            fmt_speed(avg)
        ),
        received,
        total.max(received),
        avg,
        file.label,
    );
    Ok(())
}

fn extract_zip(zip_path: &Path, out_dir: &Path, strip_first_component: bool) -> anyhow::Result<()> {
    let file = std::fs::File::open(zip_path)?;
    let mut archive = zip::ZipArchive::new(std::io::BufReader::new(file))?;
    std::fs::create_dir_all(out_dir)?;
    for i in 0..archive.len() {
        let mut entry = archive.by_index(i)?;
        let raw_name = entry.name().to_string();
        if entry.is_dir() {
            continue;
        }
        let rel: PathBuf = {
            let p = PathBuf::from(raw_name.replace('\\', "/"));
            if strip_first_component {
                p.components().skip(1).collect()
            } else {
                p
            }
        };
        if rel.as_os_str().is_empty() {
            continue;
        }
        let dest = out_dir.join(rel);
        if let Some(parent) = dest.parent() {
            std::fs::create_dir_all(parent)?;
        }
        let mut out = std::fs::File::create(&dest)?;
        std::io::copy(&mut entry, &mut out)?;
    }
    Ok(())
}

fn tmp_zip(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join("quickstt");
    let _ = std::fs::create_dir_all(&dir);
    dir.join(name)
}

// ── Per-model installers ──

fn install_vosk(desc: &ModelDescriptor, state: &SharedState) -> anyhow::Result<()> {
    let models_root = catalog::models_root();
    let model_dir = models_root.join(&desc.model_dir);

    let (url, zip_name) = match desc.model_dir.as_str() {
        "vosk/en_us_0.22_lgraph" => (
            "https://alphacephei.com/vosk/models/vosk-model-en-us-0.22-lgraph.zip",
            "vosk-model-en-us-0.22-lgraph.zip",
        ),
        "vosk/small_en_in_0.4" => (
            "https://alphacephei.com/vosk/models/vosk-model-small-en-in-0.4.zip",
            "vosk-model-small-en-in-0.4.zip",
        ),
        "vosk/small_de_0.15" => (
            "https://alphacephei.com/vosk/models/vosk-model-small-de-0.15.zip",
            "vosk-model-small-de-0.15.zip",
        ),
        _ => (
            "https://alphacephei.com/vosk/models/vosk-model-small-en-us-0.15.zip",
            "vosk-model-small-en-us-0.15.zip",
        ),
    };

    // 1. Acoustic/language model
    let zpath = tmp_zip(zip_name);
    download_file(
        url,
        &zpath,
        state,
        &FileDl {
            label: zip_name,
            index: 0,
            count: 2,
        },
    )?;
    set_dl_stage(state, 88, format!("Extracting {}…", desc.name), "Extracting…");
    let _ = std::fs::remove_dir_all(&model_dir);
    extract_zip(&zpath, &model_dir, true)?;
    let _ = std::fs::remove_file(&zpath);

    // 2. Runtime engine: platform-specific Vosk shared library + its MinGW
    // deps on Windows (libvosk.dll alone fails to load without libgcc /
    // libstdc++ / libwinpthread alongside it — the exact "no output at all"
    // failure seen on Windows).
    #[cfg(target_os = "windows")]
    {
        set_status(state, "Downloading libvosk runtime (Windows)…".into());
        let rtpath = tmp_zip("vosk-win64-0.3.42.zip");
        // Only re-download when the DLL set is incomplete.
        let rt_dir = models_root.join("runtimes/vosk");
        let need_rt = !rt_dir.join("libvosk.dll").exists()
            || !rt_dir.join("libgcc_s_seh-1.dll").exists()
            || !rt_dir.join("libstdc++-6.dll").exists()
            || !rt_dir.join("libwinpthread-1.dll").exists();
        if need_rt {
            download_file(
                "https://github.com/alphacep/vosk-api/releases/download/v0.3.42/vosk-win64-0.3.42.zip",
                &rtpath,
                state,
                &FileDl {
                    label: "vosk-win64-0.3.42.zip",
                    index: 1,
                    count: 2,
                },
            )?;
            set_dl_stage(state, 96, "Extracting libvosk.dll…".into(), "Extracting…");
            std::fs::create_dir_all(&rt_dir)?;
            extract_zip(&rtpath, &rt_dir, false)?;
            let _ = std::fs::remove_file(&rtpath);
            // The zip nests files under vosk-win64-0.3.42/ — flatten the DLLs up.
            for dll in [
                "libvosk.dll",
                "libgcc_s_seh-1.dll",
                "libstdc++-6.dll",
                "libwinpthread-1.dll",
            ] {
                if !rt_dir.join(dll).exists() {
                    if let Some(found) = find_file(&rt_dir, dll) {
                        let _ = std::fs::rename(&found, rt_dir.join(dll));
                    }
                }
            }
        }
        // Also seed from the repo-bundled copy when present (offline installs).
        seed_vosk_dlls_from_repo(&rt_dir);
    }
    #[cfg(not(target_os = "windows"))]
    {
        set_status(state, "Downloading libvosk runtime…".into());
        let rtpath = tmp_zip("vosk-linux-x86_64-0.3.45.zip");
        download_file(
            "https://github.com/alphacep/vosk-api/releases/download/v0.3.45/vosk-linux-x86_64-0.3.45.zip",
            &rtpath,
            state,
            &FileDl {
                label: "vosk-linux-x86_64-0.3.45.zip",
                index: 1,
                count: 2,
            },
        )?;
        let rt_dir = models_root.join("runtimes/vosk");
        set_dl_stage(state, 96, "Extracting libvosk.so…".into(), "Extracting…");
        extract_zip(&rtpath, &rt_dir, true)?;
        let _ = std::fs::remove_file(&rtpath);
        // The archive contains libvosk.so at its root; ensure expected location.
        if !rt_dir.join("libvosk.so").exists() {
            if let Some(found) = find_file(&rt_dir, "libvosk.so") {
                std::fs::rename(&found, rt_dir.join("libvosk.so"))?;
            }
        }
        #[cfg(unix)]
        {
            let so = rt_dir.join("libvosk.so");
            use std::os::unix::fs::PermissionsExt;
            if let Ok(meta) = std::fs::metadata(&so) {
                let mut perm = meta.permissions();
                perm.set_mode(perm.mode() | 0o755);
                let _ = std::fs::set_permissions(&so, perm);
            }
        }
    }
    Ok(())
}

/// Copy the repo-bundled MinGW DLLs next to an existing libvosk.dll when the
/// downloader ran offline (the DLLs ship in vosk_api/vosk-win64-0.3.42/).
#[cfg(target_os = "windows")]
fn seed_vosk_dlls_from_repo(rt_dir: &std::path::Path) {
    let exe_dir = std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|p| p.to_path_buf()))
        .unwrap_or_default();
    let mut roots = vec![
        exe_dir.join("../vosk_api/vosk-win64-0.3.42"),
        exe_dir.join("../../vosk_api/vosk-win64-0.3.42"),
    ];
    // Workspace layout when running from target/{debug,release}.
    if let Some(ws) = exe_dir.parent().and_then(|p| p.parent()) {
        roots.push(ws.join("vosk_api/vosk-win64-0.3.42"));
        roots.push(ws.join("QuickSTT_App/../vosk_api/vosk-win64-0.3.42"));
    }
    for root in roots {
        for dll in [
            "libgcc_s_seh-1.dll",
            "libstdc++-6.dll",
            "libwinpthread-1.dll",
        ] {
            let dest = rt_dir.join(dll);
            if dest.exists() {
                continue;
            }
            let src = root.join(dll);
            if src.exists() {
                let _ = std::fs::copy(&src, &dest);
            }
        }
    }
}

const PARAKEET_BASE: &str =
    "https://huggingface.co/KasuleTrevor/parakeet-tdt-0.6b-v3-onnx-int8/resolve/main";
const PARAKEET_FILES: &[&str] = &[
    "encoder-model.int8.onnx",
    "encoder-model.int8.onnx.data",
    "decoder_joint-model.int8.onnx",
    "decoder_joint-model.int8.onnx.data",
    "nemo128.onnx",
    "vocab.txt",
];

fn install_parakeet(desc: &ModelDescriptor, state: &SharedState) -> anyhow::Result<()> {
    let models_root = catalog::models_root();
    let model_dir = models_root.join(&desc.model_dir);
    std::fs::create_dir_all(&model_dir)?;

    // 0. ONNX Runtime shared library (official MS release built on Ubuntu
    //    20.04 → glibc 2.31, works on Mint 21/22). parakeet_engine uses
    //    ort/load-dynamic and is pointed at it via ORT_DYLIB_PATH.
    #[cfg(target_os = "linux")]
    install_onnxruntime_linux(state)?;

    for (i, fname) in PARAKEET_FILES.iter().enumerate() {
        let url = format!("{}/{}", PARAKEET_BASE, fname);
        let dest = model_dir.join(fname);
        download_file(
            &url,
            &dest,
            state,
            &FileDl {
                label: fname,
                index: i,
                count: PARAKEET_FILES.len(),
            },
        )?;
    }
    Ok(())
}

#[cfg(target_os = "linux")]
fn install_onnxruntime_linux(state: &SharedState) -> anyhow::Result<()> {
    const ORT_URL: &str =
        "https://github.com/microsoft/onnxruntime/releases/download/v1.19.2/onnxruntime-linux-x64-1.19.2.tgz";
    let rt_dir = catalog::models_root().join("runtimes/parakeet");
    let dest_so = rt_dir.join("libonnxruntime.so");
    if dest_so.exists() {
        return Ok(());
    }
    set_status(state, "Downloading ONNX Runtime…".into());
    let tgz = tmp_zip("onnxruntime-linux-x64-1.19.2.tgz");
    download_file(
        ORT_URL,
        &tgz,
        state,
        &FileDl {
            label: "onnxruntime.tgz",
            index: 0,
            count: 1,
        },
    )?;
    set_dl_stage(state, 96, "Extracting ONNX Runtime…".into(), "Extracting…");
    let unpack = tmp_zip("ort-unpack");
    let _ = std::fs::remove_dir_all(&unpack);
    std::fs::create_dir_all(&unpack)?;
    let f = std::fs::File::open(&tgz)?;
    let gz = flate2::read::GzDecoder::new(std::io::BufReader::new(f));
    let mut archive = tar::Archive::new(gz);
    archive.unpack(&unpack)?;
    // locate lib/libonnxruntime.so.1.19.2 inside the extracted tree
    let src = find_file(&unpack, "libonnxruntime.so.1.19.2")
        .or_else(|| find_file(&unpack, "libonnxruntime.so"));
    if let Some(src) = src {
        std::fs::create_dir_all(&rt_dir)?;
        std::fs::copy(&src, &dest_so)?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            if let Ok(meta) = std::fs::metadata(&dest_so) {
                let mut perm = meta.permissions();
                perm.set_mode(perm.mode() | 0o755);
                let _ = std::fs::set_permissions(&dest_so, perm);
            }
        }
    } else {
        anyhow::bail!("onnxruntime archive did not contain libonnxruntime.so");
    }
    let _ = std::fs::remove_file(&tgz);
    let _ = std::fs::remove_dir_all(&unpack);
    Ok(())
}

fn find_file(dir: &Path, name: &str) -> Option<PathBuf> {
    let stack = vec![dir.to_path_buf()];
    let mut queue = stack;
    while let Some(cur) = queue.pop() {
        if let Ok(entries) = std::fs::read_dir(&cur) {
            for e in entries.flatten() {
                let p = e.path();
                if p.is_dir() {
                    queue.push(p);
                } else if p.file_name().map(|f| f == name).unwrap_or(false) {
                    return Some(p);
                }
            }
        }
    }
    None
}

// ── Nemotron 3.5 ASR Streaming 0.6B (transcribe.cpp GGUF) ──
// Q4_K_M is the working CPU quant (473MB, verified clean JFK decode;
// the shipped Q8_0 batch decodes to all-<unk> on this engine). Q4 is also
// 34% smaller and faster on CPU — the efficient native choice.

const NEMOTRON_Q4_URL: &str =
    "https://huggingface.co/handy-computer/nemotron-3.5-asr-streaming-0.6b-gguf/resolve/main/nemotron-3.5-asr-streaming-0.6b-Q4_K_M.gguf";

fn install_nemotron(desc: &ModelDescriptor, state: &SharedState) -> anyhow::Result<()> {
    let models_root = catalog::models_root();
    let model_dir = models_root.join(&desc.model_dir);
    std::fs::create_dir_all(&model_dir)?;
    set_status(state, "Downloading Nemotron 3.5 ASR Streaming Q4_K_M (473MB, CPU-optimised)…".into());
    let dest = model_dir.join("nemotron-3.5-asr-streaming-0.6b-Q4_K_M.gguf");
    if dest.exists() && std::fs::metadata(&dest).map(|m| m.len() > 400_000_000).unwrap_or(false) {
        return Ok(());
    }
    download_file(
        NEMOTRON_Q4_URL,
        &dest,
        state,
        &FileDl {
            label: "nemotron-3.5-asr-streaming-0.6b-Q4_K_M.gguf",
            index: 0,
            count: 1,
        },
    )?;
    // Remove the broken Q8_0 if it sits alongside (the worker prefers Q8 by
    // name and would keep returning <unk>). Keep it only when Q4 is missing.
    let q8 = model_dir.join("nemotron-3.5-asr-streaming-0.6b-Q8_0.gguf");
    if q8.exists() && dest.exists() {
        let _ = std::fs::remove_file(&q8);
    }
    Ok(())
}

// ── Whisper.cpp GGML (Tiny EN Q5_1 + Large v3 Turbo Q8_0) ──
// Same whisper.cpp CPU runtime for both (best overall for Whisper, no other
// runtime): the CLI auto-handles any ggml-*.bin. URLs match the C++ registry.

const WHISPER_TINY_Q5_URL: &str =
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en-q5_1.bin";
const WHISPER_LARGE_TURBO_Q8_URL: &str =
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-turbo-q8_0.bin";

fn install_whisper(desc: &ModelDescriptor, state: &SharedState) -> anyhow::Result<()> {
    let models_root = catalog::models_root();
    let model_dir = models_root.join(&desc.model_dir);
    std::fs::create_dir_all(&model_dir)?;
    let (url, file, min_size, label) = if desc.model_dir.contains("large_v3_turbo") {
        (
            WHISPER_LARGE_TURBO_Q8_URL,
            "ggml-large-v3-turbo-q8_0.bin",
            700_000_000u64,
            "Whisper Large v3 Turbo Q8_0 (~800MB)",
        )
    } else {
        (
            WHISPER_TINY_Q5_URL,
            "ggml-tiny.en-q5_1.bin",
            30_000_000u64,
            "Whisper Tiny EN Q5_1 (32MB)",
        )
    };
    set_status(state, format!("Downloading {}…", label));
    let dest = model_dir.join(file);
    if dest.exists() && std::fs::metadata(&dest).map(|m| m.len() > min_size).unwrap_or(false) {
        return Ok(());
    }
    download_file(url, &dest, state, &FileDl { label: file, index: 0, count: 1 })?;
    Ok(())
}

// ── Canary 180M Flash (Handy GGUF, CPU Q5_K_M 151MB) ──
// Registered for 1-click download only — never fetched automatically.
// Runtime is the Handy transcribe worker (same as Nemotron); see engine.rs.

const CANARY_Q5_URL: &str =
    "https://huggingface.co/handy-computer/canary-180m-flash-gguf/resolve/main/canary-180m-flash-Q5_K_M.gguf";

fn install_canary(desc: &ModelDescriptor, state: &SharedState) -> anyhow::Result<()> {
    let models_root = catalog::models_root();
    let model_dir = models_root.join(&desc.model_dir);
    std::fs::create_dir_all(&model_dir)?;
    set_status(state, "Downloading Canary 180M Flash Q5_K_M (151MB, CPU)…".into());
    let dest = model_dir.join("canary-180m-flash-Q5_K_M.gguf");
    if dest.exists() && std::fs::metadata(&dest).map(|m| m.len() > 140_000_000).unwrap_or(false) {
        return Ok(());
    }
    download_file(
        CANARY_Q5_URL,
        &dest,
        state,
        &FileDl {
            label: "canary-180m-flash-Q5_K_M.gguf",
            index: 0,
            count: 1,
        },
    )?;
    Ok(())
}

// ── Moondream Photon checkpoints (Ultra 1.26GB / Redux 178MB) ──
// Plain HuggingFace files (safetensors + tokenizer + config); the Photon
// Python runtime reads them from its own HF cache at worker start, so this
// local copy is the offline record + what the installed-flag checks.
// Registered for 1-click download only — never fetched automatically.

fn photon_repo_id(desc: &ModelDescriptor) -> Option<&'static str> {
    crate::models::catalog::photon_repo_id(&desc.model_dir)
}

fn install_photon(desc: &ModelDescriptor, state: &SharedState) -> anyhow::Result<()> {
    let repo = photon_repo_id(desc)
        .ok_or_else(|| anyhow::anyhow!("No Photon repo registered for {}", desc.name))?;
    // Ultra ships 3 files; Redux adds ternary.json (same loop covers both).
    let files: &[(&str, u64)] = if desc.model_dir.contains("parakeet_redux") {
        &[
            ("model.safetensors", 170_000_000),
            ("config.json", 1_000),
            ("ternary.json", 1_000),
            ("tokenizer.json", 100_000),
        ]
    } else {
        &[
            ("model.safetensors", 1_200_000_000),
            ("config.json", 500),
            ("tokenizer.json", 100_000),
        ]
    };
    let models_root = catalog::models_root();
    let model_dir = models_root.join(&desc.model_dir);
    std::fs::create_dir_all(&model_dir)?;
    set_status(
        state,
        format!("Downloading {} ({} files, Photon)…", desc.name, files.len()),
    );
    for (i, (file, min_size)) in files.iter().enumerate() {
        let dest = model_dir.join(file);
        if dest.exists()
            && std::fs::metadata(&dest).map(|m| m.len() > *min_size).unwrap_or(false)
        {
            continue;
        }
        let url = format!("https://huggingface.co/{}/resolve/main/{}", repo, file);
        download_file(
            &url,
            &dest,
            state,
            &FileDl {
                label: file,
                index: i,
                count: files.len(),
            },
        )?;
    }
    Ok(())
}

// ── Moonshine v2 Base En (sherpa-onnx, 135M INT8) ──
// Native runtime: sherpa-onnx-offline (CPU, 4 threads) with the official
// quantized ONNX bundle — the same tarball the C++ app ships. The old
// downloader fetched HuggingFace safetensors (PyTorch, unusable by the ONNX
// CLI) into the same dir, which is exactly why Moonshine "never output text".

const MOONSHINE_BASE_TAR_URL: &str =
    "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/sherpa-onnx-moonshine-base-en-quantized-2026-02-27.tar.bz2";

fn install_moonshine(desc: &ModelDescriptor, state: &SharedState) -> anyhow::Result<()> {
    let models_root = catalog::models_root();
    let model_dir = models_root.join(&desc.model_dir);
    std::fs::create_dir_all(&model_dir)?;
    // Already installed when the ONNX trio is present — never clobber a good
    // install with a re-download (and never drop safetensors here).
    if model_dir.join("encoder_model.ort").exists()
        && model_dir.join("decoder_model_merged.ort").exists()
        && model_dir.join("tokens.txt").exists()
    {
        return Ok(());
    }
    set_status(state, "Downloading Moonshine v2 Base En (135M INT8, CPU)…".into());
    let tbz = tmp_zip("sherpa-onnx-moonshine-base-en-quantized.tar.bz2");
    download_file(
        MOONSHINE_BASE_TAR_URL,
        &tbz,
        state,
        &FileDl {
            label: "moonshine-base-en.tar.bz2",
            index: 0,
            count: 1,
        },
    )?;
    set_dl_stage(state, 96, "Extracting Moonshine…".into(), "Extracting…");
    extract_tar_bz2(&tbz, &model_dir)?;
    let _ = std::fs::remove_file(&tbz);
    // The tarball may nest files one level deep — flatten the trio up.
    for f in ["encoder_model.ort", "decoder_model_merged.ort", "tokens.txt"] {
        if !model_dir.join(f).exists() {
            if let Some(found) = find_file(&model_dir, f) {
                let _ = std::fs::rename(&found, model_dir.join(f));
            }
        }
    }
    if !model_dir.join("encoder_model.ort").exists() {
        anyhow::bail!("Moonshine archive did not contain encoder_model.ort");
    }
    Ok(())
}

fn extract_tar_bz2(archive: &std::path::Path, out_dir: &std::path::Path) -> anyhow::Result<()> {
    let f = std::fs::File::open(archive)?;
    let bz = bzip2::read::BzDecoder::new(std::io::BufReader::new(f));
    let mut tar = tar::Archive::new(bz);
    tar.unpack(out_dir)?;
    Ok(())
}

