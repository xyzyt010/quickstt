use crate::audio::vad::{self, EnergyVad};
use std::collections::VecDeque;
use std::path::PathBuf;

const PREROLL_MS: usize = 500;
const MAX_UTTERANCE_SECS: f32 = 12.0;
const SILENCE_THRESHOLD_MS: usize = 800;
const HARD_SILENCE_MS: usize = 200;
const SAMPLE_RATE: usize = 16000;

pub struct SpeechSegmenter {
    vad: EnergyVad,
    preroll: VecDeque<Vec<i16>>,
    preroll_max_chunks: usize,
    utterance: Vec<i16>,
    in_utterance: bool,
    silence_frames: usize,
    hard_silence_frames: usize,
    cache_dir: PathBuf,
}

pub enum SegmenterEvent {
    SpeechStarted,
    SpeechContinues,
    UtteranceComplete(PathBuf),
}

impl SpeechSegmenter {
    pub fn new(cache_dir: PathBuf) -> Self {
        let chunks_per_ms = SAMPLE_RATE / 1280;
        let preroll_chunks = (PREROLL_MS * chunks_per_ms) / 1000;
        Self {
            vad: EnergyVad::with_default_threshold(),
            preroll: VecDeque::with_capacity(preroll_chunks + 2),
            preroll_max_chunks: preroll_chunks.max(1),
            utterance: Vec::new(),
            in_utterance: false,
            silence_frames: 0,
            hard_silence_frames: 0,
            cache_dir,
        }
    }

    pub fn reset(&mut self) {
        self.vad.reset();
        self.preroll.clear();
        self.utterance.clear();
        self.in_utterance = false;
        self.silence_frames = 0;
        self.hard_silence_frames = 0;
    }

    pub fn process_chunk(&mut self, chunk: &[i16]) -> Option<SegmenterEvent> {
        let vad_result = self.vad.process(chunk);
        let level = vad::audio_level_0_100(chunk);

        if !self.in_utterance {
            self.preroll.push_back(chunk.to_vec());
            while self.preroll.len() > self.preroll_max_chunks {
                self.preroll.pop_front();
            }

            if vad_result.speech_likely && level >= 2 {
                self.in_utterance = true;
                for pr in self.preroll.drain(..) {
                    self.utterance.extend_from_slice(&pr);
                }
                self.utterance.extend_from_slice(chunk);
                self.silence_frames = 0;
                self.hard_silence_frames = 0;
                return Some(SegmenterEvent::SpeechStarted);
            }
            return None;
        }

        self.utterance.extend_from_slice(chunk);

        let utterance_secs = self.utterance.len() as f32 / SAMPLE_RATE as f32;
        let chunk_ms = (chunk.len() * 1000) / SAMPLE_RATE;

        if !vad_result.speech_likely {
            self.silence_frames += chunk_ms;
        } else {
            self.silence_frames = 0;
        }

        if level <= 2 {
            self.hard_silence_frames += chunk_ms;
        } else {
            self.hard_silence_frames = 0;
        }

        let max_reached = utterance_secs >= MAX_UTTERANCE_SECS;
        let silence_reached = utterance_secs > 0.5 && self.silence_frames >= SILENCE_THRESHOLD_MS;
        let hard_silence = self.hard_silence_frames >= HARD_SILENCE_MS;

        if max_reached || silence_reached || hard_silence {
            let wav_path = self.write_wav();
            self.utterance.clear();
            self.in_utterance = false;
            self.silence_frames = 0;
            self.hard_silence_frames = 0;
            return Some(SegmenterEvent::UtteranceComplete(wav_path));
        }

        Some(SegmenterEvent::SpeechContinues)
    }

    fn write_wav(&self) -> PathBuf {
        let ts = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis();
        let path = self.cache_dir.join(format!("stt_{}.wav", ts));

        std::fs::create_dir_all(&self.cache_dir).ok();

        let spec = hound::WavSpec {
            channels: 1,
            sample_rate: SAMPLE_RATE as u32,
            bits_per_sample: 16,
            sample_format: hound::SampleFormat::Int,
        };

        if let Ok(mut writer) = hound::WavWriter::create(&path, spec) {
            for &sample in &self.utterance {
                writer.write_sample(sample).ok();
            }
            writer.finalize().ok();
        }

        path
    }
}
