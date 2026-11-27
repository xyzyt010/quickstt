//! Cloud transcription providers (desktop parity: Groq / OpenAI / Deepgram).
//!
//! Catalog rule, mirroring the desktop model menu: an API model is
//! selectable **only** when its provider has a non-empty stored key, and a
//! local model is selectable **only** when downloaded. Keys never live in
//! Rust or in plain preferences — they are held by the Java helper
//! `ai.quickstt.app.ApiClient`, encrypted with an AndroidKeyStore key.
//! This module is the pure, host-testable catalog + selection logic; the
//! HTTPS call itself goes through `platform::api_transcribe` (Java
//! `HttpURLConnection`, same stack as model downloads).

pub struct Provider {
    pub id: &'static str,
    pub name: &'static str,
    pub models: &'static [&'static str],
    /// OpenAI-compatible multipart endpoint (Groq, OpenAI). Deepgram differs.
    pub openai_compatible: bool,
}

pub const PROVIDERS: &[Provider] = &[
    Provider {
        id: "groq",
        name: "Groq",
        models: &["whisper-large-v3-turbo", "whisper-large-v3"],
        openai_compatible: true,
    },
    Provider {
        id: "openai",
        name: "OpenAI",
        models: &["gpt-4o-mini-transcribe", "whisper-1"],
        openai_compatible: true,
    },
    Provider {
        id: "deepgram",
        name: "Deepgram",
        models: &["nova-3", "nova-2"],
        openai_compatible: false,
    },
];

pub fn provider(id: &str) -> Option<&'static Provider> {
    PROVIDERS.iter().find(|p| p.id == id)
}

/// A local model may be used only when its file is on disk.
pub fn local_selectable(installed: bool) -> bool {
    installed
}

/// An API model may be used only when the provider key is non-empty.
/// `has_key` must come from the Keystore-backed store, never from UI text.
pub fn api_selectable(provider_id: &str, model_id: &str, has_key: bool) -> bool {
    has_key
        && provider(provider_id)
            .map(|p| p.models.contains(&model_id))
            .unwrap_or(false)
}

/// Why a model cannot be picked, for the one-line status. Empty = pickable.
pub fn unselectable_reason(is_api: bool, installed: bool, has_key: bool) -> &'static str {
    if is_api {
        if has_key {
            ""
        } else {
            "Connect the provider with an API key first"
        }
    } else if installed {
        ""
    } else {
        "Download the model first"
    }
}

/// Languages offered by the picker (ISO codes the engines/APIs accept).
pub const LANGUAGES: &[(&str, &str)] = &[
    ("en", "English"),
    ("auto", "Auto-detect"),
    ("es", "Spanish"),
    ("fr", "French"),
    ("de", "German"),
    ("it", "Italian"),
    ("pt", "Portuguese"),
    ("nl", "Dutch"),
    ("ja", "Japanese"),
    ("ko", "Korean"),
    ("zh", "Chinese"),
    ("hi", "Hindi"),
];

pub fn language_index(code: &str) -> usize {
    LANGUAGES.iter().position(|(c, _)| *c == code).unwrap_or(0)
}

/// Idle-offload choices offered by the picker, in seconds.
pub const OFFLOAD_CHOICES: &[u64] = &[15, 30, 60, 120, 300, 900];

pub fn offload_index(secs: u64) -> usize {
    OFFLOAD_CHOICES.iter().position(|s| *s == secs).unwrap_or(1)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn api_models_require_a_key() {
        assert!(!api_selectable("groq", "whisper-large-v3-turbo", false));
        assert!(api_selectable("groq", "whisper-large-v3-turbo", true));
        assert!(!api_selectable("groq", "not-a-model", true));
        assert!(!api_selectable("bogus", "whisper-1", true));
        assert!(!local_selectable(false));
        assert!(local_selectable(true));
    }

    #[test]
    fn providers_have_unique_models() {
        for p in PROVIDERS {
            assert!(!p.models.is_empty(), "{} has no models", p.name);
            for m in p.models {
                assert!(!m.is_empty());
            }
        }
        assert!(provider("groq").unwrap().openai_compatible);
        assert!(!provider("deepgram").unwrap().openai_compatible);
        assert_eq!(unselectable_reason(true, false, false), "Connect the provider with an API key first");
        assert_eq!(unselectable_reason(false, false, false), "Download the model first");
        assert_eq!(unselectable_reason(false, true, false), "");
    }
}
