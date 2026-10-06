//! Engine detection and matching logic

use anyhow::Result;
use std::path::PathBuf;

/// Represents a detected STT engine/backend
/// v2.0: Cloud engine variant removed. Only local engines remain.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SttEngine {
    WhisperCpp,
}

impl SttEngine {
    /// Get the engine name as used in the UI
    pub fn display_name(&self) -> &'static str {
        match self {
            SttEngine::WhisperCpp => "Whisper.cpp (whisper-rs)",
        }
    }

    /// All v2.0 engines require a local model
    pub fn requires_local_model(&self) -> bool {
        true
    }
}

/// Represents a wake word detection engine
/// v2.0: Only livekit-wakeword (ONNX) is supported.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum WakeEngine {
    LivekitWakeword,
}

impl WakeEngine {
    pub fn display_name(&self) -> &'static str {
        match self {
            WakeEngine::LivekitWakeword => "livekit-wakeword (ONNX)",
        }
    }

    pub fn from_str(s: &str) -> Option<Self> {
        match s {
            "livekit-wakeword (ONNX)" => Some(WakeEngine::LivekitWakeword),
            _ => None,
        }
    }
}

/// Detect available compute targets (GPU/CPU)
///
/// Discrete-GPU rules follow handy_discrete_gpu_asr_integration.md:
/// iGPUs (Intel UHD/Iris, AMD 780M/APU, Apple integrated, NPUs) must NOT
/// count as discrete — those systems stay on the CPU path.
#[derive(Debug, Clone)]
pub struct ComputeTarget {
    pub name: String,
    pub is_gpu: bool,
    pub memory_mb: u64,
    pub vendor: String,
    /// Dedicated VRAM in MB (DXGI DedicatedVideoMemory; 0 when unknown).
    pub dedicated_vram_mb: u64,
    /// True only for a physical discrete GPU (see `is_discrete_gpu_name`).
    pub is_discrete: bool,
    /// Raw adapter description for UI display.
    pub device_name: String,
}

/// Single selected compute backend + device for the recommendation engine.
#[derive(Debug, Clone)]
pub struct HardwareInfo {
    pub has_discrete_gpu: bool,
    pub vendor: String,
    pub device_name: String,
    pub vram_mb: u64,
    /// cuda | rocm | vulkan | cpu (shipped: cpu + vulkan; cuda needs CUDA build).
    pub backend: String,
    pub system_ram_gb: u64,
}

impl HardwareInfo {
    pub fn detect() -> Self {
        let targets = ComputeTarget::detect_all();
        let system_ram_gb = system_ram_gb();
        // Priority after iGPU filtering: NVIDIA > AMD > Intel > CPU.
        let pick = targets
            .iter()
            .filter(|t| t.is_gpu && t.is_discrete)
            .find(|t| t.vendor == "NVIDIA")
            .or_else(|| {
                targets
                    .iter()
                    .filter(|t| t.is_gpu && t.is_discrete)
                    .find(|t| t.vendor == "AMD")
            })
            .or_else(|| {
                targets
                    .iter()
                    .filter(|t| t.is_gpu && t.is_discrete)
                    .find(|t| t.vendor == "Intel")
            });
        match pick {
            Some(d) => {
                let backend = if d.vendor == "NVIDIA" {
                    // CUDA when a CUDA whisper build is present, else Vulkan/CPU
                    // fallback (shipped binaries are CPU+Vulkan; no CUDA yet).
                    if cuda_runtime_present() { "cuda" } else { "vulkan" }
                } else if d.vendor == "AMD" {
                    if rocm_available() { "rocm" } else { "vulkan" }
                } else {
                    "vulkan"
                };
                Self {
                    has_discrete_gpu: true,
                    vendor: d.vendor.clone(),
                    device_name: d.device_name.clone(),
                    vram_mb: d.dedicated_vram_mb,
                    backend: backend.to_string(),
                    system_ram_gb,
                }
            }
            None => Self {
                has_discrete_gpu: false,
                vendor: "CPU".to_string(),
                device_name: "CPU".to_string(),
                vram_mb: 0,
                backend: "cpu".to_string(),
                system_ram_gb,
            },
        }
    }

    pub fn summary(&self) -> String {
        if self.has_discrete_gpu {
            format!(
                "{} {} • {} • {} MB VRAM",
                self.vendor, self.device_name, self.backend, self.vram_mb
            )
        } else {
            format!("CPU • {} GB RAM", self.system_ram_gb)
        }
    }
}

fn system_ram_gb() -> u64 {
    #[cfg(target_os = "windows")]
    {
        use windows::Win32::System::SystemInformation::{GlobalMemoryStatusEx, MEMORYSTATUSEX};
        unsafe {
            let mut st = MEMORYSTATUSEX {
                dwLength: std::mem::size_of::<MEMORYSTATUSEX>() as u32,
                ..Default::default()
            };
            if GlobalMemoryStatusEx(&mut st).is_ok() {
                return (st.ullTotalPhys / (1024 * 1024 * 1024)).max(1);
            }
        }
        16
    }
    #[cfg(not(target_os = "windows"))]
    {
        if let Ok(text) = std::fs::read_to_string("/proc/meminfo") {
            for line in text.lines() {
                if line.starts_with("MemTotal:") {
                    let kb: u64 = line
                        .split_whitespace()
                        .nth(1)
                        .and_then(|v| v.parse().ok())
                        .unwrap_or(16 * 1024 * 1024);
                    return (kb / (1024 * 1024)).max(1);
                }
            }
        }
        16
    }
}

#[cfg(target_os = "windows")]
fn cuda_runtime_present() -> bool {
    // A CUDA whisper build ships nvcuda-adjacent DLLs next to the exe.
    // Shipped 2.0.0-alpha binaries are CPU+Vulkan only → false for now.
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            for cand in ["nvcuda.dll", "cublas64_12.dll", "whisper-cuda.dll"] {
                if dir.join(cand).exists() {
                    return true;
                }
            }
        }
    }
    false
}
#[cfg(not(target_os = "windows"))]
fn cuda_runtime_present() -> bool {
    std::path::Path::new("/usr/local/cuda/lib64/libcudart.so").exists()
}

fn rocm_available() -> bool {
    #[cfg(target_os = "windows")]
    {
        false // ROCm on Windows: Vulkan fallback per spec.
    }
    #[cfg(not(target_os = "windows"))]
    {
        std::process::Command::new("rocm-smi")
            .arg("--showid")
            .output()
            .map(|o| o.status.success())
            .unwrap_or(false)
    }
}

/// Discrete-GPU name heuristic (spec: never trust "GPU present" alone).
/// Returns true only for physical discrete cards:
/// NVIDIA RTX/GTX/Quadro/Tesla, AMD RX/PRO/WX (not 780M/760M/680M/APU/m series),
/// Intel Arc A/B-series. Everything else (UHD, Iris, Radeon 780M, Apple, NPU)
/// is integrated.
pub fn is_discrete_gpu_name(name: &str, vendor: &str) -> bool {
    let n = name.to_lowercase();
    match vendor {
        "NVIDIA" => {
            // Exclude Jetson/embedded + GRID virtual (no local VRAM advantage).
            if n.contains("jetson") || n.contains("grid") || n.contains("virtual") {
                return false;
            }
            n.contains("rtx") || n.contains("gtx") || n.contains("quadro") || n.contains("tesla")
        }
        "AMD" => {
            if n.contains("780m")
                || n.contains("760m")
                || n.contains("680m")
                || n.contains("660m")
                || n.contains("apu")
                || n.contains("ryzen")
                || n.contains("radeon graphics")
                    && !n.contains("rx")
            {
                return false;
            }
            n.contains(" rx ") || n.contains("rx ") || n.contains(" rx") || n.contains("radeon rx") || n.contains("radeon pro") || n.contains(" wx ")
        }
        "Intel" => {
            // Arc discrete only; UHD/Iris/Iris Xe are iGPU.
            n.contains("arc") && (n.contains(" a") || n.contains(" b") || n.contains("a3") || n.contains("a5") || n.contains("a7") || n.contains("b5") || n.contains("b7"))
        }
        _ => false,
    }
}

impl ComputeTarget {
    /// Get all available compute targets
    pub fn detect_all() -> Vec<Self> {
        let mut targets = Vec::new();

        targets.push(ComputeTarget {
            name: "CPU".to_string(),
            is_gpu: false,
            memory_mb: 0,
            vendor: "System".to_string(),
            dedicated_vram_mb: 0,
            is_discrete: false,
            device_name: "CPU".to_string(),
        });

        if let Ok(gpus) = detect_gpus_dxgi() {
            targets.extend(gpus);
        }

        targets
    }

    /// Get the best compute target for whisper-rs (prefers GPU)
    pub fn best_for_engine(_engine: &SttEngine) -> Option<Self> {
        let targets = Self::detect_all();
        targets
            .iter()
            .find(|t| t.is_gpu)
            .cloned()
            .or_else(|| targets.iter().find(|t| !t.is_gpu).cloned())
    }
}

/// Detect GPUs — DXGI on Windows, lspci/vulkan probe on Linux
fn detect_gpus_dxgi() -> Result<Vec<ComputeTarget>> {
    #[cfg(target_os = "windows")]
    {
        use windows::Win32::Graphics::Dxgi::*;
        let mut targets = Vec::new();
        unsafe {
            let factory: IDXGIFactory1 = CreateDXGIFactory1()?;
            let mut adapter_index = 0;
            loop {
                let adapter = factory.EnumAdapters1(adapter_index);
                match adapter {
                    Ok(adapter) => {
                        let mut desc = std::mem::zeroed::<DXGI_ADAPTER_DESC1>();
                        adapter.GetDesc1(&mut desc)?;
                        let vendor_id = desc.VendorId;
                        let dedicated_memory = desc.DedicatedVideoMemory;
                        let shared_memory = desc.SharedSystemMemory;
                        let vendor = match vendor_id {
                            0x10DE => "NVIDIA",
                            0x1002 => "AMD",
                            0x8086 => "Intel",
                            _ => "Unknown",
                        };
                        // Adapter description for discrete heuristics + UI.
                        let raw: &[u16] = &desc.Description;
                        let end = raw.iter().position(|&c| c == 0).unwrap_or(raw.len());
                        let dev_name = String::from_utf16_lossy(&raw[..end]);
                        let dedicated_mb = (dedicated_memory as u64) / (1024 * 1024);
                        let short = format!("GPU {} ({})", adapter_index, vendor);
                        targets.push(ComputeTarget {
                            name: format!("{} — {}", short, dev_name.trim()),
                            is_gpu: true,
                            memory_mb: (dedicated_memory as u64 + shared_memory as u64) / (1024 * 1024),
                            vendor: vendor.to_string(),
                            dedicated_vram_mb: dedicated_mb,
                            is_discrete: is_discrete_gpu_name(dev_name.trim(), vendor),
                            device_name: dev_name.trim().to_string(),
                        });
                        adapter_index += 1;
                    }
                    Err(_) => break,
                }
            }
        }
        Ok(targets)
    }
    #[cfg(not(target_os = "windows"))]
    {
        detect_gpus_linux()
    }
}

#[cfg(not(target_os = "windows"))]
fn detect_gpus_linux() -> Result<Vec<ComputeTarget>> {
    let mut targets = Vec::new();
    // Try lspci first
    if let Ok(output) = std::process::Command::new("lspci").arg("-nn").output() {
        let text = String::from_utf8_lossy(&output.stdout);
        for line in text.lines() {
            let lower = line.to_lowercase();
            let vendor = if lower.contains("nvidia") { Some("NVIDIA") }
            else if lower.contains("amd") || lower.contains("advanced micro") { Some("AMD") }
            else if lower.contains("intel") { Some("Intel") }
            else { None };
            if let Some(v) = vendor {
                // Only treat VGA/3D/display controllers as GPUs
                if lower.contains("vga") || lower.contains("3d") || lower.contains("display") {
                    // Linux has no cheap VRAM query here — discrete flag comes
                    // from the name heuristic; VRAM stays 0 (Tiny fallback).
                    let dev = line.trim().to_string();
                    targets.push(ComputeTarget {
                        name: format!("GPU ({})", v),
                        is_gpu: true,
                        memory_mb: 0,
                        vendor: v.to_string(),
                        dedicated_vram_mb: 0,
                        is_discrete: is_discrete_gpu_name(&dev, v),
                        device_name: dev,
                    });
                }
            }
        }
    }
    // Fallback: try /proc/meminfo for CPU memory hint
    Ok(targets)
}

/// Match a model name to an engine
/// v2.0: Only whisper-based models are supported.
pub fn match_model_to_engine(model_name: &str) -> Option<SttEngine> {
    let name = model_name.to_lowercase();

    if name.contains("whisper") || name.contains("ggml") {
        Some(SttEngine::WhisperCpp)
    } else {
        None
    }
}

/// Find model directory by name
pub fn find_model_dir(models_root: &PathBuf, model_name: &str) -> Option<PathBuf> {
    use std::fs;

    if !models_root.exists() {
        return None;
    }

    let entries = fs::read_dir(models_root).ok()?;

    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            let dir_name = path.file_name()?.to_string_lossy();
            if dir_name.eq_ignore_ascii_case(model_name)
                || dir_name.contains(&model_name.to_lowercase())
            {
                return Some(path);
            }
        }
    }

    None
}

/// Get model marker file path (used to verify installation)
pub fn get_model_marker_path(model_dir: &PathBuf) -> PathBuf {
    model_dir.join(".installed")
}

/// Check if a model is installed (has marker file)
pub fn is_model_installed(model_dir: &PathBuf) -> bool {
    get_model_marker_path(model_dir).exists()
}

/// Create model marker file
pub fn mark_model_installed(model_dir: &PathBuf) -> Result<()> {
    use std::fs;
    fs::write(get_model_marker_path(model_dir), "installed")?;
    Ok(())
}

/// Remove model marker file
pub fn mark_model_uninstalled(model_dir: &PathBuf) -> Result<()> {
    use std::fs;
    let marker = get_model_marker_path(model_dir);
    if marker.exists() {
        fs::remove_file(marker)?;
    }
    Ok(())
}
