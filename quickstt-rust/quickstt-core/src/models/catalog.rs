use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ModelDescriptor {
    pub name: String,
    pub engine_family: EngineFamily,
    pub model_dir: String,
    pub size_mb: u32,
    pub widget_selectable: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum EngineFamily {
    WhisperCpp,
    SherpaOnnx,
    NemoTransducer,
    NemoCTC,
    ParakeetRust,
    Vosk,
    Nemotron,
}

impl EngineFamily {
    pub fn display_name(&self) -> &'static str {
        match self {
            EngineFamily::WhisperCpp => "whisper.cpp",
            EngineFamily::SherpaOnnx => "sherpa-onnx",
            EngineFamily::NemoTransducer => "nemo-transducer",
            EngineFamily::NemoCTC => "nemo-ctc",
            EngineFamily::ParakeetRust => "parakeet-rust",
            EngineFamily::Vosk => "vosk",
            EngineFamily::Nemotron => "nemotron",
        }
    }
}

impl std::fmt::Display for EngineFamily {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.display_name())
    }
}

pub fn all_descriptors() -> Vec<ModelDescriptor> {
    vec![
        ModelDescriptor {
            name: "Whisper Tiny EN Q5".into(),
            engine_family: EngineFamily::WhisperCpp,
            model_dir: "whisper_cpp/tiny_en_q5".into(),
            size_mb: 32,
            widget_selectable: false,
        },
        ModelDescriptor {
            name: "Whisper Tiny EN Q8".into(),
            engine_family: EngineFamily::WhisperCpp,
            model_dir: "whisper_cpp/tiny_en_q8".into(),
            size_mb: 44,
            widget_selectable: false,
        },
        ModelDescriptor {
            name: "Whisper Base EN Q8".into(),
            engine_family: EngineFamily::WhisperCpp,
            model_dir: "whisper_cpp/base_en_q8".into(),
            size_mb: 74,
            widget_selectable: false,
        },
        ModelDescriptor {
            name: "Whisper Small EN Q5".into(),
            engine_family: EngineFamily::WhisperCpp,
            model_dir: "whisper_cpp/small_en_q5".into(),
            size_mb: 190,
            widget_selectable: false,
        },
        ModelDescriptor {
            name: "Whisper Small EN Q8".into(),
            engine_family: EngineFamily::WhisperCpp,
            model_dir: "whisper_cpp/small_en_q8".into(),
            size_mb: 264,
            widget_selectable: false,
        },
        ModelDescriptor {
            name: "Whisper Large v3 Turbo Q8".into(),
            engine_family: EngineFamily::WhisperCpp,
            model_dir: "whisper_cpp/large_v3_turbo_q8".into(),
            size_mb: 1500,
            widget_selectable: false,
        },
        ModelDescriptor {
            name: "Moonshine v2 Tiny EN".into(),
            engine_family: EngineFamily::SherpaOnnx,
            model_dir: "moonshine_v2/tiny_en".into(),
            size_mb: 43,
            widget_selectable: false,
        },
        ModelDescriptor {
            name: "Moonshine v2 Base EN".into(),
            engine_family: EngineFamily::SherpaOnnx,
            model_dir: "moonshine_v2/base_en".into(),
            size_mb: 135,
            widget_selectable: false,
        },
        ModelDescriptor {
            name: "NVIDIA Parakeet CTC 110M INT8".into(),
            engine_family: EngineFamily::NemoCTC,
            model_dir: "nemo/ctc_110m_int8".into(),
            size_mb: 126,
            widget_selectable: false,
        },
        ModelDescriptor {
            name: "NVIDIA Parakeet TDT 0.6B v3 INT8 (ONNX/Rust)".into(),
            engine_family: EngineFamily::ParakeetRust,
            model_dir: "nemo/tdt_0_6b_v3_int8".into(),
            size_mb: 640,
            widget_selectable: true,
        },
        ModelDescriptor {
            name: "Vosk Small EN (50M)".into(),
            engine_family: EngineFamily::Vosk,
            model_dir: "vosk/small_en_us_0.15".into(),
            size_mb: 50,
            widget_selectable: true,
        },
        ModelDescriptor {
            name: "Nemotron 3.5 ASR Streaming 0.6B Q8_0 (GGUF)".into(),
            engine_family: EngineFamily::Nemotron,
            model_dir: "nemotron/streaming_0.6b_q8_0".into(),
            size_mb: 716,
            widget_selectable: true,
        },
    ]
}

pub fn models_root() -> PathBuf {
    // Windows: %APPDATA%\QuickSTT\models ; Linux: ~/.local/share/QuickSTT/models (XDG)
    #[cfg(target_os = "windows")]
    {
        if let Some(appdata) = std::env::var_os("APPDATA") {
            return PathBuf::from(appdata).join("QuickSTT").join("models");
        }
    }
    #[cfg(not(target_os = "windows"))]
    {
        if let Some(data_dir) = dirs::data_dir() {
            let p = data_dir.join("QuickSTT").join("models");
            // Prefer XDG if it already exists or as default
            if p.exists() || std::env::var_os("XDG_DATA_HOME").is_some() {
                return p;
            }
            // Fallback: also check XDG even if not exists
            return p;
        }
    }
    // Fallback: exe-relative data/models (portable install)
    let exe = std::env::current_exe().unwrap_or_default();
    exe.parent()
        .unwrap_or(Path::new("."))
        .join("data")
        .join("models")
}

pub fn is_model_installed(desc: &ModelDescriptor) -> bool {
    let root = models_root();
    let model_path = root.join(&desc.model_dir);
    model_path.exists() && model_path.is_dir()
}

pub fn installed_models() -> Vec<ModelDescriptor> {
    all_descriptors()
        .into_iter()
        .filter(|d| d.widget_selectable && is_model_installed(d))
        .collect()
}

pub fn display_name(desc: &ModelDescriptor) -> String {
    let installed = if is_model_installed(desc) {
        ""
    } else {
        " [Not Installed]"
    };
    format!("{} ({}MB){}", desc.name, desc.size_mb, installed)
}
