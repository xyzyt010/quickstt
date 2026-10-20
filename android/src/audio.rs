// Android microphone capture (AAudio) + WAV writer.
//
// cpal has no Android backend, so the capture goes through a tiny C shim
// (src/aaudio.c) that wraps the NDK's AAudio recording API. The frames are
// mono float at 16 kHz — exactly what whisper.cpp wants — so no resampling
// is needed between capture and transcription.

use std::path::Path;

/// Android: the real NDK AAudio shim (src/aaudio.c).
#[cfg(target_os = "android")]
mod ffi {
    extern "C" {
        pub fn qs_audio_start(sample_rate: i32) -> i32;
        pub fn qs_audio_read(out: *mut f32, frames: i32) -> i32;
        pub fn qs_audio_channels() -> i32;
        pub fn qs_audio_sample_rate() -> i32;
        pub fn qs_audio_stop();
    }
}

/// Desktop preview: no microphone — the stub fails every start, so the
/// preview window still renders (and the record button reports why).
#[cfg(not(target_os = "android"))]
mod ffi {
    pub fn qs_audio_start(_sample_rate: i32) -> i32 {
        -2
    }
    pub fn qs_audio_read(_out: *mut f32, _frames: i32) -> i32 {
        -1
    }
    pub fn qs_audio_channels() -> i32 {
        1
    }
    pub fn qs_audio_sample_rate() -> i32 {
        0
    }
    pub fn qs_audio_stop() {}
}

/// Sample rate used for capture + transcription (whisper.cpp native rate).
pub const SAMPLE_RATE: i32 = 16_000;

/// Microphone recorder. `start` opens the AAudio stream, `read` pulls
/// mono-16 kHz frames, `stop` closes it and releases the mic.
pub struct Recorder {
    active: bool,
    /// Peak absolute sample of the last read chunk (0..1).
    pub peak: f32,
    /// RMS of the last read chunk (0..1).
    pub rms: f32,
    /// Input conditioning: DC removal, soft limiting, health watch.
    cond: crate::conditioner::Conditioner,
    /// Last known input health (see `conditioner::HEALTH_*`).
    health: u8,
}

impl Recorder {
    pub fn new() -> Self {
        Self {
            active: false,
            peak: 0.0,
            rms: 0.0,
            cond: crate::conditioner::Conditioner::new(SAMPLE_RATE as u32),
            health: crate::conditioner::HEALTH_OK,
        }
    }

    /// Input health: 0 = usable, 1 = clipped, 2 = no audio at all.
    pub fn health(&self) -> u8 {
        self.health
    }

    /// True while the microphone input is clipped (hot gain).
    pub fn clipped(&self) -> bool {
        self.health == crate::conditioner::HEALTH_SATURATED
    }

    /// True when the microphone delivers nothing (muted / broken).
    pub fn dead(&self) -> bool {
        self.health == crate::conditioner::HEALTH_DEAD
    }

    pub fn is_active(&self) -> bool {
        self.active
    }

    /// Open the microphone. Returns an error message on failure (permission
    /// denied, no input device, busy).
    pub fn start(&mut self) -> Result<(), String> {
        if self.active {
            return Ok(());
        }
        let rc = unsafe { ffi::qs_audio_start(SAMPLE_RATE) };
        if rc != 0 {
            return Err(match rc {
                -1 => "could not create the audio stream".to_string(),
                -2 => {
                    "the microphone does not support 16 kHz float capture".to_string()
                }
                -3 => "the microphone could not be started".to_string(),
                _ => format!("audio error {rc}"),
            });
        }
        self.active = true;
        self.peak = 0.0;
        self.rms = 0.0;
        // Fresh stream: fresh conditioning state.
        self.cond = crate::conditioner::Conditioner::new(SAMPLE_RATE as u32);
        self.health = crate::conditioner::HEALTH_OK;
        Ok(())
    }

    /// Read up to `frames` mono samples. Returns the samples read.
    pub fn read(&mut self, out: &mut [f32]) -> usize {
        if !self.active || out.is_empty() {
            return 0;
        }
        let channels = unsafe { ffi::qs_audio_channels() }.max(1) as usize;
        // AAudio fills interleaved frames; read into a scratch buffer when
        // the device gave us more than one channel.
        let mut scratch = vec![0f32; out.len() * channels];
        let n = unsafe { ffi::qs_audio_read(scratch.as_mut_ptr(), out.len() as i32) };
        if n <= 0 {
            return 0;
        }
        let frames = (n as usize).min(out.len());
        if channels == 1 {
            out[..frames].copy_from_slice(&scratch[..frames]);
        } else {
            for f in 0..frames {
                let mut acc = 0f32;
                for c in 0..channels {
                    acc += scratch[f * channels + c];
                }
                out[f] = acc / channels as f32;
            }
        }
        // Condition the samples: DC removal + soft limiting. A hot mic
        // delivers a railed signal; without this the level meter reads
        // "speech" forever and whisper transcribes noise.
        for v in out[..frames].iter_mut() {
            *v = self.cond.condition(*v * 32767.0) / 32767.0;
        }
        self.cond.end_chunk(frames);
        self.health = self.cond.health();
        // Level stats for the UI meter (measured on the conditioned signal).
        let mut peak = 0f32;
        let mut sum = 0f32;
        for v in &out[..frames] {
            let a = v.abs();
            if a > peak {
                peak = a;
            }
            sum += v * v;
        }
        self.peak = peak;
        self.rms = if frames > 0 {
            (sum / frames as f32).sqrt()
        } else {
            0.0
        };
        frames
    }

    /// Stop capture and release the microphone.
    pub fn stop(&mut self) {
        if self.active {
            unsafe { ffi::qs_audio_stop() };
            self.active = false;
        }
    }
}

impl Drop for Recorder {
    fn drop(&mut self) {
        self.stop();
    }
}

/// Write mono 16-bit PCM as a WAV file (what whisper.cpp expects).
pub fn write_wav(path: &Path, samples: &[f32], sample_rate: u32) -> std::io::Result<()> {
    let pcm: Vec<i16> = samples
        .iter()
        .map(|s: &f32| (s.clamp(-1.0, 1.0) * 32767.0) as i16)
        .collect();
    let data_len = (pcm.len() * 2) as u32;
    let mut bytes: Vec<u8> = Vec::with_capacity(44 + pcm.len() * 2);
    bytes.extend_from_slice(b"RIFF");
    bytes.extend_from_slice(&(36u32 + data_len).to_le_bytes());
    bytes.extend_from_slice(b"WAVE");
    bytes.extend_from_slice(b"fmt ");
    bytes.extend_from_slice(&16u32.to_le_bytes()); // PCM chunk size
    bytes.extend_from_slice(&1u16.to_le_bytes()); // format = PCM
    bytes.extend_from_slice(&1u16.to_le_bytes()); // channels = mono
    bytes.extend_from_slice(&sample_rate.to_le_bytes());
    bytes.extend_from_slice(&(sample_rate * 2).to_le_bytes()); // byte rate
    bytes.extend_from_slice(&2u16.to_le_bytes()); // block align
    bytes.extend_from_slice(&16u16.to_le_bytes()); // bits per sample
    bytes.extend_from_slice(b"data");
    bytes.extend_from_slice(&data_len.to_le_bytes());
    for s in pcm {
        bytes.extend_from_slice(&s.to_le_bytes());
    }
    std::fs::write(path, bytes)
}
