#!/usr/bin/env bash
# docker-build-amd64.sh — reproducible Ubuntu 22.04 amd64 build from any host (ARM64 or x86_64)
# Uses Docker --platform linux/amd64 with Ubuntu 22.04 (glibc 2.35) for max compatibility
# Run on host with Docker:  ./quickstt-rust/scripts/docker-build-amd64.sh
# Or on VM 129.151.239.19 (ARM64) via:  docker run --platform linux/amd64 ...
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WS_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$WS_ROOT/.." && pwd)"
echo "[DOCKER] Building amd64 artifacts inside Ubuntu 22.04 container (glibc 2.35)…"
echo "[DOCKER] Host: $(uname -m)  Docker: $(docker --version 2>&1 | head -1)"

# Ensure Docker buildx for cross
docker buildx ls 2>&1 | head -5 || true

# Pull base
docker pull --platform linux/amd64 ubuntu:22.04

docker run --rm --platform linux/amd64 \
  -v "$REPO_ROOT:/work" -w /work \
  -e DEBIAN_FRONTEND=noninteractive \
  ubuntu:22.04 bash -c '
set -euo pipefail
echo "[CONTAINER] $(lsb_release -ds)  arch=$(uname -m)  $(cat /etc/os-release | head -1)"
apt-get update
apt-get install -y curl git build-essential pkg-config cmake protobuf-compiler \
  libgtk-3-dev libayatana-appindicator3-dev librsvg2-dev \
  libasound2-dev libpulse-dev portaudio19-dev \
  libx11-dev libxi-dev libxtst-dev libxdo-dev libssl-dev libglib2.0-dev \
  lsb-release alsa-utils file patchelf zsync desktop-file-utils
curl --proto "=https" --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal
source "$HOME/.cargo/env"
rustup target add x86_64-unknown-linux-gnu
cargo install cargo-deb --locked || true
# AppImage deps
apt-get install -y wget || true
# Build
bash quickstt-rust/scripts/build-linux.sh --no-appimage || bash quickstt-rust/scripts/build-linux.sh
echo "[CONTAINER] Build done. Artifacts:"
ls -lh quickstt-rust/dist/* 2>/dev/null || ls -lh quickstt-rust/target/debian/* 2>/dev/null || true
ls -lh build/stt_service 2>/dev/null || true
# Checksums
( cd quickstt-rust/dist && sha256sum *.deb *.AppImage *.tar.gz 2>/dev/null | tee SHA256SUMS; cat SHA256SUMS )
'

echo "[DOCKER] Host dist/:"
ls -lh "$WS_ROOT/dist/" 2>/dev/null || ls -lh "$REPO_ROOT/build/" 2>/dev/null || true
cat "$WS_ROOT/dist/SHA256SUMS" 2>/dev/null || true

echo ""
echo "[NEXT] On Ubuntu/Debian amd64 target:"
echo "  sudo dpkg -i quickstt-rust/dist/quickstt_*.deb; sudo apt-get install -f -y"
echo "  quickstt  # or /usr/bin/quickstt"
echo "  # AppImage: chmod +x dist/*.AppImage && ./dist/*.AppImage"
