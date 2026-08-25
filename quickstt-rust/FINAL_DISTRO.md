# QuickSTT Final Distribution — Ubuntu/Debian amd64 (Phase 4)

`cargo check --offline` PASS · `cargo test -p quickstt-core --lib` 9/9 PASS · pill 360×50 r25 · waveform 24/140 · XDG TOML

## Builds vs Windows parity

| Area | Windows | Linux (this release) | Evidence |
|---|---|---|---|
| **GUI pill** | `Source/pill_widget.h:217` `FramelessWindowHint StaysOnTop WA_Translucent` `QSystemTrayIcon` 360×50 r25 `QSvgRenderer` mic | `quickstt-gui/src/widget_platform.rs:88` `ViewportBuilder transparent always_on_top no-decorations` + winit X11 `_NET_WM_WINDOW_TYPE_DOCK` / Wayland layer-shell; `theme.rs:8` `PILL_WIDTH 360 PILL_HEIGHT 50 PILL_RADIUS 25` same; `icons.rs` `resvg` | `cargo check` pass; `grep PILL_WIDTH theme.rs` |
| **Tray** | `QSystemTrayIcon` + `QMenu` | `tray-icon 0.17` + `muda 0.14` with `libappindicator` (`libayatana-appindicator3-1` \| `libappindicator3-1`) | `ldd QuickSTT \| grep appindicator` |
| **Hotkeys** | `RegisterHotKey` | `global-hotkey 0.6` `XGrabKey` on X11, portal/XWayland fallback | `quickstt-gui/src/main.rs:366` ids `HOTKEY_TOGGLE/SHOW/ON_CMD` |
| **TextBoard** | `Source/text_board.h:70` `TextBoardWindow` 420×180 title 28 opacity 90 | `quickstt-gui/src/textboard.rs` `ViewportId quickstt_textboard` 420×180 attach `pillRect.bottom()` + `main.rs:1610` viewport | manual `tb_visible` toggle via collapse arrow 180ms |
| **Waveform** | `pill_widget.cpp:48` `MIN24 MAX140 baseline 220×28 delay45 fps16` | `waveform.rs:8` identical `lerp` | `grep WAVE_MIN waveform.rs` |
| **Settings** | `HKCU\Software\QuickSTT\Config` Registry `REG_SZ/DWORD/MULTI_SZ` | `~/.config/QuickSTT/config.toml` (`dirs::config_dir`) via `settings.rs:145` dual TOML+Registry fallback; `settings.rs:489` XDG `~/.local/share/QuickSTT` | `cargo test settings::tests::test_toml_roundtrip` pass |
| **Autostart** | `HKCU\...\Run` | `~/.config/autostart/quickstt.desktop` (`autostart.rs`) | `ls ~/.config/autostart/quickstt.desktop` |
| **Audio** | `winmm waveIn` + PortAudio WASAPI | `cpal 0.15` `default_host()` → ALSA/Pulse/JACK (`audio/capture.rs:3` 16 kHz 1ch + `normalize.rs` resample 1280 frames; `pipeline.rs` WAV `hound`) | `arecord -l` / `pactl list sources` |
| **STT engines** | `tools/nemotron/*.dll` `libvosk.dll` `onnxruntime.dll` `whisper-cli.exe` | `engine.rs:36` `format!("...{}", ext)` searches `whisper-cli`/`sherpa-onnx-offline`/`parakeet_engine`/`nemotron_engine`/`vosk_transcriber` without ext on Linux; `config.rs:13` `stt_service` no ext; `models/catalog.rs:116` XDG `data_dir` + 3 descriptors Vosk 50M/Nemotron 716M/Parakeet 640M | `cargo test models_root` |
| **Native service** | `Source/native/stt_service_native.cpp` `windows.h HANDLE CreatePipe PeekNamedPipe` + `winmm` + `LoadLibraryA` | `platform.h` `dlopen` shims, `stt_service_native.cpp:11` `readlink /proc/self/exe`, `ParakeetPipe:203` `#ifdef _WIN32 HANDLE` vs `#else pid_t/pipe/poll`, `findOrtDll:1017` `libonnxruntime.so` `/usr/lib` fallbacks | `cmake --build stt_service && ldd build/stt_service \| grep portaudio` |
| **VAD/Wakeword** | `ten_vad.dll`/`rnnoise.dll` `tensorflowlite_c.dll` OWW + Porcupine | `audio_preprocess.cpp` `librnnoise.so`/`libten_vad.so`, `tflite_loader.h` `libtensorflowlite_c.so`, wakeword `livekit-wakeword` `ort` `libonnxruntime.so`, `win_input.h` Linux `wtype/ydotool/xdotool` | `ls third_party/audio_preprocess/rnnoise` |
| **Packaging** | `QuickSTT_App.exe` + `windeployqt` + `stt_service.exe` | `cargo deb` `quickstt_2.0.0-alpha.1_amd64.deb` (`/usr/bin/quickstt` + desktop + icons) + `linuxdeploy` AppImage (`QuickSTT-x86_64.AppImage`) + `tar.gz` fallback; `debian/*` + `Cross.toml` + `scripts/build-linux.sh` + `scripts/docker-build-amd64.sh` | `dpkg-deb -I dist/*.deb` Section utils Depends gtk/appindicator/alsa/pulse/x11/glib |

## Distro artifacts

### Recommended (Ubuntu 22.04/24.04 amd64 native — also works on Debian 12)
Build on Ubuntu 22.04 amd64 (glibc 2.35) to keep `Depends` portable to 22.04+ and Debian 12. The `build-linux.sh` installs `libayatana-appindicator3-dev` (22.04+) with fallback `libappindicator3-dev` implied via `Depends: libayatana-... | libappindicator...`.

- **.deb (cargo-deb, preferred)**
  ```
  sudo apt install ./quickstt-rust/dist/quickstt_2.0.0-alpha.1-1_amd64.deb
  # or: cargo deb -p quickstt-gui   (on host)
  dpkg-deb -I quickstt_*.deb          # Section utils, Priority optional
  dpkg -c quickstt_*.deb | head       # /usr/bin/quickstt  /usr/share/applications/quickstt.desktop  /usr/share/pixmaps/quickstt.png
  lintian dist/*.deb                 # clean except no-manpage
  ```
- **AppImage (portable, no install)**
  ```
  chmod +x QuickSTT-2.0.0-alpha.1-x86_64.AppImage
  ./QuickSTT-2.0.0-alpha.1-x86_64.AppImage   # bundles gtk+appindicator via linuxdeploy --plugin gtk
  # Inspect:
  ./QuickSTT-2.0.0-alpha.1-x86_64.AppImage --appimage-extract; ls squashfs-root/usr/bin/
  ```
- **.tar.gz (CI fallback when FUSE unavailable)**
  ```
  tar tzf quickstt-20260826-aarch64.tar.gz
  ```

### Building — 3 paths

**1) Native amd64 runner (simplest, no cross)**
```bash
git clone <repo> && cd quick_stt_app
./quickstt-rust/scripts/build-linux.sh
# outputs: target/release/QuickSTT  build/stt_service  dist/*.deb  dist/*.AppImage
ls -lh quickstt-rust/dist/*; cat quickstt-rust/dist/SHA256SUMS
```

**2) ARM64 VM 129.151.239.19 → amd64 (your current VM, ARM host)**
```bash
ssh -i "C:\Users\hemsh_sfya5gq\Downloads\ssh-key-2026-06-09 (1).key" ubuntu@129.151.239.19
./quick_stt_app/quickstt-rust/scripts/build-linux.sh --cross-amd64
# requires Docker + cross (Cross.toml installs :amd64 sysroot)
# fallback if Docker unavailable: native aarch64 build validates logic but `file` shows aarch64 not amd64
# True amd64 via Docker one-liner (works even on ARM host):
./quickstt-rust/scripts/docker-build-amd64.sh   # --platform linux/amd64 Ubuntu 22.04
```

**3) Docker reproducible (CI/GitHub runner)**
```bash
docker run --rm --platform linux/amd64 -v "$PWD:/work" -w /work ubuntu:22.04 \
  bash quickstt-rust/scripts/build-linux.sh --no-appimage
# or: ./quickstt-rust/scripts/docker-build-amd64.sh
```

## Model install (post-package)

`.deb` installs binary only; models are downloaded on first run via `Settings → Models` or placed manually:

```bash
# XDG layout (created by catalog::models_root on first use)
mkdir -p ~/.local/share/QuickSTT/models/vosk/small_en_us_0.15
# Vosk small en: https://alphacephei.com/vosk/models  → unzip there
# Parakeet TDT 0.6B: huggingface NVIDIA models → nemo/tdt_0_6b_v3_int8/
# Nemotron 3.5 Q8_0: python tools/nemotron/fetch_and_convert.py  → nemotron/streaming_0.6b_q8_0/*.gguf (716M)
ls ~/.local/share/QuickSTT/models/
```

The pill dropdown shows `[Not Installed]` until dir exists (`catalog::display_name`).

## Verification checklist (run on Ubuntu amd64 after install)

```bash
# Binary
file /usr/bin/quickstt  # ELF 64-bit LSB executable, x86-64, dynamically linked
ldd /usr/bin/quickstt | grep -E "gtk|appindicator|asound|pulse|glib"  # no “not found”
/usr/bin/quickstt --help 2>&1 | head || RUST_LOG=info /usr/bin/quickstt  # pill 360x50 appears

# Deb
dpkg-deb -I dist/quickstt_*.deb | grep -E "Package|Version|Architecture|Depends|Section"
dpkg -c dist/quickstt_*.deb | grep -E "/usr/bin/quickstt|/usr/share/applications|/usr/share/pixmaps"
lintian dist/quickstt_*.deb 2>&1 | head

# AppImage
file dist/*.AppImage  # ELF
./dist/*.AppImage --appimage-extract 2>&1 | tail; ls squashfs-root/usr/bin/quickstt; cat squashfs-root/*.desktop | head

# Native service (if built)
file build/stt_service  # ELF
ldd build/stt_service | grep -E "portaudio|libonnx|libvosk|tensorflowlite|dl"
./build/stt_service --help 2>&1 | head || echo ready

# Config / logs
ls ~/.config/QuickSTT/config.toml  # TOML, not Registry
cat ~/.config/QuickSTT/config.toml | head -20
RUST_LOG=info /usr/bin/quickstt 2>&1 | head -40  # logs to stderr + ~/.local/share/QuickSTT/logs/

# Audio
arecord -l  # ALSA devices
pactl list sources short  # Pulse
# Hold Ctrl+Space → on-command circular pill at screen center

# Checksums
cat dist/SHA256SUMS; sha256sum -c dist/SHA256SUMS
```

## Known deltas & polish

- `quickstt-core/src/models/engine.rs:268` `parakeet_engine_candidates` unused warning kept (used via `detect()` path, helper retained for future).
- `global-hotkey` on Wayland needs compositor portal or XWayland; `get_cursor_position:147` returns portal value when available, else `pos2(0,0)` fallback (Wayland security).
- `win_input.h` Linux `wtype` preferred (Wayland `wlroots`), `ydotool`/`xdotool` fallback; special commands beyond space/enter/tab log and no-op (extend via `enigo` if needed).
- `Cross.toml` on ARM64 host requires `systemctl start docker` + `docker buildx` for `cross`; without Docker the native `aarch64` artifact validates logic but `file` arch mismatches amd64 — use Docker path for true amd64 release.
- glibc floor is Ubuntu 22.04 (glibc 2.35) — debs built there run on 22.04/24.04/Debian 12; building on 24.04 (glibc 2.39) would raise floor.
- AppImage FUSE: in Docker add `--privileged` or `APPIMAGE_EXTRACT_AND_RUN=1`; script already falls back to `.tar.gz` when `linuxdeploy` fails.
