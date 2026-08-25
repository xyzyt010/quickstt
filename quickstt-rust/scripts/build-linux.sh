#!/usr/bin/env bash
# build-linux.sh — QuickSTT Linux full app (Phase 3) for Ubuntu/Debian amd64
# Covers: Rust GUI (egui pill + TextBoard) + Rust backend (Vosk/Parakeet/Nemotron/wakeword)
#         + C++ stt_service_native (PortAudio + libvosk/so) + .deb & AppImage
# Usage on VM (129.151.239.19):  ./quickstt-rust/scripts/build-linux.sh [--cross-amd64] [--deb] [--appimage] [--clean]
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WS_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$WS_ROOT/.." && pwd)"
ARCH="$(uname -m)"
DISTRO="$(lsb_release -ds 2>/dev/null || cat /etc/os-release 2>/dev/null | head -1)"
TARGET_X86="x86_64-unknown-linux-gnu"
echo "[BUILD] WS: $WS_ROOT  Arch: $ARCH  Distro: $DISTRO"
echo "[BUILD] Repo: $REPO_ROOT"

DO_CROSS=0; DO_DEB=1; DO_APPIMAGE=1; DO_CLEAN=0
for a in "$@"; do case "$a" in --cross-amd64) DO_CROSS=1;; --deb) DO_DEB=1;; --no-deb) DO_DEB=0;; --appimage) DO_APPIMAGE=1;; --no-appimage) DO_APPIMAGE=0;; --clean) DO_CLEAN=1;; esac; done
if [[ "$ARCH" != "x86_64" && "$DO_CROSS" != "1" ]]; then
  echo "[INFO] Host $ARCH != x86_64 but building native $ARCH binary (validates logic)."
  echo "       To emit amd64 artifacts from ARM64 host, run with --cross-amd64 (requires Docker/cross)."
fi
if [[ "$ARCH" != "x86_64" && "$DO_CROSS" == "1" ]]; then
  if ! command -v cross &>/dev/null && ! command -v docker &>/dev/null; then
    echo "[WARN] cross/Docker missing — installing cross (needs Docker)."
  fi
fi
if [[ "$DO_CLEAN" == "1" ]]; then echo "[CLEAN] cargo clean + rm -rf build dist"; rm -rf "$WS_ROOT/target" "$REPO_ROOT/build" "$WS_ROOT/dist" 2>/dev/null || true; fi

echo "[1/7] System deps…"
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
  build-essential pkg-config cmake protobuf-compiler \
  libgtk-3-dev libayatana-appindicator3-dev librsvg2-dev \
  libasound2-dev libpulse-dev portaudio19-dev \
  libx11-dev libxi-dev libxtst-dev libxdo-dev libssl-dev libglib2.0-dev \
  lsb-release alsa-utils curl git file patchelf zsync desktop-file-utils

if ! command -v cargo &>/dev/null; then echo "[INSTALL] rustup"; curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y; source "$HOME/.cargo/env"; fi
rustup target add "$TARGET_X86" >/dev/null 2>&1 || true
cargo --version; rustc --version
if ! command -v cargo-deb &>/dev/null; then echo "[INSTALL] cargo-deb"; cargo install cargo-deb --locked || true; fi
if [[ "$DO_CROSS" == "1" ]] && ! command -v cross &>/dev/null; then cargo install cross --git https://github.com/cross-rs/cross --locked || true; fi

echo "[2/7] cargo check (workspace)…"
cd "$WS_ROOT"
cargo check --workspace
echo "[OK] cargo check"

echo "[3/7] cargo build --release (native $ARCH)…"
# Use offline if Cargo.lock present else online
cargo build --release
BIN_NATIVE="$WS_ROOT/target/release/QuickSTT"
[[ -f "$BIN_NATIVE" ]] || BIN_NATIVE=$(find "$WS_ROOT/target/release" -maxdepth 1 -type f -executable -name "*QuickSTT*" | head -1 || echo "")
echo "[BIN_NATIVE] $BIN_NATIVE  size=$(du -h "$BIN_NATIVE" 2>/dev/null | cut -f1 || echo ?)"
ldd "$BIN_NATIVE" 2>&1 | head -30 || true
file "$BIN_NATIVE" || true

if [[ "$DO_CROSS" == "1" ]]; then
  echo "[3b/7] cross build --release --target $TARGET_X86 (amd64 artifact)…"
  # Cross needs gtk sysroot via Docker; cross will pull image
  if command -v cross &>/dev/null; then
    cross build --release --target "$TARGET_X86" || echo "[WARN] cross build failed (see above) — native $ARCH binary still valid"
    BIN_X86="$WS_ROOT/target/$TARGET_X86/release/QuickSTT"
    if [[ -f "$BIN_X86" ]]; then echo "[BIN_X86] $BIN_X86  size=$(du -h "$BIN_X86"|cut -f1)"; file "$BIN_X86"; fi
  else
    echo "[SKIP] cross not available"
  fi
fi

# Determine primary binary for packaging (prefer amd64 if cross built, else native)
BIN="$BIN_NATIVE"
if [[ "$DO_CROSS" == "1" && -f "$WS_ROOT/target/$TARGET_X86/release/QuickSTT" ]]; then BIN="$WS_ROOT/target/$TARGET_X86/release/QuickSTT"; fi
[[ -f "$BIN" ]] || { echo "[ERROR] Binary not found"; exit 1; }

echo "[4/7] C++ stt_service_native (PortAudio + dl)…"
# Build native stt_service (Linux) — optional, Rust backend covers Vosk/Parakeet but native gives extra throughput
STT_CMAKE_ARGS="-DCMAKE_BUILD_TYPE=Release"
if [[ "$DO_CROSS" == "1" && "$ARCH" != "x86_64" ]]; then
  echo "[SKIP] stt_service cross from ARM host needs separate toolchain — building native stt_service only"
fi
mkdir -p "$REPO_ROOT/build"
# Only build if cmake can configure (Qt6 may be missing on minimal CI, but stt_service_native does not need Qt6 after Phase 3 patch)
if cmake -S "$REPO_ROOT" -B "$REPO_ROOT/build" $STT_CMAKE_ARGS 2>&1 | tee /tmp/cmake_log; then
  cmake --build "$REPO_ROOT/build" --target stt_service_native -j"$(nproc)" || echo "[WARN] stt_service_native build failed (non-fatal — Rust backend sufficient)"
  if [[ -f "$REPO_ROOT/build/stt_service" ]]; then echo "[STT] $REPO_ROOT/build/stt_service  $(du -h "$REPO_ROOT/build/stt_service"|cut -f1)  $(file "$REPO_ROOT/build/stt_service")"; ldd "$REPO_ROOT/build/stt_service" 2>&1 | head -20 || true; fi
  # Copy stt_service beside Rust binary so models_root autodetect finds it
  cp -f "$REPO_ROOT/build/stt_service" "$(dirname "$BIN")/stt_service" 2>/dev/null || true
  cp -f "$REPO_ROOT/build/stt_service" "$WS_ROOT/target/release/stt_service" 2>/dev/null || true
else
  echo "[WARN] cmake configure failed (see /tmp/cmake_log) — skipping stt_service build"
fi

echo "[5/7] Desktop + autostart…"
DESKTOP_DIR="$HOME/.local/share/applications"
mkdir -p "$DESKTOP_DIR" "$HOME/.config/autostart" "$WS_ROOT/dist"
ASSET_DESKTOP="$WS_ROOT/assets/quickstt.desktop"
ASSET_ICON="$WS_ROOT/assets/icon_app.png"
# Ensure icon exists (was generated via Pillow on Windows host)
if [[ ! -f "$ASSET_ICON" ]]; then echo "[WARN] $ASSET_ICON missing — creating placeholder"; python3 -c "from PIL import Image; Image.new('RGBA',(256,256),(26,26,26,255)).save('$ASSET_ICON')" 2>/dev/null || true; fi
cat > "$DESKTOP_DIR/quickstt.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=QuickSTT
GenericName=Voice Typing
Comment=Floating voice typing widget — X11 & Wayland
Exec=$BIN
Icon=$ASSET_ICON
Terminal=false
Categories=Utility;AudioVideo;Accessibility;
Keywords=stt;voice;dictation;whisper;parakeet;nemotron;vosk;
StartupNotify=false
X-GNOME-Autostart-enabled=false
DESK
desktop-file-validate "$DESKTOP_DIR/quickstt.desktop" 2>&1 || true
echo "[OK] $DESKTOP_DIR/quickstt.desktop"

echo "[6/7] .deb (cargo-deb)…"
if [[ "$DO_DEB" == "1" ]]; then
  if command -v cargo-deb &>/dev/null; then
    cd "$WS_ROOT"
    # Native deb
    cargo deb --no-build -p quickstt-gui 2>&1 | tee /tmp/cargo_deb_native.log || echo "[WARN] cargo deb native failed"
    ls -lh target/debian/*.deb 2>/dev/null || ls -lh "$WS_ROOT/target/debian/*.deb" 2>/dev/null || true
    # Cross amd64 deb if cross binary exists
    if [[ "$DO_CROSS" == "1" && -f "$WS_ROOT/target/$TARGET_X86/release/QuickSTT" ]]; then
      cargo deb --no-build --target "$TARGET_X86" -p quickstt-gui 2>&1 | tee /tmp/cargo_deb_x86.log || echo "[WARN] cargo deb x86 failed"
      ls -lh "target/$TARGET_X86/debian/*.deb" 2>/dev/null || true
      # Copy deb to dist/
      mkdir -p "$WS_ROOT/dist"
      cp -f target/debian/*.deb "$WS_ROOT/dist/" 2>/dev/null || true
      cp -f "target/$TARGET_X86/debian/*.deb" "$WS_ROOT/dist/" 2>/dev/null || true
    else
      mkdir -p "$WS_ROOT/dist"; cp -f target/debian/*.deb "$WS_ROOT/dist/" 2>/dev/null || true
    fi
    ls -lh "$WS_ROOT/dist/"*.deb 2>/dev/null || true
    # Lint deb
    if command -v lintian &>/dev/null; then lintian "$WS_ROOT/dist/"*.deb 2>&1 | head -40 || true; fi
    if command -v dpkg-deb &>/dev/null; then dpkg-deb -I "$WS_ROOT/dist/"*.deb 2>&1 | head -40 || true; fi
  else
    echo "[SKIP] cargo-deb not installed"
  fi
else echo "[SKIP] --no-deb"; fi

echo "[7/7] AppImage (linuxdeploy)…"
if [[ "$DO_APPIMAGE" == "1" ]]; then
  # Fetch linuxdeploy if not present
  if ! command -v linuxdeploy &>/dev/null; then
    echo "[FETCH] linuxdeploy"
    curl -L -o /tmp/linuxdeploy-x86_64.AppImage https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage || true
    chmod +x /tmp/linuxdeploy-x86_64.AppImage 2>/dev/null || true
    sudo mv /tmp/linuxdeploy-x86_64.AppImage /usr/local/bin/linuxdeploy 2>/dev/null || mv /tmp/linuxdeploy-x86_64.AppImage "$WS_ROOT/linuxdeploy" 2>/dev/null || true
    LD=linuxdeploy; [[ -x /usr/local/bin/linuxdeploy ]] && LD=/usr/local/bin/linuxdeploy; [[ -x "$WS_ROOT/linuxdeploy" ]] && LD="$WS_ROOT/linuxdeploy"
  else LD=linuxdeploy; fi
  if ! command -v linuxdeploy-plugin-gtk.sh &>/dev/null; then
    curl -L -o /tmp/linuxdeploy-plugin-gtk.sh https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh 2>/dev/null || true
    chmod +x /tmp/linuxdeploy-plugin-gtk.sh 2>/dev/null; sudo mv /tmp/linuxdeploy-plugin-gtk.sh /usr/local/bin/linuxdeploy-plugin-gtk.sh 2>/dev/null || true
  fi
  if command -v "$LD" &>/dev/null || [[ -x "$LD" ]]; then
    STT_BIN="$(dirname "$BIN")/stt_service"
    [[ -f "$STT_BIN" ]] || STT_BIN="$REPO_ROOT/build/stt_service"
    EXTRA_E=""
    if [[ -f "$STT_BIN" ]]; then EXTRA_E="-e $STT_BIN"; echo "[APPIMAGE] bundling stt_service: $STT_BIN"; fi
    echo "[APPIMAGE] $LD --appdir $WS_ROOT/dist/AppDir -e $BIN $EXTRA_E -d $ASSET_DESKTOP -i $ASSET_ICON --output appimage"
    mkdir -p "$WS_ROOT/dist"
    rm -rf "$WS_ROOT/dist/AppDir" 2>/dev/null || true
    # linuxdeploy needs FUSE; fallback to tarball if fails
    "$LD" --appdir "$WS_ROOT/dist/AppDir" -e "$BIN" $EXTRA_E -d "$ASSET_DESKTOP" -i "$ASSET_ICON" --plugin gtk --output appimage 2>&1 | tee /tmp/linuxdeploy.log || echo "[WARN] linuxdeploy failed — creating tarball fallback"
    # Fallback tarball if AppImage not produced
    if ! ls "$WS_ROOT/dist/"*.AppImage 1>/dev/null 2>&1; then
      echo "[FALLBACK] tarball"
      tar czf "$WS_ROOT/dist/quickstt-$(date +%Y%m%d)-$ARCH.tar.gz" -C "$(dirname "$BIN")" "$(basename "$BIN")" -C "$WS_ROOT" assets/quickstt.desktop 2>/dev/null || true
    fi
    ls -lh "$WS_ROOT/dist/"*.AppImage "$WS_ROOT/dist/"*.tar.gz 2>/dev/null || true
  else echo "[SKIP] linuxdeploy not available"; fi
else echo "[SKIP] --no-appimage"; fi

echo ""
echo "═══════════════════════════════════════════"
echo " QuickSTT Linux Phase 3 build complete"
echo " Binary:  $BIN  ($(file "$BIN" | cut -d: -f2))"
if [[ -f "$REPO_ROOT/build/stt_service" ]]; then echo " Native: $REPO_ROOT/build/stt_service"; fi
echo " Desktop: $DESKTOP_DIR/quickstt.desktop"
echo " Config:  ~/.config/QuickSTT/config.toml  (XDG TOML)"
echo " Models:  ~/.local/share/QuickSTT/models/  (or exe-side ./models)"
echo "         • vosk/small_en_us_0.15  (Vosk 50M)"
echo "         • nemotron/streaming_0.6b_q8_0/*.gguf  (716M)"
echo "         • nemo/tdt_0_6b_v3_int8   (Parakeet 640M)"
echo " Deb:     $WS_ROOT/dist/*.deb  (dpkg -i)"
echo " AppImg:  $WS_ROOT/dist/*.AppImage  (chmod +x && ./)"
echo " Run:     $BIN  — pill 360x50 r25, tray appindicator, global hotkeys"
echo " Logs:    RUST_LOG=info $BIN  |  ~/.local/share/QuickSTT/logs/"
echo "═══════════════════════════════════════════"
grep -E "PILL_WIDTH|PILL_HEIGHT|DOT_SIZE|WAVE_FPS" "$WS_ROOT/quickstt-gui/src/theme.rs" 2>/dev/null || true

# Checksums
if ls "$WS_ROOT/dist/"*.deb "$WS_ROOT/dist/"*.AppImage "$WS_ROOT/dist/"*.tar.gz 1>/dev/null 2>&1; then
  (cd "$WS_ROOT/dist" && sha256sum *.deb *.AppImage *.tar.gz 2>/dev/null | tee SHA256SUMS; echo "[SHA256] $WS_ROOT/dist/SHA256SUMS")
  cat "$WS_ROOT/dist/SHA256SUMS" 2>/dev/null || true
fi
# Smoke hints
echo "[SMOKE] Quick checks:"
echo "  dpkg-deb -I dist/*.deb  &&  dpkg -c dist/*.deb | head -20"
echo "  ./dist/*.AppImage --appimage-extract  &&  ls squashfs-root/usr/bin/  # AppImage inspect"
echo "  ldd $BIN | grep -E 'gtk|appindicator|asound|pulse' "
echo "  file $BIN  # should be ELF 64-bit LSB executable, x86-64 (amd64) or aarch64 (ARM)"
echo "  RUST_LOG=info $BIN  # pill should appear; Ctrl+Space hold for on-command"
