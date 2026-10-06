//! Community openWakeWord runtime (scratch redesign).
//!
//! The retired custom WakeWordNet heads answered ~0.5 to DIGITAL SILENCE
//! (see `wakeword_discrimination_probe`), so no threshold could ever separate
//! wakewords from nothing. This backend runs the community-trained
//! openWakeWord classifier heads (dscripka/openWakeWord v0.5.1 release —
//! `hey_jarvis_v0.1.onnx`, `alexa_v0.1.onnx`, CC BY-NC-SA 4.0, personal use;
//! see `HEADS.md`) on the shared mel→embedding frontend vendored from
//! livekit-wakeword 0.1.3 (Apache-2.0 — see `oww_assets/ATTRIBUTION.md`).
//!
//! Pipeline per inference (mirrors livekit-wakeword semantics): trailing
//! 2.4s of 16kHz mono f32 → melspectrogram ONNX (32 bins) → 76-frame windows
//! at stride 8 → embedding ONNX (96-dim each) → trailing 16 embeddings as
//! (1,16,96) → each head positionally → scalar probability in [0,1].
//! Community heads are trained for a 0.5 default threshold with <0.5/hour
//! false accepts on hours of continuous speech — real discrimination.

use anyhow::{Context, Result};
use ndarray::{Array, Array1, Array2, Axis};
use ort::session::Session;
use ort::value::Tensor;
use std::collections::VecDeque;

const MEL_BYTES: &[u8] = include_bytes!("oww_assets/melspectrogram.onnx");
const EMB_BYTES: &[u8] = include_bytes!("oww_assets/embedding_model.onnx");

pub const MEL_BINS: usize = 32;
pub const EMB_DIM: usize = 96;
pub const EMB_WINDOW: usize = 76;
pub const EMB_STRIDE: usize = 8;
pub const MIN_EMBS: usize = 16;
/// Mel rows needed for a full 16-embedding window: 15*8+76.
pub const NEED_MEL_ROWS: usize = (MIN_EMBS - 1) * EMB_STRIDE + EMB_WINDOW;
/// Left context (samples) prepended to each tail mel run so fresh rows are
/// computed with settled left context. Fresh audio per production inference
/// is 2×80ms chunks (2560 samples); tail runs cover ctx + fresh.
pub const MEL_CTX: usize = 4096;
pub const MEL_CACHE_CAP: usize = 340;
pub const EMB_CACHE_CAP: usize = 64;

fn ensure_backend() {
    #[cfg(use_ort_tract)]
    {
        static INIT: std::sync::Once = std::sync::Once::new();
        INIT.call_once(|| {
            ort::set_api(ort_tract::api());
        });
    }
}

fn build_session(bytes: &[u8]) -> Result<Session> {
    ensure_backend();
    Ok(Session::builder()?.commit_from_memory(bytes)?)
}

/// Shared mel + embedding frontend.
pub struct OwwFrontend {
    mel: Session,
    emb: Session,
}

impl OwwFrontend {
    pub fn new() -> Result<Self> {
        Ok(Self {
            mel: build_session(MEL_BYTES)?,
            emb: build_session(EMB_BYTES)?,
        })
    }

    /// Raw 16kHz mono f32 in [-1, 1] → mel frames (T, 32).
    pub fn mel_frames(&mut self, samples: &[f32]) -> Result<Array2<f32>> {
        // Mel: (1, N) → (T, 32), with openWakeWord's x/10+2 post-processing.
        let audio = Array1::from_vec(samples.to_vec()).insert_axis(Axis(0));
        let tensor = Tensor::from_array(audio)?;
        let mel_out = self.mel.run(ort::inputs![tensor])?;
        let raw = mel_out
            .get("output")
            .context("mel output 'output' missing")?
            .try_extract_array::<f32>()?;
        let (rows, cols) = (raw.shape()[2], raw.shape()[3]);
        if cols != MEL_BINS {
            anyhow::bail!("unexpected mel bins: {}", cols);
        }
        let mut mel: Array2<f32> = raw.into_owned().into_shape_with_order((rows, cols))?;
        mel.mapv_inplace(|x| x / 10.0 + 2.0);
        Ok(mel)
    }

    /// Raw 16kHz mono f32 in [-1, 1] (reference length) → all 96-dim
    /// embeddings. Full-recompute reference used by the tail-equivalence
    /// test; production streams through the incremental caches.
    pub fn embeddings_for(&mut self, samples: &[f32]) -> Result<Vec<[f32; EMB_DIM]>> {
        let mel = self.mel_frames(samples)?;
        let rows = mel.shape()[0];
        let mut out = Vec::new();
        let mut start = 0;
        while start + EMB_WINDOW <= rows {
            let flat: Vec<f32> = mel
                .slice(ndarray::s![start..start + EMB_WINDOW, ..])
                .as_standard_layout()
                .as_slice()
                .context("non-contiguous mel window")?
                .to_vec();
            let input = Array::from_shape_vec((1, EMB_WINDOW, MEL_BINS, 1), flat)?;
            let tensor = Tensor::from_array(input)?;
            let res = self.emb.run(ort::inputs![tensor])?;
            let raw = res
                .get("conv2d_19")
                .context("embedding output 'conv2d_19' missing")?
                .try_extract_array::<f32>()?;
            let emb: Array1<f32> = raw.into_owned().into_shape_with_order(EMB_DIM)?;
            let slice = emb.as_slice().context("non-contiguous embedding")?;
            let mut arr = [0f32; EMB_DIM];
            arr.copy_from_slice(&slice[..EMB_DIM.min(slice.len())]);
            out.push(arr);
            start += EMB_STRIDE;
        }
        Ok(out)
    }
}

/// One community classifier head. I/O is positional (single input/output —
/// asserted at load); the scalar output is the wake probability in [0,1].
pub struct OwwHead {
    session: Session,
    output_name: String,
    pub name: String,
}

impl OwwHead {
    pub fn load(path: &std::path::Path, name: &str) -> Result<Self> {
        ensure_backend();
        let bytes =
            std::fs::read(path).with_context(|| format!("reading {}", path.display()))?;
        let session = Session::builder()?.commit_from_memory(&bytes)?;
        if session.inputs().len() != 1 || session.outputs().len() != 1 {
            anyhow::bail!(
                "head '{}' must have exactly 1 input + 1 output (got {} in, {} out)",
                name,
                session.inputs().len(),
                session.outputs().len()
            );
        }
        let output_name = session.outputs()[0].name().to_string();
        tracing::info!(
            "OwwHead '{}': in='{}' out='{}' ({})",
            name,
            session.inputs()[0].name(),
            output_name,
            path.display()
        );
        Ok(Self {
            session,
            output_name,
            name: name.to_string(),
        })
    }

    /// Score trailing embeddings (needs ≥MIN_EMBS, else 0.0).
    pub fn score(&mut self, emds: &[[f32; EMB_DIM]]) -> Result<f32> {
        if emds.len() < MIN_EMBS {
            return Ok(0.0);
        }
        let src = &emds[emds.len() - MIN_EMBS..];
        let mut flat = Vec::with_capacity(MIN_EMBS * EMB_DIM);
        for e in src {
            flat.extend_from_slice(e);
        }
        let arr = Array::from_shape_vec((1, MIN_EMBS, EMB_DIM), flat)?;
        let tensor = Tensor::from_array(arr)?;
        let out = self.session.run(ort::inputs![tensor])?;
        let out_name = self.output_name.clone();
        let raw = out
            .get(&out_name)
            .context("head output missing")?
            .try_extract_array::<f32>()?;
        Ok(raw.iter().copied().next().unwrap_or(0.0).clamp(0.0, 1.0))
    }
}

/// All loaded heads + incremental streaming caches.
///
/// Per inference the mel model runs ONLY on new audio + a small left context
/// (~6.7k samples vs 38k full-ring — the mel CNN dominates cost). Row counts
/// depend only on audio length, so the leading rows(MEL_CTX) rows are the
/// already-cached overlap and only trailing fresh rows append (by count, not
/// content — identical silence frames are still distinct time steps). Only
/// newly-completable embedding windows run (~2 per inference vs ~20).
pub struct OwwBackend {
    frontend: OwwFrontend,
    pub heads: Vec<OwwHead>,
    /// Trailing raw audio for tail mel runs (cap MEL_CTX + 2 chunks).
    ctx: VecDeque<f32>,
    /// Trailing mel rows (cap MEL_CACHE_CAP).
    mel_cache: VecDeque<[f32; MEL_BINS]>,
    /// Total mel rows ever appended (cache may trim; this stays absolute so
    /// embedding window indices never slide).
    mel_total: usize,
    /// Mel rows for exactly MEL_CTX samples — calibrated once at load by
    /// running the real model on zeros. The overlap prefix to discard.
    discard: usize,
    /// Trailing embeddings (cap EMB_CACHE_CAP).
    emb_cache: VecDeque<[f32; EMB_DIM]>,
    emb_windows_done: usize,
    infers: u32,
    /// Last inference cost split (mel_ms, emb_ms, heads_ms) — diagnostics.
    pub last_ms: (u128, u128, u128),
}

impl OwwBackend {
    pub fn new(heads: &[(std::path::PathBuf, String)]) -> Result<Self> {
        let mut loaded = Vec::with_capacity(heads.len());
        for (path, name) in heads {
            match OwwHead::load(path, name) {
                Ok(m) => loaded.push(m),
                Err(e) => tracing::warn!(
                    "Wakeword head '{}' failed to load, skipped: {}",
                    name,
                    e
                ),
            }
        }
        if loaded.is_empty() {
            anyhow::bail!("no wakeword heads could be loaded");
        }
        let mut frontend = OwwFrontend::new()?;
        // Calibrate the overlap prefix: rows depend only on length, so run
        // the real mel model once on MEL_CTX zeros. Every tail run discards
        // exactly this many leading rows (the re-covered cached audio).
        let discard = frontend
            .mel_frames(&vec![0.0f32; MEL_CTX])
            .map(|m| m.shape()[0])
            .unwrap_or(24);
        tracing::info!("OwwBackend: MEL_CTX={} → discard {} mel rows", MEL_CTX, discard);
        Ok(Self {
            frontend,
            heads: loaded,
            ctx: VecDeque::with_capacity(MEL_CTX + 2560),
            mel_cache: VecDeque::with_capacity(MEL_CACHE_CAP),
            mel_total: 0,
            discard,
            emb_cache: VecDeque::with_capacity(EMB_CACHE_CAP),
            emb_windows_done: 0,
            infers: 0,
            last_ms: (0, 0, 0),
        })
    }

    pub fn reset(&mut self) {
        self.ctx.clear();
        self.mel_cache.clear();
        self.mel_total = 0;
        self.emb_cache.clear();
        self.emb_windows_done = 0;
        self.infers = 0;
    }

    /// Buffer one mic chunk (16kHz mono i16). Call on EVERY chunk.
    pub fn push_audio(&mut self, audio: &[i16]) {
        for &s in audio {
            self.ctx.push_back(s as f32 / 32768.0);
        }
        while self.ctx.len() > MEL_CTX + 2560 {
            self.ctx.pop_front();
        }
    }

    pub fn mel_rows(&self) -> usize {
        self.mel_cache.len()
    }

    /// Buffered raw samples (diagnostics + tests: proves push_audio runs
    /// even when the energy gate skips scoring).
    pub fn buffered_samples(&self) -> usize {
        self.ctx.len()
    }

    /// Advance the mel + embedding caches over the buffered tail. Call on
    /// EVERY inference tick, gated or not: filling caches through silence
    /// and room noise is what keeps them hot, so a short phrase arriving
    /// after quiet scores from its first tick instead of spending the whole
    /// phrase building context (the old cold-start path made first attempts
    /// after silence never score). ~32ms per tick; head scoring stays gated.
    pub fn update(&mut self) {
        let t_mel = std::time::Instant::now();
        let tail: Vec<f32> = self.ctx.iter().copied().collect();
        if tail.len() < MEL_CTX + 1280 {
            // Cold start: not enough context yet.
            return;
        }
        let mel = match self.frontend.mel_frames(&tail) {
            Ok(m) => m,
            Err(e) => {
                tracing::warn!("OWW mel failed: {}", e);
                return;
            }
        };
        let rows = mel.shape()[0];
        if rows <= self.discard {
            return;
        }
        // By count, not content: the leading `discard` rows re-cover cached
        // audio (row counts depend only on length). Identical silence frames
        // are still distinct time steps and must append.
        for r in self.discard..rows {
            if self.mel_cache.len() >= MEL_CACHE_CAP {
                self.mel_cache.pop_front();
            }
            let row = mel.row(r);
            let slice = match row.as_slice() {
                Some(s) if s.len() >= MEL_BINS => s,
                _ => return,
            };
            let mut arr = [0f32; MEL_BINS];
            arr.copy_from_slice(&slice[..MEL_BINS]);
            self.mel_cache.push_back(arr);
            self.mel_total += 1;
        }
        // Consume fresh audio (keep the left context for the next tail run).
        while self.ctx.len() > MEL_CTX {
            self.ctx.pop_front();
        }
        let mel_ms = t_mel.elapsed().as_millis();
        let t_emb = std::time::Instant::now();
        // Absolute window indices: mel_total never slides, so windows never
        // double-run or stall after the cache trims.
        let total_windows = if self.mel_total >= EMB_WINDOW {
            (self.mel_total - EMB_WINDOW) / EMB_STRIDE + 1
        } else {
            0
        };
        let cache_start = self.mel_total.saturating_sub(self.mel_cache.len());
        for k in self.emb_windows_done..total_windows {
            let abs_base = k * EMB_STRIDE;
            if abs_base < cache_start || abs_base + EMB_WINDOW > self.mel_total {
                // Window scrolled out of the trimmed cache (should not happen
                // while CAP >> fresh rows, but never panic: skip forward).
                self.emb_windows_done = k + 1;
                continue;
            }
            let base = abs_base - cache_start;
            let mut flat = Vec::with_capacity(EMB_WINDOW * MEL_BINS);
            for r in 0..EMB_WINDOW {
                flat.extend_from_slice(&self.mel_cache[base + r]);
            }
            let input = match Array::from_shape_vec((1, EMB_WINDOW, MEL_BINS, 1), flat) {
                Ok(a) => a,
                Err(_) => return,
            };
            let tensor = match Tensor::from_array(input) {
                Ok(t) => t,
                Err(_) => return,
            };
            let res = match self.frontend.emb.run(ort::inputs![tensor]) {
                Ok(o) => o,
                Err(e) => {
                    tracing::warn!("OWW embedding failed: {}", e);
                    return;
                }
            };
            let extracted = res.get("conv2d_19").map(|v| v.try_extract_array::<f32>());
            let raw = match extracted {
                Some(Ok(a)) => a,
                _ => return,
            };
            let emb: Array1<f32> = match raw.into_owned().into_shape_with_order(EMB_DIM) {
                Ok(e) => e,
                Err(_) => return,
            };
            let slice = match emb.as_slice() {
                Some(s) if s.len() >= EMB_DIM => s,
                _ => return,
            };
            let mut arr = [0f32; EMB_DIM];
            arr.copy_from_slice(&slice[..EMB_DIM]);
            if self.emb_cache.len() >= EMB_CACHE_CAP {
                self.emb_cache.pop_front();
            }
            self.emb_cache.push_back(arr);
            self.emb_windows_done += 1;
        }
        let emb_ms = t_emb.elapsed().as_millis();
        self.last_ms.0 = mel_ms;
        self.last_ms.1 = emb_ms;
    }

    /// Score enabled heads on the current caches. Empty until ~2s of audio
    /// has filled the embedding cache. Gated callers skip this (saving the
    /// head runs) while still calling update() so caches stay hot.
    pub fn score_heads(&mut self, enabled: &[String]) -> Vec<(String, f32)> {        if self.emb_cache.len() < MIN_EMBS {
            return Vec::new();
        }
        self.infers += 1;
        let t_heads = std::time::Instant::now();
        let embs: Vec<[f32; EMB_DIM]> = self.emb_cache.iter().copied().collect();
        let mut out = Vec::new();
        for h in self.heads.iter_mut() {
            if !enabled.iter().any(|e| e == &h.name) {
                continue;
            }
            match h.score(&embs) {
                Ok(s) => out.push((h.name.clone(), s)),
                Err(e) => tracing::warn!("Wakeword head '{}' failed: {}", h.name, e),
            }
        }
        self.last_ms.2 = t_heads.elapsed().as_millis();
        out
    }

    /// Update caches, then score enabled heads. Tests/probe path (mirrors
    /// the gated live path minus the gates).
    pub fn score_enabled(&mut self, enabled: &[String]) -> Vec<(String, f32)> {
        self.update();
        self.score_heads(enabled)
    }

    /// Score all heads (tests/probe path).
    pub fn score_all(&mut self) -> Vec<(String, f32)> {
        let enabled: Vec<String> = self.heads.iter().map(|h| h.name.clone()).collect();
        self.score_enabled(&enabled)
    }

    pub fn infer_count(&self) -> u32 {
        self.infers
    }

    /// Cached embedding windows (diagnostics: proves caches stay hot).
    pub fn emb_cached(&self) -> usize {
        self.emb_cache.len()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    fn models_dir() -> PathBuf {
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../wakeword_models")
    }

    fn read_wav_mono16(path: &PathBuf) -> Vec<i16> {
        let reader = hound::WavReader::open(path).expect("wav readable");
        let spec = reader.spec();
        assert_eq!(spec.sample_rate, 16000, "fixture must be 16kHz");
        assert_eq!(spec.channels, 1, "fixture must be mono");
        reader
            .into_samples::<i16>()
            .map(|s| s.expect("sample"))
            .collect()
    }

    fn synth(kind: &str) -> Vec<i16> {
        const N: usize = 48000; // 3s (fills the 2.4s ring + margin)
        match kind {
            "silence" => vec![0i16; N],
            "white" => {
                let mut x: u32 = 0x12345678;
                (0..N)
                    .map(|_| {
                        x ^= x << 13;
                        x ^= x >> 17;
                        x ^= x << 5;
                        ((x >> 8) as i16 % 6000) - 3000
                    })
                    .collect()
            }
            "tone440" => (0..N)
                .map(|i| {
                    ((2.0 * std::f32::consts::PI * 440.0 * i as f32 / 16000.0).sin() * 3000.0)
                        as i16
                })
                .collect(),
            _ => vec![0i16; N],
        }
    }

    /// Tail-equivalence: incremental streaming must match full recompute on
    /// IDENTICAL trailing audio. Streams white noise chunk-by-chunk
    /// (production path), then scores the same trailing 2.4s from scratch
    /// via embeddings_for + heads, and compares. Proves the
    /// context/discard/cache plumbing is bit-honest (or exposes drift).
    #[test]
    fn oww_mel_tail_equivalence() {
        let dir = models_dir();
        let heads = vec![
            (dir.join("hey_jarvis_v0.1.onnx"), "hey_jarvis".to_string()),
            (dir.join("alexa_v0.1.onnx"), "alexa".to_string()),
        ];
        let mut x: u32 = 0xabcdef01;
        let audio: Vec<i16> = (0..48000)
            .map(|_| {
                x ^= x << 13;
                x ^= x >> 17;
                x ^= x << 5;
                ((x >> 8) as i16 % 6000) - 3000
            })
            .collect();
        let enabled = vec!["hey_jarvis".to_string(), "alexa".to_string()];

        // Production path: chunk-by-chunk, production cadence.
        let mut streamed = OwwBackend::new(&heads).expect("backend loads");
        let mut streamed_scores = Vec::new();
        for (i, chunk) in audio.chunks(1280).enumerate() {
            streamed.push_audio(chunk);
            if i % 2 == 0 {
                continue;
            }
            let s = streamed.score_enabled(&enabled);
            if !s.is_empty() {
                streamed_scores = s;
            }
        }
        assert!(!streamed_scores.is_empty(), "streamed path never scored");

        // Reference: same trailing 38400 samples, full recompute, heads scored
        // on the trailing 16 embeddings.
        let tail: Vec<f32> = audio[audio.len() - 38400..]
            .iter()
            .map(|&s| s as f32 / 32768.0)
            .collect();
        let mut frontend = OwwFrontend::new().expect("frontend loads");
        let embs = frontend.embeddings_for(&tail).expect("embeddings run");
        assert!(embs.len() >= MIN_EMBS, "reference too short");
        let mut reference_scores = Vec::new();
        for (path, name) in &heads {
            let mut head = OwwHead::load(path, name).expect("head loads");
            let s = head.score(&embs).expect("head scores");
            reference_scores.push((name.clone(), s));
        }
        for ((n1, s1), (n2, s2)) in streamed_scores.iter().zip(reference_scores.iter()) {
            assert_eq!(n1, n2);
            println!("TAIL-EQUIV {}: streamed={:.6} reference={:.6}", n1, s1, s2);
            assert!(
                (s1 - s2).abs() < 1e-4,
                "{} drifted: streamed={} reference={}",
                n1,
                s1,
                s2
            );
        }
    }

    /// Community-head discrimination + timing probe. Health assertions are
    /// permanent: non-phrase audio must stay far below the 0.5 firing line.
    /// (True-positive relies on the published openWakeWord evaluation +
    /// live user testing — no matched-phrase sample ships with the repo.)
    /// Gain calibration probe (diagnostic, print-only): streams the user's
    /// own recorded utterance (foreground segmenter WAV — same mic and
    /// levels as the live path) through the CURRENT backend at gains
    /// 1x/2x/3x and prints peak head scores. Picks any software input gain
    /// empirically instead of guessing: if 1x peaks <0.15 but 2-3x reaches
    /// firing range on real speech, the bar isn't the problem — level is.
    #[test]
    fn oww_gain_calibration_probe() {
        let wav = std::path::PathBuf::from(
            std::env::var("TEMP").unwrap_or_else(|_| "C:\\Windows\\Temp".to_string()),
        )
        .join("quickstt")
        .join("utterances")
        .join("stt_1791011713392.wav");
        if !wav.exists() {
            println!("GAIN-PROBE: no utterance wav at {}, skipping", wav.display());
            return;
        }
        let dir = models_dir();
        let heads = vec![
            (dir.join("hey_jarvis_v0.1.onnx"), "hey_jarvis".to_string()),
            (dir.join("alexa_v0.1.onnx"), "alexa".to_string()),
        ];
        let audio = read_wav_mono16(&wav);
        println!("GAIN-PROBE: {} samples from {}", audio.len(), wav.display());
        for gain in [1.0f32, 2.0, 3.0] {
            let gained: Vec<i16> = audio
                .iter()
                .map(|&s| ((s as f32 * gain).clamp(-32768.0, 32767.0)) as i16)
                .collect();
            let peak = gained.iter().map(|s| s.abs()).max().unwrap_or(0);
            let mut backend = OwwBackend::new(&heads).expect("backend loads");
            let enabled = vec!["hey_jarvis".to_string(), "alexa".to_string()];
            let mut best = (0f32, 0f32);
            let mut scored = 0;
            for (i, chunk) in gained.chunks(1280).enumerate() {
                backend.push_audio(chunk);
                if i % 2 == 0 {
                    continue;
                }
                let s = backend.score_enabled(&enabled);
                if !s.is_empty() {
                    scored += 1;
                    for (n, v) in &s {
                        if n == "hey_jarvis" {
                            best.0 = best.0.max(*v);
                        } else {
                            best.1 = best.1.max(*v);
                        }
                    }
                }
            }
            println!(
                "GAIN-PROBE gain={:.1}x peak={} infers={} max_hey_jarvis={:.3} max_alexa={:.3}",
                gain, peak, scored, best.0, best.1
            );
        }
    }

    #[test]
    fn oww_community_heads_probe() {        let dir = models_dir();
        let heads = vec![
            (dir.join("hey_jarvis_v0.1.onnx"), "hey_jarvis".to_string()),
            (dir.join("alexa_v0.1.onnx"), "alexa".to_string()),
        ];
        for f in [&heads[0].0, &heads[1].0] {
            assert!(f.exists(), "missing head: {}", f.display());
        }
        let neg = read_wav_mono16(&dir.join("oww_negative.wav"));
        // oww_positive.wav contains "hey LiveKit" — the WRONG phrase for both
        // heads, so it must score LOW (discrimination evidence).
        let pos = read_wav_mono16(&dir.join("oww_positive.wav"));

        // Repeat short files so every case fills the 2.4s ring identically.
        fn looped(audio: &[i16], secs: usize) -> Vec<i16> {
            let need = secs * 16000;
            let mut out = Vec::with_capacity(need);
            while out.len() < need {
                let take = (need - out.len()).min(audio.len());
                out.extend_from_slice(&audio[..take]);
            }
            out
        }

        let cases: Vec<(&str, Vec<i16>, f32)> = vec![
            ("silence", synth("silence"), 0.10),
            ("white", synth("white"), 0.10),
            ("tone440", synth("tone440"), 0.10),
            ("negative.wav", looped(&neg, 3), 0.30),
            ("positive.wav (wrong phrase)", looped(&pos, 3), 0.40),
        ];
        for (label, audio, ceiling) in cases {
            let mut backend = OwwBackend::new(&heads).expect("backend loads");
            let t0 = std::time::Instant::now();
            let mut scores = Vec::new();
            let mut infers = 0;
            // Production cadence: score every 2nd 80ms chunk (fresh=2560).
            for (i, chunk) in audio.chunks(1280).enumerate() {
                backend.push_audio(chunk);
                if i % 2 == 0 {
                    continue;
                }
                let s = backend.score_enabled(&[
                    "hey_jarvis".to_string(),
                    "alexa".to_string(),
                ]);
                if !s.is_empty() {
                    infers += 1;
                    scores = s;
                }
            }
            assert!(infers > 0, "{}: ring never filled", label);
            let ms = t0.elapsed().as_millis() as f64 / infers as f64;
            let (mel_ms, emb_ms, head_ms) = backend.last_ms;
            let mut line = format!(
                "OWW-PROBE {:<24} {:>6.1}ms/infer (mel={} emb={} heads={})",
                label, ms, mel_ms, emb_ms, head_ms
            );
            for (name, s) in &scores {
                line.push_str(&format!(" | {:<10}={:.3}", name, s));
                assert!(
                    *s < ceiling,
                    "{}: '{}' scored {:.3} (ceiling {:.2})",
                    label,
                    name,
                    s,
                    ceiling
                );
            }
            println!("{}", line);
        }
    }
}
