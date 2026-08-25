//! Async pipe reader/writer helpers for IPC with stt_service.exe

use anyhow::{anyhow, Context, Result};
use quickstt_ipc::protocol::{InboundCommand, OutboundEvent};
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader, BufWriter};
use tokio::process::{Child, ChildStdin, ChildStdout};

/// Read a single line from the child process stdout
pub async fn read_line(reader: &mut BufReader<ChildStdout>) -> Result<Option<String>> {
    let mut line = String::new();
    let bytes_read = reader.read_line(&mut line).await?;
    if bytes_read == 0 {
        return Ok(None); // EOF
    }
    Ok(Some(line))
}

/// Parse a line from the child process into an OutboundEvent
pub async fn read_event(reader: &mut BufReader<ChildStdout>) -> Result<Option<OutboundEvent>> {
    match read_line(reader).await? {
        Some(line) => {
            let event = OutboundEvent::parse(&line)
                .ok_or_else(|| anyhow!("Failed to parse outbound event: {:?}", line))?;
            Ok(Some(event))
        }
        None => Ok(None),
    }
}

/// Write a command to the child process stdin
pub async fn write_command(writer: &mut BufWriter<ChildStdin>, cmd: &InboundCommand) -> Result<()> {
    let data = cmd.serialize();
    writer.write_all(data.as_bytes()).await?;
    writer.flush().await?;
    Ok(())
}

/// Spawn a child process with piped stdin/stdout/stderr
pub fn spawn_child(cmd: &mut tokio::process::Command) -> Result<Child> {
    cmd.stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .context("Failed to spawn child process")
}

/// Background reader task: reads lines from stdout, parses events, sends them via channel
pub async fn stdout_reader_task(
    stdout: ChildStdout,
    event_tx: tokio::sync::mpsc::Sender<OutboundEvent>,
) {
    let mut reader = BufReader::new(stdout);
    loop {
        match read_event(&mut reader).await {
            Ok(Some(event)) => {
                if event_tx.send(event).await.is_err() {
                    break; // Receiver dropped
                }
            }
            Ok(None) => break, // EOF
            Err(e) => {
                tracing::error!("Error reading from stdout: {}", e);
                break;
            }
        }
    }
}

/// Background reader task for stderr: logs error output
pub async fn stderr_reader_task(stderr: tokio::process::ChildStderr) {
    let mut reader = BufReader::new(stderr);
    let mut line = String::new();
    loop {
        line.clear();
        match reader.read_line(&mut line).await {
            Ok(0) => break,
            Ok(_) => {
                tracing::warn!("stt_service stderr: {}", line.trim_end());
            }
            Err(_) => break,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn test_write_and_read_command() {
        let cmd = InboundCommand::Model("Vosk Small En".to_string());
        let serialized = cmd.serialize();
        assert_eq!(serialized, "MODEL:Vosk Small En\n");

        let parsed = InboundCommand::parse(&serialized).unwrap();
        assert_eq!(parsed, cmd);
    }

    #[tokio::test]
    async fn test_read_event_parse() {
        let raw = "STATE|1,ready\n";
        let event = OutboundEvent::parse(raw).unwrap();
        match event {
            OutboundEvent::State { code, message } => {
                assert_eq!(code, 1);
                assert_eq!(message, "ready");
            }
            _ => panic!("Wrong event type"),
        }
    }
}
