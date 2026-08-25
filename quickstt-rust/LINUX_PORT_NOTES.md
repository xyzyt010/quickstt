# QuickSTT Linux Port — Phase 1 (GUI/Frontend) — Delivery Notes

Date: 2026-02-09  Branch: linux-gui-phase1  Target: Ubuntu 22.04/24.04 + Debian 12, amd64

## What was ported (surgical, file:line refs)

### 1. Settings storage — Registry → XDG TOML
- `quickstt-core/src/settings.rs:1` — split into `#[cfg(target_os="windows")]` Registry path and `#[cfg(not windows)]` TOML path.
- Linux path: `~/.config/QuickSTT/config.toml` (`dirs::config_dir()`), fallback `~/.config/QuickSTT/config.toml`.
- Methods `save_string/save_dword/save_bool/save_multi_string` now dual-write: Registry on Windows, TOML on Linux. Added `set_string_field/set_dword_field/set_multi_string_field` mapping Qt key names → struct fields.
- Added `config_file_path()`, `save_to_toml()`, `load_from_toml()`, `save_all()`.
- Tests updated: `test_recording_dir_custom` now uses `/tmp/custom` on Linux.

### 2. Error & GPU detection
- `quickstt-core/src/error.rs:50` — added `PlatformError`, `From<windows::core::Error>` now `cfg(windows)` only.
- `quickstt-core/src/engine.rs:88` — `detect_gpus_dxgi()` now has Linux branch `detect_gpus_linux()` via `lspci -nn` parsing (NVIDIA/AMD/Intel), CPU fallback preserved.
- `quickstt-core/src/models/engine.rs:536` — `compact_working_set()` Linux branch calls `malloc_trim(0)` via `libc`.

### 3. Widget platform — transparent always-on-top
- `quickstt-gui/src/widget_platform.rs:88` — Linux `configure_widget_window` is now explicit no-op with comment: Wayland layer-shell / X11 `_NET_WM_WINDOW_TYPE_DOCK` are set via `ViewportBuilder` flags (`with_transparent`, `with_always_on_top`, `with_decorations(false)`). Added `linux_window_hints()` helper.
- `quickstt-slint/src/widget_platform.rs:3` — fixed `#[cfg(windows)]` → `#[cfg(target_os="windows")]`.

### 4. Main GUI — Windows → Linux
- `quickstt-gui/src/main.rs:1` — `windows_subsystem` now `cfg_attr(windows)`.
- `read_registry_bool` now has Linux branch reading `Settings::load().extra` + known keys.
- `detect_compute_targets` now probes `lspci` on Linux.
- `get_cursor_position` / `get_screen_size` — Linux stubs with Wayland-aware comments; real values come from egui viewport.
- `show_widget`/`hide_widget` — Linux branch calls `widget_platform::configure_widget_window(())` no-op safely.
- Added `widget_hwnd` remains `cfg(windows)` only.

### 5. New modules — TextBoard + Autostart
- `quickstt-gui/src/textboard.rs` — full egui port of `Source/text_board.h:70` / `text_board.cpp:59`. Viewport `quickstt_textboard` with title 28px, opacity, monospace text, attach/detach, `reposition_attached` snaps to `pillRect.bottom()`. Mirrors `appendText` normalization, header, scroll.
- `quickstt-gui/src/autostart.rs` — Linux XDG `~/.config/autostart/quickstt.desktop` with `Exec=... --minimized`, Windows still uses Registry `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.
- `quickstt-gui/src/lib.rs:1` — exports `autostart`, `textboard`.

### 6. Build system
- `quickstt-rust/Cargo.toml` — added `dirs 5.0`, `toml 0.8` to workspace deps.
- `quickstt-core/Cargo.toml`, `quickstt-gui/Cargo.toml` — added `dirs`, `toml` deps.
- `quickstt-gui/build.rs:3` — already `cfg(windows)` for winresource, safe on Linux.
- `scripts/build-linux.sh` — one-shot Ubuntu setup+build: `apt install libgtk-3-dev libayatana-appindicator3-dev librsvg2-dev libasound2-dev libpulse-dev libx11-dev libxi-dev libxtst-dev`, `cargo build --release`, desktop file, optional `cargo deb` / AppImage.
- `assets/quickstt.desktop` — freedesktop entry for deb/AppImage.

## Verification
- `cargo check --offline` on Windows: **pass** (0 errors, only warnings about unused helpers).
- `cargo check --target x86_64-unknown-linux-gnu` from Windows cross: fails at `glib-sys` pkg-config (expected without Linux sysroot). Native `cargo check` **must be run on Ubuntu VM** where `libgtk-3-dev` is present — see `scripts/build-linux.sh` step 3.
- Pill constants preserved: `theme.rs` `PILL_WIDTH 360 PILL_HEIGHT 50 PILL_RADIUS 25` == `pill_widget.cpp:551 pillWidth`. Waveform `waveform.rs` `MIN 24 MAX 140 baseline 220x28 delay 45ms fps 16ms` == `pill_widget.cpp:48`.
- Behaviour parity table vs Windows: see `LINUX_PORT_NOTES.md` table below.

## Behaviour parity (Windows → Linux)

| Feature | Windows | Linux (this phase) |
|---|---|---|
| Settings | Registry `HKCU\Software\QuickSTT\Config` | `~/.config/QuickSTT/config.toml` (TOML pretty) + Registry fallback on Wine |
| Data root | `APPDATA\QuickSTT` / exe-side `data/` | `~/.local/share/QuickSTT` (`dirs::data_dir`) / exe-side `data/` |
| Pill | `WS_POPUP WS_EX_TOOLWINDOW` etc | `ViewportBuilder transparent + always_on_top + no-decorations` (winit handles X11/Wayland) |
| Tray | `QSystemTrayIcon` / `tray-icon Win32` | `tray-icon` with `libappindicator` (requires `libayatana-appindicator3-dev`) |
| Hotkeys | `RegisterHotKey` | `global-hotkey` X11 `XGrabKey`; Wayland needs portal (XWayland fallback) |
| Autostart | Registry `Run` | `~/.config/autostart/quickstt.desktop` |
| GPU detect | `DXGI` | `lspci -nn` |
| Working set | `SetProcessWorkingSetSize` | `malloc_trim(0)` |
| TextBoard | `TextBoardWindow` QWidget | egui viewport `quickstt_textboard` (new) |
| Waveform | `buildWaveformLayout` C++ | `waveform.rs` identical math |

## Next (Phase 2 — gated)
Backend port (PortAudio→cpal ALSA/Pulse, Vosk/Nemotron .so, DeepFilter Linux binary) will be done **after your explicit approval** per your instruction. Phase 1 GUI artefacts are ready to build-test on `ubuntu@129.151.239.19` via `scripts/build-linux.sh`.

## How to test on your VM

```bash
ssh -i "C:\Users\hemsh_sfya5gq\Downloads\ssh-key-2026-06-09 (1).key" ubuntu@129.151.239.19
git clone <this repo> && cd quick_stt_app
chmod +x quickstt-rust/scripts/build-linux.sh
./quickstt-rust/scripts/build-linux.sh
# then
./quickstt-rust/target/release/QuickSTT
```

If host is ARM64, use `cross build --target x86_64-unknown-linux-gnu --release` or run on an amd64 runner for final amd64 artifacts (ARM64 will produce aarch64 binary which validates logic but not amd64 .deb).
