# Architecture — Windows C++/Qt + Linux Rust (egui) Unified

QuickSTT ships two complementary stacks that share the same pipe protocol, settings keys, and model layout.

## Stacks

| Stack | Primary platform | Pill | Audio | STT | Wakeword | Build |
|---|---|---|---|---|---|---|
| **C++/Qt** (`Source/`) | Windows 10/11 | `pill_widget.h:217` `QWidget FramelessWindowHint+StaysOnTop WA_Translucent` 360×50 r25, `QSvgRenderer` mic | `winmm waveIn` + `PortAudio` `SoftwareAGC -20dBFS` | `libvosk.dll` `onnxruntime.dll` `whisper-cli.exe` Parakeet `parakeet_engine.exe` | `oww_tflite.h` `tflite_loader.h` `tensorflowlite_c.dll` + Porcupine `pv_native.h` | `CMakeLists.txt` + `BuildApp.bat` (`windeployqt`) |
| **Rust** (`quickstt-rust/`) | Ubuntu 22.04/24.04, Debian 12, Mint 21/22 amd64 | `quickstt-gui/src/widget_platform.rs:88` `ViewportBuilder transparent always_on_top no-decorations` (X11 `_NET_WM_WINDOW_TYPE_DOCK` / Wayland layer-shell) `theme.rs PILL_WIDTH 360` | `cpal 0.15` `default_host()` → ALSA/Pulse/JACK 16 kHz 1ch dual-stream, `normalize.rs` resample `TARGET_CHUNK 1280`, `pipeline.rs` `hound` WAV | `libvosk.so` `libonnxruntime.so` `whisper-cli` `parakeet_engine` `nemotron_engine` via `dlopen` (`platform.h`) — `models/engine.rs` | `livekit-wakeword 0.1` ONNX `ort` + `audio/vad.rs` `ten_vad` (`libten_vad.so`) | `cargo build -p quickstt-gui --release` + `cmake stt_service_native` |

## Rust workspace (`quickstt-rust/`)

```
quickstt-gui  → eframe 0.29 glow, egui 0.29, tray-icon 0.17, muda 0.14, global-hotkey 0.6, resvg 0.44
quickstt-core → orchestration, tokio async, cpal dasp, whisper-rs 0.11, livekit-wakeword, tract-onnx (YAMNet), hound
quickstt-ipc  → serde protocol STATE| FINAL_TEXT| AUDIO_LEVEL|  ↔  TOGGLE/STOP/SLEEP/MODEL:/WAKEWORDS:
quickstt-slint→ Slint 1.9 alternative shell (experimental)
```

Data flow (Rust, same on Windows C++ service):

1. **Wakeword / manual** (`Ctrl+Space` hold → circular pill `ViewportId quickstt_on_command` `main.rs:1656`, or `TOGGLE` hotkey) → `orchestration.rs:125` `AudioControlCommand::Open`
2. **Audio** `capture.rs:130` `cpal InputStream` 16 kHz mono → `normalize.rs` phase resampler → `pipeline.rs` `SpeechSegmenter` → temp WAV
3. **VAD** `vad.rs` energy + `audio_preprocess` `ten_vad` hold 12 frames gates `speechLikely`
4. **STT** `SttEngineConfig::detect()` searches `models_root` (`~/.local/share/QuickSTT/models` or `%APPDATA%`) and `exe_dir/tools/...` with `ext = if windows {".exe"} else {""}` (`engine.rs:36`) → dispatch `WhisperCpp|SherpaOnnx|ParakeetRust|Vosk|Nemotron`
5. **Result** `OutboundEvent::FinalText` → `orchestration.rs:278` `TextRecognized` → `main.rs:808` `transcript_buffer` → `render_textboard` (`textboard.rs` viewport `quickstt_textboard` 420×180) + `WinInput::typeUtf8` (`wtype` on Wayland, `SendInput` on Win)

Settings: `settings.rs:145` Registry `HKCU\Software\QuickSTT\Config` on Windows ↔ TOML `~/.config/QuickSTT/config.toml` on Linux (`dirs` + `toml` crate, `test_toml_roundtrip`).

## C++ service (`Source/native/`)

- `stt_service_native.cpp` — 3600 lines, `#ifdef _WIN32` for `windows.h/mmdevice/endpointvolume` vs Linux `readlink /proc/self/exe`, `XDG_DATA_HOME`, `pipe/fork/poll`.
- Dynamic loaders: `tflite_loader.h`/`ort_loader.h`/`vosk_api.h` via `platform.h` (`LoadLibraryA` ↔ `dlopen`), `oww_tflite.h`/`wakenet_native.h` path joins via `fs::path`, `pv_native.h` `dlopen` on Linux.
- `ParakeetPipe` — Windows `HANDLE CreatePipe PeekNamedPipe` vs Linux `pid_t pipe fork poll read/write` (same JSON `{"action":"load","model_path":..}` / `{"action":"transcribe","audio_path":..}`).
- `win_input.h` — Windows `SendInput KEYEVENTF_UNICODE` vs Linux `wtype/ydotool/xdotool` via `system`.

## IPC

Both stacks speak `TYPE|payload\n` over anonymous `stdin/stdout` pipe (`tokio::process::Command piped`, `HANDLE` vs POSIX `pipe`). No `NamedPipe` on Linux, no `Unix socket` — plain `pipe(2)` — so the same `quickstt-ipc::protocol` parses on both.

## Configuration

| Key | Windows | Linux Mint/Ubuntu |
|---|---|---|
| `selectedModel` | `RegQueryValueExA` `HKCU\...` | `config.toml` TOML |
| `wakeWords`/`closeWords` | `REG_MULTI_SZ` | `String` array |
| `widgetFlexible` `pill_width/height` | `DWORD` | TOML `u32` |
| Autostart | `HKCU\...\Run` | `~/.config/autostart/quickstt.desktop` (`autostart.rs`) |

## Performance targets

- Idle: <5 MB (whisper/vosk lazy `WhisperContext::new_with_params` only when wakeword fires)
- Listening (no model): ~30 MB
- Models on demand: Vosk 50M, Parakeet 640M, Nemotron 716M (XDG `models/`), `compact_working_set()` `SetProcessWorkingSetSize` ↔ `malloc_trim(0)` on Linux.

See `quickstt-rust/LINUX_PHASE3_NOTES.md` for the exact `x86_64-unknown-linux-gnu` sysroot (`libgtk-3-dev`, `libayatana-appindicator3-dev`, `portaudio19-dev`).

