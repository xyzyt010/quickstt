# Changelog

## [2.0.0-alpha.18] — 2026-10-20

### Fixed
- **Linux: dictation started by itself and the wake word never fired.**
  Both were the same fault — the microphone input was *clipped*. With the
  capture gain cranked up (+30 dB internal boost **+** +30 dB ADC gain) the
  ADC delivered a rail-to-rail square wave instead of speech: the VAD read
  it as continuous speech, the wake word head scored 0.000 and the transient
  detector saw random onsets, so the app started dictating on nothing. The
  input path now conditions every sample (20 Hz DC-blocking high-pass +
  soft limiter), watches the raw rail fraction, reports a broken input
  ("Microphone input is CLIPPED (saturated)" / "No audio is arriving from
  the microphone") and **never triggers a wake word or a clap on it**.
- **Any platform: a noisy room recorded your voice and produced
  "+0 chars, nothing heard".** The segmenter's start gate was inverted: it
  armed on a *non-speech* chunk and opened the utterance on the next speech
  chunk, so a continuously loud signal never opened an utterance at all. The
  gate now does what it always claimed to do — the first qualifying chunk
  arms, the second consecutive one starts the turn.
- **The dashboard's microphone selection was ignored by two of the three
  capture paths.** Only the foreground stream used it; the background wake
  word listener and the engine stream always opened the OS default. All
  three now follow the selection, and changing it re-opens the background
  stream immediately.
- **PTT and the background listener fought over the microphone** (Linux
  shares one ALSA PCM): the foreground session could end up with pure
  silence. The background listener is now stopped — and waited for — before
  the foreground stream opens.
- **The engine stream could not open a 48 kHz USB headset.** It now falls
  back to the device's native format and resamples on the way in.
- **Android: a silent take was sent to whisper**, which hallucinates
  short words ("you", "yeah") on noise-only audio. Silent takes are now
  rejected before the engine runs.

### Added
- **Android: the same input protection as the desktop.** The AAudio capture
  is conditioned (DC removal + soft limiting), the setup card shows the live
  input health ("clipped" / "no signal"), a new **"Test mic"** button
  verifies the microphone without recording, and the app says what is wrong
  instead of pretending to listen to silence.
- **The foreground capture logs its level**, so a silent session is
  diagnosable instead of mysterious.
- **Linux: the README now documents the clipped-microphone fix** with the
  exact `amixer` / `pactl` commands.

## [2.0.0-alpha.17] — 2026-10-20

### Fixed
- **Linux: every transcription failed ("Transcribe failed", 0 characters).**
  The engine resolver never looked in the app's *own* `lib/quickstt/tools`
  directory, so it always fell through to the system-wide copy in
  `/usr/lib/quickstt` — an older release whose `whisper-cli` and
  `transcribe-cli` were compiled with `-march=native` and die with **SIGILL
  ("Illegal instruction")** on other CPUs. The model was never loaded at all:
  `whisper_model_load` stopped right after `n_langs = 99`. The app now
  prefers its own bundled engines, falls back to the next candidate when one
  dies, and states the real reason ("terminated by signal 4 — the engine
  binary was built for a different CPU").
- **Linux: the mini pill never appeared at all.** Slint reported the widget
  window as visible while the X11 window stayed `IsUnMapped` (Xfwm4 race), so
  nothing was on screen and Ctrl+Space looked completely dead. The window is
  now mapped explicitly after `show()` and re-mapped (at most every 2 s) if
  the WM unmaps it behind Slint's back.
- **Pill visibility (Windows + Linux).** The collapsed widget is a hairline
  bar that can disappear over dark videos; the window is now mapped
  explicitly and re-mapped when the window manager unmaps it, so the pill is
  always on screen. The pill's look is unchanged.
- **CI: Linux engines are built for the portable x86-64 baseline**
  (`GGML_NATIVE=OFF`, `-march=x86-64 -mtune=generic`) for whisper.cpp,
  transcribe.cpp and the native backend, so a release can no longer ship a
  binary that crashes with SIGILL on a user's CPU.

### Added
- **Android: real on-device dictation.** The touch app now captures the
  microphone for real (NDK AAudio through a small C shim — cpal has no
  Android backend), transcribes it on-device (whisper.cpp cross-compiled for
  arm64-v8a with the NDK, shipped inside the APK and unpacked on first run),
  copies the finished text to the clipboard and lists it in History. The
  model (Whisper Tiny EN, 32 MB) is downloaded in-app, never bundled.
- **Android: setup steps + floating bubble.** The Listen tab now walks
  through the three setup steps (microphone permission → model download →
  overlay permission) and explains how to use the app. A floating bubble is
  drawn above other apps (WindowManager overlay) showing the mic state and
  the latest transcript.
- **Qwen3-ASR 0.6B (Q4_K_M)** and **Cactus Whistle 16M** are now real,
  selectable catalog models on Windows and Linux (auto-detect language).

## [2.0.0-alpha.16] — 2026-10-20

### Fixed
- **Dashboard clipped on high-DPI / small windows.** The window minimum was
  applied in *physical* pixels while the Slint layout works in DIPs, so at 125%
  the smallest allowed window was 20% narrower than the layout minimum and the
  right edge (copy / delete / ⋮) was drawn outside the surface. The dashboard
  now opens at its designed 940×700 DIP, is clamped to the monitor work area,
  is centred on first show, and can no longer be resized below the layout
  minimum.
- **‹/› chevron replaced by a hamburger.** Same collapse/expand action, drawn
  three-bar icon, 44×40 touch target, no rotation, nav items never shift.
- Instant sidebar toggle, delete, copy ("Copied" toast) and play/pause.

### Added
- **Touch-first desktop GUI (Windows + Linux).** Scroll views pan by dragging
  (`mouse-drag-pan-enabled`), history rows are 44 DIP, play 44, copy/delete/⋮
  40 with 34 DIP plates, nav items 48, checkboxes/combos 40, sliders 34, seek
  bar 34 with a 14 DIP handle.
- **Adaptive sidebar.** Below 820 DIP the sidebar collapses to the 56 DIP icon
  rail automatically; the persisted user toggle still wins.
- **SenseVoice Small Multilingual** (229 MB, zh/en/ja/ko/yue with auto language
  detect) downloadable in-app on Windows and Linux. CPU execution provider on
  every OS.
- **Android app scaffold** (`android/`) — a new touch-first GUI (bottom
  navigation, record orb, 48 DIP targets), `cargo apk` build, plus a desktop
  preview binary for reviewing the scene without an emulator. Runtime wiring
  and the floating overlay pill are the next milestone.

### Changed
- `parakeet_engine` links ONNX Runtime statically with the DirectML provider on
  Windows; SenseVoice is pinned to the CPU provider there (DirectML
  access-violates on the sense_voice graph).

## [Unreleased] — Widget Polish & Migration Fixes

### Fixed
- **Mic icon state inversion** — The mic icon was showing the wrong SVG based on listening state in the Rust port. Now matches the C++ semantic: idle → simple filled mic; listening → mic with a red crossed-out slash, both with a colored (blue) interior.
- **Tray context menu looked unprofessional** — Rebuilt as a rounded, elevated panel with row-style menu items (icon + label), proper hover state, divider line, and graceful close-on-outside-click / Escape behaviour.
- **Model dropdown was a raw list** — Now styled as a proper popup with rounded corners, drop shadow, hover/active states, scrollable area for long catalogs, grouped section (installed first, then not-installed), status indicators (●/○), and a footer hint.
- **Widget did not look like a widget** — Added drop shadow + subtle top highlight to the pill so it feels like an actual floating overlay rather than an app surface.

### Added
- **Adjustable widget size** — Mirrors C++ `widgetFlexible`. Toggled from the Style tab ("Allow manual resizing of Pill Widget"). When on, hovering the pill edges/corners changes the cursor, and dragging resizes the widget, with width/height persisted to the registry.
- **State-aware model label** — The pill's model label now shows an installation status glyph plus the model size (e.g. `● Whisper Small EN Q5 • 190MB`), with dim color for not-installed models.
- **Dashboard Models tab overhaul** — Installed/available counts, selected-model callout, larger catalog list with status badges, distinct Download/Uninstall buttons, and a "Widget Dropdown Models" section with color-coded chips (local / cloud / favorite) drawn from the persisted `widgetModels` / `cloudWidgetModels` / `favoriteModels` settings.

### Technical
- `widget_flexible`, `pill_width`, `pill_height` settings from `Settings` are now honored at startup; explicit persistent storage hooks were added so resize gestures survive restarts.
- `QuickSttApp` gained resize state (`widget_flexible`, `is_resizing`, `resize_edge`, anchor pos/size/topleft) and a smoothed window-size update path.

## [2.0.0] - Architecture Overhaul
### Changed
- **Massive Architecture Migration:** The entire application has been rewritten from C++ and Qt to pure Rust.
- **Memory Footprint:** Adopted aggressive memory management, utilizing `eframe` softbuffer suspension and `tokio` to achieve sub-5MB idle RAM consumption.
- **GUI Framework:** Shifted from Qt/QML to immediate-mode `egui` and `tray-icon`.
- **Audio Capture:** Shifted from QtAudio to `cpal` WASAPI dual-stream architecture to eliminate continuous overhead.
- **Local STT Engine:** Integrated `whisper-rs` natively instead of relying on FFI subprocesses.
- **Wakeword Engine:** Integrated `livekit-wakeword` (ONNX) natively to replace the old implementations.
- **Environmental Triggers:** Added `tract-onnx` inference for YAMNet-256 to detect snaps, claps, and non-verbal cues.

### Removed
- **Cloud STT Integrations:** Removed OpenAI, Google, ElevenLabs, AssemblyAI, and Sarvam integrations to focus on a 100% local, privacy-first transcription pipeline.
- **Smart Home / Android TV Integration:** Ripped out the Tuya and Android TV remote logic from the core binary.
- **CMake Build Systems:** Eliminated all C++ FFI bindings, reducing the build pipeline to pure Cargo commands.

## 1.5.1

- Restored the native application and server packaging flow
- Added portable bootstrap distribution outputs
- Added LAN UDP discovery with HTTP/TCP package transfer
- Added direct-download fallback packaging
- Added cloud model/provider management groundwork
- Added GPU-aware model catalog scaffolding
- Added Smart Life / Tuya integration groundwork

