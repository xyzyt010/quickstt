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
    std::thread::Builder::new()
        .name(format!("dl-{}", desc.engine_family))
        .spawn(move || {
            let name = desc.name.clone();
            set_status(&state, format!("Downloading {}…", name));
            match install_model(&desc, &state) {
                Ok(()) => {
                    info!("Model installed: {}", name);
                    set_status(&state, format!("Installed: {}", name));
                }
                Err(e) => {
                    warn!("Download failed for {}: {}", name, e);
                    set_status(&state, format!("Download failed: {}", e));
                }
            }
            refresh_installed_flags(&state);
        })
        .expect("spawn download thread");
}

fn set_status(state: &SharedState, msg: String) {
    if let Ok(mut s) = state.lock() {
        s.status_message = msg;
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
        EngineFamily::Vosk => install_vosk(desc, state),
        EngineFamily::ParakeetRust => install_parakeet(desc, state),
        EngineFamily::Nemotron => install_nemotron(desc, state),
        _ => anyhow::bail!("No download source registered for {}", desc.name),
    }
}

// ── HTTP helpers ──

fn http_get(url: &str) -> anyhow::Result<ureq::Response> {
    Ok(ureq::get(url)
        .timeout(std::time::Duration::from_secs(600))
        .call()?)
}

/// Stream a URL to a file, reporting percent progress via `progress`.
fn download_file(url: &str, dest: &Path, progress: &mut dyn FnMut(u8)) -> anyhow::Result<()> {
    if let Some(parent) = dest.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let resp = http_get(url)?;
    let total = resp
        .header("Content-Length")
        .and_then(|v| v.parse::<u64>().ok())
        .unwrap_or(0);
    let mut reader = resp.into_reader();
    let mut file = std::fs::File::create(dest)?;
    let mut buf = [0u8; 64 * 1024];
    let mut received: u64 = 0;
    loop {
        let n = reader.read(&mut buf)?;
        if n == 0 {
            break;
        }
        file.write_all(&buf[..n])?;
        received += n as u64;
        if total > 0 {
            let pct = ((received * 100) / total).min(100) as u8;
            progress(pct);
        }
    }
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

    // 1. Acoustic/language model
    set_status(state, "Downloading Vosk small EN (50MB)…".into());
    let zpath = tmp_zip("vosk-small-en-us-0.15.zip");
    let mut last = 255u8;
    download_file(
        "https://alphacephei.com/vosk/models/vosk-model-small-en-us-0.15.zip",
        &zpath,
        &mut |p| {
            if p != last && p % 10 == 0 {
                last = p;
                set_status(state, format!("Downloading Vosk… {}%", p));
            }
        },
    )?;
    set_status(state, "Extracting Vosk model…".into());
    let _ = std::fs::remove_dir_all(&model_dir);
    extract_zip(&zpath, &model_dir, true)?;
    let _ = std::fs::remove_file(&zpath);

    // 2. Runtime engine (libvosk.so)
    set_status(state, "Downloading libvosk runtime…".into());
    let rtpath = tmp_zip("vosk-linux-x86_64-0.3.45.zip");
    download_file(
        "https://github.com/alphacep/vosk-api/releases/download/v0.3.45/vosk-linux-x86_64-0.3.45.zip",
        &rtpath,
        &mut |_| {},
    )?;
    let rt_dir = models_root.join("runtimes/vosk");
    set_status(state, "Extracting libvosk.so…".into());
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
    Ok(())
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
        set_status(
            state,
            format!("Downloading Parakeet TDT 0.6B ({}/{})…", i + 1, PARAKEET_FILES.len()),
        );
        let url = format!("{}/{}", PARAKEET_BASE, fname);
        let dest = model_dir.join(fname);
        let mut last = 255u8;
        download_file(&url, &dest, &mut |p| {
            if p != last && p % 25 == 0 {
                last = p;
                set_status(
                    state,
                    format!("Parakeet file {}/{}… {}%", i + 1, PARAKEET_FILES.len(), p),
                );
            }
        })?;
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
    download_file(ORT_URL, &tgz, &mut |_| {})?;
    set_status(state, "Extracting ONNX Runtime…".into());
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

const NEMOTRON_Q8_URL: &str =
    "https://huggingface.co/handy-computer/nemotron-3.5-asr-streaming-0.6b-gguf/resolve/main/nemotron-3.5-asr-streaming-0.6b-Q8_0.gguf";

fn install_nemotron(desc: &ModelDescriptor, state: &SharedState) -> anyhow::Result<()> {
    let models_root = catalog::models_root();
    let model_dir = models_root.join(&desc.model_dir);
    std::fs::create_dir_all(&model_dir)?;
    set_status(state, "Downloading Nemotron 3.5 ASR Streaming Q8_0 (716MB)…".into());
    let dest = model_dir.join("nemotron-3.5-asr-streaming-0.6b-Q8_0.gguf");
    if dest.exists() && std::fs::metadata(&dest).map(|m| m.len() > 700_000_000).unwrap_or(false) {
        return Ok(());
    }
    let mut last = 255u8;
    download_file(NEMOTRON_Q8_URL, &dest, &mut |p| {
        if p != last && p % 5 == 0 {
            last = p;
            set_status(state, format!("Downloading Nemotron… {}%", p));
        }
    })?;
    Ok(())
}
