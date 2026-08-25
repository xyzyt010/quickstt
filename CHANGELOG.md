# Changelog

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

