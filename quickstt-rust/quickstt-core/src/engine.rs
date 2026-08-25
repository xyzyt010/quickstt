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
#[derive(Debug, Clone)]
pub struct ComputeTarget {
    pub name: String,
    pub is_gpu: bool,
    pub memory_mb: u64,
    pub vendor: String,
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
                        targets.push(ComputeTarget {
                            name: format!("GPU {} ({})", adapter_index, vendor),
                            is_gpu: true,
                            memory_mb: (dedicated_memory as u64 + shared_memory as u64) / (1024 * 1024),
                            vendor: vendor.to_string(),
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
                    targets.push(ComputeTarget {
                        name: format!("GPU ({})", v),
                        is_gpu: true,
                        memory_mb: 0,
                        vendor: v.to_string(),
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
