use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ModelDescriptor {
    pub name: String,
    pub engine_family: EngineFamily,
    pub model_dir: String,
    pub size_mb: u32,
    pub widget_selectable: bool,
    /// 0-100 accuracy score for dashboard bars + recommendation weighting.
    #[serde(default = "default_accuracy")]
    pub accuracy: u8,
    /// 0-100 speed score (higher = faster, CPU-relative).
    #[serde(default = "default_speed")]
    pub speed: u8,
    /// One-line card blurb (dashboard).
    #[serde(default)]
    pub blurb: String,
    /// True when the model only makes sense with a discrete GPU
    /// (hidden on CPU/iGPU machines).
    #[serde(default)]
    pub gpu_only: bool,
}

fn default_accuracy() -> u8 {
    70
}
fn default_speed() -> u8 {
    70
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
    /// Handy transcribe.cpp GGUF worker (same JSON protocol as Nemotron):
    /// Canary 180M Flash. CPU Q5_K_M per spec; GPU F16/Q8 via same worker.
    Canary,
    /// Moondream Photon (parakeet-redux / parakeet-ultra) via the persistent
    /// `tools/photon/photon_worker.py` helper: same JSON load/transcribe
    /// protocol as parakeet_engine. Needs Python + `pip install moondream`.
    Photon,
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
            EngineFamily::Canary => "canary",
            EngineFamily::Photon => "photon",
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
            name: "Whisper Tiny EN Q5_1 (GGML)".into(),
            engine_family: EngineFamily::WhisperCpp,
            model_dir: "whisper_cpp/tiny_en_q5".into(),
            size_mb: 32,
            widget_selectable: true,
            accuracy: 68,
            speed: 95,
            blurb: "Tiny and instant, runs well on any hardware".into(),
            gpu_only: false,
        },
        ModelDescriptor {
            name: "Moonshine Medium Streaming (245M)".into(),
            engine_family: EngineFamily::SherpaOnnx,
            model_dir: "moonshine_v2/base_en".into(),
            size_mb: 245,
            widget_selectable: true,
            accuracy: 78,
            speed: 85,
            blurb: "Fast and accurate streaming English".into(),
            gpu_only: false,
        },
        ModelDescriptor {
            name: "NVIDIA Parakeet TDT 0.6B v3 INT8 (ONNX/Rust)".into(),
            engine_family: EngineFamily::ParakeetRust,
            model_dir: "nemo/tdt_0_6b_v3_int8".into(),
            size_mb: 640,
            widget_selectable: true,
            accuracy: 90,
            speed: 75,
            blurb: "Fast, accurate live English transcription".into(),
            gpu_only: false,
        },
        ModelDescriptor {
            name: "Nemotron 3.5 ASR Streaming 0.6B Q4_K_M (GGUF)".into(),
            engine_family: EngineFamily::Nemotron,
            model_dir: "nemotron/nemotron-3.5-asr-streaming-0.6b".into(),
            size_mb: 473,
            widget_selectable: true,
            accuracy: 88,
            speed: 70,
            blurb: "Live multilingual transcription across 28 languages".into(),
            gpu_only: false,
        },
        ModelDescriptor {
            name: "Vosk Small EN (50M)".into(),
            engine_family: EngineFamily::Vosk,
            model_dir: "vosk-model-small-en-us-0.15".into(),
            size_mb: 50,
            widget_selectable: true,
            accuracy: 60,
            speed: 98,
            blurb: "Ultra-light offline English, tiny footprint".into(),
            gpu_only: false,
        },
        // CPU-only per spec (handy_discrete_gpu_asr_integration.md):
        // Canary 180M Flash Q5_K_M, 151MB, EN/DE/FR/ES + translation.
        // NOT downloaded by default — user-initiated only.
        ModelDescriptor {
            name: "Canary 180M Flash Q5_K_M (GGUF)".into(),
            engine_family: EngineFamily::Canary,
            model_dir: "canary/canary-180m-flash".into(),
            size_mb: 151,
            widget_selectable: true,
            accuracy: 72,
            speed: 92,
            blurb: "Tiny and instant, runs well on any hardware".into(),
            gpu_only: false,
        },
        // Moondream Parakeet Ultra 0.6B (full precision, 1.26GB) via the
        // Photon Python runtime: best accuracy in the app on CPU, but slow
        // turns (~0.5x audio duration — a 10s utterance takes ~20s on this
        // chip) and a minutes-long first load per session (persistent
        // worker amortizes it). NOT downloaded by default — user-initiated
        // only. Needs Python 3.10+ with `pip install "moondream>=2.4"`.
        ModelDescriptor {
            name: "Parakeet Ultra 0.6B (Photon, 1.3GB)".into(),
            engine_family: EngineFamily::Photon,
            model_dir: "parakeet_ultra".into(),
            size_mb: 1290,
            widget_selectable: true,
            accuracy: 93,
            speed: 45,
            blurb: "Best accuracy, slow CPU turns + slow first load".into(),
            gpu_only: false,
        },
        // GPU-only per request: Whisper Large v3 Turbo Q8_0. Shown only when
        // a discrete GPU is present; recommended only when VRAM > 3GB.
        // Same whisper.cpp runtime as Tiny (best overall for Whisper, no other
        // runtime). NOT downloaded by default.
        ModelDescriptor {
            name: "Whisper Large v3 Turbo Q8_0 (GGML)".into(),
            engine_family: EngineFamily::WhisperCpp,
            model_dir: "whisper_cpp/large_v3_turbo_q8".into(),
            size_mb: 800,
            widget_selectable: true,
            accuracy: 96,
            speed: 35,
            blurb: "Highest accuracy, broadest language coverage".into(),
            gpu_only: true,
        },
    ]
}

pub fn is_streaming_model(name: &str) -> bool {
    let n = name.to_lowercase();
    n.contains("streaming") || n.contains("nemotron") || n.contains("moonshine") || n.contains("parakeet") || n.contains("canary")
}

pub fn models_root() -> PathBuf {    // Windows: %APPDATA%\QuickSTT\models ; Linux: ~/.local/share/QuickSTT/models (XDG)
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
    // Require the actual weight files, not just the directory — a half-
    // downloaded or wrong-format dir (safetensors instead of .ort, broken Q8
    // without Q4) must show "[Not Installed]" so the user re-downloads.
    match desc.engine_family {
        EngineFamily::WhisperCpp => {
            // Any ggml bin in the descriptor dir counts.
            if model_path.is_dir() {
                if let Ok(entries) = std::fs::read_dir(&model_path) {
                    for e in entries.flatten() {
                        let n = e.file_name().to_string_lossy().to_string();
                        if n.starts_with("ggml-") && n.ends_with(".bin") {
                            return true;
                        }
                    }
                }
            }
            if root.join("whisper_cpp/tiny_en_q5/ggml-tiny.en-q5_1.bin").exists() {
                return true;
            }
            false
        }
        EngineFamily::SherpaOnnx => {
            // Moonshine v2 Base trio (INT8 ONNX). Safetensors-only dirs do NOT count.
            if model_path.join("encoder_model.ort").exists()
                && model_path.join("decoder_model_merged.ort").exists()
                && model_path.join("tokens.txt").exists()
            {
                return true;
            }
            for cand in [
                "moonshine_v2/base_en",
                "moonshine_v2/tiny_en",
            ] {
                let alt = root.join(cand);
                if alt.join("encoder_model.ort").exists()
                    && alt.join("decoder_model_merged.ort").exists()
                {
                    return true;
                }
            }
            false
        }
        EngineFamily::Nemotron => {
            // Working quant only: Q4_K_M (verified clean); Q8_0 in the shipped
            // batch decodes to all-<unk> and must NOT count as installed alone.
            for cand in [
                "nemotron/nemotron-3.5-asr-streaming-0.6b/nemotron-3.5-asr-streaming-0.6b-Q4_K_M.gguf",
                "nemotron/nemotron-3.5-asr-streaming-0.6b/nemotron-3.5-asr-streaming-0.6b-Q5_K_M.gguf",
                "nemotron/nemotron-3.5-asr-streaming-0.6b/nemotron-3.5-asr-streaming-0.6b-Q6_K.gguf",
            ] {
                if root.join(cand).exists() {
                    return true;
                }
            }
            // Descriptor dir with a working quant also counts.
            if model_path.is_dir() {
                for prefer in [
                    "nemotron-3.5-asr-streaming-0.6b-Q4_K_M.gguf",
                    "nemotron-3.5-asr-streaming-0.6b-Q5_K_M.gguf",
                    "nemotron-3.5-asr-streaming-0.6b-Q6_K.gguf",
                ] {
                    if model_path.join(prefer).exists() {
                        return true;
                    }
                }
            }
            false
        }
        EngineFamily::Vosk => {
            // Acoustic model + native runtime (Windows needs the MinGW trio
            // alongside libvosk.dll or LoadLibrary fails).
            let model_ok = model_path.join("am/final.mdl").exists()
                || model_path.join("final.mdl").exists()
                || root.join("vosk-model-small-en-us-0.15/am/final.mdl").exists();
            if !model_ok {
                return false;
            }
            #[cfg(target_os = "windows")]
            {
                let rt = root.join("runtimes/vosk");
                if rt.join("libvosk.dll").exists()
                    && rt.join("libgcc_s_seh-1.dll").exists()
                    && rt.join("libstdc++-6.dll").exists()
                {
                    return true;
                }
                // Repo-bundled fallback counts once seeded (downloader seeds it).
                return false;
            }
            #[cfg(not(target_os = "windows"))]
            {
                return true;
            }
        }
        EngineFamily::ParakeetRust => {
            if model_path.join("encoder-model.int8.onnx").exists() {
                return true;
            }
            if root.join("nemo/tdt_0_6b_v3_int8/encoder-model.int8.onnx").exists() {
                return true;
            }
            false
        }
        EngineFamily::Canary => {
            // Handy GGUF, CPU Q5_K_M default (spec). Any canary GGUF counts.
            if model_path.is_dir() {
                for prefer in [
                    "canary-180m-flash-Q5_K_M.gguf",
                    "canary-180m-flash-Q4_K_M.gguf",
                    "canary-180m-flash-Q8_0.gguf",
                    "canary-180m-flash-F16.gguf",
                ] {
                    if model_path.join(prefer).exists() {
                        return true;
                    }
                }
                if let Ok(entries) = std::fs::read_dir(&model_path) {
                    for e in entries.flatten() {
                        if e.file_name().to_string_lossy().ends_with(".gguf") {
                            return true;
                        }
                    }
                }
            }
            false
        }
        EngineFamily::Photon => {
            // Moondream Photon checkpoints: full weight set must be present
            // (Ultra: safetensors + tokenizer + config; Redux adds ternary.json
            // when its Windows kernels land — same check covers both).
            model_path.join("model.safetensors").exists()
                && model_path.join("tokenizer.json").exists()
                && model_path.join("config.json").exists()
        }
        _ => model_path.exists() && model_path.is_dir(),
    }
}

pub fn installed_models() -> Vec<ModelDescriptor> {
    all_descriptors()
        .into_iter()
        .filter(|d| d.widget_selectable && is_model_installed(d))
        .collect()
}

/// HuggingFace repo id for Photon-family checkpoints (the Python runtime
/// only accepts registered ids, not local paths). None for anything else.
pub fn photon_repo_id(model_dir: &str) -> Option<&'static str> {
    if model_dir.contains("parakeet_ultra") {
        Some("moondream/parakeet-ultra")
    } else if model_dir.contains("parakeet_redux") {
        Some("moondream/parakeet-redux")
    } else {
        None
    }
}

pub fn display_name(desc: &ModelDescriptor) -> String {
    let installed = if is_model_installed(desc) {
        ""
    } else {
        " [Not Installed]"
    };
    format!("{} ({}MB){}", desc.name, desc.size_mb, installed)
}

/// Languages a model can transcribe, as UI display names. The first entry
/// is the default. "Auto" means detect from audio (multilingual engines).
/// English-only checkpoints expose exactly one entry.
pub fn supported_languages(desc: &ModelDescriptor) -> Vec<String> {
    if desc.name.contains("German") {
        return vec!["German".to_string()];
    }
    // Canary 180M Flash: EN/DE/FR/ES (+ translation). First entry is default.
    if matches!(desc.engine_family, EngineFamily::Canary) {
        return vec!["English", "German", "French", "Spanish"]
            .into_iter()
            .map(|s| s.to_string())
            .collect();
    }
    // Nemotron streaming + Parakeet/Vosk/Moonshine/Photon checkpoints ship
    // English-only weights.
    if matches!(
        desc.engine_family,
        EngineFamily::ParakeetRust
            | EngineFamily::Vosk
            | EngineFamily::SherpaOnnx
            | EngineFamily::NemoCTC
            | EngineFamily::NemoTransducer
            | EngineFamily::Nemotron
            | EngineFamily::Photon
    ) {
        return vec!["English".to_string()];
    }
    // Whisper EN checkpoints (name carries EN).
    if desc.name.contains(" EN ") || desc.name.ends_with(" EN") {
        return vec!["English".to_string()];
    }
    // Multilingual-capable (Whisper Large, Nemotron streaming, ...).
    vec![
        "Auto", "English", "German", "French", "Spanish", "Italian", "Portuguese", "Dutch",
        "Polish", "Russian", "Turkish", "Arabic", "Hindi", "Chinese", "Japanese",
    ]
    .into_iter()
    .map(|s| s.to_string())
    .collect()
}

/// Hardware-aware model list: discrete-GPU machines see only the two
/// Whisper GPU models (Tiny + Large Turbo); CPU/iGPU machines see every
/// non-gpu_only model (existing five + Canary). GPU-only models are hidden
/// on CPU machines so nobody downloads 800MB they cannot run well.
pub fn models_for_hardware(
    has_discrete_gpu: bool,
) -> Vec<ModelDescriptor> {
    all_descriptors()
        .into_iter()
        .filter(|d| {
            if has_discrete_gpu {
                // GPU path: the two Whisper models only, no other model.
                d.name.contains("Whisper Tiny EN Q5_1")
                    || d.name.contains("Whisper Large v3 Turbo")
            } else {
                !d.gpu_only
            }
        })
        .collect()
}

/// Sophisticated best-model recommendation.
///
/// Rules (in priority order):
/// 1. Discrete GPU + VRAM > 3GB (3072MB) → Whisper Large v3 Turbo only.
///    It is the most accurate model that fits; Tiny stays as manual fallback.
/// 2. Discrete GPU + VRAM ≤ 3GB (or unknown VRAM) → Whisper Tiny (only model
///    guaranteed to fit + fastest on limited VRAM).
/// 3. CPU/iGPU → weighted score over accuracy (50%), speed (30%), footprint
///    (20%: smaller is better), plus a multilingual bonus when the user asked
///    for a non-English language, plus a low-RAM guard (≤8GB prefers ≤200MB).
/// Returns the recommended descriptor NAME (stable across index shifts) plus
/// a human-readable reason for the dashboard banner.
pub fn recommend_model(
    candidates: &[ModelDescriptor],
    has_discrete_gpu: bool,
    gpu_vendor: &str,
    gpu_name: &str,
    vram_mb: u64,
    system_ram_gb: u64,
    selected_language: &str,
) -> (Option<String>, String) {
    if candidates.is_empty() {
        return (None, "No models available for this hardware.".to_string());
    }
    // GPU path first — deterministic, VRAM-gated.
    if has_discrete_gpu {
        let large = candidates
            .iter()
            .find(|d| d.name.contains("Whisper Large v3 Turbo"));
        let tiny = candidates
            .iter()
            .find(|d| d.name.contains("Whisper Tiny EN Q5_1"));
        if vram_mb > 3072 {
            if let Some(l) = large {
                return (
                    Some(l.name.clone()),
                    format!(
                        "Discrete {} {} with {} MB VRAM — Large v3 Turbo fits and is the most accurate; Tiny remains as instant fallback.",
                        gpu_vendor,
                        gpu_name,
                        vram_mb
                    ),
                );
            }
        }
        if let Some(t) = tiny {
            return (
                Some(t.name.clone()),
                if vram_mb > 0 {
                    format!(
                        "Discrete {} {} with {} MB VRAM — Tiny is the fastest model guaranteed to fit; Large needs >3GB.",
                        gpu_vendor, gpu_name, vram_mb
                    )
                } else {
                    format!(
                        "Discrete {} {} (VRAM unknown) — Tiny is the safe default; install a VRAM-reporting driver for a Large recommendation.",
                        gpu_vendor, gpu_name
                    )
                },
            );
        }
    }
    // CPU/iGPU path — score every candidate.
    let want_non_english = !selected_language.eq_ignore_ascii_case("English")
        && !selected_language.eq_ignore_ascii_case("Auto")
        && !selected_language.trim().is_empty();
    let mut best: Option<(&ModelDescriptor, f32)> = None;
    for d in candidates {
        // Footprint score: 100 at ≤32MB → 0 at ≥800MB.
        let footprint = (100.0 - ((d.size_mb as f32 - 32.0) / (800.0 - 32.0) * 100.0)).clamp(0.0, 100.0);
        let mut score = d.accuracy as f32 * 0.5 + d.speed as f32 * 0.3 + footprint * 0.2;
        // Multilingual bonus when the user needs it.
        if want_non_english {
            let langs = supported_languages(d);
            if langs.iter().any(|l| l == selected_language) {
                score += 12.0;
            } else {
                score -= 25.0; // English-only weights cannot serve the request.
            }
        }
        // Low-RAM guard: penalise >400MB models hard on ≤8GB machines.
        if system_ram_gb <= 8 && d.size_mb > 400 {
            score -= 20.0;
        }
        // Prefer installed models slightly (instant use beats download wait).
        if is_model_installed(d) {
            score += 3.0;
        }
        match &best {
            Some((_, s)) if *s >= score => {}
            _ => best = Some((d, score)),
        }
    }
    match best {
        Some((d, s)) => (
            Some(d.name.clone()),
            format!(
                "CPU path ({} GB RAM{}) — {} scores highest ({:.0}): accuracy {} / speed {} / {}MB{}.",
                system_ram_gb,
                if want_non_english {
                    format!(", language {}", selected_language)
                } else {
                    String::new()
                },
                d.name,
                s,
                d.accuracy,
                d.speed,
                d.size_mb,
                if is_model_installed(d) {
                    ", already installed"
                } else {
                    ", 1-click download"
                }
            ),
        ),
        None => (None, "No candidate models.".to_string()),
    }
}

/// Map a UI language name to a whisper.cpp `-l` code. "Auto" and unknown
/// names return None (caller omits `-l` for auto-detect).
pub fn whisper_code(display: &str) -> Option<&'static str> {
    Some(match display {
        "English" => "en",
        "German" => "de",
        "French" => "fr",
        "Spanish" => "es",
        "Italian" => "it",
        "Portuguese" => "pt",
        "Dutch" => "nl",
        "Polish" => "pl",
        "Russian" => "ru",
        "Turkish" => "tr",
        "Arabic" => "ar",
        "Hindi" => "hi",
        "Chinese" => "zh",
        "Japanese" => "ja",
        _ => return None,
    })
}
