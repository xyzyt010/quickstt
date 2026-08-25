# Packaging — .deb · AppImage · tar.gz (Linux) + Portable.exe (Windows)

## Linux Mint / Ubuntu / Debian (amd64)

Build on **Ubuntu 22.04** (glibc 2.35) for a portable floor that runs on Mint 21 (22.04), Mint 22 (24.04), Ubuntu 22.04/24.04, Debian 12. Building on 24.04 (glibc 2.39) would break 22.04 users.

### .deb (recommended, integrates with Mint Software Manager / apt)

Produced by `cargo deb` from `quickstt-gui/Cargo.toml:50` `[package.metadata.deb]`:

```toml
[package.metadata.deb]
name = "quickstt"
depends = "libgtk-3-0, libayatana-appindicator3-1 | libappindicator3-1, librsvg2-2, libasound2, libpulse0, libx11-6, libxi6, libxtst6, libglib2.0-0"
assets = [
  ["target/release/QuickSTT", "usr/bin/quickstt", "755"],
  ["assets/quickstt.desktop", "usr/share/applications/quickstt.desktop", "644"],
  ["assets/icon_app.png", "usr/share/pixmaps/quickstt.png", "644"],
]
```

Build:

```bash
cargo install cargo-deb --locked
./quickstt-rust/scripts/build-linux.sh          # ends with cargo deb --no-build -p quickstt-gui
# or manually:
cargo build --release
cargo deb --no-build -p quickstt-gui
ls -lh target/debian/*.deb                      # quickstt_2.0.0-alpha.1-1_amd64.deb
mkdir -p quickstt-rust/dist && cp target/debian/*.deb quickstt-rust/dist/
dpkg-deb -I dist/*.deb | head -40               # Package quickstt, Section utils
dpkg -c dist/*.deb | head -30                   # /usr/bin/quickstt ...
lintian dist/*.deb 2>&1 | head                  # clean except no-manpage
```

Install on Mint:

```bash
sudo apt update && sudo apt install -y ./quickstt_*.deb
# Mint's apt resolves libayatana vs libappindicator via “|”
which quickstt && file /usr/bin/quickstt && ldd /usr/bin/quickstt | grep -E "gtk|appindicator"
quickstt &
```

Fallback `dpkg-buildpackage` (uses `debian/control`):

```bash
dpkg-buildpackage -us -uc -b   # in quickstt-rust/
```

### AppImage (portable, no sudo, no install)

Built by `linuxdeploy` + `linuxdeploy-plugin-gtk` (auto-fetched by `build-linux.sh`):

```bash
linuxdeploy --appdir dist/AppDir \
  -e target/release/QuickSTT -e build/stt_service \
  -d assets/quickstt.desktop -i assets/icon_app.png \
  --plugin gtk --output appimage
# → QuickSTT-2.0.0-alpha.1-x86_64.AppImage
chmod +x QuickSTT-*.AppImage && ./QuickSTT-*.AppImage &
# Inspect:
./QuickSTT-*.AppImage --appimage-extract && ls squashfs-root/usr/bin/
```

In Docker/FUSE-less CI the script falls back to `quickstt-*.tar.gz` (`tar czf`).

### Checksums

```bash
cat dist/SHA256SUMS
sha256sum -c dist/SHA256SUMS
```

Every Release publishes `SHA256SUMS` alongside `*.deb` + `*.AppImage` + `*.tar.gz`.

## Windows

`BuildApp.bat` bundles:

- `QuickSTT_Portable.exe` — bootstrap installer/updater (recommended for users)
- `QuickSTT_DirectDownload/QuickSTT_Basic/` — lean folder
- `QuickSTT_DirectDownload/QuickSTT_Full/` — full with services pre-expanded
- `QuickSTT_DirectDownload/release_manifest.json` + `SHA256SUMS.txt`
- `QuickSTT_Server/QuickSTT_LAN_Package.tar` — LAN

`CMakeLists.txt` `if(WIN32)`-guards `windeployqt` + MinGW DLL copy (`libgcc_s_seh-1.dll` etc) + Qt runtime.

## Which to ship to Mint users?

- **Mint users with `sudo`:** `.deb` — integrates with Update Manager, autostart, icons.
- **Mint users without `sudo` or wanting portable:** `AppImage` — `chmod +x` and run.
- **Corporate/locked Mint:** `.tar.gz` — `tar xzf && ./QuickSTT`.

Our `scripts/install.sh` auto-picks `deb` by default and falls back to `AppImage` on install failure.

See `scripts/install.sh --help` and the root README “Quick Start — Linux Mint”.
