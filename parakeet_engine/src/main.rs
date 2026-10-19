use std::io::{self, BufRead};
use serde::{Deserialize, Serialize};
use transcribe_rs::onnx::parakeet::{ParakeetModel, ParakeetParams};
use transcribe_rs::onnx::sense_voice::{SenseVoiceModel, SenseVoiceParams};

#[derive(Deserialize)]
struct Request {
    action: String,
    model_path: Option<String>,
    audio_path: Option<String>,
    /// Which transcribe-rs loader `model_path` belongs to. Absent = "parakeet"
    /// (backwards compatible: older requesters never send it). Currently
    /// "parakeet" (also Phonon-2, same arch) or "sensevoice".
    engine: Option<String>,
    /// SenseVoice only: language token ("auto"/"en"/"zh"/"ja"/"ko"/"yue").
    language: Option<String>,
    /// SenseVoice only: inverse text normalization (upstream default: true).
    itn: Option<bool>,
    /// Base64-encoded little-endian f32 PCM samples (16kHz mono)
    pcm_b64: Option<String>,
    /// Base64-encoded little-endian i16 PCM samples (16kHz mono)
    pcm_i16_b64: Option<String>,
}

#[derive(Serialize)]
struct Response {
    status: String,
    text: Option<String>,
    error: Option<String>,
}

/// One loaded model. The worker is shared by every transcribe-rs ONNX family
/// (Parakeet, Phonon-2, SenseVoice) so the app spawns ONE persistent helper
/// with one load/unload/transcribe JSON protocol.
enum LoadedModel {
    Parakeet(ParakeetModel),
    SenseVoice(SenseVoiceModel),
}

impl LoadedModel {
    fn transcribe(
        &mut self,
        samples: &[f32],
        language: Option<&str>,
        itn: Option<bool>,
    ) -> Result<String, String> {
        let result = match self {
            LoadedModel::Parakeet(m) => m.transcribe_with(samples, &ParakeetParams::default()),
            LoadedModel::SenseVoice(m) => {
                m.transcribe_with(
                    samples,
                    &SenseVoiceParams {
                        language: language.map(|s| s.to_string()),
                        use_itn: itn.or(Some(true)),
                    },
                )
            }
        };
        result.map(|r| r.text).map_err(|e| e.to_string())
    }
}

fn decode_b64_f32(b64: &str) -> Result<Vec<f32>, String> {
    use base64::Engine as _;
    let bytes = base64::engine::general_purpose::STANDARD
        .decode(b64)
        .map_err(|e| format!("base64 decode error: {}", e))?;
    if bytes.len() % 4 != 0 {
        return Err("pcm_b64 byte length not multiple of 4".to_string());
    }
    let samples: Vec<f32> = bytes
        .chunks_exact(4)
        .map(|c| f32::from_le_bytes([c[0], c[1], c[2], c[3]]))
        .collect();
    Ok(samples)
}

fn decode_b64_i16(b64: &str) -> Result<Vec<f32>, String> {
    use base64::Engine as _;
    let bytes = base64::engine::general_purpose::STANDARD
        .decode(b64)
        .map_err(|e| format!("base64 decode error: {}", e))?;
    if bytes.len() % 2 != 0 {
        return Err("pcm_i16_b64 byte length not multiple of 2".to_string());
    }
    let samples: Vec<f32> = bytes
        .chunks_exact(2)
        .map(|c| (i16::from_le_bytes([c[0], c[1]]) as f32) / 32768.0)
        .collect();
    Ok(samples)
}

#[cfg(target_os = "windows")]
fn compact_working_set() {
    use std::ffi::c_void;
    #[link(name = "psapi")]
    unsafe extern "system" {
        fn EmptyWorkingSet(h_process: *mut c_void) -> i32;
    }
    #[link(name = "kernel32")]
    unsafe extern "system" {
        fn GetCurrentProcess() -> *mut c_void;
    }
    unsafe {
        let _ = EmptyWorkingSet(GetCurrentProcess());
    }
}

#[cfg(not(target_os = "windows"))]
fn compact_working_set() {}

fn main() {
    let mut model: Option<LoadedModel> = None;
    let stdin = io::stdin();

    for line in stdin.lock().lines() {
        let line = match line {
            Ok(l) => l,
            Err(_) => break,
        };
        if line.trim().is_empty() { continue; }

        let req: Request = match serde_json::from_str(&line) {
            Ok(r) => r,
            Err(e) => {
                println!("{}", serde_json::to_string(&Response { status: "error".to_string(), text: None, error: Some(e.to_string()) }).unwrap());
                continue;
            }
        };

        if req.action == "load" {
            if let Some(path) = req.model_path {
                let engine = req.engine.as_deref().unwrap_or("parakeet");
                // Windows execution-provider pick: DirectML is great for the
                // Parakeet/Phonon family (the alpha.13 worker shipped this
                // way), but the DML EP access-violates on the SenseVoice
                // graph — that engine stays on CPU. Non-Windows builds never
                // compile the ort-directml feature in; the default (Auto)
                // resolves to CPU there.
                #[cfg(target_os = "windows")]
                transcribe_rs::set_ort_accelerator(match engine {
                    "sensevoice" | "sense-voice" => transcribe_rs::OrtAccelerator::CpuOnly,
                    _ => transcribe_rs::OrtAccelerator::DirectMl,
                });
                let load_result = match engine {
                    "sensevoice" | "sense-voice" => SenseVoiceModel::load(
                        std::path::Path::new(&path),
                        &transcribe_rs::onnx::Quantization::Int8,
                    ).map(LoadedModel::SenseVoice),
                    _ => ParakeetModel::load(
                        std::path::Path::new(&path),
                        &transcribe_rs::onnx::Quantization::Int8,
                    ).map(LoadedModel::Parakeet),
                };
                match load_result {
                    Ok(m) => {
                        model = Some(m);
                        println!("{}", serde_json::to_string(&Response { status: "ok".to_string(), text: None, error: None }).unwrap());
                    },
                    Err(e) => {
                        let err_msg: String = e.to_string();
                        println!("{}", serde_json::to_string(&Response { status: "error".to_string(), text: None, error: Some(err_msg) }).unwrap());
                    }
                }
            }
        } else if req.action == "transcribe" {
            if let Some(m) = &mut model {
                if let Some(apath) = req.audio_path {
                    let audio_res = hound::WavReader::open(&apath);
                    if let Ok(mut reader) = audio_res {
                        let samples: Vec<f32> = reader.samples::<i16>().map(|s| s.unwrap_or(0) as f32 / 32768.0).collect();
                        match m.transcribe(&samples, req.language.as_deref(), req.itn) {
                            Ok(text) => {
                                println!("{}", serde_json::to_string(&Response { status: "ok".to_string(), text: Some(text), error: None }).unwrap());
                            },
                            Err(err_msg) => {
                                println!("{}", serde_json::to_string(&Response { status: "error".to_string(), text: None, error: Some(err_msg) }).unwrap());
                            }
                        }
                    } else {
                        println!("{}", serde_json::to_string(&Response { status: "error".to_string(), text: None, error: Some("Failed to read audio".to_string()) }).unwrap());
                    }
                }
            } else {
                println!("{}", serde_json::to_string(&Response { status: "error".to_string(), text: None, error: Some("Model not loaded".to_string()) }).unwrap());
            }
        } else if req.action == "transcribe_pcm" {
            // Direct PCM inference — no file I/O, matches Handy's in-process approach
            if let Some(m) = &mut model {
                let samples_result = if let Some(b64) = req.pcm_b64 {
                    decode_b64_f32(&b64)
                } else if let Some(b64) = req.pcm_i16_b64 {
                    decode_b64_i16(&b64)
                } else {
                    Err("No pcm_b64 or pcm_i16_b64 provided".to_string())
                };

                match samples_result {
                    Ok(samples) if !samples.is_empty() => {
                        match m.transcribe(&samples, req.language.as_deref(), req.itn) {
                            Ok(text) => {
                                println!("{}", serde_json::to_string(&Response { status: "ok".to_string(), text: Some(text), error: None }).unwrap());
                            },
                            Err(err_msg) => {
                                println!("{}", serde_json::to_string(&Response { status: "error".to_string(), text: None, error: Some(err_msg) }).unwrap());
                            }
                        }
                    },
                    Ok(_) => {
                        println!("{}", serde_json::to_string(&Response { status: "error".to_string(), text: None, error: Some("Empty PCM buffer".to_string()) }).unwrap());
                    },
                    Err(e) => {
                        println!("{}", serde_json::to_string(&Response { status: "error".to_string(), text: None, error: Some(e) }).unwrap());
                    }
                }
            } else {
                println!("{}", serde_json::to_string(&Response { status: "error".to_string(), text: None, error: Some("Model not loaded".to_string()) }).unwrap());
            }
        } else if req.action == "unload" {
            model = None;
            // Drop cached allocator pages so the OS reclaims the model
            // weights instead of keeping them in the working set.
            compact_working_set();
            println!("{}", serde_json::to_string(&Response { status: "ok".to_string(), text: None, error: None }).unwrap());
            // Stay alive for potential reload (don't break)
        } else if req.action == "quit" {
            model = None;
            println!("{}", serde_json::to_string(&Response { status: "ok".to_string(), text: None, error: None }).unwrap());
            break; // Actually exit the engine
        }
    }
}
