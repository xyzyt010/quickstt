# Troubleshooting — Linux Mint / Ubuntu / Debian + Windows

## Linux Mint — “I double-clicked the .deb and nothing happens”

Mint's GDebi may hide `apt` errors. Open Terminal and:

```bash
sudo apt update
sudo dpkg -i /tmp/quickstt.deb; sudo apt --fix-broken install -y
dpkg -l | grep quickstt
which quickstt; file /usr/bin/quickstt
ldd /usr/bin/quickstt | grep "not found"
```

If `ldd` shows `not found`, install the missing `Depends`: our deb declares `libgtk-3-0, libayatana-appindicator3-1 | libappindicator3-1, librsvg2-2, libasound2, libpulse0, libx11-6, libxi6, libxtst6` — Mint 21 uses `libayatana` (new), older Mint/Debian may still need `libappindicator3-1`.

## Linux — pill doesn't appear

```bash
RUST_LOG=info quickstt 2>&1 | tail -n 80
# Look for: “Display server: x11/wayland”, “Pill 360x50”, “tray-icon created”
ps aux | grep quickstt
echo $XDG_SESSION_TYPE  # x11 vs wayland
echo $WAYLAND_DISPLAY $DISPLAY
```

- **Wayland** (rare on Mint Cinnamon, which defaults to X11): install `wtype` (`sudo apt install wtype`) for typing. `global-hotkey` needs XWayland on pure Wayland compositors. Our `get_cursor_position:147` falls back to portal.
- **Headless CI**: `xvfb-run -a quickstt` or `xvfb-run -a cargo run -p quickstt-gui`.

## Linux — no tray icon on Mint Cinnamon

Cinnamon panel may hide AppIndicators:

1. `System Settings → Applets` → ensure **System Tray** or **AppIndicator Support** is enabled.
2. Right-click panel → `Add applets` → `System Tray` → drag to panel.
3. `ps aux | grep quickstt` should still show the process even if tray is hidden.

## Linux — no audio / mic

```bash
arecord -l                          # ALSA cards: card 0: ... device 0
pactl list sources short             # Pulse: alsa_input.pci...
arecord -f S16_LE -r 16000 -c 1 /tmp/test.wav & sleep 2; kill %1; aplay /tmp/test.wav
# Mint: Menu → Sound → Input → pick your mic
```

Test inside QuickSTT: Dashboard → `Audio Level` should move when you speak.

## Linux — models show “[Not Installed]”

```bash
ls -R ~/.local/share/QuickSTT/models
cat ~/.config/QuickSTT/config.toml | grep -E "selectedModel|widgetModels|wakeWords"
# Expected:
#   vosk/small_en_us_0.15/am/final.mdl
#   nemotron/streaming_0.6b_q8_0/*.gguf
#   nemo/tdt_0_6b_v3_int8/encoder*.onnx
RUST_LOG=info quickstt 2>&1 | grep -i "model not found"
```

Dashboard → **Models** → `Download` writes to that XDG path. Our `catalog::models_root` prefers `~/.local/share/QuickSTT/models`.

## Linux — typing doesn't insert text

QuickSTT types via `win_input.h`: `wtype` (Wayland `wlroots`) → `ydotool` → `xdotool` (X11). Install at least one:

```bash
sudo apt install wtype   # Mint Cinnamon X11: xdotool also works: sudo apt install xdotool
wtype "hello "           # test
```

Focused app must accept `KEYEVENTF_UNICODE` (most do). On pure Wayland GNOME, `wtype` needs `xdg-desktop-portal-wlr`.

## Windows — Qt / build

- `Qt6_DIR` not found → `set Qt6_DIR=C:\Qt\6.6.2\msvc2019_64\lib\cmake\Qt6`
- `windeployqt` not found → add `C:\Qt\6.6.2\msvc2019_64\bin` to `PATH` before `BuildApp.bat`.
- `MSVC` vs `MinGW` mismatch: use same kit for Qt and CMake (`-G "Ninja"` finds it).

## Cross amd64 from ARM64

Your VM `129.151.239.19` is ARM64 (`uname -m` `aarch64`) but releases must be amd64:

```bash
./quickstt-rust/scripts/build-linux.sh --cross-amd64   # needs Docker + cross (Cross.toml :amd64)
# or
./quickstt-rust/scripts/docker-build-amd64.sh          # Ubuntu 22.04 --platform linux/amd64
file QuickSTT  # must say “x86-64” not “aarch64”
```

Without Docker the native `aarch64` build still validates logic.

## Getting help

File a bug with: `OS`, `quickstt --version` or `dpkg -l | grep quickstt`, `RUST_LOG=info quickstt` logs, `arecord -l` + `pactl list sources`.

See `.github/ISSUE_TEMPLATE/bug_report.yml`.
