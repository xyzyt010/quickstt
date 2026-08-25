# Building — Windows & Linux (Mint / Ubuntu / Debian)

## Prereqs

### Common
- **Rust** stable ≥1.75 (`rustup`): `curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh`
- **Git**, **pkg-config**, **protobuf-compiler** (for `livekit-wakeword` / `tract`)
- `cargo install cargo-deb --locked` (for `.deb`, Linux only)

### Windows
- **Qt 6.6+** (via `aqtinstall` or https://www.qt.io/download) + `Qt6_DIR` set
- **CMake 3.19+**, **Ninja** (or `Visual Studio 2022` with MSVC), or MinGW 13+
- Optional: `windeployqt` (ships with Qt) for `QuickSTT_App` runtime

### Linux Mint 21/22 · Ubuntu 22.04/24.04 · Debian 12 (amd64)
```bash
sudo apt update
sudo apt install -y build-essential pkg-config cmake protobuf-compiler \
  libgtk-3-dev libayatana-appindicator3-dev librsvg2-dev \
  libasound2-dev libpulse-dev portaudio19-dev \
  libx11-dev libxi-dev libxtst-dev libxdo-dev libssl-dev libglib2.0-dev \
  lsb-release alsa-utils curl git file patchelf zsync desktop-file-utils
# Wayland typing (optional, for wtype):
sudo apt install -y wtype
```

Mint's `apt` is Ubuntu's underneath — the same commands work. Mint 21 is Ubuntu 22.04 (glibc 2.35), Mint 22 is Ubuntu 24.04 (glibc 2.39).

## Quick build (Linux — primary Rust stack)

```bash
git clone https://github.com/quickstt/quickstt.git && cd quickstt

# System deps (above), then:
./quickstt-rust/scripts/build-linux.sh
# → quickstt-rust/target/release/QuickSTT
# → build/stt_service (Linux native service via PortAudio)
# → quickstt-rust/dist/quickstt_*.deb + dist/*.AppImage + dist/SHA256SUMS

# Run
./quickstt-rust/target/release/QuickSTT
# or after deb install:
quickstt &
```

**What `build-linux.sh` does (7 steps):** check `cargo`, `cargo check --workspace`, `cargo build --release`, `cmake --build stt_service_native`, desktop `~/.local/share/applications/quickstt.desktop`, `cargo deb`, `linuxdeploy AppImage`, `sha256sum`.

**Options:**
```bash
./quickstt-rust/scripts/build-linux.sh --cross-amd64   # from ARM64 host (VM 129.151.239.19) → amd64 via `cross`
./quickstt-rust/scripts/build-linux.sh --no-deb         # skip deb
./quickstt-rust/scripts/build-linux.sh --no-appimage    # skip AppImage
./quickstt-rust/scripts/docker-build-amd64.sh          # Docker Ubuntu 22.04 --platform linux/amd64 reproducible
```

**Cross amd64 from ARM64** (your VM `129.151.239.19` is ARM64 but Mint targets are amd64):
- Needs Docker. `Cross.toml` pre-installs `libgtk-3-dev:amd64` etc inside `ghcr.io/cross-rs/x86_64-unknown-linux-gnu:main`.
- Without Docker the script still builds a native `aarch64` binary that validates logic (`file` shows `aarch64` not `x86-64`); use the Docker path for true amd64.

## Quick build (Windows)

### Rust only (no Qt, single binary ~15 MB)
```powershell
cargo build -p quickstt-gui --release
.\target\release\QuickSTT.exe
```

### Full C++/Qt stack (legacy, for `QuickSTT_App.exe` + `stt_service.exe`)
```powershell
# MSVC (Qt msvc2019_64)
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release
cmake --build build --target QuickSTT_App -j
cmake --build build --target stt_service_native -j
.\BuildApp.bat   # bundles Qt runtime + Portable
# Outputs: QuickSTT_App\QuickSTT_App.exe  QuickSTT_Portable.exe  QuickSTT_DirectDownload\SHA256SUMS.txt
```

MinGW alternative:
```powershell
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
mingw32-make -C build QuickSTT_App stt_service_native
```

## Workspace checks (both platforms)

```bash
cargo check --offline          # Windows (no gtk sysroot needed)
cargo check                    # Linux (needs gtk/appindicator dev packages)
cargo test -p quickstt-core --lib   # 9 tests incl. test_toml_roundtrip
cargo fmt --check && cargo clippy --workspace  # CI lint
```

## Troubleshooting build

- **`glib-sys` / `gdk-sys` pkg-config cross error on Windows → `cargo check --target x86_64-unknown-linux-gnu`**  
  Expected without Linux sysroot. Run `cargo check` natively on Ubuntu/Mint.

- **`tray-icon` missing `libayatana-appindicator3-dev`**  
  `sudo apt install libayatana-appindicator3-dev` (22.04+) or `libappindicator3-dev` (older). Our `control` Depends `libayatana-... | libappindicator...`.

- **`portaudio19-dev` not found**  
  `sudo apt install portaudio19-dev` (provides `portaudio-2.0.pc` for `pkg_check_modules`).

- **`windeployqt` not found**  
  Add `Qt6_DIR` e.g. `C:\Qt\6.6.2\msvc2019_64\lib\cmake\Qt6` to `PATH`.

See `docs/TROUBLESHOOTING.md`.
