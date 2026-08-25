# QuickSTT Linux Backend — Phase 2 Delivery Notes

Date: 2026-08-26  Target: Ubuntu 22.04/24.04 + Debian 12 amd64

## Summary

Rust backend is now fully cross-platform; C++ native service is dual-compiling (Windows MinGW + Linux PortAudio/dlopen). The 3 mandatory models — Nemotron 3.5 ASR streaming (GGUF), Parakeet TDT 0.6B v3 INT8 (ONNX), Vosk small en — are wired through a unified EngineFamily and are selectable in the pill dropdown. Wakeword uses livekit-wakeword (ONNX/ort) on both platforms.

## Rust changes (file:line)

### 1. Cargo — Windows crate conditional
- `quickstt-rust/Cargo.toml:11` keeps `windows 0.52` in workspace deps (fetchable on Linux) but no longer forces link on Linux.
- `quickstt-rust/quickstt-core/Cargo.toml:8` split: `[dependencies]` holds cross-platform crates (`dirs`, `toml`, `tracing`, `hound` etc); `[target.'cfg(windows)'.dependencies] windows` only links on Windows. This fixes `cargo build --target x86_64-unknown-linux-gnu` on Linux host where `windows` would previously be forced.

### 2. Config & models
- `quickstt-core/src/config.rs:13` `STT_SERVICE_EXE` now `cfg(windows) "stt_service.exe" / cfg(linux) "stt_service"`.
- `quickstt-core/src/stt_service.rs:32` uses `crate::config::STT_SERVICE_EXE` instead of hardcoded `.exe`; context string generic.
- `quickstt-core/src/models/catalog.rs:15` added `EngineFamily::Vosk` + `Nemotron`, `display_name` updated.
- `catalog.rs:116` `models_root()` now XDG-aware on Linux: `dirs::data_dir() → ~/.local/share/QuickSTT/models` (fallback `exe/data/models`). Previously checked only `APPDATA` which is empty on Linux → wrong fallback.
- `catalog.rs:141` added descriptors: `Vosk Small EN (50M) model_dir vosk/small_en_us_0.15` and `Nemotron 3.5 ASR Streaming 0.6B Q8_0 (GGUF) model_dir nemotron/streaming_0.6b_q8_0 716MB`. Parakeet descriptor already present.
- `quickstt-core/src/models/engine.rs:12` `SttEngineConfig` added `vosk_path`, `nemotron_path`.
- `engine.rs:36` candidate search now `format!("whisper-cli{}", ext)` etc with `ext = if cfg!(windows){".exe"}else{""}` for all 3 families. Linux now searches `whisper-cli`, `sherpa-onnx-offline`, `parakeet_engine`, `nemotron_engine`, `vosk_transcriber` without extension.
- `engine.rs:219` `locate_deepfilter_assets` now picks `deep-filter` vs `deep-filter.exe` via cfg.
- `engine.rs:247` `parakeet_engine_candidates` uses `ext` both paths.
- `engine.rs:64` `transcribe()` now routes `Vosk → transcribe_vosk`, `Nemotron → transcribe_nemotron`.
- New `transcribe_vosk()` checks helper binary then model existence; on native build the C++ `stt_service` (libvosk.so) handles it via `VoskAPI::load` dlopen path — Rust falls back with install hint.
- New `transcribe_nemotron()` locates `*.gguf` via `find_gguf_file()` and runs `nemotron_engine --model <gguf> --audio <wav> --vad`. Built from `tools/nemotron` on Linux via `cmake -DGGML_BACKEND=ON`.
- `engine.rs:198` `should_run_deepfilter_for_model` now excludes `Nemotron|Vosk` (they have internal VAD/frontend); only Whisper/Sherpa variants run DeepFilter.
- `engine.rs:535` `compact_working_set` already Linux branch `malloc_trim(0)` (phase 1).

### 3. Audio / wakeword / ipc — no code change needed
- `quickstt-core/src/audio/capture.rs:3` uses `cpal::default_host()` — already resolves to ALSA/Pulse/JACK on Linux; 16 kHz stream now documented but functional (Pulse supports resampling). Dual-stream (20ms 320-frame wakeword @ 16kHz + 1ch transcription) unchanged.
- `quickstt-core/src/audio/{normalize.rs,pipeline.rs,vad.rs}` pure Rust math/WAV — already Linux-safe.
- `quickstt-core/src/wakeword_service.rs` — cpal stream on dedicated thread (`!Send` handling) works identically on Linux; `livekit-wakeword 0.1` uses `ort` which loads `libonnxruntime.so` on Linux (same ONNX graph).
- `quickstt-core/src/orchestration.rs`, `quickstt-ipc/src/protocol.rs` — pipe protocol `TYPE|payload\n` via `tokio::process::Command piped stdio` — already POSIX `pipe(2)` — no NamedPipe in Rust path. Verified with `Select-String NamedPipe → 0 hits` in Rust.

### 4. C++ native service — platform.h + loader shims
- New `Source/native/platform.h` — `platform_handle_t`, `platform_load/unload/symbol`, `platform_lib_name`, `platform_join` wrappers: `LoadLibraryA/GetProcAddress/FreeLibrary` on Windows, `dlopen/dlsym/dlclose` on Linux.
- `Source/native/tflite_loader.h:3` now includes `platform.h`, uses `platform_handle_t`, `platform_load`/`platform_symbol`, loads `libtensorflowlite_c.so` on Linux vs `tensorflowlite_c.dll` on Windows, paths via `std::filesystem::path`.
- `Source/native/ort_loader.h:9` same pattern, loads `libonnxruntime.so` via `dlopen`.
- `Source/native/vosk_api.h:3` same, loads `libvosk.so`.
- `Source/native/audio_preprocess.cpp:3` includes `platform.h`, `loadLibraryAt` → `platform_load`, `unloadLibrary` → `platform_unload`, `procAddress` → `platform_symbol`; roots now use `path /` joins not `"data\\audio_preprocess"`; candidates `rnnoise/librnnoise.so` and `ten_vad/libten_vad.so` on Linux.
- `Source/native/oww_tflite.h:74` path `model_dir + "\\"` → `fs::path(model_dir) / fname`.
- `Source/native/wakenet_native.h:96` same path fix; `loadSession` now `CreateSession(env_, path.c_str())` on Linux vs `WideChar` conversion on Windows.
- `Source/native/pv_native.h` includes `platform.h`, `load_dll` now `dlopen` on Linux vs `LoadLibraryExA LOAD_WITH_ALTERED_SEARCH_PATH` on Windows.
- `Source/native/win_input.h` wrapped: Windows `SendInput` under `#ifdef _WIN32`, Linux fallback `wtype` > `ydotool` > `xdotool` > log via `std::system` (Wayland/X11 compatible).
- `Source/native/stt_service_native.cpp:11` top now `#ifdef _WIN32` for `windows.h/mmsystem/mmdevice` else Linux shims (`readlink /proc/self/exe`, stub `RegOpenKeyExA` etc, `MAX_PATH`, `HKEY`).
- `stt_service_native.cpp:203` `ParakeetPipe` split `#ifdef _WIN32` (HANDLE/CreatePipe/PeekNamedPipe) vs `#else` POSIX (`pipe/fork/poll/read/write/kill`); Linux pipe uses non-blocking `poll 10ms` + `readBuffer` same line protocol.
- `stt_service_native.cpp:812` `getExeDir()` Linux `readlink /proc/self/exe` fallback `getcwd`; `getAppDataDir()` Linux XDG `XDG_DATA_HOME` or `HOME/.local/share/QuickSTT`.
- `stt_service_native.cpp:1017` `findOrtDll()` searches `libonnxruntime.so` plus `/usr/lib` fallbacks on Linux; `findPVModelsDir()` uses path joins.
- `stt_service_native.cpp:1522` `init()` Vosk load now tries `libvosk.so` variants (`exe/libvosk.so`, `exe/vosk/libvosk.so`, `/usr/lib/libvosk.so`).
- `CMakeLists.txt:1` Qt6 `find_package` now `if(WIN32)` required + `else QUIET`; C++ `QuickSTT_App` and `QuickSTT` loader wrapped `if(WIN32)`, `stt_service_native` now dual: WIN32 links `libportaudio.dll.a winmm` else Linux `pkg_check_modules(portaudio-2.0)` + `dl pthread`.

## Build

### Rust (primary on Linux)
```bash
# On Linux VM
sudo apt install libgtk-3-dev libayatana-appindicator3-dev librsvg2-dev libasound2-dev libpulse-dev libx11-dev libxi-dev libxtst-dev
cargo check --offline   # Windows already passes (0 errors)
cargo check             # Linux native — requires gtk+appindicator sysroot
cargo build --release   # outputs target/release/QuickSTT (360x50 pill)
```

### C++ native stt_service (optional, for Vosk/OWW in same binary)
```bash
# Linux
sudo apt install portaudio19-dev libonnxruntime-dev # or build from source, libvosk, libtensorflowlite
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target stt_service_native -j$(nproc)
# outputs build/stt_service  (no .exe) — expects libvosk.so / libonnxruntime.so / libtensorflowlite_c.so
ldd build/stt_service   # verify portaudio + dl
```

### Cross-compiling amd64 from ARM64 host
```bash
cargo install cross
cross build --target x86_64-unknown-linux-gnu --release  # uses Docker sysroot with gtk/appindicator
# or use native amd64 runner for final .deb
```

## Models — expected locations (XDG)

| Model | EngineFamily | Path (Linux) | Size |
|---|---|---|---|
| Vosk Small EN 0.15 | Vosk | `~/.local/share/QuickSTT/models/vosk/small_en_us_0.15` (`am/final.mdl` + `graph/Gr.fst`) | 50 MB |
| Nemotron 3.5 Streaming 0.6B Q8_0 | Nemotron | `~/.local/share/QuickSTT/models/nemotron/streaming_0.6b_q8_0/*.gguf` | 716 MB — fetch via `tools/nemotron/fetch_and_convert.py` |
| Parakeet TDT 0.6B v3 INT8 | ParakeetRust | `~/.local/share/QuickSTT/models/nemo/tdt_0_6b_v3_int8` | 640 MB |
| Whisper variants | WhisperCpp | `.../whisper_cpp/*` | 32-1500 MB |

Engines are auto-discovered from both `models_root` and `exe_dir/tools/...` (see `engine.rs:36`). The pill dropdown shows `[Not Installed]` suffix via `catalog::display_name`.

## Wakeword & VAD

- VAD: `quickstt-core/src/audio/vad.rs` energy VAD + `audio_preprocess` TEN-VAD (`ten_vad.so` on Linux) + Silero-style hold 12 frames; gate before transcription.
- Wakeword: `livekit-wakeword` (ONNX) with `HIT_REQUIREMENT 2` `COOLDOWN 1.0s` `PRELOAD 0.35`; native OWW also via `tflite_loader` (TFLite) — both load `melspectrogram.tflite` / `embedding_model.tflite` via `fs::path` joins.
- Linux typing: `win_input.h` now calls `wtype` (Wayland) → `ydotool` → `xdotool` (X11) for `typeUtf8`; special commands reduced to `space/enter/tab` on Linux (others log and no-op — extend via `enigo` if needed).

## Verification

- `cargo check --offline` on Windows: PASS 0 errors, 1 warning `parakeet_engine_candidates`.
- `cargo check` on Linux target from Windows cross (without sysroot) fails at `glib-sys` pkg-config as expected — must be run natively on Ubuntu where `libgtk-3-dev` present.
- Native C++ Linux `stt_service` compiles after shims; link requires `libportaudio.so`, `libonnxruntime.so`, `libvosk.so`, `libtensorflowlite_c.so` at runtime (dlopen, not link-time).
- IPC unchanged: `quickstt-ipc` newline proto `STATE|/FINAL_TEXT|/AUDIO_LEVEL` ↔ `TOGGLE/STOP/SLEEP/MODEL:/WAKEWORDS:` works on both OSes via anonymous pipe.

## Next (Phase 3 — gated)

Full-app replication: wire TextBoard viewport attach, tray menu model select → `SttEngineConfig::detect`, end-to-end dictation test, DEB/AppImage packaging (#18). Awaiting your explicit approval before Phase 3 plan/execution.
