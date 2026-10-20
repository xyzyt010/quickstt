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
    /// Fermion Research Phonon-2 0.6B (weights CC-BY-4.0; NVIDIA Parakeet
    /// TDT 0.6B v3 lineage). English-only, CPU — runs on the same ONNX
    /// transducer runtime (parakeet_engine/transcribe-rs) as ParakeetRust,
    /// fed by the community ONNX export (pinned revision) in the onnx-asr
    /// nemo-conformer-tdt layout.
    Phonon2,
    /// FunAudioLLM SenseVoice Small (INT8 CTC; zh/en/ja/ko/yue with auto
    /// language detect). Rides the same persistent `parakeet_engine` worker
    /// as Parakeet/Phonon (the load request's `engine` field picks the
    /// transcribe-rs loader) — OS-identical files: Windows/Linux today,
    /// Android when the worker ports.
    SenseVoice,
    Vosk,
    Nemotron,
    /// Alibaba Qwen3-ASR 0.6B via the same Handy transcribe.cpp worker as
    /// Nemotron (GGUF, offline). 30 languages with in-model auto language
    /// detect; this port accepts NULL (auto) only — explicit hints are
    /// rejected upstream, so the UI offers "Auto" alone.
    Qwen3Asr,
    /// Cactus Whistle (Apache-2.0): a single 16.9 MB `.cact` container run by
    /// the Cactus `needle` CLI. Offline, CPU-only, 7 languages with language
    /// detection. One-shot process per utterance (no idle RAM to release).
    Whistle,
    /// Handy transcribe.cpp GGUF worker (same JSON protocol as Nemotron):
    /// Canary 180M Flash. CPU Q5_K_M per spec; GPU F16/Q8 via same worker.
    Canary,
    /// Moondream Photon (parakeet-redux / parakeet-ultra) via the persistent
    /// `tools/photon/photon_worker.py` helper: same JSON load/transcribe
    /// protocol as parakeet_engine. Needs Python + `pip install moondream`.
    Photon,
    /// NVIDIA Canary-Qwen 2.5B (NeMo SALM) via the persistent
    /// `tools/canary/canary_worker.py` helper: same JSON load/transcribe
    /// protocol as Photon. Needs an NVIDIA discrete GPU (8GB+ VRAM),
    /// Python 3.10+ with CUDA torch, and `pip install "nemo_toolkit[asr]"`.
    /// Weights (~5GB) auto-fetch from HuggingFace into the shared HF cache
    /// on first GPU load — never bundled, never fetched by our downloader.
    CanaryQwen,
}

impl EngineFamily {
    pub fn display_name(&self) -> &'static str {
        match self {
            EngineFamily::WhisperCpp => "whisper.cpp",
            EngineFamily::SherpaOnnx => "sherpa-onnx",
            EngineFamily::NemoTransducer => "nemo-transducer",
            EngineFamily::NemoCTC => "nemo-ctc",
            EngineFamily::ParakeetRust => "parakeet-rust",
            EngineFamily::Phonon2 => "phonon2",
            EngineFamily::SenseVoice => "sensevoice",
            EngineFamily::Vosk => "vosk",
            EngineFamily::Nemotron => "nemotron",
            EngineFamily::Qwen3Asr => "qwen3-asr",
            EngineFamily::Whistle => "cactus-whistle",
            EngineFamily::Canary => "canary",
            EngineFamily::Photon => "photon",
            EngineFamily::CanaryQwen => "canary-qwen",
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
        // Phonon-2 — Fermion Research (weights CC-BY-4.0; attribution chain:
        // NVIDIA Parakeet TDT 0.6B v3 → Fermion Research Phonon-2 → community
        // ONNX conversion by Tiyuvta). 0.6B, 5.21% WER on seven public test
        // sets; the ONNX int8 export runs on the SAME parakeet_engine
        // runtime as the Parakeet card above (Windows/Linux CPU, identical
        // files on every OS). NOT downloaded by default — user-initiated.
        ModelDescriptor {
            name: "Phonon-2 0.6B INT8 (ONNX/Rust)".into(),
            engine_family: EngineFamily::Phonon2,
            model_dir: "phonon2/phonon-2-0.6b-int8".into(),
            size_mb: 688,
            widget_selectable: true,
            accuracy: 92,
            speed: 74,
            blurb: "Fermion Research Phonon-2 — 5.21% WER, CC-BY-4.0".into(),
            gpu_only: false,
        },
        // SenseVoice Small — FunAudioLLM/Xiaomi via the sherpa-onnx int8 ONNX
        // export (Apache-2.0 chain via FunASR). 5 languages with in-model
        // auto language detect; a single CTC forward pass makes it the fastest
        // multilingual option here. NOT downloaded by default — user-initiated.
        ModelDescriptor {
            name: "SenseVoice Small Multilingual INT8 (ONNX/Rust)".into(),
            engine_family: EngineFamily::SenseVoice,
            model_dir: "sensevoice/small_multilingual".into(),
            size_mb: 229,
            widget_selectable: true,
            accuracy: 84,
            speed: 90,
            blurb: "5-language CTC (zh/en/ja/ko/yue) with auto language detect".into(),
            gpu_only: false,
        },
        // Qwen3-ASR 0.6B — Alibaba (Apache-2.0), transcribe.cpp GGUF port by
        // Handy. Offline multilingual (30 languages) with in-model language
        // detection. Q4_K_M (562 MiB) is the CPU default; the port accepts
        // auto-detect only, so the UI language list is just "Auto".
        ModelDescriptor {
            name: "Qwen3-ASR 0.6B Q4_K_M (GGUF)".into(),
            engine_family: EngineFamily::Qwen3Asr,
            model_dir: "qwen3_asr/qwen3-0.6b-q4km".into(),
            size_mb: 562,
            widget_selectable: true,
            accuracy: 94,
            speed: 80,
            blurb: "30-language offline ASR with auto language detect (Qwen3-ASR)".into(),
            gpu_only: false,
        },
        // Cactus Whistle — Cactus Compute (Apache-2.0). A single 16.9 MB
        // `.cact` container on the Cactus `needle` CPU engine; 7 languages
        // (en/de/fr/es/it/nl/pl) with detection. One-shot CLI per utterance,
        // so it never holds RAM while idle.
        ModelDescriptor {
            name: "Cactus Whistle 16M (CACT)".into(),
            engine_family: EngineFamily::Whistle,
            model_dir: "whistle/whistle-16m".into(),
            size_mb: 16,
            widget_selectable: true,
            accuracy: 82,
            speed: 88,
            blurb: "16.9 MB multilingual ASR — 7 languages, auto detect, CPU".into(),
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
        // GPU-only per request: NVIDIA Canary-Qwen 2.5B (NeMo SALM, CC-BY-4.0)
        // — SOTA English ASR at 418 RTFx on data-center GPUs. Shown only when
        // a discrete NVIDIA GPU is present. Needs Python 3.10+ with CUDA
        // torch + `pip install "nemo_toolkit[asr]"` (8GB+ VRAM recommended:
        // 2.5B BF16 weights alone are ~5GB). Weights auto-fetch from
        // HuggingFace on first load — our downloader never fetches them, and
        // NOTHING is downloaded by integrating this card.
        ModelDescriptor {
            name: "NVIDIA Canary-Qwen 2.5B (NeMo, GPU)".into(),
            engine_family: EngineFamily::CanaryQwen,
            model_dir: "canary_qwen/canary-qwen-2.5b".into(),
            size_mb: 5000,
            widget_selectable: true,
            accuracy: 95,
            speed: 70,
            blurb: "SOTA English ASR. Needs NVIDIA GPU + NeMo (see card); weights auto-fetch on first run".into(),
            gpu_only: true,
        },
    ]
}

pub fn is_streaming_model(name: &str) -> bool {
    let n = name.to_lowercase();
    // Canary-Qwen is per-utterance (paste after the window) even though its
    // name contains "canary" like the streaming Canary-180M GGUF worker.
    if n.contains("canary-qwen") || n.contains("canary_qwen") {
        return false;
    }
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
        EngineFamily::Qwen3Asr => {
            // Any complete Qwen3-ASR quant counts; Q4_K_M is the download
            // default. Partial downloads must show "[Not Installed]".
            let complete = |name: &str, min: u64| {
                model_path
                    .join(name)
                    .metadata()
                    .map(|m| m.len() >= min)
                    .unwrap_or(false)
            };
            complete("Qwen3-ASR-0.6B-Q4_K_M.gguf", 580_000_000)
                || complete("Qwen3-ASR-0.6B-Q5_K_M.gguf", 630_000_000)
                || complete("Qwen3-ASR-0.6B-Q6_K.gguf", 680_000_000)
                || complete("Qwen3-ASR-0.6B-Q8_0.gguf", 840_000_000)
                || complete("Qwen3-ASR-0.6B-F16.gguf", 1_560_000_000)
                || complete("Qwen3-ASR-0.6B-BF16.gguf", 1_550_000_000)
        }
        EngineFamily::Whistle => {
            // Single-file container: 16,919,407 B for the shipped Whistle.
            // A truncation must show "[Not Installed]" so the card offers a
            // clean re-download.
            model_path
                .join("whistle.cact")
                .metadata()
                .map(|m| m.len() >= 16_000_000)
                .unwrap_or(false)
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
        EngineFamily::Phonon2 => {
            // Full quartet, non-truncated: a partial download must show
            // "[Not Installed]" so the card offers a clean re-download.
            let complete = |name: &str, min: u64| {
                model_path
                    .join(name)
                    .metadata()
                    .map(|m| m.len() >= min)
                    .unwrap_or(false)
            };
            complete("encoder-model.int8.onnx", 500_000_000)
                && complete("decoder_joint-model.int8.onnx", 60_000_000)
                && complete("nemo128.onnx", 1_000_000)
                && complete("vocab.txt", 80_000)
        }
        EngineFamily::SenseVoice => {
            // The int8 export (239MB) or the fp32 fallback (937MB) plus the
            // token table — either weight file loads (resolve_model_path
            // prefers .int8.onnx), but tokens.txt is mandatory.
            let weights = model_path
                .join("model.int8.onnx")
                .metadata()
                .map(|m| m.len() >= 230_000_000)
                .unwrap_or(false)
                || model_path
                    .join("model.onnx")
                    .metadata()
                    .map(|m| m.len() >= 900_000_000)
                    .unwrap_or(false);
            weights
                && model_path
                    .join("tokens.txt")
                    .metadata()
                    .map(|m| m.len() >= 200_000)
                    .unwrap_or(false)
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
        EngineFamily::CanaryQwen => {
            // NeMo weights live in the shared HuggingFace cache (fetched by
            // NeMo itself on first GPU load, never by our downloader): a
            // populated snapshot for the repo counts as installed.
            hf_snapshot_present("nvidia--canary-qwen-2.5b")
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

/// True when the shared HuggingFace cache holds a populated snapshot for a
/// repo (`models--<org>--<name>/snapshots/<rev>/` non-empty). Used for
/// runtimes (NeMo) that fetch their own weights outside our downloader.
fn hf_snapshot_present(repo_cache: &str) -> bool {
    // Candidate hub roots, first hit wins: HF_HOME override, then the
    // huggingface_hub default (~/.cache on every OS, including Windows),
    // then the platform cache dir as a last resort.
    let mut hubs = Vec::new();
    if let Some(hf_home) = std::env::var_os("HF_HOME") {
        hubs.push(PathBuf::from(hf_home));
    }
    if let Some(home) = dirs::home_dir() {
        hubs.push(home.join(".cache").join("huggingface").join("hub"));
    }
    hubs.push(dirs::cache_dir().unwrap_or_else(std::env::temp_dir).join("huggingface").join("hub"));
    hubs.into_iter().any(|hub| {
        let snaps = hub.join(format!("models--{repo_cache}")).join("snapshots");
        match std::fs::read_dir(snaps) {
            Ok(entries) => entries.flatten().any(|e| {
                e.path().is_dir()
                    && std::fs::read_dir(e.path())
                        .map(|mut it| it.next().is_some())
                        .unwrap_or(false)
            }),
            Err(_) => false,
        }
    })
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
    // SenseVoice: exactly the model's own language tokens — Auto first so
    // the in-model detector gets a chance to pick the right one per segment.
    if matches!(desc.engine_family, EngineFamily::SenseVoice) {
        return ["Auto", "English", "Chinese", "Japanese", "Korean", "Cantonese"]
            .iter()
            .map(|s| s.to_string())
            .collect();
    }
    // Qwen3-ASR auto-detects among 30 languages; this transcribe.cpp port
    // rejects explicit hints (TRANSCRIBE_ERR_UNSUPPORTED_LANGUAGE), so "Auto"
    // is the only valid selection.
    if matches!(desc.engine_family, EngineFamily::Qwen3Asr) {
        return vec!["Auto".to_string()];
    }
    if matches!(desc.engine_family, EngineFamily::Whistle) {
        return ["Auto", "English", "German", "French", "Spanish", "Italian", "Dutch", "Polish"]
            .iter()
            .map(|s| s.to_string())
            .collect();
    }
    // Canary 180M Flash: EN/DE/FR/ES (+ translation). First entry is default.
    if matches!(desc.engine_family, EngineFamily::Canary) {
        return vec!["English", "German", "French", "Spanish"]
            .into_iter()
            .map(|s| s.to_string())
            .collect();
    }
    // Nemotron streaming + Parakeet/Vosk/Moonshine/Photon/Canary-Qwen
    // checkpoints ship English-only weights.
    if matches!(
        desc.engine_family,
        EngineFamily::ParakeetRust
            | EngineFamily::Phonon2
            | EngineFamily::Vosk
            | EngineFamily::SherpaOnnx
            | EngineFamily::NemoCTC
            | EngineFamily::NemoTransducer
            | EngineFamily::Nemotron
            | EngineFamily::Photon
            | EngineFamily::CanaryQwen
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

/// Hardware-aware model list: discrete-GPU machines see the two Whisper GPU
/// models (Tiny + Large Turbo) plus Canary-Qwen 2.5B (NeMo, NVIDIA-only)
/// and Phonon-2 (CPU engine, but still selectable on GPU boxes); CPU/iGPU
/// machines see every non-gpu_only model. GPU-only models are hidden on CPU
/// machines so nobody downloads gigabytes they cannot run.
pub fn models_for_hardware(
    has_discrete_gpu: bool,
) -> Vec<ModelDescriptor> {
    all_descriptors()
        .into_iter()
        .filter(|d| {
            if has_discrete_gpu {
                // GPU path: Whisper Tiny + Large Turbo + Canary-Qwen, plus
                // Phonon-2 (CPU engine; most accurate compact English model
                // and stays selectable on GPU boxes) and Qwen3-ASR (CPU
                // offline multilingual).
                d.name.contains("Whisper Tiny EN Q5_1")
                    || d.name.contains("Whisper Large v3 Turbo")
                    || d.name.contains("Canary-Qwen")
                    || d.name.contains("Phonon-2")
                    || d.name.contains("Qwen3-ASR")
                    || d.name.contains("Whistle")
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn canary_qwen_is_non_streaming_gpu_only() {
        // The name contains "canary" like the streaming Canary-180M GGUF
        // worker — the explicit override must win (paste after the window).
        assert!(!is_streaming_model("NVIDIA Canary-Qwen 2.5B (NeMo, GPU)"));
        assert!(is_streaming_model("Canary 180M Flash Q5_K_M (GGUF)"));
        let qwen = all_descriptors()
            .into_iter()
            .find(|d| matches!(d.engine_family, EngineFamily::CanaryQwen))
            .expect("CanaryQwen descriptor registered");
        assert!(qwen.gpu_only);
        assert_eq!(supported_languages(&qwen), vec!["English".to_string()]);
    }

    #[test]
    fn canary_qwen_visible_only_on_discrete_gpu() {
        let gpu: Vec<String> = models_for_hardware(true)
            .into_iter()
            .map(|d| d.name)
            .collect();
        assert!(gpu.iter().any(|n| n.contains("Canary-Qwen")));
        assert!(gpu.iter().any(|n| n.contains("Whisper Large v3 Turbo")));
        let cpu: Vec<String> = models_for_hardware(false)
            .into_iter()
            .map(|d| d.name)
            .collect();
        assert!(!cpu.iter().any(|n| n.contains("Canary-Qwen")));
        assert!(!cpu.iter().any(|n| n.contains("Large v3 Turbo")));
    }

    #[test]
    fn phonon2_registered_and_visible_everywhere() {
        let p = all_descriptors()
            .into_iter()
            .find(|d| matches!(d.engine_family, EngineFamily::Phonon2))
            .expect("Phonon2 descriptor registered");
        assert!(!p.gpu_only, "Phonon-2 runs on CPU");
        assert_eq!(supported_languages(&p), vec!["English".to_string()]);
        assert_eq!(p.engine_family.display_name(), "phonon2");
        // Selectable on CPU boxes AND on discrete-GPU boxes (the machine
        // class this model was primarily added for).
        assert!(models_for_hardware(false).iter().any(|d| d.name == p.name));
        assert!(models_for_hardware(true).iter().any(|d| d.name == p.name));
    }

    #[test]
    fn qwen3_asr_registered_offline_and_auto_only() {
        let q = all_descriptors()
            .into_iter()
            .find(|d| matches!(d.engine_family, EngineFamily::Qwen3Asr))
            .expect("Qwen3Asr descriptor registered");
        assert!(!q.gpu_only, "Qwen3-ASR runs on CPU");
        assert_eq!(q.engine_family.display_name(), "qwen3-asr");
        // The transcribe.cpp port rejects explicit language hints, so the UI
        // must expose Auto only (no unsupported-language failures).
        assert_eq!(supported_languages(&q), vec!["Auto".to_string()]);
        // Offline per-utterance model, never the streaming/paste-after path.
        assert!(!is_streaming_model(&q.name));
        assert!(models_for_hardware(false).iter().any(|d| d.name == q.name));
        assert!(models_for_hardware(true).iter().any(|d| d.name == q.name));
    }

    #[test]
    fn whistle_registered_multilingual_and_cpu_visible() {
        let w = all_descriptors()
            .into_iter()
            .find(|d| matches!(d.engine_family, EngineFamily::Whistle))
            .expect("Whistle descriptor registered");
        assert!(!w.gpu_only, "Whistle runs on CPU");
        assert_eq!(w.engine_family.display_name(), "cactus-whistle");
        assert_eq!(
            supported_languages(&w),
            vec![
                "Auto".to_string(),
                "English".to_string(),
                "German".to_string(),
                "French".to_string(),
                "Spanish".to_string(),
                "Italian".to_string(),
                "Dutch".to_string(),
                "Polish".to_string(),
            ]
        );
        assert!(!is_streaming_model(&w.name));
        assert!(models_for_hardware(false).iter().any(|d| d.name == w.name));
        assert!(models_for_hardware(true).iter().any(|d| d.name == w.name));
    }

    #[test]
    fn sensevoice_registered_multilingual_and_cpu_visible() {
        let s = all_descriptors()
            .into_iter()
            .find(|d| matches!(d.engine_family, EngineFamily::SenseVoice))
            .expect("SenseVoice descriptor registered");
        assert!(!s.gpu_only, "SenseVoice runs on CPU");
        assert_eq!(s.engine_family.display_name(), "sensevoice");
        // Its own language tokens (Auto first), not the English-only list.
        assert_eq!(
            supported_languages(&s),
            vec![
                "Auto".to_string(),
                "English".to_string(),
                "Chinese".to_string(),
                "Japanese".to_string(),
                "Korean".to_string(),
                "Cantonese".to_string(),
            ]
        );
        // Listed on CPU/iGPU boxes like every other CPU engine (Parakeet,
        // Moonshine, Nemotron…). Discrete-GPU boxes use the curated
        // allow-list in models_for_hardware(true); this model joins it only
        // if explicitly requested there — never by accident.
        assert!(models_for_hardware(false).iter().any(|d| d.name == s.name));
        // Per-utterance CTC — never treated as a streaming/paste-after model.
        assert!(!is_streaming_model(&s.name));
    }
}
