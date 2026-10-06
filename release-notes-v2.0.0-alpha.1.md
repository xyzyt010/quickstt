# QuickSTT v2.0.0-alpha.1 — Windows + Ubuntu / Debian / Linux Mint amd64

Floating offline voice typing. Pill 360×50 r25, <5 MB idle, tray + TextBoard + wakeword.

## Highlights

- **Linux first-class**: Ubuntu 22.04/24.04, Debian 12, **Linux Mint 21/22** amd64 (glibc 2.35) via `cargo deb` + `AppImage`
- **Windows**: `QuickSTT_Portable.exe` bootstrap + `QuickSTT_App.exe` Qt + `stt_service.exe` native
- **3 local STT backends**: Vosk small EN 0.15 (50M), Parakeet TDT 0.6B v3 INT8 (640M), Nemotron 3.5 Streaming Q8_0 (716M GGUF, Handy-compatible)
- **Rust rewrite**: `egui` pill, `cpal` ALSA/Pulse dual-stream, `whisper-rs`/`livekit-wakeword` lazy, XDG `~/.config/QuickSTT/config.toml`
- **Packaging**: `quickstt_2.0.0-alpha.1_amd64.deb` (`/usr/bin/quickstt`), `QuickSTT-2.0.0-alpha.1-x86_64.AppImage`, `SHA256SUMS`

## Linux Mint — one-liner (amd64, exact)

```bash
sudo apt update && sudo apt install -y wget ca-certificates
wget -O /tmp/quickstt.deb https://github.com/quickstt/quickstt/releases/download/v2.0.0-alpha.1/quickstt_2.0.0-alpha.1_amd64.deb
sudo apt install -y /tmp/quickstt.deb && quickstt &
```

AppImage (no sudo):

```bash
wget -O /tmp/QuickSTT.AppImage https://github.com/quickstt/quickstt/releases/download/v2.0.0-alpha.1/QuickSTT-2.0.0-alpha.1-x86_64.AppImage
chmod +x /tmp/QuickSTT.AppImage && /tmp/QuickSTT.AppImage &
```

Installer script (auto-picks deb):

```bash
curl -fsSL https://raw.githubusercontent.com/quickstt/quickstt/main/scripts/install.sh | bash
```

See README → Quick Start — Linux Mint for `wtype` Wayland note, tray, mic.

## Windows

- Download `QuickSTT_Portable.exe` below → double-click (bootstraps `QuickSTT_App.exe` + `stt_service.exe`)
- Or `QuickSTT_DirectDownload/QuickSTT_Full/` for USB/manual

## Models

Place or download via Dashboard → Models:

- `~/.local/share/QuickSTT/models/vosk/small_en_us_0.15/` / `%APPDATA%\QuickSTT\models\…`
- `.../nemo/tdt_0_6b_v3_int8/` 
- `.../nemotron/streaming_0.6b_q8_0/*.gguf` (via `python tools/nemotron/fetch_and_convert.py`)

## Checksums

```
cat SHA256SUMS; sha256sum -c SHA256SUMS
```

## Building

```bash
# Linux Mint
./quickstt-rust/scripts/build-linux.sh                # native
./quickstt-rust/scripts/build-linux.sh --cross-amd64  # from ARM host via cross
./quickstt-rust/scripts/docker-build-amd64.sh          # Docker Ubuntu 22.04 --platform linux/amd64
# Windows
.\BuildApp.bat
cargo build -p quickstt-gui --release
```

Full: `docs/BUILDING.md`, `docs/PACKAGING.md`, `docs/MODELS.md`.

## Changes

See `CHANGELOG.md` and `docs/ARCHITECTURE.md`.

