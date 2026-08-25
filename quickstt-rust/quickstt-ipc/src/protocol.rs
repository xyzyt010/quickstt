use serde::{Deserialize, Serialize};
use std::fmt;

/// IPC Protocol between QuickSTT GUI and stt_service.exe
///
/// Format: `TYPE|payload\n` (newline terminated, single-line payload)
///
/// IN commands (GUI → Service):
///   TOGGLE, STOP, SLEEP, MODEL:, WAKEWORDS:, CLOSEWORDS:, WAKEMODE:, SET_REC_DIR:,
///   OFFLOAD:, OFFLOADDELAY:, QUIT, RELOAD, TRANSCRIBE_MODE:, FRONTEND_SEGMENTATION:, CLOUD_DONE
///
/// OUT events (Service → GUI):
///   STATE|, FINAL_TEXT|, AUDIO_LEVEL|, DL_PROGRESS|, DL_COMPLETE|, ERROR|

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum InboundCommand {
    Toggle,
    Stop,
    Sleep,
    Model(String),
    WakeWords(Vec<String>),
    CloseWords(Vec<String>),
    WakeMode(String),
    SetRecDir(String),
    Offload(bool),
    OffloadDelay(u32),
    Quit,
    Reload,
    TranscribeMode(TranscribeMode),
    FrontendSegmentation(FrontendSegmentationMode),
    CloudDone,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum TranscribeMode {
    Cloud,
    Local,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum FrontendSegmentationMode {
    Normal,
    Balanced,
    Fast,
    Accurate,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum OutboundEvent {
    State { code: i32, message: String },
    FinalText(String),
    AudioLevel(u8), // 0-100
    DlProgress(u8), // percent
    DlComplete(String),
    Error(String),
}

#[derive(Debug, Clone)]
pub enum VoiceEvent {
    /// Wakeword detected (engine, keyword)
    WakeWord { engine: String, keyword: String },
    /// Audio chunk ready for model (raw PCM data)
    AudioChunk(Vec<i8>), // signed 8-bit PCM for CPU efficiency
    /// VAD: speech started
    SpeechStarted,
    /// VAD: speech ended
    SpeechEnded,
    /// Final transcription result (text, model_name)
    Transcription(String, String),
    /// Model loaded successfully
    ModelLoaded { name: String },
    /// Model offloaded (unloaded)
    ModelOffloaded,
    /// Error from backend
    BackendError(String),
    /// Service process exited
    ProcessExited(i32, String), // exit code, diagnostic
    /// Auto-restart triggered
    AutoRestart,
}

impl fmt::Display for InboundCommand {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.serialize())
    }
}

impl InboundCommand {
    /// Serialize to pipe format: "COMMAND:payload\n"
    pub fn serialize(&self) -> String {
        match self {
            Self::Toggle => "TOGGLE\n".to_string(),
            Self::Stop => "STOP\n".to_string(),
            Self::Sleep => "SLEEP\n".to_string(),
            Self::Model(name) => format!("MODEL:{}\n", name),
            Self::WakeWords(words) => format!("WAKEWORDS:{}\n", words.join(",")),
            Self::CloseWords(words) => format!("CLOSEWORDS:{}\n", words.join(",")),
            Self::WakeMode(mode) => format!("WAKEMODE:{}\n", mode),
            Self::SetRecDir(dir) => format!("SET_REC_DIR:{}\n", dir),
            Self::Offload(enabled) => {
                format!("OFFLOAD:{}\n", if *enabled { "true" } else { "false" })
            }
            Self::OffloadDelay(secs) => format!("OFFLOADDELAY:{}\n", secs),
            Self::Quit => "QUIT\n".to_string(),
            Self::Reload => "RELOAD\n".to_string(),
            Self::TranscribeMode(mode) => format!("TRANSCRIBE_MODE:{}\n", mode.as_str()),
            Self::FrontendSegmentation(mode) => {
                format!("FRONTEND_SEGMENTATION:{}\n", mode.as_str())
            }
            Self::CloudDone => "CLOUD_DONE\n".to_string(),
        }
    }

    /// Parse from pipe format
    pub fn parse(raw: &str) -> Option<Self> {
        let raw = raw.trim_end_matches('\n').trim_end_matches('\r');
        let (cmd, payload) = if let Some(pos) = raw.find(':') {
            (&raw[..pos], &raw[pos + 1..])
        } else {
            (raw, "")
        };

        Some(match cmd {
            "TOGGLE" => Self::Toggle,
            "STOP" => Self::Stop,
            "SLEEP" => Self::Sleep,
            "MODEL" => Self::Model(payload.to_string()),
            "WAKEWORDS" => {
                Self::WakeWords(payload.split(',').map(|s| s.trim().to_string()).collect())
            }
            "CLOSEWORDS" => {
                Self::CloseWords(payload.split(',').map(|s| s.trim().to_string()).collect())
            }
            "WAKEMODE" => Self::WakeMode(payload.to_string()),
            "SET_REC_DIR" => Self::SetRecDir(payload.to_string()),
            "OFFLOAD" => Self::Offload(payload.eq_ignore_ascii_case("true") || payload == "1"),
            "OFFLOADDELAY" => Self::OffloadDelay(payload.parse::<u32>().unwrap_or(15)),
            "QUIT" => Self::Quit,
            "RELOAD" => Self::Reload,
            "CLOUD_DONE" => Self::CloudDone,
            _ => return None,
        })
    }
}

impl TranscribeMode {
    pub fn as_str(&self) -> &'static str {
        match self {
            Self::Cloud => "CLOUD",
            Self::Local => "LOCAL",
        }
    }
}

impl FrontendSegmentationMode {
    pub fn as_str(&self) -> &'static str {
        match self {
            Self::Normal => "0",
            Self::Balanced => "1",
            Self::Fast => "2",
            Self::Accurate => "3",
        }
    }
}

impl OutboundEvent {
    /// Parse from pipe format "TYPE|payload\n"
    pub fn parse(raw: &str) -> Option<Self> {
        let raw = raw.trim_end_matches('\n').trim_end_matches('\r');
        let (event_type, payload) = if let Some(pos) = raw.find('|') {
            (&raw[..pos], &raw[pos + 1..])
        } else {
            (raw, "")
        };

        Some(match event_type {
            "STATE" => {
                let (code, msg) = if let Some(comma) = payload.find(',') {
                    (
                        payload[..comma].parse::<i32>().unwrap_or(0),
                        payload[comma + 1..].to_string(),
                    )
                } else {
                    (payload.parse::<i32>().unwrap_or(0), String::new())
                };
                Self::State { code, message: msg }
            }
            "FINAL_TEXT" => Self::FinalText(payload.to_string()),
            "AUDIO_LEVEL" => Self::AudioLevel(payload.parse::<u8>().unwrap_or(0)),
            "DL_PROGRESS" => Self::DlProgress(payload.parse::<u8>().unwrap_or(0)),
            "DL_COMPLETE" => Self::DlComplete(payload.to_string()),
            "ERROR" => Self::Error(payload.to_string()),
            _ => return None,
        })
    }

    /// Serialize to pipe format
    pub fn serialize(&self) -> String {
        match self {
            Self::State { code, message } => format!("STATE|{},{}\n", code, message),
            Self::FinalText(text) => format!("FINAL_TEXT|{}\n", text),
            Self::AudioLevel(level) => format!("AUDIO_LEVEL|{}\n", level),
            Self::DlProgress(percent) => format!("DL_PROGRESS|{}\n", percent),
            Self::DlComplete(name) => format!("DL_COMPLETE|{}\n", name),
            Self::Error(msg) => format!("ERROR|{}\n", msg),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_command_roundtrip() {
        let cmd = InboundCommand::Model("Vosk Small En".to_string());
        let serialized = cmd.serialize();
        let parsed = InboundCommand::parse(&serialized).unwrap();
        assert_eq!(parsed, InboundCommand::Model("Vosk Small En".to_string()));
    }

    #[test]
    fn test_wakewords_roundtrip() {
        let words = vec!["hey quickstt".to_string(), "alexa".to_string()];
        let cmd = InboundCommand::WakeWords(words);
        let serialized = cmd.serialize();
        let parsed = InboundCommand::parse(&serialized).unwrap();
        assert_eq!(
            parsed,
            InboundCommand::WakeWords(vec!["hey quickstt".to_string(), "alexa".to_string()])
        );
    }

    #[test]
    fn test_event_parse() {
        let raw = "FINAL_TEXT|hello world\n";
        let event = OutboundEvent::parse(raw).unwrap();
        match event {
            OutboundEvent::FinalText(text) => assert_eq!(text, "hello world"),
            _ => panic!("Wrong event type"),
        }
    }
}
