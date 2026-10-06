#!/usr/bin/env bash
# QuickSTT all-in-one installer for Debian/Ubuntu (incl. Linux Mint) amd64.
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/xyzyt010/quickstt/main/scripts/install.sh | bash
#   ... or pin a version:  ... | VERSION=v2.0.0-alpha.13 bash
# Installs XFCE4/Wayland/X11 prerequisites, the .deb from GitHub Releases,
# verifies the install, and prints first-run steps (model download happens
# in-app on first launch — no models ship in the package).
set -euo pipefail

REPO="${REPO:-xyzyt010/quickstt}"
VERSION="${VERSION:-latest}"

log() { printf '\033[1;34m[quickstt]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[quickstt:ERROR]\033[0m %s\n' "$*" >&2; exit 1; }

# 1. Preconditions -----------------------------------------------------------
[ "$(uname -m)" = "x86_64" ] || die "only amd64 is supported (got $(uname -m))"
command -v apt-get >/dev/null || die "apt-based distro required (Debian/Ubuntu/Mint)"
command -v curl >/dev/null || command -v wget >/dev/null || die "need curl or wget"

# 2. Resolve version + asset -----------------------------------------------
if [ "$VERSION" = "latest" ]; then
    if command -v curl >/dev/null; then
        API_JSON="$(curl -fsSL "https://api.github.com/repos/${REPO}/releases/latest")"
    else
        API_JSON="$(wget -qO- "https://api.github.com/repos/${REPO}/releases/latest")"
    fi
    VERSION="$(printf '%s' "$API_JSON" | grep -m1 '"tag_name"' | cut -d'"' -f4)"
    [ -n "$VERSION" ] || die "could not resolve latest release"
fi
log "installing QuickSTT ${VERSION}"

# cargo-deb names it quickstt_<ver-with-dots>-1_amd64.deb, e.g.
# v2.0.0-alpha.13 -> quickstt_2.0.0.alpha.13-1_amd64.deb
DEB_NAME="quickstt_$(printf '%s' "${VERSION#v}" | sed 's/-/./')-1_amd64.deb"
URL="https://github.com/${REPO}/releases/download/${VERSION}/${DEB_NAME}"
TMP_DEB="$(mktemp /tmp/quickstt-XXXXXX.deb)"
trap 'rm -f "$TMP_DEB"' EXIT

log "downloading ${DEB_NAME}"
if command -v curl >/dev/null; then
    curl -fL --progress-bar -o "$TMP_DEB" "$URL" || die "download failed: $URL"
else
    wget -O "$TMP_DEB" "$URL" || die "download failed: $URL"
fi

# 3. Checksum (best effort — skipped if the release has no SHA256SUMS) ------
SUM_URL="https://github.com/${REPO}/releases/download/${VERSION}/SHA256SUMS"
if command -v sha256sum >/dev/null; then
    if command -v curl >/dev/null; then
        curl -fsSL -o /tmp/quickstt.SHA256SUMS "$SUM_URL" 2>/dev/null || true
    else
        wget -qO /tmp/quickstt.SHA256SUMS "$SUM_URL" 2>/dev/null || true
    fi
    if [ -f /tmp/quickstt.SHA256SUMS ] && grep -q "$DEB_NAME" /tmp/quickstt.SHA256SUMS 2>/dev/null; then
        (cd /tmp && sha256sum -c --status <(grep "$DEB_NAME" /tmp/quickstt.SHA256SUMS)) \
            && log "checksum OK" || die "checksum FAILED for ${DEB_NAME}"
    else
        log "no checksum published for this release — continuing"
    fi
fi

# 4. System prerequisites (tray, audio, hotkeys, typing) ---------------------
log "installing system prerequisites (sudo required)"
sudo apt-get update
# libayatana-appindicator3 covers XFCE4/MATE/GNOME trays; xdotool = X11 typing,
# wtype + wl-clipboard = Wayland typing; portaudio/pulse = mic capture.
sudo apt-get install -y \
    libgtk-3-0 'libayatana-appindicator3-1|libappindicator3-1' \
    librsvg2-2 libasound2 libpulse0 libportaudio2 \
    libx11-6 libxi6 libxtst6 libglib2.0-0 \
    xdotool wtype wl-clipboard

# 5. Install the package ------------------------------------------------------
log "installing ${DEB_NAME} (sudo required)"
sudo apt install -y "$TMP_DEB"

# 6. Verify (no GUI launch — `quickstt` with no daemon args starts the pill,
# so verification is file/package based only) -------------------------------
dpkg -s quickstt >/dev/null 2>&1 && log "package registered with dpkg" \
    || die "dpkg does not know package 'quickstt'"
test -x /usr/bin/quickstt || die "/usr/bin/quickstt missing or not executable"
log "binary present: /usr/bin/quickstt"
test -f /usr/share/applications/quickstt.desktop && log "desktop entry present"
test -f /usr/lib/quickstt/wakeword_models/hey_jarvis_v0.1.onnx \
&& test -f /usr/lib/quickstt/wakeword_models/alexa_v0.1.onnx \
    && log "wakeword models present" \
    || die "wakeword heads missing under /usr/lib/quickstt — reinstall or report this"

# 7. Next steps ----------------------------------------------------------------
cat <<EOF

QuickSTT is installed. First launch:
  quickstt &

On first launch the setup screen offers STT + wakeword models to download —
nothing is bundled, you pick what you need (small Vosk for instant use,
larger ones for accuracy). Then:
  - Pill appears bottom-center (works on X11, Wayland, XFCE4, multi-monitor).
  - Ctrl+Shift+Space toggles dictation, hold Ctrl+Space for push-to-talk.
  - Wayland typing uses wtype; X11 uses xdotool (both installed above).
EOF
log "done"
