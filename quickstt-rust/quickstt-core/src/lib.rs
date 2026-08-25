pub mod config;
pub mod engine;
pub mod error;
pub mod ipc;
pub mod managers;
pub mod models;
pub mod orchestration;
pub mod settings;
pub mod stt_service;
pub mod wakeword_loader;

#[cfg(feature = "wakeword")]
pub mod ml;

#[cfg(feature = "audio-capture")]
pub mod audio;

#[cfg(all(feature = "wakeword", feature = "audio-capture"))]
pub mod wakeword_service;
