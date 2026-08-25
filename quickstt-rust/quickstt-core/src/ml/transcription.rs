use anyhow::Result;
use whisper_rs::{WhisperContext, WhisperContextParameters, WhisperState};

/// Wrapper for whisper-rs to handle local STT natively in Rust using whisper.cpp under the hood.
/// As per architecture constraints, this is lazy-loaded strictly on activation to conserve idle RAM.
pub struct TranscriptionEngine {
    context: WhisperContext,
}

impl TranscriptionEngine {
    /// Loads the whisper model from disk.
    /// Should only be called AFTER a wakeword is detected.
    pub fn load(model_path: &str) -> Result<Self> {
        let params = WhisperContextParameters::default();
        let context = WhisperContext::new_with_params(model_path, params)?;
        Ok(Self { context })
    }

    /// Transcribes a buffer of audio (16kHz f32 mono) and returns the text.
    pub fn transcribe(&self, audio_data: &[f32]) -> Result<String> {
        let mut state = self.context.create_state()?;
        let mut params =
            whisper_rs::FullParams::new(whisper_rs::SamplingStrategy::Greedy { best_of: 1 });
        params.set_language(Some("en"));
        params.set_print_progress(false);
        params.set_print_special(false);
        params.set_print_realtime(false);
        params.set_print_timestamps(false);

        state.full(params, audio_data)?;

        let mut result_text = String::new();
        let num_segments = state.full_n_segments()?;
        for i in 0..num_segments {
            if let Ok(segment) = state.full_get_segment_text(i) {
                result_text.push_str(&segment);
            }
        }

        Ok(result_text)
    }
}

/// Explicit drop to ensure memory is released when going back to idle state.
impl Drop for TranscriptionEngine {
    fn drop(&mut self) {
        // WhisperContext is dropped, releasing the GGML model memory back to the OS.
        tracing::debug!("TranscriptionEngine dropped, model unloaded to conserve RAM.");
    }
}
