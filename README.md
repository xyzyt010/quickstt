# QuickSTT — Offline Voice Typing for Windows & Linux

<p align="center">
  <img src="quickstt-rust/assets/icon_app.png" width="96" alt="QuickSTT logo"/>
</p>

<p align="center">
  <a href="https://github.com/quickstt/quickstt/actions/workflows/ci.yml"><img src="https://github.com/quickstt/quickstt/actions/workflows/ci.yml/badge.svg" alt="CI"/></a>
  <a href="https://github.com/quickstt/quickstt/releases"><img src="https://img.shields.io/github/v/release/quickstt/quickstt?label=release&color=blue" alt="Release"/></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green" alt="MIT"/></a>
  <a href="#platform-support"><img src="https://img.shields.io/badge/platform-Windows%20%7C%20Ubuntu%2022.04%2B%20%7C%20Debian%2012%20%7C%20Linux%20Mint%2021%2B-lightgrey" alt="Platform"/></a>
  <img src="https://img.shields.io/badge/memory-%3C5MB%20idle-9cf" alt="Memory"/>
  <img src="https://img.shields.io/badge/offline-100%25%20local-success" alt="Offline"/>
</p>

> **Floating, offline voice typing.** A 360×50 pill that lives on your desktop. Press to talk, wakeword to listen, type anywhere. No cloud required.

QuickSTT is a **native** Linux + Windows app with two complementary stacks: a legacy **C++/Qt** desktop (still maintained for Windows power users) and a new **Rust** workspace (`egui`/`cpal`/`whisper-rs`/`ort`) that is the primary build on **Ubuntu 22.04/24.04, Debian 12, and Linux Mint 21/22** (amd64). Both stacks share the same pipe protocol, XDG/Registry settings, and three local STT backends — no network, no API keys.

---

## Table of Contents

- [Features](#features)
- [Quick Start — Linux Mint](#quick-start--linux-mint) ← **exact copy-paste command**
- [Quick Start — Ubuntu / Debian](#quick-start--ubuntu--debian)
- [Quick Start — Windows](#quick-start--windows)
- [Screenshots](#screenshots)
- [Architecture](#architecture)
- [Models](#models)
- [Building from Source](#building-from-source)
- [Packaging](#packaging)
- [Configuration](#configuration)
- [Troubleshooting](#troubleshooting)
- [Contributing](#contributing)
- [Security](#security)
- [License](#license)

---

## Features

| | Windows | Linux |
|---|---|---|
| **Widget** | `FramelessWindowHint` pill 360×50 r25, `WA_TranslucentBackground`, drag/resize, waveform 24–140 | `winit` `ViewportBuilder` transparent + always-on-top + no-decorations; X11 `_NET_WM_WINDOW_TYPE_DOCK` / Wayland layer-shell |
| **Tray** | `QSystemTrayIcon` | `tray-icon 0.17` + `libayatana-appindicator3` (fallback `libappindicator3`) |
| **Hotkeys** | `RegisterHotKey` | `global-hotkey 0.6` `XGrabKey` (X11) + portal on Wayland |
| **TextBoard** | `TextBoardWindow` 420×180 attach to pill bottom | `ViewportId quickstt_textboard` egui, same geometry, mono, opacity 90 |
| **On-Command** | Hold `Ctrl+Space` circular pill | Same — `ViewportId quickstt_on_command` |
| **STT** | `libvosk.dll` / `onnxruntime.dll` / `whisper-cli.exe` / Parakeet | `libvosk.so` / `libonnxruntime.so` / `whisper-cli` / `parakeet_engine` / `nemotron_engine` via `dlopen` |
| **VAD / Wakeword** | `ten_vad.dll` / `rnnoise.dll` / `tensorflowlite_c.dll` OWW, Porcupine | `libten_vad.so` / `librnnoise.so` / `libtensorflowlite_c.so` + `livekit-wakeword` ONNX |
| **Typing** | `SendInput` `KEYEVENTF_UNICODE` | `wtype` (Wayland) → `ydotool` → `xdotool` (X11) |
| **Settings** | `HKCU\Software\QuickSTT\Config` | `~/.config/QuickSTT/config.toml` |
| **Memory** | — | <5 MB idle, ~30 MB listening (no model), models on-demand |

Three **local** models out-of-the-box (see [Models](#models)):

1. **Vosk small EN 0.15** — 50 MB, fastest CPU, best for dictation
2. **NVIDIA Parakeet TDT 0.6B v3 INT8 (ONNX/Rust)** — 640 MB, best accuracy
3. **Nemotron 3.5 ASR Streaming 0.6B Q8_0 (GGUF)** — 716 MB, streaming + VAD, Handy-compatible

All run fully offline via `EngineFamily::Vosk|ParakeetRust|Nemotron` (`quickstt-core/src/models/catalog.rs:15`).

---

## Quick Start — Linux Mint

> Linux Mint 21.x (based on Ubuntu 22.04) and 22.x (based on Ubuntu 24.04) — **amd64 only**. Mint's Software Manager is `apt` underneath; the same `.deb` works.

### One-liner (recommended): install latest `.deb` from GitHub Releases

Copy-paste this **single block** into your Mint Terminal (`Ctrl+Alt+T`):

```bash
# 1. Install system deps that Mint doesn't ship by default
sudo apt update
sudo apt install -y wget ca-certificates

# 2. Download the latest amd64 .deb (Ubuntu 22.04 glibc 2.35 build — runs on Mint 21/22)
wget -O /tmp/quickstt.deb \
  https://github.com/quickstt/quickstt/releases/latest/download/quickstt_2.0.0-alpha.1_amd64.deb

# 3. Install + auto-fix deps (gtk, appindicator, pulse, etc.)
sudo apt install -y /tmp/quickstt.deb
# If apt complains about missing deps:  sudo apt --fix-broken install -y

# 4. Run
quickstt &
# or: /usr/bin/quickstt &
```

You should see the dark pill (360×50) at the top-center of your screen. Right-click tray icon → **Models** to download a model. Press the mic or say **“hey jarvis”** (default wakeword).

### Alternative: AppImage (no `sudo`, no install)

```bash
wget -O /tmp/QuickSTT.AppImage \
  https://github.com/quickstt/quickstt/releases/latest/download/QuickSTT-2.0.0-alpha.1-x86_64.AppImage
chmod +x /tmp/QuickSTT.AppImage
/tmp/QuickSTT.AppImage &
# Optional: integrate into Mint menu
/tmp/QuickSTT.AppImage --appimage-extract  # inspect if needed
```

### Alternative: `install.sh` (auto-detects distro, picks deb vs AppImage)

```bash
curl -fsSL https://raw.githubusercontent.com/quickstt/quickstt/main/scripts/install.sh | bash
# Explicit:  curl -fsSL https://raw.githubusercontent.com/quickstt/quickstt/main/scripts/install.sh | bash -s -- --deb
# AppImage:  curl -fsSL https://raw.githubusercontent.com/quickstt/quickstt/main/scripts/install.sh | bash -s -- --appimage
```

### Mint Cinnamon quirks

- **Wayland?** Mint defaults to X11 (Cinnamon). `tray-icon` + `global-hotkey` are native. If you switched to experimental Wayland, install `wtype`: `sudo apt install wtype`, and use XWayland for hotkeys.
- **No tray icon?** Mint Cinnamon panel may hide indicators: `System Settings → Applets` ensure **System Tray** or **AppIndicator** is enabled. Or run `quickstt` and check `ps aux | grep quickstt`.
- **No mic?** `Menu → Sound → Input` select your mic, or `pactl list sources`.

---

## Quick Start — Ubuntu / Debian

Same artifact as Mint (Mint is Ubuntu-based). On **Ubuntu 22.04 / 24.04** or **Debian 12** (amd64):

```bash
# Ubuntu 22.04/24.04 amd64
sudo apt update && sudo apt install -y wget ca-certificates
wget -O /tmp/quickstt.deb https://github.com/quickstt/quickstt/releases/latest/download/quickstt_2.0.0-alpha.1_amd64.deb
sudo apt install -y /tmp/quickstt.deb
quickstt

# Debian 12 amd64 — identical, but if libayatana-appindicator3-1 is missing:
sudo apt update && sudo apt install -y libappindicator3-1
sudo dpkg -i /tmp/quickstt.deb; sudo apt --fix-broken install -y
```

Verify:

```bash
dpkg -l | grep quickstt
dpkg -L quickstt | head                 # /usr/bin/quickstt  /usr/share/applications/quickstt.desktop
file /usr/bin/quickstt                 # ELF 64-bit LSB executable, x86-64
ldd /usr/bin/quickstt | grep -E "gtk|appindicator|asound"  # no “not found”
ls ~/.config/QuickSTT/config.toml      # XDG config
ls ~/.local/share/QuickSTT/models/     # models root
```

---

## Quick Start — Windows

### Portable (no installer, no admin)

1. Download **QuickSTT_Portable.exe** from the latest Release:  
   `https://github.com/quickstt/quickstt/releases/latest/download/QuickSTT_Portable.exe`
2. Double-click it. It bootstraps `QuickSTT_App.exe` + `stt_service.exe` + models on first run.

### Direct download artifacts

- `QuickSTT_Portable.exe` — bootstrap installer/updater (recommended)
- `QuickSTT_DirectDownload/QuickSTT_Full/` — full folder for manual copy / USB
- `QuickSTT_DirectDownload/SHA256SUMS.txt` — checksums
- `QuickSTT_Server/QuickSTT_LAN_Package.tar` — LAN distribution

Windows is still the primary target for the **C++/Qt** stack (`BuildApp.bat`).

```powershell
# From source on Windows (MSVC or MinGW + Qt 6.6+ + Rust)
.\BuildApp.bat        # produces QuickSTT_App/ + QuickSTT_Portable.exe
# Or pure Rust (no Qt)
cargo build -p quickstt-gui --release
.\target\release\QuickSTT.exe
```

---

## Screenshots

| Pill (idle) | Pill (listening) | Dashboard | TextBoard |
|---|---|---|---|
| ![Pill idle](REFRENCE_SS/pill-idle.png) | ![Pill listening](REFRENCE_SS/pill-listening.png) | ![Dashboard](REFRENCE_SS/dashboard.png) | ![TextBoard](REFRENCE_SS/textboard.png) |

> Placeholder paths `REFRENCE_SS/` — replace with your captures. The pill is 360×50 r25 `#1A1A1A` with waveform bars 24–140, baseline 220×28.

---

## Architecture

### Rust workspace (`quickstt-rust/` — primary on Linux)

```
quickstt-rust/
  quickstt-gui   # eframe/egui pill + tray-icon + global-hotkey + TextBoard viewports
  quickstt-core  # orchestration, cpal audio, whisper-rs/livekit-wakeword/onnx, models/catalog
  quickstt-ipc   # newline pipe protocol  STATE| FINAL_TEXT| AUDIO_LEVEL|  ↔  TOGGLE/STOP/MODEL:
  quickstt-slint # Slint alternative shell (experimental)
  assets/        # quickstt.desktop, icon_app.png (256), app_icon.svg
  debian/        # dpkg-buildpackage fallback
  scripts/       # build-linux.sh, docker-build-amd64.sh, install.sh
  Cross.toml     # cross-rs sysroot for amd64 from ARM host
```

- **Audio:** `cpal` `default_host()` → ALSA/Pulse/JACK on Linux, WASAPI on Windows. Dual-stream 16 kHz 1ch: 20 ms (320) wakeword + 16 kHz transcription with `InputNormalizer` (320 frames, `TARGET_CHUNK 1280`).
- **VAD:** `audio/vad.rs` energy + `ten_vad` (`libten_vad.so` / `ten_vad.dll`) hold 12 frames.
- **Wakeword:** `livekit-wakeword 0.1` ONNX (`ort` `libonnxruntime.so`) `HIT_REQUIREMENT 2` `COOLDOWN 1.0s`; native OWW via `tflite_loader` (`libtensorflowlite_c.so`).
- **STT:** `models/catalog.rs` → `models/engine.rs` `SttEngineConfig::detect()` searches both `models_root` (`~/.local/share/QuickSTT/models` or `APPDATA\QuickSTT\models` on Win) and `exe_dir/tools/...` with correct extension (`ext = if windows {".exe"} else {""}`). Families `WhisperCpp|SherpaOnnx|ParakeetRust|Vosk|Nemotron`.
- **IPC:** `tokio::process::Command` piped `stdin/stdout` `TYPE|payload\n` — POSIX `pipe(2)` on Linux, no `NamedPipe`.
- **Settings:** `settings.rs:145` Registry on Windows, TOML `~/.config/QuickSTT/config.toml` on Linux via `dirs`/`toml`.

### C++ legacy (`Source/` — Windows Qt)

- `pill_widget.h:217` `FramelessWindowHint|StaysOnTop|Tool` `WA_TranslucentBackground`, `QSystemTrayIcon`, `PillComboBox` 180ms, `StatusTextLabel`
- `native/stt_service_native.cpp` 3600-line service: `libvosk`/`onnxruntime`/`tflite` dynamic (`platform.h` `dlopen` on Linux, `LoadLibraryA` on Win), `PortAudio` capture, `SoftwareAGC` -20 dBFS, `ParakeetPipe` (`HANDLE` on Win, `pid_t`+`pipe/poll` on Linux via `#ifdef _WIN32`)
- `CMakeLists.txt` now `if(WIN32)`-guards Qt6/C++ app; on Linux only `stt_service_native` + `rnnoise_static` + `pkg_check_modules(portaudio-2.0)`.

See `docs/ARCHITECTURE.md` for the full constraints.

---

## Models

| Model | Engine | Path (Linux Mint) | Path (Windows) | Size |
|---|---|---|---|---|
| **Vosk small EN 0.15** | `Vosk` `libvosk.so` | `~/.local/share/QuickSTT/models/vosk/small_en_us_0.15/` | `%APPDATA%\QuickSTT\models\vosk\small_en_us_0.15\` | 50 MB |
| **Parakeet TDT 0.6B v3 INT8** | `ParakeetRust` `transcribe-rs onnx` | `.../nemo/tdt_0_6b_v3_int8/` | same | 640 MB |
| **Nemotron 3.5 Streaming 0.6B Q8_0** | `Nemotron` `nemotron_engine` (GGML) | `.../nemotron/streaming_0.6b_q8_0/*.gguf` | same | 716 MB |

The pill dropdown shows `●` installed / `○` not installed + size (`catalog::display_name`). Download via Dashboard `Models` or manually place as above. Nemotron GGUF is fetched via `tools/nemotron/fetch_and_convert.py` (Handy-compatible).

More at `docs/MODELS.md`.

---

## Building from Source

### Linux (Mint / Ubuntu 22.04+)

```bash
sudo apt update
sudo apt install -y build-essential pkg-config cmake protobuf-compiler \
  libgtk-3-dev libayatana-appindicator3-dev librsvg2-dev \
  libasound2-dev libpulse-dev portaudio19-dev \
  libx11-dev libxi-dev libxtst-dev libxdo-dev libssl-dev libglib2.0-dev \
  curl git

# Rust
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
source $HOME/.cargo/env
cargo install cargo-deb --locked   # for .deb

# Build (native arch)
git clone https://github.com/quickstt/quickstt.git && cd quickstt
./quickstt-rust/scripts/build-linux.sh
# Outputs: quickstt-rust/target/release/QuickSTT  build/stt_service  quickstt-rust/dist/*.deb  quickstt-rust/dist/*.AppImage
./quickstt-rust/target/release/QuickSTT  # run pill

# Cross amd64 from ARM64 host (your VM 129.151.239.19)
./quickstt-rust/scripts/build-linux.sh --cross-amd64
# Needs Docker: Cross.toml installs libgtk-3-dev:amd64 sysroot inside cross image
# Or reproducible Docker:
./quickstt-rust/scripts/docker-build-amd64.sh
```

### Windows

```powershell
# Prerequisites: Qt 6.6+, CMake 3.19+, Rust stable, MSVC or MinGW
git clone https://github.com/quickstt/quickstt.git; cd quickstt
.\BuildApp.bat                      # Qt C++ stack → QuickSTT_App\QuickSTT_App.exe
cargo build -p quickstt-gui --release  # Rust stack → target\release\QuickSTT.exe
```

See `docs/BUILDING.md`.

---

## Packaging

| Platform | Artifact | How | Install |
|---|---|---|---|
| **Linux Mint / Ubuntu / Debian** | `quickstt_2.0.0-alpha.1_amd64.deb` | `cargo deb -p quickstt-gui` or `quickstt-rust/scripts/build-linux.sh` | `sudo apt install ./quickstt_*.deb` |
| **Portable Linux** | `QuickSTT-2.0.0-alpha.1-x86_64.AppImage` | `linuxdeploy --appdir dist/AppDir -e QuickSTT -d assets/quickstt.desktop -i assets/icon_app.png --plugin gtk --output appimage` | `chmod +x *.AppImage && ./QuickSTT*.AppImage` |
| **Fallback** | `quickstt-20260826-x86_64.tar.gz` | `build-linux.sh` when FUSE unavailable | `tar xzf && ./QuickSTT` |
| **Windows** | `QuickSTT_Portable.exe` + `QuickSTT_Basic/Full` + `SHA256SUMS.txt` | `BuildApp.bat` | `QuickSTT_Portable.exe` bootstrap |

Checksums: `cat quickstt-rust/dist/SHA256SUMS; sha256sum -c dist/SHA256SUMS`.

See `docs/PACKAGING.md`.

---

## Configuration

| | Linux Mint / Ubuntu | Windows |
|---|---|---|
| **Config** | `~/.config/QuickSTT/config.toml` | `HKCU\Software\QuickSTT\Config` (Registry, mirrored to `data/config.toml` if present) |
| **Data** | `~/.local/share/QuickSTT/` | `%APPDATA%\QuickSTT\` or exe-side `data/` |
| **Models** | `~/.local/share/QuickSTT/models/` | `%APPDATA%\QuickSTT\models\` |
| **Recordings** | `~/.local/share/QuickSTT/Recordings/` or custom | same |
| **Autostart** | `~/.config/autostart/quickstt.desktop` (`autostart.rs`) | `HKCU\...\Run` |
| **Logs** | `~/.local/share/QuickSTT/logs/` + `RUST_LOG=info` stderr | `QuickSTT_App\logs\` |

Edit headless: `nano ~/.config/QuickSTT/config.toml` → `selectedModel`, `wakeWords`, `widgetFlexible`, `autoOffload`.

---

## Troubleshooting

<details>
<summary><b>Deb won't install: dependency “libappindicator”</b></summary>

Mint 22 (Ubuntu 24.04) moved to `libayatana-appindicator3-1`. Our deb declares `libayatana-appindicator3-1 | libappindicator3-1` so `apt` resolves. On plain Debian 12, `sudo apt install libappindicator3-1` first then `sudo dpkg -i quickstt.deb; sudo apt --fix-broken install -y`.
</details>

<details>
<summary><b>Pill doesn't appear / crashes on Wayland</b></summary>

Mint Cinnamon is X11 — pill uses `x11` backend. If you switched to Wayland, install `wtype` (`sudo apt install wtype`) for typing. `RUST_LOG=info quickstt 2>&1 | tail -n 50` shows `Display server: x11/wayland`. On headless, run under `xvfb-run`.
</details>

<details>
<summary><b>No tray icon on Mint</b></summary>

Ensure Cinnamon `System Settings → Applets → System Tray` is enabled. Also try `ps aux | grep quickstt`; tray may be in `libappindicator` fallback.
</details>

<details>
<summary><b>No audio / mic</b></summary>

`arecord -l` (ALSA) and `pactl list sources short` (Pulse). Mint `Sound → Input`. Test: `arecord -f S16_LE -r 16000 -c 1 /tmp/test.wav` then `aplay`.
</details>

<details>
<summary><b>Models show “[Not Installed]”</b></summary>

Place correctly: `ls ~/.local/share/QuickSTT/models/vosk/small_en_us_0.15/am/final.mdl` and `.../nemotron/streaming_0.6b_q8_0/*.gguf`. Download from Dashboard or `tools/nemotron/fetch_and_convert.py`.
</details>

<details>
<summary><b>Need amd64 on ARM machine</b></summary>

Your VM `129.151.239.19` is ARM64 but Mint targets are amd64. Use `cross`:
`./quickstt-rust/scripts/build-linux.sh --cross-amd64` (needs Docker) or `./quickstt-rust/scripts/docker-build-amd64.sh`. `file QuickSTT` should say `x86-64` not `aarch64`.
</details>

More at `docs/TROUBLESHOOTING.md`.

---

## Contributing

We welcome speech, packaging, and UI contributions.

- Read `CONTRIBUTING.md` → small focused PRs, include `risk` + `screenshots`.
- Help wanted: macOS, Linux packaging, GPU runtimes, wakeword.
- Dev: `cargo check --offline`, `cargo test -p quickstt-core --lib` (9 tests), `cargo build -p quickstt-gui --release`.

## Security

- Mic access is local only; no cloud STT by default.
- Windows secrets via DPAPI (`windows_secret_store.cpp`), Linux via TOML (extend with `libsecret` if needed).
- Report vulnerabilities privately per `SECURITY.md`.

## License

MIT — see `LICENSE`. Commercial use allowed.

## Platform Ambition

1. Windows polish & release stability
2. Ubuntu/Debian/Mint amd64 (done)
3. macOS
4. Additional local runtimes, stronger signing

