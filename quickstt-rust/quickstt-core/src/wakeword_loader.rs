use serde::Deserialize;
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, Deserialize)]
pub struct WakeWordConfig {
    pub name: String,
    pub phrase: String,
    pub sample_rate: u32,
    pub threshold: f32,
}

#[derive(Debug, Clone)]
pub struct WakeWordModelInfo {
    pub config: WakeWordConfig,
    pub onnx_path: PathBuf,
}

pub fn discover_models(models_dir: &Path) -> Vec<WakeWordModelInfo> {
    let mut models = Vec::new();

    let entries = match std::fs::read_dir(models_dir) {
        Ok(entries) => entries,
        Err(e) => {
            tracing::warn!("Could not read wakeword_models dir {:?}: {}", models_dir, e);
            return models;
        }
    };

    for entry in entries.flatten() {
        let path = entry.path();
        if !path.is_dir() {
            continue;
        }

        let config_path = path.join("config.json");
        let onnx_path = path.join("model.onnx");

        if !config_path.exists() || !onnx_path.exists() {
            tracing::warn!("Skipping {:?}: missing config.json or model.onnx", path);
            continue;
        }

        match std::fs::read_to_string(&config_path) {
            Ok(json_str) => match serde_json::from_str::<WakeWordConfig>(&json_str) {
                Ok(config) => {
                    tracing::info!(
                        "Discovered wakeword model: {} (phrase: {:?})",
                        config.name,
                        config.phrase
                    );
                    models.push(WakeWordModelInfo { config, onnx_path });
                }
                Err(e) => tracing::warn!("Invalid config.json in {:?}: {}", path, e),
            },
            Err(e) => tracing::warn!("Could not read config.json in {:?}: {}", path, e),
        }
    }

    models.sort_by(|a, b| a.config.name.cmp(&b.config.name));
    models
}

/// Community openWakeWord heads (`hey_jarvis_v0.1.onnx`, `alexa_v0.1.onnx`)
/// sitting at the models-dir root. Returns (path, head_name, phrase).
///
/// NOTE: the legacy per-phrase `model.onnx` subdirectories (custom
/// WakeWordNet heads) are deliberately NOT loaded: the discrimination probe
/// proved they answer ~0.5 to digital silence, so no threshold can separate
/// wakewords from nothing. They stay on disk untouched for a future
/// retraining path, but never enter the live pipeline.
pub fn discover_oww_heads(models_dir: &Path) -> Vec<(PathBuf, String, String)> {
    const HEADS: &[(&str, &str, &str)] = &[
        ("hey_jarvis_v0.1.onnx", "hey_jarvis", "hey jarvis"),
        ("alexa_v0.1.onnx", "alexa", "alexa"),
    ];
    let mut out = Vec::new();
    for (file, name, phrase) in HEADS {
        let path = models_dir.join(file);
        if path.exists() {
            tracing::info!("Discovered OWW head: {} (phrase: {:?})", name, phrase);
            out.push((path, name.to_string(), phrase.to_string()));
        } else {
            tracing::warn!("Missing OWW head: {}", path.display());
        }
    }
    out
}

pub fn default_models_dir() -> PathBuf {
    if let Ok(exe_path) = std::env::current_exe() {
        if let Some(exe_dir) = exe_path.parent() {
            let primary = exe_dir.join("wakeword_models");
            if primary.exists() {
                return primary;
            }
            // deb layout: models live under /usr/lib/quickstt
            let system = PathBuf::from("/usr/lib/quickstt/wakeword_models");
            if system.exists() {
                return system;
            }
            if exe_dir.ends_with("debug") || exe_dir.ends_with("release") {
                if let Some(ws_root) = exe_dir.parent().and_then(|p| p.parent()) {
                    let fallback = ws_root.join("wakeword_models");
                    if fallback.exists() {
                        return fallback;
                    }
                    // Workspace-root fallback: dev/resource trees keep the
                    // community OWW heads at the repo root (quickstt-rust is
                    // one level below it), not under quickstt-rust/.
                    if let Some(repo_root) = ws_root.parent() {
                        let root_fallback = repo_root.join("wakeword_models");
                        if root_fallback.exists() {
                            return root_fallback;
                        }
                    }
                }
            }
        }
    }
    PathBuf::from("wakeword_models")
}
