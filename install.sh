#!/bin/sh
# QuickSTT installer for Linux (v1.0.0).
#
#   curl -fsSL https://raw.githubusercontent.com/xyzyt010/quickstt/main/install.sh | sh
#   QUICKSTT_VERSION=desktop-v1.0.0 curl -fsSL .../install.sh | sh   # pin a release
#   sh install.sh --uninstall
#
# Debian-family x86_64 installs the .deb; everything else installs the
# universal tarball into ~/.local/share/quickstt (no sudo). Idempotent:
# re-running replaces the previous tarball install / reinstalls the .deb.
set -eu
# pipefail is not POSIX; enable it only where the shell supports it (bash/ksh).
if (set -o pipefail) 2>/dev/null; then set -o pipefail; fi

REPO="xyzyt010/quickstt"
RAW_VERSION="${QUICKSTT_VERSION:-desktop-v1.0.0}"
INSTALL_DIR="${HOME}/.local/share/quickstt"
BIN_LINK="${HOME}/.local/bin/quickstt"
DESKTOP_FILE="${HOME}/.local/share/applications/quickstt.desktop"

log() { printf '[quickstt] %s\n' "$*"; }
die() { printf '[quickstt:ERROR] %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
QuickSTT installer (Linux)

Usage:
  curl -fsSL https://raw.githubusercontent.com/xyzyt010/quickstt/main/install.sh | sh
  sh install.sh [options]

Options:
  --help        Show this help and exit.
  --uninstall   Remove QuickSTT (user tarball install and, if present, the .deb).
  --version X   Install release X instead of the default.

Environment:
  QUICKSTT_VERSION   Release to install. Accepts "1.0.0", "v1.0.0" or
                     "desktop-v1.0.0" (default: desktop-v1.0.0).

Debian/Ubuntu/Mint on x86_64 installs the .deb via apt (sudo if needed).
All other distros/arches install the tarball to ~/.local/share/quickstt,
symlink ~/.local/bin/quickstt and write a desktop entry. No sudo there.
EOF
}

# --- argument parsing (before any platform check, so --help works anywhere)
UNINSTALL=0
while [ $# -gt 0 ]; do
    case "$1" in
        --help|-h) usage; exit 0 ;;
        --uninstall) UNINSTALL=1 ;;
        --version) [ $# -ge 2 ] || die "--version needs an argument"; RAW_VERSION="$2"; shift ;;
        --version=*) RAW_VERSION="${1#--version=}" ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
    shift
done

# --- normalise version: 1.0.0 | v1.0.0 | desktop-v1.0.0 -> PLAIN + tag
VER="$RAW_VERSION"
case "$VER" in desktop-*) VER="${VER#desktop-}" ;; esac
case "$VER" in v*) VER="${VER#v}" ;; esac
[ -n "$VER" ] || die "empty version"
DESKTOP_TAG="desktop-v${VER}"
BASE_URL="https://github.com/${REPO}/releases/download/${DESKTOP_TAG}"

# --- platform detection
OS="$(uname -s 2>/dev/null || echo unknown)"
if [ "$OS" != "Linux" ]; then
    die "install.sh only supports Linux (detected: ${OS}). On Windows use install.ps1; on macOS build from source."
fi

case "$(uname -m 2>/dev/null || echo unknown)" in
    x86_64|amd64)  ARCH="x86_64" ;;
    aarch64|arm64) ARCH="aarch64" ;;
    *) die "unsupported architecture: $(uname -m) (need x86_64 or aarch64)" ;;
esac

# --- distro family from /etc/os-release
DISTRO_ID=""; DISTRO_LIKE=""
if [ -r /etc/os-release ]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    DISTRO_ID="${ID:-}"; DISTRO_LIKE="${ID_LIKE:-}"
fi
FAMILY="unknown"
case " ${DISTRO_ID} ${DISTRO_LIKE} " in
    *" debian "*|*" ubuntu "*) FAMILY="debian" ;;
    *" fedora "*|*" rhel "*|*" centos "*) FAMILY="fedora" ;;
    *" arch "*) FAMILY="arch" ;;
    *" gentoo "*) FAMILY="gentoo" ;;
    *" suse "*|*" opensuse "*) FAMILY="opensuse" ;;
esac
case "${DISTRO_ID}" in
    debian|ubuntu|linuxmint|pop|elementary|zorin|raspbian|kali|mx) FAMILY="debian" ;;
    fedora|rhel|centos|rocky|almalinux) FAMILY="fedora" ;;
    arch|manjaro|endeavouros|garuda) FAMILY="arch" ;;
    gentoo|funtoo) FAMILY="gentoo" ;;
    opensuse*|sles|sled) FAMILY="opensuse" ;;
esac

# --- helpers
SUDO=""
if [ "$(id -u 2>/dev/null || echo 1000)" != "0" ] && command -v sudo >/dev/null 2>&1; then
    SUDO="sudo"
fi

download() { # download URL DEST
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 3 -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -O "$2" "$1"
    else
        die "need curl or wget to download QuickSTT"
    fi
}

verify_sha256() { # verify_sha256 FILE ASSET_NAME (best effort: SHA256SUMS may be absent)
    sums_file="$WORK_DIR/SHA256SUMS"
    if ! download "${BASE_URL}/SHA256SUMS" "$sums_file" 2>/dev/null; then
        log "no SHA256SUMS published for ${DESKTOP_TAG} - skipping checksum verification"
        return 0
    fi
    expected="$(awk -v a="$2" '$2 == a || $2 == ("*" a) {print $1; exit}' "$sums_file")"
    if [ -z "$expected" ]; then
        log "SHA256SUMS has no entry for $2 - skipping checksum verification"
        return 0
    fi
    if command -v sha256sum >/dev/null 2>&1; then
        actual="$(sha256sum "$1" | awk '{print $1}')"
        # Also honour the canonical check for the downloaded file name.
        ( cd "$WORK_DIR" && printf '%s  %s\n' "$expected" "$(basename "$1")" | sha256sum -c - >/dev/null 2>&1 ) || true
    elif command -v shasum >/dev/null 2>&1; then
        actual="$(shasum -a 256 "$1" | awk '{print $1}')"
    else
        log "no sha256sum/shasum available - skipping checksum verification"
        return 0
    fi
    [ "$actual" = "$expected" ] || die "checksum FAILED for $2"
    log "checksum OK ($2)"
}

dep_hint() {
    case "$FAMILY" in
        debian)   log "runtime deps hint (Debian family): sudo apt install -y libgtk-3-0 libayatana-appindicator3-1 librsvg2-2 'libasound2t64|libasound2' libpulse0 libportaudio2 libopenblas0 libx11-6 libxi6 libxtst6 xdotool xclip wtype wl-clipboard" ;;
        fedora)   log "runtime deps hint (Fedora family): sudo dnf install -y gtk3 alsa-lib pulseaudio-libs portaudio libX11 libXi libXtst xdotool xclip openblas wtype wl-clipboard" ;;
        arch)     log "runtime deps hint (Arch family): sudo pacman -S --needed gtk3 alsa-lib libpulse portaudio libx11 libxi libxtst xdotool xclip openblas wtype wl-clipboard" ;;
        gentoo)   log "runtime deps hint (Gentoo): sudo emerge -av x11-libs/gtk+:3 media-libs/alsa-lib media-libs/libpulse media-libs/portaudio x11-libs/libX11 x11-libs/libXi x11-libs/libXtst x11-misc/xdotool x11-misc/xclip sci-libs/openblas (or use packaging/quickstt.ebuild)" ;;
        opensuse) log "runtime deps hint (openSUSE): sudo zypper install -y gtk3 alsa pulseaudio portaudio libX11-6 libXi6 libXtst6 xdotool xclip libopenblas0 wtype wl-clipboard" ;;
        *)        log "runtime deps hint: install GTK3, ALSA, PulseAudio, PortAudio, libX11/libXi/libXtst, xdotool/xclip (X11) and wtype/wl-clipboard (Wayland) with your package manager" ;;
    esac
}

session_line() {
    if [ -n "${HYPRLAND_INSTANCE_SIGNATURE:-}" ]; then session="Wayland (Hyprland)"
    elif [ "${XDG_SESSION_TYPE:-}" = "wayland" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; then session="Wayland"
    elif [ "${XDG_SESSION_TYPE:-}" = "x11" ] || [ -n "${DISPLAY:-}" ]; then session="X11"
    else session="unknown (no DISPLAY/WAYLAND_DISPLAY)"
    fi
    log "session: ${session} - the same QuickSTT binary auto-supports X11 (incl. Xfce) and Wayland/Hyprland via winit; if your Wayland compositor blocks global hotkeys/tray, use the XWayland fallback (and install wtype/ydotool for typing)."
}

# --- uninstall
if [ "$UNINSTALL" = "1" ]; then
    rm -rf "$INSTALL_DIR"
    rm -f "$BIN_LINK" "$DESKTOP_FILE"
    log "removed user install: ${INSTALL_DIR}, ${BIN_LINK}, ${DESKTOP_FILE}"
    if [ "$FAMILY" = "debian" ] && command -v dpkg >/dev/null 2>&1 && dpkg -s quickstt >/dev/null 2>&1; then
        if command -v apt >/dev/null 2>&1; then $SUDO apt remove -y quickstt || true
        elif command -v apt-get >/dev/null 2>&1; then $SUDO apt-get remove -y quickstt || true
        fi
        log "removed quickstt .deb package (if it was installed)"
    fi
    log "uninstall done (config/models under ~/.config/QuickSTT and ~/.local/share/QuickSTT were kept)"
    exit 0
fi

# --- temp workspace with cleanup
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT
trap 'rm -rf "$WORK_DIR"; exit 130' INT
trap 'rm -rf "$WORK_DIR"; exit 143' TERM

log "QuickSTT ${DESKTOP_TAG} | os=Linux arch=${ARCH} distro=${DISTRO_ID:-unknown} family=${FAMILY}"
session_line

# --- Debian family + x86_64: prefer the .deb
if [ "$FAMILY" = "debian" ] && [ "$ARCH" = "x86_64" ]; then
    # cargo-deb naming: quickstt_<ver with '-' as '.'>-1_amd64.deb (1.0.0 -> quickstt_1.0.0-1_amd64.deb)
    DEB_VER="$(printf '%s' "$VER" | tr '-' '.')"
    DEB_ASSET="quickstt_${DEB_VER}-1_amd64.deb"
    log "downloading ${DEB_ASSET}"
    download "${BASE_URL}/${DEB_ASSET}" "$WORK_DIR/$DEB_ASSET" || die "download failed: ${BASE_URL}/${DEB_ASSET}"
    verify_sha256 "$WORK_DIR/$DEB_ASSET" "$DEB_ASSET"
    log "installing .deb via apt (its dependencies are resolved automatically)"
    if command -v apt >/dev/null 2>&1; then
        $SUDO apt install -y "$WORK_DIR/$DEB_ASSET"
    elif command -v apt-get >/dev/null 2>&1; then
        $SUDO apt-get install -y "$WORK_DIR/$DEB_ASSET"
    else
        $SUDO dpkg -i "$WORK_DIR/$DEB_ASSET" || $SUDO apt-get -f install -y
    fi
    log "installed. Run: quickstt"
    exit 0
fi

# --- everyone else: universal tarball, user install, no sudo
ASSET="quickstt-${DESKTOP_TAG}-${ARCH}.tar.gz"
log "downloading ${ASSET}"
download "${BASE_URL}/${ASSET}" "$WORK_DIR/$ASSET" || die "download failed: ${BASE_URL}/${ASSET}"
verify_sha256 "$WORK_DIR/$ASSET" "$ASSET"

STAGE="$WORK_DIR/stage"
mkdir -p "$STAGE"
tar xzf "$WORK_DIR/$ASSET" -C "$STAGE"
# Flatten a single wrapping directory, if the tarball has one.
if [ ! -f "$STAGE/QuickSTT" ]; then
    inner="$(find "$STAGE" -mindepth 2 -maxdepth 2 -name QuickSTT -type f | head -n 1 || true)"
    if [ -n "$inner" ]; then
        inner_dir="$(dirname "$inner")"
        cp -a "$inner_dir/." "$STAGE/"
        rm -rf "$inner_dir"
    fi
fi
[ -f "$STAGE/QuickSTT" ] || die "tarball did not contain a QuickSTT binary"

mkdir -p "${HOME}/.local/share" "${HOME}/.local/bin" "${HOME}/.local/share/applications"
rm -rf "$INSTALL_DIR"
mv "$STAGE" "$INSTALL_DIR"
chmod +x "$INSTALL_DIR/QuickSTT" || true
if [ -d "$INSTALL_DIR/tools" ]; then
    find "$INSTALL_DIR/tools" -type f \( -name 'whisper-cli' -o -name 'transcribe-cli' -o -name 'parakeet_engine' -o -name 'llama-cli' \) -exec chmod +x {} + 2>/dev/null || true
fi
ln -sf "$INSTALL_DIR/QuickSTT" "$BIN_LINK"

ICON_LINE="Icon=quickstt"
if [ -f "$INSTALL_DIR/icon_app.png" ]; then ICON_LINE="Icon=${INSTALL_DIR}/icon_app.png"; fi
cat > "$DESKTOP_FILE" <<EOF
[Desktop Entry]
Type=Application
Name=QuickSTT
GenericName=Voice Typing Widget
Comment=Floating offline voice typing - X11 & Wayland/Hyprland
Exec=${INSTALL_DIR}/QuickSTT
${ICON_LINE}
Terminal=false
Categories=Utility;AudioVideo;Accessibility;
Keywords=stt;voice;dictation;whisper;parakeet;nemotron;
StartupNotify=false
StartupWMClass=QuickSTT
EOF

dep_hint
case ":${PATH}:" in
    *":${HOME}/.local/bin:"*) ;;
    *) log "note: ${HOME}/.local/bin is not on your PATH - add it or run ${INSTALL_DIR}/QuickSTT directly" ;;
esac
log "installed to ${INSTALL_DIR}"
log "Run: quickstt    (or: ${INSTALL_DIR}/QuickSTT)"
