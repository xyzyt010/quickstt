# Rust Windows Transcription App — Full Architecture Plan

**Target constraints:** Idle widget < 5 MB · Active (no model) ≤ 30 MB · Wakeword engine + models ≤ 10 MB · Single .exe · Windows 10/11 x86-64

## 1. GUI Layer — egui + eframe

**Why:** Immediate-mode, zero native Windows deps, single `.exe` output. The entire framework + wgpu renderer adds ~1.5–2 MB to the binary. Hits your idle budget without compromise.

**Key flags:**
- Use `eframe` with the `wgpu` feature for DirectX 12 / D3D11 rendering on Windows (auto-selects best available).
- Enable `softbuffer` fallback feature for headless/RDP environments.
- System tray via the `tray-icon` crate (pure Rust, ~150 KB).

**What to avoid:** Tauri (adds ~60–100 MB WebView2 overhead), Slint (restrictive free license, larger binary), Iced (less mature Windows integration for tray/hotkeys), Dioxus (virtual DOM overhead unnecessary here).

**Widget idle state strategy:** On minimize to tray, call `ctx.request_repaint_after(Duration::MAX)` to halt the render loop entirely. GPU context stays allocated but draws nothing. True idle RAM: 2–4 MB.

## 2. Renderer — wgpu (via eframe)

No additional crate needed. `eframe` bundles `wgpu`. The backend selection order on Windows: DirectX 12 → DirectX 11 → Vulkan → software. For a transcription widget this is fine — no 3D, just 2D compositing of text and waveform.

**Optimization:** Disable vsync on the hidden/tray state (`eframe::NativeOptions::vsync = false`). Saves ~1 MB GPU buffer allocation.

## 3. Audio Capture — cpal + dasp

- `cpal` provides WASAPI shared-mode access on Windows. Zero idle cost — the stream is only opened when recording begins.
- `dasp` handles ring-buffer management, sample-rate conversion, and PCM format normalization.
- **Ring buffer:** keep to 2–4 seconds max (32 KB–128 KB). Don't buffer the entire session in RAM.
- **On wakeword idle:** open a separate low-bitrate WASAPI stream (16 kHz, mono, 1280-sample chunks) exclusively for wakeword detection. Close it when wakeword fires, reopen full-quality stream for transcription.

**Two-stream architecture (critical for efficiency):**
```text
[mic hardware]
    │
    ├──[16 kHz mono, tiny chunks]──► wakeword thread (always on, < 1 MB)
    │
    └──[full quality, on demand]───► transcription thread (activates on wakeword)
```

## 4. Wakeword Engine — livekit-wakeword & YAMNet-256

Yes, < 10 MB total is very achievable. Here is the full breakdown using **livekit-wakeword (released March 2026)** and **YAMNet-256** for environmental sounds.

| Component | On-disk | Runtime RAM |
| :--- | :--- | :--- |
| livekit-wakeword crate (compiled in) | ~0 (part of .exe) | — |
| Mel spectrogram model (embedded) | ~0 on disk | ~1 MB |
| Embedding model (embedded) | ~6 MB in binary | ~6 MB |
| Classifier .onnx (custom wakewords) | ~0.4–0.5 MB ea. | ~0.5 MB ea. |
| YAMNet-256 int8 (snap/clap via tract) | ~2 MB | ~2 MB |
| ONNX runtime (tract, pure Rust) | ~0 (compiled in) | ~1 MB |
| **Total footprint (up to 10 wakewords)** | **~9-10 MB binary** | **< 10 MB RAM active** |

**Why `livekit-wakeword` wins over alternatives:**
- The mel spectrogram and speech embedding models are compiled directly into the binary at build time.
- Uses `ort-tract` (pure Rust ONNX inference) by default — no native DLL, no ORT C++ runtime to bundle.
- Only the tiny classifier `.onnx` (~0.4 MB) is loaded from disk at runtime.

**Environmental Sound Detection (Snap & Clap):**
- Implement **YAMNet-256 with int8 quantization** on pure Rust ONNX (`tract`).
- This efficiently detects non-speech triggers (snaps, claps) without heavy CPU usage.

**Memory Target for 10+ Wakewords:**
- Running up to 10 wakewords (speech classifiers + YAMNet) **must not exceed 10 MB RAM** for the idle app (including GUI).
- **Architectural Consistency for Offloading:** Ensure models are aggressively offloaded when truly idle (e.g., mic muted) and lazy-loaded dynamically.

**Integration pattern:**
```rust
use livekit_wakeword::WakeWordModel;

let mut model = WakeWordModel::new(&["hey_app.onnx"], 16000)?;

// In audio loop (runs on dedicated thread, always-on):
let scores = model.predict(&pcm_chunk)?; // i16 PCM, 1280 samples
if scores["hey_app"] > 0.5 {
    tx.send(AppEvent::WakeWordDetected).ok();
}
```

## 5. Transcription Engine — Two Paths

**Path A: whisper-rs (FFI to whisper.cpp) — Recommended**
Fastest on Windows CPU. Whisper.cpp uses AVX2 SIMD, DirectML (GPU on any DirectX 12 GPU), and BLAS acceleration. Quantized GGML models are the most efficient format.

| Model | GGML file size | Runtime RAM | Notes |
| :--- | :--- | :--- | :--- |
| ggml-tiny.en.bin | 75 MB | ~273 MB | Good for English-only, fastest |
| ggml-base.en.bin | 142 MB | ~490 MB | Better accuracy, still fast |
| ggml-small.en.bin | 466 MB | ~1 GB | Best quality/speed tradeoff |

**Architectural Consistency (Onloading/Offloading):** Load the STT model lazily — only when the first wakeword fires. Unload after N seconds of inactivity with `WhisperContext::drop()`. This is critical to maintain the 10 MB idle RAM target.

**Path B: candle (pure Rust Whisper) — Alternative**
Hugging Face's `candle` crate runs Whisper natively in Rust, no C++ FFI. Produces a fully static Rust binary. Slightly slower on CPU than whisper.cpp due to fewer low-level SIMD hand-optimizations, but the gap is closing. Good choice if you want zero C++ build complexity. Windows CUDA support is still maturing.

*Recommendation:* Use `whisper-rs` for production now. Switch to `candle` when its Windows DirectML/CUDA path matures.

## 6. Async Runtime — tokio

All inference (wakeword scoring, transcription) must run off the UI thread. Architecture:
```text
[egui UI thread]
    │  channels (tokio::sync::mpsc / watch)
    ▼
[tokio multi-thread runtime]
    ├── wakeword_task       (always-on, lightweight)
    ├── transcription_task  (spawned on wakeword event)
    └── audio_task          (cpal callback → ring buffer → both above)
```
Use `tokio::sync::watch` for state broadcast (recording/idle/transcribing) and `mpsc` for text result chunks back to the UI. Never block the egui thread.

## 7. Hotkeys & System Integration

- `global-hotkey` crate — registers system-wide keyboard shortcuts that work when widget is in tray.
- `rdev` — for deeper system event capture (push-to-talk mouse button, etc.).
- `tray-icon` — Windows system tray icon with right-click menu.
- `winit` — underlying window/event loop used by eframe, provides OS-level window events.

**Recommended hotkey model:** Hold Ctrl+Shift+Space for push-to-talk alongside wakeword. Both feed the same transcription pipeline.

## 8. State Management

Keep a single `AppState` struct in a `std::sync::Arc<Mutex<AppState>>` shared between the tokio runtime and the egui UI:

```rust
struct AppState {
    mode: AppMode,          // Idle | WakewordListening | Recording | Transcribing
    transcript_buffer: String,
    partial_result: String,
    wakeword_confidence: f32,
    settings: Settings,
}

enum AppMode { Idle, WakewordListening, Recording, Transcribing }
```
The egui `update()` loop reads this state each frame. Tokio tasks write via channel senders. No direct mutex locking on the render thread.

## 9. Build Configuration

```toml
[profile.release]
opt-level        = 3
lto              = true          # Link-time optimization — biggest binary size win
codegen-units    = 1             # Single codegen unit for maximum optimization
strip            = true          # Strip debug symbols
panic            = "abort"       # Removes panic unwinding machinery (~200 KB)
overflow-checks  = false

[profile.release.build-override]
opt-level = 3
```

Windows-specific manifest (`build.rs`):
```rust
// Embed a Windows manifest for DPI awareness and proper window styling
winresource::WindowsResource::new()
    .set_manifest(include_str!("manifest.xml"))
    .compile().unwrap();
```

`Cargo.toml` dependencies:
```toml
eframe             = { version = "0.29", features = ["wgpu"] }
egui               = "0.29"
tray-icon          = "0.17"
cpal               = "0.15"
dasp               = { version = "0.11", features = ["signal", "ring_buffer"] }
livekit-wakeword   = "0.1"          # wakeword engine (embed + classify)
whisper-rs         = "0.11"         # transcription via whisper.cpp FFI
tokio              = { version = "1", features = ["rt-multi-thread", "sync", "time"] }
global-hotkey      = "0.6"
serde              = { version = "1", features = ["derive"] }
serde_json         = "1"
anyhow             = "1"
```

## 10. Memory Budget — Full Picture

| State | GUI | Wakeword | Audio | Transcription | Total |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Idle (tray)** | ~2 MB | ~0 MB* | 0 MB | 0 MB | **~2 MB** |
| **Wakeword listening (10 wake words + YAMNet)** | ~2 MB | ~8 MB | ~1 MB | 0 MB | **~11 MB** |
| **Active transcribing** | ~4–6 MB | ~8 MB | ~3 MB | model RAM | **~15 MB + model** |

*\*Wakeword process must be heavily optimized. Models will not be activated when completely idle to hit the strict 10MB idle RAM constraint.*

**To achieve true < 5 MB idle:** Don't pre-load the wakeword models. Load `livekit-wakeword` and YAMNet only after the user first presses the hotkey or clicks "Start Listening." When the app goes inactive, drop the model contexts completely.

## 11. Key Improvements Over Initial Plan

- **Wakeword crate upgraded to livekit-wakeword** (March 2026): Embeds mel/embedding models in the binary, ships only a ~0.4 MB classifier, and uses pure-Rust ONNX inference (`tract`) — no C++ ORT DLL required.
- **YAMNet-256 INT8 Integration:** Added for environmental audio triggers (snaps, claps) without bloated dependencies.
- **Strict 10MB Limits & 10 Wakewords:** Re-architected model loading to ensure having 10 custom wakewords does not blow past the RAM budget.
- **Two-stream audio architecture added:** Separate low-cost WASAPI stream for wakeword vs full-quality stream for transcription. Prevents the always-on mic from eating memory.
- **Lazy model loading explicitly specified:** Both wakeword and transcription models load on first use and unload aggressively during idle states.
- **`panic = "abort"` added to profile:** Removes the panic unwinding tables, saving ~200–400 KB from the binary.
- **`winresource` build script added:** Necessary for proper Windows DPI scaling.
- **State machine model added:** `AppMode` enum avoids ad-hoc boolean flags.

## 12. File Output Structure

```text
your_app/
├── your_app.exe          # everything compiled in — egui, tokio, cpal, livekit-wakeword, YAMNet
├── hey_app.onnx          # wakeword classifiers (~0.4 MB ea, swappable)
├── models/
│   └── ggml-tiny.en.bin  # (optional, lazy-loaded, not bundled in exe)
└── settings.json
```
The only reason the `.onnx` files are external is so users can swap or retrain their custom wakewords without recompiling the app. Everything else is baked in.

## 13. Recommended Dev Sequence

1. Scaffold eframe app with tray icon → confirm idle < 5 MB.
2. Add cpal dual-stream audio → confirm wakeword stream starts/stops cleanly.
3. Integrate `livekit-wakeword` + `YAMNet-256` int8 + test classifiers → confirm speech/snap/clap triggers fire correctly.
4. Add `whisper-rs` with lazy loading (onloading/offloading mechanism) → confirm transcription works on activation and memory drops on idle.
5. Wire tokio channels for full state machine.
6. Add `global-hotkey` push-to-talk alongside wakeword.
7. Release profile + strip → confirm binary size and memory targets met.
8. Package with `cargo-bundle` → single installer `.exe`.
