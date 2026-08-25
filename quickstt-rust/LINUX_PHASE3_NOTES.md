# QuickSTT Linux Phase 3 — Full App & Packaging (Ubuntu/Debian amd64)

Date: 2026-08-26  VM: `ssh -i "C:\Users\hemsh_sfya5gq\Downloads\ssh-key-2026-06-09 (1).key" ubuntu@129.151.239.19` (Ubuntu ARM64 host, amd64 target via `cross`)

## What Phase 3 does

Completes the replication: Rust egui pill (360x50 r25) + waveform + tray `libappindicator` + TextBoard viewport (`ViewportId quickstt_textboard` attached to pill bottom) + `global-hotkey` X11 `XGrabKey` (Wayland via portal/XWayland) are wired to the Phase 2 backends (`cpal` ALSA/Pulse, `libvosk.so`/`libonnxruntime.so`/`libtensorflowlite_c.so` dynamic, Parakeet TDT 0.6B v3 INT8 via `transcribe-rs onnx`, Nemotron 3.5 GGUF via `tools/nemotron/nemotron_engine`, Vosk small en) and to wakeword (`livekit-wakeword` HIT_REQUIREMENT 2) with VAD gating. The C++ `stt_service` is now also buildable on Linux (`Source/native/platform.h` `dlopen` shims, `CMakeLists.txt` `pkg_check_modules(portaudio-2.0)` + `if(WIN32)` guards).

## Key wires (file:line)

- `quickstt-gui/src/main.rs:1610` `if self.tb_visible { ctx.show_viewport_immediate(ViewportId::from_hash_of("quickstt_textboard"), ... render_textboard(...)) }` — attached TextBoard mirrors `Source/text_board.h:70` (28px title, opacity, mono); `main.rs:1228` collapse toggles `tb_visible` (arrow 180ms) same as `pill_widget.cpp:551`.
- `main.rs:745-789` `on_command_transcription` circular pill (`ViewportId quickstt_on_command`) with hold `Ctrl+Space` (global-hotkey `HOTKEY_ON_CMD`, `main.rs:366`); Linux `get_cursor_position:147` stub returns Wayland portal value when available.
- `main.rs:366` tray `TrayIcon`/`MenuItem new` ids `menu_dash/show/hide/quit` mirror `pill_widget.cpp:setupTray()`; model dropdown `model_names` comes from `catalog::installed_models()` → includes new `Vosk Small EN` / `Nemotron 3.5 Q8_0` / `Parakeet TDT 0.6B` (catalog.rs:141) with `[Not Installed]` marker `catalog.rs:140`.
- Orchestration `quickstt-core/src/orchestration.rs:278` `transcribe()` spawns `SttEngineConfig::detect()` → `models/engine.rs:64` dispatch `Vosk/Whisper/Parakeet/Nemotron`; engine candidates now `format!("...{}", ext)` with `ext = .exe?` (`engine.rs:36`), `STT_SERVICE_EXE` cfg in `config.rs:13`/`stt_service.rs:32` (`piped stdin/stdout` already POSIX `pipe(2)` — no NamedPipe).
- Settings `quickstt-core/src/settings.rs:145` `load()` Registry→TOML `~/.config/QuickSTT/config.toml`; `autostart.rs` XDG `~/.config/autostart/quickstt.desktop`.
- Native `Source/native/platform.h` + `tflite_loader.h:3`/`ort_loader.h:9`/`vosk_api.h:3` `platform_load/dlopen`, `audio_preprocess.cpp:33` `rnnoise/librnnoise.so`; `stt_service_native.cpp:11` `windows.h` shim + `readlink /proc/self/exe`, `ParakeetPipe:203` `#ifdef _WIN32 HANDLE` vs `#else pid_t/pipe/poll`, `findOrtDll:1017` `libonnxruntime.so` `/usr/lib` fallbacks.
- Packaging `quickstt-gui/Cargo.toml:49` `[package.metadata.deb]` defines `quickstt_2.0.0-alpha.1_amd64.deb` assets `target/release/QuickSTT→/usr/bin/quickstt`, `assets/quickstt.desktop→/usr/share/applications`, `assets/icon_app.png→/usr/share/pixmaps` (+hicolor) `depends` gtk/appindicator/alsa/pulse/x11. `Cross.toml` pre-build installs `:amd64` sysroot for `cross build --target x86_64-unknown-linux-gnu` from ARM64 host. `debian/*` fallback for `dpkg-buildpackage`. `assets/icon_app.png` 256x256 generated via Pillow on Windows host (fallback in `build-linux.sh` if missing).

## Build matrix

| Host | Command | Artifact | Notes |
|---|---|---|---|
| Ubuntu 22.04 amd64 native | `./quickstt-rust/scripts/build-linux.sh` | `target/release/QuickSTT` + `build/stt_service` + `dist/quickstt_*.deb` + `dist/*.AppImage` | `apt install` gtk/appindicator/portaudio, `cargo build --release` (~3 min), `cmake --build stt_service`, `cargo deb`, `linuxdeploy --appdir dist/AppDir -e bin -d desktop -i png --plugin gtk --output appimage` |
| Ubuntu ARM64 (VM 129.151.239.19) → amd64 | `./quickstt-rust/scripts/build-linux.sh --cross-amd64` | `target/x86_64-unknown-linux-gnu/release/QuickSTT` + `dist/quickstt_*.deb` (amd64) | Uses `cross` Docker `ghcr.io/cross-rs/x86_64-unknown-linux-gnu:main` + `Cross.toml` installs `:amd64` libs; `docker` must be installed+running, else warns and keeps native aarch64 binary (logic valid, arch wrong) |
| Docker amd64 single-shot | `docker run --platform linux/amd64 -v $PWD:/work -w /work ubuntu:22.04 bash quickstt-rust/scripts/build-linux.sh` | same as native | For CI where host lacks `cross` |

System deps for all (from `build-linux.sh:18`): `build-essential pkg-config cmake protobuf-compiler libgtk-3-dev libayatana-appindicator3-dev librsvg2-dev libasound2-dev libpulse-dev portaudio19-dev libx11-dev libxi-dev libxtst-dev libxdo-dev libssl-dev libglib2.0-dev`.

## Outputs — expected

```
quickstt-rust/target/release/QuickSTT              # ~15-25 MB stripped, file ELF 64-bit LSB executable, x86-64 / aarch64
quickstt-rust/target/x86_64-unknown-linux-gnu/release/QuickSTT  # when --cross-amd64
build/stt_service                                   # ELF, needs libportaudio.so at runtime (dlopen for vosk/ort)
quickstt-rust/dist/quickstt_2.0.0-alpha.1-1_amd64.deb   # cargo-deb native or cross
quickstt-rust/dist/QuickSTT-2.0.0-alpha.1-x86_64.AppImage # linuxdeploy (chmod +x)
~/.local/share/applications/quickstt.desktop        # user-local fallback from build-linux.sh
/usr/bin/quickstt                                   # after dpkg -i
```

Checks (in script):
- `ldd QuickSTT | head -30` shows `libgtk-3.so.0 =>`, `libappindicator`, `libasound`, `libpulse` etc, no missing `not found`.
- `dpkg-deb -I dist/*.deb` Section utils, Depends gtk/appindicator/alsa/pulse/x11, `dpkg -c dist/*.deb` contains `/usr/bin/quickstt` `/usr/share/applications/quickstt.desktop` `/usr/share/pixmaps/quickstt.png`.
- `lintian dist/*.deb` clean (only possible `no-manpage` warning).
- AppImage: `linuxdeploy --appdir dist/AppDir -e QuickSTT -d assets/quickstt.desktop -i assets/icon_app.png --plugin gtk --output appimage` produces `QuickSTT-x86_64.AppImage`; fallback `tar.gz` if FUSE unavailable in Docker.

## Models root on Linux

`catalog::models_root()` → `~/.local/share/QuickSTT/models` (XDG `dirs::data_dir`) else `exe/data/models` (portable). Expected layout after `tools/nemotron/fetch_and_convert.py` and model installs:
```
~/.local/share/QuickSTT/models/
  vosk/small_en_us_0.15/{am/final.mdl,graph/Gr.fst}
  nemotron/streaming_0.6b_q8_0/{*.gguf}
  nemo/tdt_0_6b_v3_int8/{encoder.int8.onnx,decoder...}
```
 Pill dropdown shows `[Not Installed]` per `catalog::display_name` until dir exists; wakeword models likewise via `wakeword_loader::discover_models` `wakeword_models/*/model.onnx + config.json`.

## Verification on your VM

```bash
ssh -i "C:\Users\hemsh_sfya5gq\Downloads\ssh-key-2026-06-09 (1).key" ubuntu@129.151.239.19
# Native aarch64 validation (logic identical, arch differs)
./quick_stt_app/quickstt-rust/scripts/build-linux.sh
./quickstt-rust/target/release/QuickSTT           # pill appears (X11 via XWayland on headless needs xvfb)
# For true amd64 .deb/AppImage (recommended on amd64 runner; on ARM64 needs Docker)
./quickstt-rust/scripts/build-linux.sh --cross-amd64 --deb --appimage
ls -lh quickstt-rust/dist/*.deb quickstt-rust/dist/*.AppImage
dpkg-deb -I quickstt-rust/dist/*.deb
# Or Docker amd64 path:
docker run --rm --platform linux/amd64 -v "$PWD:/work" -w /work ubuntu:22.04 \
  bash -c "apt update && apt install -y curl git && bash quickstt-rust/scripts/build-linux.sh"
```

If your VM stays ARM64, the pending amd64 `build/stt_service` cross requires a separate x86_64 toolchain (skip is non-fatal — Rust `stt_service` via `libvosk.so` covers decoding). For final release, build on Ubuntu 22.04 amd64 GitHub runner or `cross` with Docker (`systemctl start docker`).

## Edge notes

- `quickstt-gui/Cargo.toml` `windows` now `target.'cfg(windows)'.dependencies` only — `cargo check --offline` still PASS on Windows, `cargo check` on Linux needs native gtk sysroot (glib-sys failure cross from Windows is expected, not a bug).
- `winresource` in `build-dependencies` is harmless on Linux (skipped in `build.rs:3` `cfg(windows)` guard).
- `Cross.toml` `:amd64` multiarch on Ubuntu 22.04 requires `dpkg --add-architecture amd64` which the pre-build hook does.
- AppImage FUSE: in Docker add `--privileged` or `export APPIMAGE_EXTRACT_AND_RUN=1`; script falls back to `.tar.gz` if `linuxdeploy` fails.
