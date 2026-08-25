use anyhow::{anyhow, Context, Result};
use quickstt_ipc::protocol::{InboundCommand, OutboundEvent, VoiceEvent};
use std::path::PathBuf;
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::process::{Child, Command};
use tokio::sync::mpsc;
use tokio::task::JoinHandle;

/// Manages the stt_service.exe child process and IPC communication
pub struct SttServiceManager {
    process: Option<Child>,
    event_tx: mpsc::Sender<VoiceEvent>,
    _reader_handle: Option<JoinHandle<()>>,
    exe_dir: PathBuf,
}

impl SttServiceManager {
    pub fn new(event_tx: mpsc::Sender<VoiceEvent>, exe_dir: PathBuf) -> Self {
        Self {
            process: None,
            event_tx,
            _reader_handle: None,
            exe_dir,
        }
    }

    /// Start or restart the STT service process
    pub async fn start(&mut self) -> Result<()> {
        // Stop existing if running
        self.stop().await;

        let exe_name = crate::config::STT_SERVICE_EXE;
        let service_path = self.exe_dir.join(exe_name);
        if !service_path.exists() {
            return Err(anyhow!(
                "STT service executable not found: {:?}",
                service_path
            ));
        }

        let mut child = Command::new(&service_path)
            .current_dir(&self.exe_dir)
            .stdin(std::process::Stdio::piped())
            .stdout(std::process::Stdio::piped())
            .stderr(std::process::Stdio::piped())
            .spawn()
            .context("Failed to start stt_service")?;

        let stdout = child
            .stdout
            .take()
            .ok_or(anyhow!("Failed to take stdout"))?;

        let tx_clone = self.event_tx.clone();
        let reader_handle = tokio::spawn(async move {
            let mut reader = BufReader::new(stdout);
            let mut line = String::new();
            loop {
                line.clear();
                match reader.read_line(&mut line).await {
                    Ok(0) => break,
                    Ok(_) => {
                        let event = OutboundEvent::parse(&line);
                        let voice_event = match event {
                            Some(OutboundEvent::State { code, message }) => Some(
                                VoiceEvent::BackendError(format!("STATE|{}, {}", code, message)),
                            ),
                            Some(OutboundEvent::FinalText(text)) => {
                                Some(VoiceEvent::Transcription(text, "stt".to_string()))
                            }
                            Some(OutboundEvent::AudioLevel(level)) => {
                                Some(VoiceEvent::AudioChunk(vec![level as i8; 64]))
                            }
                            Some(OutboundEvent::Error(msg)) => Some(VoiceEvent::BackendError(msg)),
                            _ => None,
                        };

                        if let Some(event) = voice_event {
                            let _ = tx_clone.send(event).await;
                        }
                    }
                    Err(_) => break,
                }
            }
        });

        self._reader_handle = Some(reader_handle);
        self.process = Some(child);

        tracing::info!("STT Service started");
        Ok(())
    }

    /// Send a command to the service
    pub async fn send_command(&mut self, cmd: &InboundCommand) -> Result<()> {
        if let Some(ref mut process) = self.process {
            if let Some(ref mut stdin) = process.stdin {
                let data = cmd.serialize();
                stdin.write_all(data.as_bytes()).await?;
                stdin.flush().await?;
                Ok(())
            } else {
                Err(anyhow!("No stdin available on child process"))
            }
        } else {
            Err(anyhow!("STT Service not running"))
        }
    }

    /// Stop the service
    pub async fn stop(&mut self) {
        if let Some(mut process) = self.process.take() {
            let _ = self.send_command(&InboundCommand::Quit).await;
            tokio::time::sleep(std::time::Duration::from_millis(500)).await;
            let _ = process.kill().await;
        }
    }

    /// Check if service is running
    pub fn is_running(&self) -> bool {
        self.process.is_some()
    }
}
