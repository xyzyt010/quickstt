# QuickSTT Rust Migration — Complete Agent Guide

## Project Context

QuickSTT is a Windows desktop voice-control app. The current C++/Qt6 implementation is being rewritten in Rust using `windows-rs` (native Win32) for the GUI and `tokio` for async orchestration. The STT backend engine (`stt_service.exe`) stays as C++ — it communicates via a stdin/stdout pipe protocol that is **preserved exactly**.

**Root path:** `C:\Users\hemsh_sfya5gq\.gemini\antigravity\scratch\quick_stt_app`

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│  Rust GUI (quickstt-gui)                                │
│  - Win32 native window (windows-rs)                     │
│  - Direct2D for waveform rendering                      │
│  - System tray, global hotkeys                          │
│  - PillWidget (floating pill) + MainWindow (dashboard)  │
├─────────────────────────────────────────────────────────┤
│  Rust Core (quickstt-core)                              │
│  - tokio async runtime                                  │
│  - Settings (Windows Registry: HKCU\Software\QuickSTT)  │
│  - SttServiceManager (pipe IPC with stt_service.exe)    │
│  - CloudSttManager (reqwest + provider APIs)            │
│  - SmartLifeManager, AndroidTvManager, HomeAssistantMgr │
│  - LocalModelManager (model catalog, install, download) │
│  - OptionalServiceManager (Tuya, AHK bridge, etc.)      │
├─────────────────────────────────────────────────────────┤
│  IPC Protocol (quickstt-ipc)                            │
│  - InboundCommand enum (GUI → Service)                  │
│  - OutboundEvent enum (Service → GUI)                   │
│  - VoiceEvent enum (internal app events)                │
│  - Format: "TYPE|payload\n" newline-terminated          │
├─────────────────────────────────────────────────────────┤
│  C++ STT Service (UNCHANGED — do not modify)            │
│  - Source/native/stt_service_native.cpp                 │
│  - Builds to stt_service.exe                            │
│  - Uses: libvosk.dll, onnxruntime, portaudio, rnnoise   │
│  - Wake word: OpenWakeWord (TFLite), Porcupine, Vosk    │
└─────────────────────────────────────────────────────────┘
```

---

## Rust Workspace Structure

```
quickstt-rust/
├── Cargo.toml                          # Workspace root, release profile (LTO=fat, codegen-units=1)
├── quickstt-ipc/                       # Shared protocol types (no async, minimal deps)
│   ├── Cargo.toml                      # deps: serde, serde_json, thiserror
│   └── src/
│       ├── lib.rs                      # pub mod protocol; pub use protocol::*;
│       └── protocol.rs                 # InboundCommand, OutboundEvent, VoiceEvent enums + serialize/parse
├── quickstt-core/                      # All business logic
│   ├── Cargo.toml                      # deps: quickstt-ipc, tokio, serde, serde_json, windows, tracing, anyhow, thiserror, once_cell
│   └── src/
│       ├── lib.rs                      # pub mod config; pub mod engine; pub mod ipc; pub mod managers; pub mod settings; pub mod stt_service;
│       ├── config.rs                   # [EMPTY] App config constants
│       ├── engine.rs                   # [EMPTY] Engine detection/matching logic
│       ├── ipc.rs                      # [EMPTY] Async pipe reader/writer helpers
│       ├── managers.rs                 # [EMPTY] Manager trait definitions
│       ├── settings.rs                 # [EMPTY] Windows Registry settings read/write
│       └── stt_service.rs              # SttServiceManager — spawns/monitors stt_service.exe, sends InboundCommand, receives OutboundEvent
└── quickstt-gui/                       # Win32 GUI application
    ├── Cargo.toml                      # deps: quickstt-core, quickstt-ipc, windows, tokio, tracing, anyhow, thiserror
    └── src/
        ├── main.rs                     # Entry point — tokio::main, initializes tracing
        └── lib.rs                      # [EMPTY] Win32 window creation, message loop, D2D rendering
```

---

## C++ Source Files Reference (What Each Maps To)

| C++ Source File | Rust Target Module | Notes |
|---|---|---|
| `Source/main.rs` | `quickstt-gui/src/main.rs` | Entry point, single-instance guard, activation server |
| `Source/pill_widget.h/.cpp` | `quickstt-gui/src/pill.rs` | Floating pill widget — mic button, model combo, status label, waveform |
| `Source/mainwindow.h/.cpp` | `quickstt-gui/src/dashboard.rs` | Settings dashboard — tabs for models, smart home, updates, wakeword |
| `Source/text_board.h/.cpp` | `quickstt-gui/src/text_board.rs` | Transcript text board — attached/detached, auto-scroll |
| `Source/setup_wizard.h/.cpp` | `quickstt-gui/src/setup_wizard.rs` | First-run setup wizard |
| `Source/ahk_bridge.h/.cpp` | `quickstt-core/src/managers/ahk_bridge.rs` | AutoHotkey bridge for special commands |
| `Source/cloud_stt_manager.h/.cpp` | `quickstt-core/src/managers/cloud_stt.rs` | Cloud STT API calls (OpenAI, Google, ElevenLabs, etc.) |
| `Source/local_model_manager.h/.cpp` | `quickstt-core/src/managers/local_model.rs` | Local model catalog, install, download |
| `Source/local_model_support.h/.cpp` | `quickstt-core/src/managers/local_model.rs` | Model descriptors, compute targets, install paths |
| `Source/local_frontend_stt_manager.h/.cpp` | `quickstt-core/src/managers/local_frontend.rs` | Frontend-managed STT (parakeet, ggml server) |
| `Source/smart_life_manager.h/.cpp` | `quickstt-core/src/managers/smart_life.rs` | Tuya Smart Life device control |
| `Source/android_tv_manager.h/.cpp` | `quickstt-core/src/managers/android_tv.rs` | Android TV remote control |
| `Source/home_assistant_manager.h/.cpp` | `quickstt-core/src/managers/home_assistant.rs` | Home Assistant integration |
| `Source/optional_service_manager.h/.cpp` | `quickstt-core/src/managers/optional_service.rs` | Optional service install/status |
| `Source/optional_service_support.h/.cpp` | `quickstt-core/src/managers/optional_service.rs` | Service descriptors, package info |
| `Source/startup_utils.h/.cpp` | `quickstt-gui/src/main.rs` | Startup registry, background mode |
| `Source/windows_secret_store.h/.cpp` | `quickstt-core/src/settings.rs` | Windows Credential Manager for secrets |
| `Source/loader.cpp` | N/A (replaced by Rust binary) | Lightweight loader — no longer needed |
| `Source/qt_server_app.cpp` | N/A (removed) | Qt server app — not needed in Rust version |
| `Source/native/stt_service_native.cpp` | **DO NOT MODIFY** | C++ STT engine — stays as-is, builds to stt_service.exe |

---

## IPC Protocol (Critical — Must Match Exactly)

The pipe protocol between GUI and `stt_service.exe` is defined in `quickstt-ipc/src/protocol.rs`.

**Outbound (Service → GUI):** `TYPE|payload\n`
- `STATE|code,message` — state change
- `FINAL_TEXT|text` — final recognized text
- `AUDIO_LEVEL|0-100` — audio level
- `DL_PROGRESS|percent` — download progress
- `DL_COMPLETE|name` — download finished
- `ERROR|message` — error

**Inbound (GUI → Service):** `COMMAND:payload\n` or `COMMAND\n`
- `TOGGLE` — toggle recording
- `STOP` — stop recording
- `SLEEP` — sleep/background
- `MODEL:name` — load model
- `WAKEWORDS:csv` — set wake words (comma-separated)
- `CLOSEWORDS:csv` — set close words
- `WAKEMODE:engine` — set wake engine
- `SET_REC_DIR:path` — set recording directory
- `OFFLOAD:true|false` — auto-offload toggle
- `OFFLOADDELAY:seconds` — auto-offload delay
- `QUIT` — quit service
- `RELOAD` — reload model
- `TRANSCRIBE_MODE:CLOUD|LOCAL` — transcription mode
- `FRONTEND_SEGMENTATION:0|1|2|3` — segmentation mode
- `CLOUD_DONE` — cloud transcription complete

---

## Settings Storage (Windows Registry)

All settings stored in `HKEY_CURRENT_USER\Software\QuickSTT\Config`.

Key settings and their types:
- `selectedModel` (REG_SZ) — e.g. "Vosk Small En"
- `widgetModels` (REG_MULTI_SZ) — list of models shown in combo
- `cloudWidgetModels` (REG_MULTI_SZ) — cloud models in combo
- `favoriteModels` (REG_MULTI_SZ) — favorite models
- `wakeWords` (REG_MULTI_SZ) — wake words list
- `closeWords` (REG_MULTI_SZ) — close words list
- `wakeEngine` (REG_SZ) — e.g. "OpenWakeWord (TFLite)"
- `porcupineAccessKey` (REG_SZ)
- `recordingDir` (REG_SZ)
- `autoOffload` (REG_SZ "true"/"false" or REG_DWORD)
- `offloadSeconds` (REG_DWORD) — or `offloadMinutes` (legacy)
- `autoModelLoad` (REG_SZ or REG_DWORD)
- `startupEnabled` (REG_SZ or REG_DWORD)
- `startupBackground` (REG_SZ or REG_DWORD)
- `specialCommands` (REG_SZ or REG_DWORD)
- `haptics` (REG_SZ or REG_DWORD)
- `sound` (REG_SZ or REG_DWORD)
- `widgetFlexible` (REG_SZ or REG_DWORD)
- `firstLaunch` (REG_SZ or REG_DWORD)
- `setupCompleted` (REG_SZ or REG_DWORD)
- `pillWidth`, `pillHeight`, `pillRadius` (REG_DWORD)
- `activeOpacity`, `iconSize`, `trayIconSize` (REG_DWORD)
- `txtOpacity`, `txtSize` (REG_DWORD)
- `r`, `o` (REG_DWORD) — color values
- `showWaveform` (REG_SZ or REG_DWORD)
- `waveformSensitivity` (REG_DWORD)
- SmartLife: `smartLifeAccountMode`, `smartLifeEndpoint`, `smartLifeAccessId`, `smartLifeAccessKey`, `smartLifeDeveloperUid`, `smartLifeUsername`, `smartLifePassword`, `smartLifeCountryCode`, `smartLifeSchema`
- HomeAssistant: `haUrl`, `haToken`
- AndroidTV: `androidTvAutoScan`
- Cloud provider keys: per-provider settings

---

## Step-by-Step Migration Checklist

Work through these steps in order. Each step must compile and pass tests before moving to the next.

---

### Step 1: Workspace Setup & IPC Protocol
**Status:** [x] COMPLETED (partially — protocol done, workspace compiles)

**What was done:**
- Created `quickstt-rust/` workspace with 3 crates
- Created `Cargo.toml` with workspace dependencies and release profile
- Created `quickstt-ipc/src/protocol.rs` with full InboundCommand, OutboundEvent, VoiceEvent enums
- Created `quickstt-core/src/stt_service.rs` with SttServiceManager skeleton
- Created stub files for all modules

**What remains:**
- [ ] Fix `stt_service.rs` — has `std::path::Buf` typo (should be `PathBuf`), missing `Context` import
- [ ] Create `quickstt-core/src/config.rs` — app constants (registry key path, app name, version)
- [ ] Create `quickstt-core/src/engine.rs` — engine detection/matching logic
- [ ] Create `quickstt-core/src/ipc.rs` — async pipe reader/writer helpers
- [ ] Create `quickstt-core/src/managers.rs` — manager trait definitions
- [ ] Create `quickstt-core/src/settings.rs` — Windows Registry settings read/write
- [ ] Create `quickstt-core/src/error.rs` — error types
- [ ] Verify `cargo build --workspace` compiles
- [ ] Run `cargo test -p quickstt-ipc` — protocol roundtrip tests

**Files to create/modify:**
- `quickstt-rust/quickstt-core/src/stt_service.rs` — fix imports
- `quickstt-rust/quickstt-core/src/config.rs` — new
- `quickstt-rust/quickstt-core/src/engine.rs` — new
- `quickstt-rust/quickstt-core/src/ipc.rs` — new
- `quickstt-rust/quickstt-core/src/managers.rs` — new
- `quickstt-rust/quickstt-core/src/settings.rs` — new
- `quickstt-rust/quickstt-core/src/error.rs` — new

---

### Step 2: Settings Module (Windows Registry)
**Status:** [ ] NOT STARTED

**Reference C++ files:**
- `Source/native/stt_service_native.cpp` lines 211-400 (Settings struct, loadSettings())
- `Source/pill_widget.cpp` — all `QSettings` usage

**What to implement:**
- `Settings` struct with all fields listed in "Settings Storage" section above
- `load_settings()` — reads from `HKCU\Software\QuickSTT\Config` using `windows::Win32::System::Registry`
- `save_setting(key, value)` — writes a single value
- `Settings::data_root()` — returns exe dir or `%APPDATA%\QuickSTT`
- `Settings::models_root()` — returns `{data_root}/models`
- Handle both REG_SZ and REG_DWORD types (Qt writes both)
- Handle REG_MULTI_SZ for list values (wake words, close words, model lists)

**Tests:**
- Write a test that creates a temp registry key, writes settings, reads them back

---

### Step 3: STT Service Manager (Pipe IPC)
**Status:** [x] PARTIALLY DONE (skeleton exists, needs fixes and completion)

**Reference C++ files:**
- `Source/pill_widget.cpp` lines 969-1083 (startBackend, onBackendFinished, onBackendError, ensureBackendRunning)
- `Source/pill_widget.cpp` lines 2471-2504 (sendBackendCommand)
- `Source/pill_widget.cpp` lines 1647-1900 (onProcessOutput — parsing pipe events)
- `Source/native/stt_service_native.cpp` lines 163-170 (sendEvent — the service side)

**What to implement:**
- Fix existing `stt_service.rs` (import issues)
- `SttServiceManager::start()` — spawn `stt_service.exe`, capture stdout
- `SttServiceManager::stop()` — send QUIT, wait, kill
- `SttServiceManager::send_command()` — write InboundCommand to stdin
- `SttServiceManager::restart()` — stop + start with 2s delay
- Background task: read stdout lines, parse OutboundEvent, send VoiceEvent via channel
- Health check: 5s timer, auto-restart if process died
- Pending command queue: buffer commands when process not running, flush on start

**Tests:**
- Mock test: spawn `cmd.exe` as dummy process, send/receive lines

---

### Step 4: Win32 Main Window & Message Loop
**Status:** [ ] NOT STARTED

**Reference C++ files:**
- `Source/main.cpp` — single instance guard, activation server, setup wizard check
- `Source/pill_widget.cpp` lines 600-967 (PillWidget constructor)

**What to implement in `quickstt-gui/src/main.rs`:**
- `WinMain` entry point (or `main` with `#[windows_subsystem = "windows"]`)
- Single instance guard using `CreateMutexW` (like `QSharedMemory` in C++)
- Activation server: named pipe or `WM_COPYDATA` for "SHOW" command (like `QLocalServer`)
- Check `firstLaunch` / `setupCompleted` registry keys → show setup wizard if needed
- Initialize `tracing-subscriber` for logging
- Create tokio runtime
- Create main Win32 window (message-only window for IPC)
- Standard Win32 message loop: `GetMessageW` / `TranslateMessage` / `DispatchMessageW`
- Load `icon_app.ico` from exe dir, set class icon

**Key Win32 APIs needed:**
- `RegisterClassExW`, `CreateWindowExW`, `ShowWindow`, `UpdateWindow`
- `GetMessageW`, `TranslateMessage`, `DispatchMessageW`
- `LoadImageW` (for icon), `LoadCursorW`
- `CreateMutexW`, `GetLastError` (for single instance)

---

### Step 5: System Tray Icon
**Status:** [ ] NOT STARTED

**Reference C++ files:**
- `Source/pill_widget.cpp` lines 1938-2000+ (setupTray, updateTrayIcon)
- `Source/pill_widget.cpp` lines 1279-1380 (updateTrayIcon — drawing tray icon)

**What to implement:**
- `Shell_NotifyIconW` with `NOTIFYICONDATAW`
- Tray icon: load from `icon_app.ico` or draw programmatically
- Context menu: "Show Dashboard", "Toggle Mic", "Exit"
- Left-click: toggle mic
- Right-click: show context menu
- Balloon messages for status updates
- Custom callback message handler for tray icon events

---

### Step 6: Pill Widget (Floating Window)
**Status:** [ ] NOT STARTED

**Reference C++ files:**
- `Source/pill_widget.h` — full class definition
- `Source/pill_widget.cpp` — all methods
- `Source/pill_widget.cpp` lines 1383-1450 (customResize, paintEvent)
- `Source/pill_widget.cpp` lines 2423-2450 (updateWaveformFrame)
- `Source/pill_widget.cpp` lines 2300-2420 (mouse events, resizing, dragging)

**What to implement:**
- Create a borderless, topmost Win32 window (`WS_POPUP | WS_VISIBLE`, `WS_EX_TOPMOST | WS_EX_TOOLWINDOW`)
- Custom paint: rounded rectangle background, opacity support
- Child controls (as child windows):
  - Mic button (toggle recording)
  - Model combo box (PillComboBox equivalent)
  - Status text label
  - Close/minimize button
  - Model download button
- Dragging: `WM_NCHITTEST` → `HTCAPTION` for client area
- Resizing: detect edges/corners, `SetCursor`, `WM_MOUSEMOVE` handling
- Opacity: `SetLayeredWindowAttributes` or `UpdateLayeredWindow`
- Waveform visualization: Direct2D rendering in a sub-rect
- Blink animation: timer-based state toggle for recording indicator
- Auto-offload timer

**Direct2D waveform rendering:**
- Create `ID2D1HwndRenderTarget` on the pill window
- In `WM_PAINT`: clear, draw rounded rect background, draw waveform bars
- Waveform bars: use `ID2D1SolidColorBrush` with animated heights
- Match the C++ bar style: rounded caps, gradient colors

---

### Step 7: Dashboard Window (Settings)
**Status:** [ ] NOT STARTED

**Reference C++ files:**
- `Source/mainwindow.h` — all widget declarations
- `Source/mainwindow.cpp` — all setup methods

**What to implement:**
- Main window with tab control (`WC_TABCONTROL`)
- **Models tab:**
  - Local model list (checkable listbox)
  - Cloud model list
  - Model search/edit
  - Add/remove/download buttons
  - Model details panel (read-only text)
  - Backend combo for local models
- **Smart Home tab:**
  - Smart Life section: account mode, credentials, device tree
  - Android TV section: discovery, profiles, remote control buttons
  - Home Assistant section: URL, token, entity list
- **Wakeword tab:**
  - Engine combo (OpenWakeWord, Porcupine, Vosk)
  - Wake word list (checkable)
  - Close word list (editable)
  - Porcupine key edit
- **Updates tab:**
  - Check for updates button
  - Auto-update checkbox
  - Server URL list
- **General tab:**
  - Startup checkbox
  - Startup in background checkbox
  - Special commands checkbox
  - Haptics, sound checkboxes
  - Widget flexible checkbox
  - Appearance sliders (opacity, icon size, text size, text opacity, color)

**Win32 controls to use:**
- `WC_TABCONTROL`, `WC_LISTVIEW` (report mode with checkboxes), `WC_EDIT`, `WC_BUTTON`, `WC_COMBOBOX`, `WC_STATIC`, `WC_SCROLLBAR`
- `WC_TREEVIEW` for device tree
- Custom draw for colored items

---

### Step 8: Text Board Window
**Status:** [ ] NOT STARTED

**Reference C++ files:**
- `Source/text_board.h` — full class
- `Source/text_board.cpp` — all methods

**What to implement:**
- Borderless topmost window
- Title bar with chain/link button (attach/detach toggle)
- `QTextEdit` equivalent: `WC_EDIT` with `ES_MULTILINE | ES_READONLY | WS_VSCROLL`
- Custom scrollbar (owner-drawn)
- Auto-scroll to bottom on new text (unless user scrolled up)
- Attach/detach: when attached, follows pill widget position
- Opacity control
- Text size control
- Save settings on close

---

### Step 9: Setup Wizard
**Status:** [ ] NOT STARTED

**Reference C++ files:**
- `Source/setup_wizard.h`
- `Source/setup_wizard.cpp`

**What to implement:**
- Multi-page wizard using property sheet (`PROPSHEETPAGEW` / `PropertySheetW`)
- Pages: Welcome, Model Selection, Wakeword Configuration, Smart Home Setup, Completion
- On completion: write `setupCompleted=true`, `firstLaunch=false` to registry
- Apply settings from wizard selections

---

### Step 10: Cloud STT Manager
**Status:** [ ] NOT STARTED

**Reference C++ files:**
- `Source/cloud_stt_manager.h` — full class
- `Source/cloud_stt_manager.cpp` — all methods

**What to implement in `quickstt-core/src/managers/cloud_stt.rs`:**
- `CloudSttManager` struct with `reqwest::Client`
- Provider settings loading from registry
- API calls: OpenAI, Google, ElevenLabs, AssemblyAI, Sarvam, Reverie
- Audio file upload via HTTP POST
- Response parsing (JSON → transcript)
- Status tracking per provider/model
- All cloud model catalog functions (from `cloud_stt_manager.h` top-level functions)

**Dependencies to add to `quickstt-core/Cargo.toml`:**
- `reqwest = { version = "0.11", features = ["json", "multipart"] }`
- `tokio-tungstenite = "0.21"` (if WebSocket support needed)

---

### Step 11: Local Model Manager
**Status:** [ ] NOT STARTED

**Reference C++ files:**
- `Source/local_model_manager.h`
- `Source/local_model_manager.cpp`
- `Source/local_model_support.h`
- `Source/local_model_support.cpp`

**What to implement in `quickstt-core/src/managers/local_model.rs`:**
- Model catalog: all known models with metadata (size, engine, GPU requirements)
- Compute target detection (GPU enumeration via DXGI)
- Model installation: download from server, extract, verify markers
- Model uninstall: remove owned paths
- Model path resolution: find model directory by name matching
- Widget selection persistence
- Download progress reporting

---

### Step 12: Local Frontend STT Manager
**Status:** [ ] NOT STARTED

**Reference C++ files:**
- `Source/local_frontend_stt_manager.h`
- `Source/local_frontend_stt_manager.cpp`

**What to implement in `quickstt-core/src/managers/local_frontend.rs`:**
- `LocalFrontendSttManager` — manages parakeet/ggml-server subprocesses
- Queue-based transcription: enqueue audio files, process sequentially
- Audio preprocessing (deep filter, TEN VAD)
- Transcript extraction from stdout
- Process lifecycle management

---

### Step 13: Smart Life Manager (Tuya)
**Status:** [ ] NOT STARTED

**Reference C++ files:**
- `Source/smart_life_manager.h`
- `Source/smart_life_manager.cpp`

**What to implement in `quickstt-core/src/managers/smart_life.rs`:**
- Tuya API client (HTTP REST)
- Device discovery and control
- Account mode: cloud vs local
- Device tree management
- Quick toggle buttons
- Credential management (access ID/Key or username/password)

---

### Step 14: Android TV Manager
**Status:** [ ] NOT STARTED

**Reference C++ files:**
- `Source/android_tv_manager.h`
- `Source/android_tv_manager.cpp`

**What to implement in `quickstt-core/src/managers/android_tv.rs`:**
- mDNS/UDP discovery of Android TV devices
- Pairing protocol (PIN code)
- Remote control commands (keycodes, volume)
- Profile management (save/load/delete)
- Auto-reconnect on startup

---

### Step 15: Home Assistant Manager
**Status:** [ ] NOT STARTED

**Reference C++ files:**
- `Source/home_assistant_manager.h`
- `Source/home_assistant_manager.cpp`

**What to implement in `quickstt-core/src/managers/home_assistant.rs`:**
- WebSocket connection to HA server
- Entity list fetching
- Service call execution
- State monitoring
- Token-based auth

---

### Step 16: AHK Bridge
**Status:** [ ] NOT STARTED

**Reference C++ files:**
- `Source/ahk_bridge.h`
- `Source/ahk_bridge.cpp`

**What to implement in `quickstt-core/src/managers/ahk_bridge.rs`:**
- Spawn `AutoHotkey.exe` with bridge script
- Pipe-based command dispatch
- Transcript → AHK command mapping
- Special command parsing (function keys, media keys, etc.)
- Result handling (command executed / passthrough)

---

### Step 17: Optional Service Manager
**Status:** [ ] NOT STARTED

**Reference C++ files:**
- `Source/optional_service_manager.h`
- `Source/optional_service_manager.cpp`
- `Source/optional_service_support.h`
- `Source/optional_service_support.cpp`

**What to implement in `quickstt-core/src/managers/optional_service.rs`:**
- Service descriptor catalog
- Install/uninstall via HTTP download + extract
- Status checking (installed/missing/version)
- Package sequence handling

---

### Step 18: Command Processing & Voice Pipeline
**Status:** [ ] NOT STARTED

**Reference C++ files:**
- `Source/pill_widget.cpp` lines 2614-2800 (processRecognizedText, special command detection)
- `Source/pill_widget.cpp` lines 124-200 (special command parsing)

**What to implement:**
- `CommandProcessor` struct
- Text normalization (lowercase, trim, remove punctuation)
- Close word detection ("stop listening", "go to sleep")
- Special command detection:
  - Function keys: "function key 1" through "function key 24"
  - Media keys: play, pause, next, previous, volume up/down
  - Navigation: tab, enter, escape, arrow keys
  - Custom AHK commands
- `SendInput` for key simulation
- Cloud transcription routing
- Smart home command routing (Tuya, HA, Android TV)

---

### Step 19: Resource Files & Icons
**Status:** [ ] NOT STARTED

**Reference files:**
- `Source/mic_active.svg`
- `Source/mic_inactive.svg`
- `Source/WhiteTick.svg`
- `icon_app.ico`
- `resources_app.rc`

**What to implement:**
- Embed icons as resources in the Rust binary
- Use `winres` crate or `embed-resource` crate for `.rc` compilation
- Load SVG via `resvg` crate or convert to PNG/ICO at build time
- Or: use `LoadImageW` to load ICO from file at runtime (simpler)

**Add to `quickstt-gui/Cargo.toml`:**
```toml
[build-dependencies]
embed-resource = "2.4"
```

---

### Step 20: Integration, Testing & Polish
**Status:** [ ] NOT STARTED

**What to do:**
- Full integration test: launch GUI, verify STT service starts, send TOGGLE, receive events
- Test all manager integrations
- Verify registry read/write matches C++ format exactly
- Test single-instance behavior
- Test setup wizard flow
- Test model download/install
- Performance benchmark: memory usage, startup time vs C++ version
- Fix any `clippy` warnings
- Run `cargo test --workspace`
- Build release: `cargo build --release --package quickstt-gui`
- Verify output binary: `target/release/QuickSTT.exe`

---

## Build Commands

```bash
# Navigate to workspace
cd C:\Users\hemsh_sfya5gq\.gemini\antigravity\scratch\quick_stt_app\quickstt-rust

# Build everything
cargo build --workspace

# Build release (optimized)
cargo build --release --package quickstt-gui

# Run tests
cargo test --workspace

# Run specific crate tests
cargo test -p quickstt-ipc
cargo test -p quickstt-core

# Check without building
cargo check --workspace

# Lint
cargo clippy --workspace
```

---

## Key Dependencies to Add Later

These are NOT in the workspace `Cargo.toml` yet — add them when implementing the relevant step:

```tompl
# For Step 10 (Cloud STT)
reqwest = { version = "0.11", features = ["json", "multipart"] }

# For Step 19 (Resources)
embed-resource = "2.4"  # build-dependency

# For SVG rendering (if needed)
resvg = "0.37"

# For mDNS discovery (Android TV)
mdns-sd = "0.10"

# For WebSocket (Home Assistant)
tokio-tungstenite = "0.21"
futures-util = "0.3"
```

---

## Critical Rules

1. **DO NOT modify `Source/native/stt_service_native.cpp`** — it is the STT engine and must remain C++
2. **DO NOT modify the pipe protocol format** — `TYPE|payload\n` must match exactly
3. **Registry format must match** — Qt writes REG_SZ and REG_DWORD; Rust must read both
4. **All file paths use `\` on Windows** — the C++ code uses Windows paths throughout
5. **Error handling: use `anyhow` for propagation, `thiserror` for custom types**
6. **Async: use `tokio` for all I/O, `std::sync::mpsc` or `tokio::sync::mpsc` for channels**
7. **Win32: always use the `W` (wide/Unicode) variants of APIs, never `A`**
8. **Threading: Win32 UI calls must be on the main thread; spawn tokio tasks for async work**
