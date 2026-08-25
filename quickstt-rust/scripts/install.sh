#!/usr/bin/env bash
# QuickSTT install.sh — one-liner for Ubuntu / Debian / Linux Mint amd64
# Mint 21 (Ubuntu 22.04) and Mint 22 (Ubuntu 24.04) fully supported.
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/quickstt/quickstt/main/scripts/install.sh | bash
#   curl -fsSL https://raw.githubusercontent.com/quickstt/quickstt/main/scripts/install.sh | bash -s -- --appimage
#   curl -fsSL https://raw.githubusercontent.com/quickstt/quickstt/main/scripts/install.sh | bash -s -- --deb
#   ./scripts/install.sh --check            # dry-run
#   ./scripts/install.sh --uninstall        # remove .deb
set -euo pipefail

REPO="quickstt/quickstt"
VERSION="latest"  # or v2.0.0-alpha.1
CHANNEL="deb"     # deb|appimage
CHECK_ONLY=0
UNINSTALL=0

for a in "$@"; do
  case "$a" in
    --appimage) CHANNEL="appimage" ;;
    --deb) CHANNEL="deb" ;;
    --version=*) VERSION="${a#--version=}" ;;
    --check) CHECK_ONLY=1 ;;
    --uninstall) UNINSTALL=1 ;;
    --help|-h) cat <<EOF
QuickSTT installer for Linux Mint / Ubuntu / Debian amd64
Usage: $0 [--deb|--appimage] [--version=vX.Y.Z] [--check] [--uninstall]
Examples:
  $0 --deb
  $0 --appimage
  $0 --version=v2.0.0-alpha.1 --deb
  curl -fsSL https://raw.githubusercontent.com/$REPO/main/scripts/install.sh | bash
  curl -fsSL https://raw.githubusercontent.com/$REPO/main/scripts/install.sh | bash -s -- --appimage
EOF
    exit 0 ;;
  esac
done

if [[ "$UNINSTALL" == "1" ]]; then
  echo "[quickstt] Uninstalling..."
  if dpkg -l | grep -q quickstt; then sudo apt remove -y quickstt; echo "[quickstt] .deb removed"; fi
  rm -f "$HOME/.local/bin/QuickSTT.AppImage" 2>/dev/null || true
  rm -f "$HOME/.local/share/applications/quickstt.desktop" 2>/dev/null || true
  echo "[quickstt] Done. Config/models kept at ~/.config/QuickSTT and ~/.local/share/QuickSTT"
  exit 0
fi

# Detect distro
detect_distro() {
  if [[ -f /etc/os-release ]]; then . /etc/os-release; echo "$ID $VERSION_ID ($PRETTY_NAME) arch=$(uname -m)"; else echo "unknown $(uname -m)"; fi
}
ARCH="$(uname -m)"
if [[ "$ARCH" != "x86_64" ]]; then
  echo "[quickstt] ERROR: You are on $ARCH. This installer ships amd64 (x86_64) binaries only." >&2
  echo "[quickstt] For ARM64, build from source:" >&2
  echo "  git clone https://github.com/$REPO.git && cd $(basename "$REPO")" >&2
  echo "  ./quickstt-rust/scripts/build-linux.sh && ./quickstt-rust/target/release/QuickSTT" >&2
  exit 1
fi
echo "[quickstt] Detected: $(detect_distro)"
if ! grep -qiE "mint|ubuntu|debian" /etc/os-release 2>/dev/null; then
  echo "[quickstt] Warning: Not Mint/Ubuntu/Debian — trying anyway. Requires apt + gtk3 + appindicator."
fi

need_cmd() { command -v "$1" >/dev/null 2>&1 || { echo "[quickstt] Missing: $1 — installing"; sudo apt update && sudo apt install -y "$1"; }; }

# Resolve latest version if requested
if [[ "$VERSION" == "latest" ]]; then
  if command -v curl >/dev/null 2>&1; then
    RESOLVED=$(curl -fsSL -o /dev/null -w "%{url_effective}" "https://github.com/$REPO/releases/latest" 2>/dev/null | sed 's#.*/tag/##' || echo "latest")
    if [[ -n "$RESOLVED" && "$RESOLVED" != "latest" ]]; then VERSION="$RESOLVED"; echo "[quickstt] Latest is $VERSION"; fi
  fi
fi

if [[ "$CHECK_ONLY" == "1" ]]; then
  echo "[quickstt] Check only — would install $CHANNEL $VERSION for $ARCH from $REPO"
  exit 0
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

install_deb() {
  local ver="$1"
  local url
  if [[ "$ver" == "latest" ]]; then
    url="https://github.com/$REPO/releases/latest/download/quickstt_2.0.0-alpha.1_amd64.deb"
  else
    # try versioned name, fallback to latest pattern
    url="https://github.com/$REPO/releases/download/$ver/quickstt_${ver#v}_amd64.deb"
  fi
  echo "[quickstt] Downloading .deb..."
  echo "  $url -> $TMPDIR/quickstt.deb"
  if command -v wget >/dev/null 2>&1; then wget -O "$TMPDIR/quickstt.deb" "$url"
  elif command -v curl >/dev/null 2>&1; then curl -fL -o "$TMPDIR/quickstt.deb" "$url"
  else echo "[quickstt] Need wget or curl"; exit 1; fi
  echo "[quickstt] Installing (may ask for sudo)..."
  sudo apt update
  # Mint/Ubuntu use libayatana-appindicator3-1 on newer releases; our deb Depends covers both
  sudo apt install -y "$TMPDIR/quickstt.deb" || { echo "[quickstt] apt install failed — trying dpkg + fix"; sudo dpkg -i "$TMPDIR/quickstt.deb" || true; sudo apt --fix-broken install -y; }
  echo "[quickstt] Installed. Binary: /usr/bin/quickstt  Desktop: /usr/share/applications/quickstt.desktop"
  echo "[quickstt] Run: quickstt &"
  # Quick verify
  dpkg -l | grep quickstt || true
  file /usr/bin/quickstt 2>/dev/null | head -1 || true
}

install_appimage() {
  local ver="$1"
  local url
  if [[ "$ver" == "latest" ]]; then
    url="https://github.com/$REPO/releases/latest/download/QuickSTT-2.0.0-alpha.1-x86_64.AppImage"
  else
    url="https://github.com/$REPO/releases/download/$ver/QuickSTT-${ver#v}-x86_64.AppImage"
  fi
  echo "[quickstt] Downloading AppImage..."
  echo "  $url -> $TMPDIR/QuickSTT.AppImage"
  if command -v wget >/dev/null 2>&1; then wget -O "$TMPDIR/QuickSTT.AppImage" "$url"
  else curl -fL -o "$TMPDIR/QuickSTT.AppImage" "$url"; fi
  chmod +x "$TMPDIR/QuickSTT.AppImage"
  mkdir -p "$HOME/.local/bin" "$HOME/.local/share/applications" "$HOME/.local/share/icons"
  cp -f "$TMPDIR/QuickSTT.AppImage" "$HOME/.local/bin/QuickSTT.AppImage"
  # Desktop integration (minimal)
  cat > "$HOME/.local/share/applications/quickstt.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=QuickSTT
GenericName=Voice Typing
Comment=Floating voice typing widget — X11 & Wayland
Exec=$HOME/.local/bin/QuickSTT.AppImage
Icon=audio-input-microphone
Terminal=false
Categories=Utility;AudioVideo;Accessibility;
DESK
  echo "[quickstt] AppImage at $HOME/.local/bin/QuickSTT.AppImage"
  echo "[quickstt] Run: $HOME/.local/bin/QuickSTT.AppImage &"
  echo "[quickstt] (Add to Mint menu: Menu → run AppImage once, or add to Startup Applications)"
}

case "$CHANNEL" in
  deb) install_deb "$VERSION" ;;
  appimage) install_appimage "$VERSION" ;;
esac

echo ""
echo "[quickstt] Done. After launch, right-click tray → Models to install Vosk/Parakeet/Nemotron."
echo "[quickstt] Config: ~/.config/QuickSTT/config.toml   Models: ~/.local/share/QuickSTT/models/"
echo "[quickstt] Logs: RUST_LOG=info quickstt  (or RUST_LOG=info ~/.local/bin/QuickSTT.AppImage)"
echo "[quickstt] Uninstall: $0 --uninstall  or  sudo apt remove quickstt"
