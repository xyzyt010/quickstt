# QuickSTT Rust — Full Backend + GUI Replication Plan

## Current State (What's Broken)

1. **No working dropdown** — model list cycles on click instead of real dropdown
2. **No STT models** — no model catalog, no transcription pipeline
3. **No VAD** — C++ uses TEN-VAD (DLL), Rust has nothing
4. **Wakewords not wired** — model discovery works but detection never runs
5. **No audio capture connected** — `AudioCaptureManager` exists but never instantiated
6. **GUI icons wrong** — hand-drawn approximations instead of actual SVG files
7. **Pill shape imprecise** — chevrons, close button sizing off from C++
8. **Demo waveform** — sine wave fake data, not real audio levels
9. **TextBoard static** — no real transcript text ever appears
10. **Auto-offload cosmetic** — sets a bool, sends no command
11. **Dashboard viewport read-only** — can't switch tabs or interact

---

## Architecture Decision: Subprocess vs In-Process

The C++ app uses a **subprocess model**: `stt_service.exe` handles audio/VAD/STT via pipe protocol. The Qt GUI is just the frontend.

We'll replicate this in Rust:
- **In-process**: Audio capture (cpal), wakeword detection (livekit-wakeword), VAD, audio level metering
- **Subprocess**: STT transcription via external runtimes (whisper-cli.exe, sherpa-onnx-offline.exe)
- **Fallback**: If whisper-rs compiles, use it in-process

---

## Part A: Exact GUI Replication (8 files)

### A1. SVG Icons — `quickstt-gui/src/icons.rs` (NEW)
- Copy `mic_active.svg`, `mic_inactive.svg`, `Untitled-1.svg` (app icon) from `Source/` to `quickstt-rust/assets/`
- Embed via `include_bytes!` at compile time
- Use `resvg` + `usvg` to render SVGs to RGBA pixel buffers at desired size
- Upload as `egui::TextureHandle` on first frame
- Functions: `load_icons(ctx) → IconSet`, `IconSet.mic_active`, `IconSet.mic_inactive`, `IconSet.app_icon`
- Exact fill color `#e3e3e3` matches C++ SVGs

### A2. Proper ComboBox Dropdown — in main.rs
- Replace click-to-cycle with `egui::ComboBox::from_id_salt("model_select")`
- Style: bg #333333, text #DDDDDD, 12px font, 12px border-radius, 24px height
- Width: `max(120, min(220, pill_w/2 - 20))`
- Custom painted dropdown arrow: white chevron at right-14px, 2.2px stroke, round caps
- Populated from model catalog (Part D)

### A3. Exact Pill Shape
- `drawRoundedRect` equivalent: `painter.rect_filled(pill_rect, Rounding::same(pill_r), PILL_BG)`
- Already correct for default 360×50 r25, but verify exact match
- Fix close button: exact `✕` (U+2715), 18×18px, 12px from right, #888→#FF4444 hover, 14px bold
- Fix collapse chevron: 22×22 hit area (not 26), exact arm geometry (-5,2.5)→(0,-2.5)→(5,2.5), 2.0px stroke, #CCCCCC, round caps
- Fix red dot: 10px, drawn as filled circle, exact state colors
- Fix mic button: `iconSize + 6` wide, full pill height

### A4. TextBoard Exact Replication
- Attach/detach: when attached, snap to pill bottom, match pill width
- When detached, independent window (separate viewport)
- Title bar: 28px, #1E1E1E, left label 11px #CCCCCC, right chain button 26×26
- Text area: rgba(20,20,20,240), white text, Consolas/monospace 14px, padding 6px
- Auto-scroll to bottom unless user scrolled up
- Custom scrollbar: 14px width, groove rgba(255,255,255,18), handle rgba(245,245,245,182), 3 center dots

### A5. Dashboard as Real Interactive Window
- Instead of `show_viewport_immediate` (can't borrow self), use shared state via `Arc<Mutex<DashboardState>>`
- Full tab switching: Models, General, Wake Word
- All sliders functional and synced to pill state
- Model list from catalog with radio-button selection

---

## Part B: Audio Pipeline (4 files)

### B1. `quickstt-core/src/audio/capture.rs` — UPDATE
- Actually instantiate `AudioCaptureManager` from orchestrator
- Dual output: wakeword stream (i16, 16kHz, 1280 samples) + transcription stream (f32, 16kHz)
- Audio level calculation: RMS → 0-100 scale, sent to GUI for waveform

### B2. `quickstt-core/src/audio/vad.rs` — NEW
- Energy-based VAD matching TEN-VAD behavior:
  - `speech_threshold`: configurable (default 0.50)
  - `speech_hold_frames`: 12 frames hold after speech detected (like C++ `m_tenVadSpeechHoldFrames`)
  - Process in 256-sample hops
  - Output: `VadResult { speech_likely: bool, probability: f32 }`
- Optional: Add webrtc-vad crate for better accuracy

### B3. `quickstt-core/src/audio/pipeline.rs` — NEW
- `AudioPipeline`: receives raw audio chunks, runs VAD, routes to wakeword/STT
- Speech segmentation for cloud/frontend mode:
  - Pre-roll buffer (80ms-1250ms)
  - Utterance capture on speech detection
  - Termination: max duration, silence threshold, hard silence
  - Write WAV segments for STT

### B4. `quickstt-core/src/audio/mod.rs` — UPDATE
- Add `pub mod vad;` and `pub mod pipeline;`

---

## Part C: Wakeword Pipeline (2 files)

### C1. `quickstt-core/src/ml/wakeword.rs` — REWRITE
- `WakeWordEngine`:
  - Load ONNX models via `livekit-wakeword`
  - Process 16kHz mono i16 chunks continuously
  - Per-model hit counting: `hit_counts: HashMap<String, u32>`
  - Trigger threshold: configurable per model (from config.json), default 0.50
  - Hit requirement: 2 consecutive above-threshold (matching C++ `wakeHitRequirement = 2`)
  - Cooldown: 1.0s since last activation (`last_activation_time`)
  - Suppression: `suppress_until: Option<Instant>` (0.8s-1.2s after deactivation)
  - Predictive preload: when any score ≥ 0.35, start loading STT model in background

### C2. Wakeword → Widget Activation Flow
- On trigger: set mode=Active, send state update to GUI
- GUI receives state change → shows widget, starts waveform, shows "Listening..."
- If widget was hidden: auto-show (unless `suppress_auto_show`)
- Wakeword detected → model RELOAD if offloaded → mode=Listening

---

## Part D: STT Engine + Model Catalog (3 files)

### D1. `quickstt-core/src/models/catalog.rs` — NEW
Model catalog matching C++ `allDescriptors()`:
```
- Whisper Tiny EN Q5 (32 MB) — whisper_cpp engine
- Whisper Tiny EN Q8 (44 MB) — whisper_cpp engine  
- Whisper Base EN Q8 (74 MB) — whisper_cpp engine
- Whisper Small EN Q5 (190 MB) — whisper_cpp engine
- Whisper Small EN Q8 (264 MB) — whisper_cpp engine
- Moonshine v2 Tiny EN (43 MB) — sherpa_onnx engine
- Moonshine v2 Base EN (135 MB) — sherpa_onnx engine
- NVIDIA Parakeet CTC 110M INT8 (126 MB) — sherpa_onnx engine
- NVIDIA Parakeet TDT 0.6B v3 INT8 (640 MB) — sherpa_onnx engine
```
Each entry: `ModelDescriptor { name, engine_family, model_dir, file_pattern, size_mb, installed: bool }`
- `installed` checked by scanning `%APPDATA%\QuickSTT\models\` directory
- Dropdown populated from installed models only (with "Not installed" grayed)

### D2. `quickstt-core/src/models/engine.rs` — NEW
STT engine dispatcher:
- `WhisperCppEngine`: spawns `whisper-cli.exe` with WAV path, reads stdout for transcription
- `SherpaOnnxEngine`: spawns `sherpa-onnx-offline.exe` with model args
- Common trait: `SttEngine::transcribe(wav_path) → Result<String>`
- Model loading = checking files exist; offloading = killing subprocess
- RAM compaction: `SetProcessWorkingSetSize(-1, -1)` after offload

### D3. `quickstt-core/src/models/mod.rs` — NEW
- `pub mod catalog;`
- `pub mod engine;`
- `ModelManager`: tracks active model, handles switching, load/offload state

---

## Part E: Orchestration Rewrite (1 file)

### E1. `quickstt-core/src/orchestration.rs` — MAJOR REWRITE
Connect everything:
```
Audio Capture (cpal)
    ├→ Wakeword Engine (livekit-wakeword, always running in IDLE)
    ├→ Audio Level Meter → GUI waveform  
    └→ VAD + Speech Segmenter
         └→ STT Engine (whisper-cli / sherpa-onnx subprocess)
              └→ Recognized Text → GUI TextBoard + SendInput
```

State machine:
- IDLE: wakeword listening, audio level metering, auto-offload timer
- LISTENING: VAD active, speech segmentation, wakeword still checking for close words
- TRANSCRIBING: WAV segment sent to STT engine, waiting for result
- OFFLOADED: STT model freed, wakeword still running

Commands handled:
- StartListening → IDLE→LISTENING, load model if offloaded
- StopListening → LISTENING→IDLE, suppress wakeword 0.8s
- AudioChunk → route to wakeword + VAD + level meter
- WakewordTriggered → auto-activate if in IDLE
- TextRecognized → send to GUI, check for close words
- ModelSwitch → offload current, load new
- Offload/Reload → free/load STT model

---

## Part F: Dependency Changes

### quickstt-core/Cargo.toml
```toml
# ADD:
webrtc-vad = "0.4"          # Voice activity detection
hound = "3.5"                # WAV file writing for STT segments
```

### quickstt-gui/Cargo.toml  
```toml
# ADD:
resvg = "0.43"               # SVG rendering for exact icons
usvg = "0.43"                # SVG parsing
image = "0.25"               # Image buffer conversion
```

### Workspace Cargo.toml
```toml
# ADD to [workspace.dependencies]:
webrtc-vad = "0.4"
hound = "3.5"
resvg = "0.43"
```

---

## File Change Summary

| File | Action | Lines (est) |
|------|--------|-------------|
| `quickstt-rust/assets/mic_active.svg` | COPY from Source/ | — |
| `quickstt-rust/assets/mic_inactive.svg` | COPY from Source/ | — |
| `quickstt-rust/assets/app_icon.svg` | COPY from Source/Untitled-1.svg | — |
| `quickstt-gui/src/icons.rs` | NEW | ~120 |
| `quickstt-gui/src/main.rs` | REWRITE | ~900 |
| `quickstt-gui/src/lib.rs` | UPDATE | +1 line |
| `quickstt-gui/Cargo.toml` | UPDATE | +3 deps |
| `quickstt-core/src/audio/vad.rs` | NEW | ~80 |
| `quickstt-core/src/audio/pipeline.rs` | NEW | ~200 |
| `quickstt-core/src/audio/mod.rs` | UPDATE | +2 lines |
| `quickstt-core/src/models/catalog.rs` | NEW | ~150 |
| `quickstt-core/src/models/engine.rs` | NEW | ~180 |
| `quickstt-core/src/models/mod.rs` | NEW | ~5 |
| `quickstt-core/src/ml/wakeword.rs` | REWRITE | ~200 |
| `quickstt-core/src/orchestration.rs` | REWRITE | ~350 |
| `quickstt-core/src/lib.rs` | UPDATE | +1 line |
| `quickstt-core/Cargo.toml` | UPDATE | +2 deps |
| `Cargo.toml` (workspace) | UPDATE | +3 deps |

**Total estimated new/changed code: ~2,200 lines across 18 files**

---

## Build Order (after all code is written)

1. `cargo build --features wakeword` (no whisper-rs, avoids broken bindings)
2. Fix any compilation errors
3. Copy wakeword models + SVG assets to target/debug/
4. Test launch
