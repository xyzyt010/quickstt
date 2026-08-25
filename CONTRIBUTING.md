# Contributing to QuickSTT

Thanks for helping make offline voice typing excellent on Windows and Linux Mint / Ubuntu / Debian.

## Quick start for contributors

```bash
git clone https://github.com/quickstt/quickstt.git && cd quickstt
# Linux
./quickstt-rust/scripts/build-linux.sh --no-appimage
cargo check --offline && cargo test -p quickstt-core --lib
cargo fmt --check && cargo clippy --workspace -- -D warnings
# Windows
.\BuildApp.bat
```

## Project goals

- **Offline-first**: no cloud STT by default; models run via `libvosk` / `ort` / `transcribe-rs` / `nemotron_engine`.
- **Tiny idle**: <5 MB when idle (lazy `whisper-rs`/`livekit-wakeword`), ~30 MB listening.
- **Native packaging**: `.deb` + `AppImage` on Linux, `QuickSTT_Portable.exe` on Windows — no large binaries in git.

## How to contribute

- **Bugs**: use `.github/ISSUE_TEMPLATE/bug_report.yml` with `OS`, `arch`, `version`, repro steps, `RUST_LOG=info` logs, `arecord -l`.
- **Features**: open a `feature_request.yml` with user impact; discuss before large PRs.
- **PRs**: keep them surgical — one concern per PR, include `cargo check` + `cargo test` evidence and screenshots for UI changes.

## Branch & commit

- Branch from `main`: `feat/<area>-short-desc`, `fix/<area>-desc`, `docs/...`, `chore/...`.
- Commit: `feat(gui): add tray model select` / `fix(linux): dlopen libvosk.so fallback` (Conventional Commits optional but appreciated).
- Keep PRs <500 lines when possible; split large migrations.

## Areas where help is valuable

- **Linux packaging**: `cargo deb`, `linuxdeploy`, AppIndicator `libayatana` vs `libappindicator`, Wayland `wtype` vs X11 `xdotool`, `cross` `aarch64→amd64`.
- **GPU runtimes**: `libonnxruntime.so` / `libtensorflowlite_c.so` bundling, Vulkan `ggml`.
- **Wakeword**: `livekit-wakeword` / `oww_tflite.h` accuracy on diverse mics.
- **macOS**: not yet supported — WIP welcome.

## Pull request checklist

- [ ] `cargo check --offline` (Windows) and `cargo check` on Ubuntu 22.04 (with `libgtk-3-dev` etc) pass
- [ ] `cargo test -p quickstt-core --lib` 9 tests pass
- [ ] `cargo fmt` clean, `clippy` no new warnings
- [ ] Updated `README.md` / `docs/…` if user-facing
- [ ] No `*.deb` / `*.AppImage` / `*.gguf` / `*.onnx` committed (use Releases)
- [ ] Screenshots for pill/tray/dashboard/TextBoard changes

## Review & release

- CI runs `rust-check` (Ubuntu+Windows), `cpp-linux` (`stt_service`), `lint`. Release tags `v*` cut `.deb`/`AppImage` + `Portable.exe` via `release.yml` and publish `SHA256SUMS`.
- Maintainers may ask for a rebase or split if a PR mixes concerns (e.g., GUI + model catalog).

Questions? Open a Discussion or ask in your PR.
