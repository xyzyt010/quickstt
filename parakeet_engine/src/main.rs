use std::io::{self, BufRead};
use serde::{Deserialize, Serialize};
use transcribe_rs::onnx::parakeet::{ParakeetModel, ParakeetParams};

#[derive(Deserialize)]
struct Request {
    action: String,
    model_path: Option<String>,
    audio_path: Option<String>,
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

fn main() {
    let mut model: Option<ParakeetModel> = None;
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
                match ParakeetModel::load(std::path::Path::new(&path), &transcribe_rs::onnx::Quantization::Int8) {
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
                        let params = ParakeetParams::default();
                        match m.transcribe_with(&samples, &params) {
                            Ok(text) => {
                                println!("{}", serde_json::to_string(&Response { status: "ok".to_string(), text: Some(text.text), error: None }).unwrap());
                            },
                            Err(e) => {
                                let err_msg: String = e.to_string();
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
                        let params = ParakeetParams::default();
                        match m.transcribe_with(&samples, &params) {
                            Ok(text) => {
                                println!("{}", serde_json::to_string(&Response { status: "ok".to_string(), text: Some(text.text), error: None }).unwrap());
                            },
                            Err(e) => {
                                let err_msg: String = e.to_string();
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
            println!("{}", serde_json::to_string(&Response { status: "ok".to_string(), text: None, error: None }).unwrap());
            // Stay alive for potential reload (don't break)
        } else if req.action == "quit" {
            model = None;
            println!("{}", serde_json::to_string(&Response { status: "ok".to_string(), text: None, error: None }).unwrap());
            break; // Actually exit the engine
        }
    }
}
