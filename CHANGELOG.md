# Changelog

## [2.0.0-alpha.35] — 2026-11-16

Linux pill-menu stacking + shape fix.

### Fixed
- **Linux (Xfce/Mutter/XWayland): the mini-pill's right-click menu could not
  escape the pill.** On this machine the pill is an override-redirect
  (true-widget) window; managed siblings can't raise above an
  override-redirect sibling under xfwm4 no matter the `StackMode::ABOVE`
  raise ordering, so the menu (a managed window) was always parked below
  the pill. The frameless menu windows now join the same override-redirect
  layer on first show — exactly what the pill itself does — so they stack
  above the expanded pill at all times. Keyboard focus/dismissal paths are
  unchanged (Winit/XSetInputFocus handles it).
- **Menus now raise ABOVE the pill after the hide/show re-creation,
  not before.** `SLINT_DESTROY_WINDOW_ON_HIDE=1` tears down and
  re-creates the window on every hide/show on Windows (and avoids a freshly
  re-created Linux window seating itself below the pill); the z-order raise
  is the last step now, matching the 2-second z-order heartbeat which always
  raised the pill first and the menus after.
- **The right-click menu had an opaque dark square frame.** The frameless
  frameless menu windows now use `background: transparent` (like the pill and
  the dock overlay) so the rounded card composites cleanly — the 14/12/10px
  radius shows through as intended on the Linux desktop, matching Windows.
- Tray menu (`TrayMenu`), pill menu (`PillMenu`) and submenu flyout
  (`MenuFlyout`) all fixed.

### Changed
- Bump version to 2.0.0-alpha.35.

## [2.0.0-alpha.34] — 2026-11-15

The release that makes models configurable. Every catalog model now surfaces
the parameters its engine actually honours — one panel on the Speech Models
tab, beside the existing per-model language dropdown — plus two long-standing
fixes carried over from the alpha.33 bring-up.

### Added
- **Model parameters panel (all catalogued engines).** A new "Model
  parameters" card sits under the per-model language dropdown on the Speech
  Models tab (Tab 1) and adapts its rows to the active model:
  - **Whisper.cpp**: Threads (slider 1–16, default 4), Beam size (slider
    1–8, default 5), Translate to English (toggle, `-tr`).
  - **Sherpa-ONNX / NeMo Transducer / NeMo CTC (Moonshine family)**: Threads
    (`--num-threads=N`).
  - **Nemotron 3.5 Streaming / Qwen3-ASR / Canary Flash**: Threads
    (`--threads N` at worker spawn — verified accepted by the shipped
    transcribe.cpp Handy worker and by the TCPP fallback CLI), Context window
    (`--n-ctx N`, Choice 0–4096; 0 = model maximum), Canary additionally
    exposes Translate to English (`--translate`).
  - **Cactus Whistle**: Threads (`--threads N`), Mode (Choice, `clip` default
    / `stream` — stream feeds the utterance through `--audio-stream
    --audio-chunk 1` so it decodes like a live pass; output pieces join with
    spaces).
  - **LFM3-Audio 2B**: Threads (`llama-cli -t N`), Context window (`-c N`,
    default 4096; the whole dictation KV cache).
  - Parakeet TDT / Phonon-2 / SenseVoice / Vosk / Photon / Canary-Qwen expose
    **no** knobs through this layer (Handy ONNX/Python workers carry no
    session-level CLI surface we can honestly drive); the panel stays hidden
    for them.
  - Values persist per model in settings (`model_params` — registry
    `modelParams` on Windows, TOML on Linux/macOS), are refreshed per
    transcription turn, and the persistent transcribe.cpp workers respawn
    automatically when the user changes Threads/Context/Translate
    (spawn-args comparison, no stale sessions).

### Fixed
- **Taskbar minimize → restore left the dashboard blank/white** on this
  Windows build even after the alpha.33 SW_HIDE/SW_SHOW cycle on `IsIconic`
  transition. The cycle now uses Slint's `window.hide()`/`window.show()`
  instead of raw Win32 calls (with `SLINT_DESTROY_WINDOW_ON_HIDE=1` set, hide
  destroys and show() re-creates the HWND, which re-seats the DWM redirect
  bitmap), followed by `configure_app_window` + `request_redraw()`.
  Verified across repeated minimize/restore cycles.
- **Sidebar collapse toggle direction** (alpha.33 showed the X on the rail).
  Final layout: expanded sidebar shows the **X (close)** glyph on top (click
  to collapse), collapsed rail shows the **hamburger (menu)** glyph on top
  (click to expand). Rail icons remain pixel-centered.
- **Cactus Whistle runtime could never be downloaded**: the staging URL
  pointed at `Cactus-Compute/needle3`, which only ships model weights
  (404 on the expected path). The runtime now comes from the pinned
  `Cactus-Compute/cactus` release (`releases/4.1.1`), per-OS binary
  (linux-x86_64 / linux-arm64 / windows-x86_64 / windows-arm64), staged as
  `cactus(.exe)`. Legacy `needle` names are still accepted by engine
  detection so existing installs keep working.

### Changed
- Bump version to 2.0.0-alpha.34.

## [2.0.0-alpha.33] — 2026-11-15

Bug fix release for wakeword-off dictation, the side-bar collapse toggle,
and minimize→restore rendering.

### Fixed
- **When wakewords are Off, the background mic is fully muted for triggers.**
  The background listener still ran for clap detection (and level metering),
  and the clap arbiter would start dictation on many non-speech sounds
  (coughs, doors, keyboard thumps, music onsets) — even with wake = Off.
  With wakewords off the entire acoustic auto-start path is now silenced:
  no wakewords, no claps, no applause. Dictation only ever starts from an
  explicit user action (Ctrl+Space / mic tap).
- **Sidebar collapsed-state toggle was missing/mis-rendered**: the brand row
  with the logo + "QuickSTT" wordmark hid the collapse button in rail mode and
  the collapsed-mode X glyph was briefly drawn as the literal text "E5C".
  - The rail now shows one morphing toggle: hamburger (menu icon) in expanded
    sidebar → X (close icon) in the collapsed rail, matching the Material
    baseline.
  - The toggle sits at the left edge of the sidebar in both modes (12px padding
    in expanded, centered in rail).
  - Glyph-escaping fixed before the Slint compile (the previous double-
    backslash stored the literal characters `\u{E5CD}` / `\u{E5D2}` in the
    string), so the hamburger/X are real font glyphs on Windows.
- **Taskbar minimize → restore left the dashboard white / invisible** on this
  Windows build. The 50ms poll now also detects the `IsIconic` transition
  (minimized → restored) and re-runs the same SW_HIDE/SW_SHOW sequence used
  on first show. No manual show/hide ever needed.

## [2.0.0-alpha.32] — 2026-11-15

Rendering + polish fixes for the dashboard and floating overlays.

### Fixed
- **First-show ghost on the dashboard/overlay windows** (Settings, menu, pill,
  textboard): Slint's winit backend never composites a first-shown window on
  this Windows build class — the visible bug is a fully transparent window that
  passes clicks through and shows nothing until some unrelated interaction.
  Workaround: a short SW_HIDE→SW_SHOW cycle inside `show_app_window`/`show_widget`
  right after show re-seats the redirect bitmap (verified in-session — the
  dashboard now renders perfectly on its first show).
- **Menu windows with `background: transparent` rendered as ghost rectangles on
  this Windows build** (menu rows, toast arrows, outline shapes can be invisible
  against black backgrounds). All menu/toast/textboard root backgrounds now use
  an opaque near-black instead — the rows, outlines and caret all render, and
  the pill menu paints fully every time.
- **Dashboard no longer stays pinned above everything forever.** The topmost
  hint is disarmed ~2s after show; clicking another app puts QuickSTT behind it
  like any normal window.
- **Frequent forced redraw during visible dashboard** reinstated (dropped the
  500ms throttle introduced in alpha.31 after investigation clarified the issue
  is interception-related, not repaint-related).
- **Menus hidden on creation never cleanly re-appeared** (`hide_show_cycle`)
  — flyout/tray/pill/textboard re-present on show.

### Changed
- Bump version to 2.0.0-alpha.32 (which includes alpha.32's installer checksum bump).

## [2.0.0-alpha.31] — 2026-11-15

UI/windowing fixes for the pill, the menus and the dashboard.

### Fixed
- **Compressed sidebar icons are pixel-centered in the collapsed rail** — all
  six section glyphs and the brand logo now place by direct pixel offsets,
  both axes, on every platform (Slint's layout-centering was 5–16 px off for
  several glyphs and the brand image).
- **Linux pill right-click menu opened UNDER the pill.** The pill is
  override-redirect on X11 (it wins sibling raises on its own), so the managed
  context menu rendered beneath it. The menus are now raised ABOVE the pill —
  at show time and on the ~2s re-raise heartbeat.
- **Linux pill context menu never dismissed.** `clicked_outside` was hardcoded
  to `false` for non-Windows, so the menu lingered (occluding the pill and
  reading as "frozen"). It now dismisses automatically when the pointer leaves
  the pill/menu/flyout cluster for ~650 ms, **Escape** inside the menu also
  closes it, and the pill glides back to its collapsed footprint seamlessly —
  matching Windows.
- **Dashboard never lost focus to other windows** — it was re-pinned TOPMOST
  every 2 seconds forever. The topmost hint now stands down ~2 s after show,
  clicking another app sends QuickSTT behind like any normal window, and the
  forced per-tick full redraw was throttled to 2 Hz (the DWM ghosting/window
  rendering as "transparent until hovered" went with it).

## [2.0.0-alpha.30] — 2026-11-15

Release pipeline: the GitHub Release now actually ships a working Windows
portable alongside the Linux builds, and both deliverables bundle the full
STT engine set (Whisper, Parakeet, Nemotron, LFM3-Audio).

### Fixed
- **Windows portable job was a no-op for years**: the job built the Qt widget app
  (which silently failed, `continue-on-error`), produced no QuickSTT
  distribution, and uploaded an empty `windows-dist` artifact while the
  release notes pointed at a non-existent `QuickSTT_Portable.exe`. The job now
  stages a **real, self-contained portable payload** from the Slint build:
  app exe, fonts, icons, LICENSE, wakeword models, Parakeet engine, Nemotron
  transcribe-cli (the x64 binary tracked in-tree), whisper.cpp built from
  source on the runner, LFM3-Audio pinned to llama.cpp b11960, and the
  Photon/Canary/disfluency Python workers — then zips it as
  `quickstt-desktop-v<TAG>-win64-portable.zip`.
- Engine resolution on fresh machines finds everything next to the exe on both
  deliverables.
- The Windows installer now also ships `whisper-cli.exe` (same engine, previously
  missing — Whisper models could not transcribe on a fresh install).
- Release notes and artifact collection reference the real portable asset name;
  Windows Qt build steps are gone (they nothing to the release).
- Repo hygiene: local CI-staged scratch (`QuickSTT-Portable/`, portable zips,
  `tools/whisper_cpp/`) is git-ignored.

## [2.0.0-alpha.29] — 2026-11-15

UI pass: Compact side rail icons are properly centered, Speech Models page is
drastically simplified and unified, and releases now bundle every engine
runtime (Linux whisper-cli / transcribe-cli / llama-cli — previously-only-
built-on-Linux engines are now part of the Windows production too, via the
release workflow).

### Fixed
- **Compressed Speech Models nav icons + rail expand button** are now exactly
  centered in every section (General, Speech Models, Widget Style, Wake Words,
  System, History): every icon gets explicit vertical centering inside its
  48px row, and rail mode has its own centered expand glyph in the 44px brand
  box (the expanded-wordmark collapse button is hidden in rail mode).
- **Speech Models page is drastically simpler and unified:**
  - One recommendation banner with a single action that flips Download
    <size> → % → Use it (replaces "Recommended" pill, "Use it" button, and
    packaging of a separate already-downloaded "Use it" / "Downloading").
  - One "Model" dropdown + one "Language" dropdown side by side. The language
    dropdown always shows exactly what the selected model supports — a single
    English entry when the model is English-only, or Auto + every language
    otherwise — never a different note or hidden picker.
  - One unified catalog card per model: tap-to-select (installed) or
    Download (missing). One action, one place, no duplicate sections,
    no "Downloaded" vs "Available" duplication, no card pasting of the
    progress bar.
- Recommendation state is refreshed on model selection, on switching the
  recommended priority, and when a download completes — the "Recommended"
  badge and banner are now always honest against the current install state.
- "Optimise for" combo and the models-info/hardware-info prose were removed
  (redundant with the unified banner and the catalog tags).

## [2.0.0-alpha.28] — 2026-11-14

Fixes the side-dock startup expand/collapse GUI glitch, eliminates the startup
style fight that hid the pill briefly on Windows launch, fixes wakewords so
they actually fire across all microphone levels, and strictly honors the
Wakeword ON/OFF setting.

### Fixed
- **Pill startup GUI glitch on side docks (and startup invisibility).**
  At startup, `main.rs` restored `pill_dock` to the Slint UI (`vertical=true`,
  `mirror=true`) but left Rust's `vertical` mutex hardcoded to `false`. The
  pill looked normal on first launch, expanded with the wrong bottom-dock
  footprint, and on its very first collapse (`anim_ticker`), Rust resized the
  window to the horizontal `36x8` (`45x10` px) mini footprint while Slint
  still rendered a `9x41` vertical pill — slicing it into a tiny deformed
  stub until dragged to another edge. Additionally, `ensure_frameless` kept
  re-applying `WS_EX_LAYERED` on a `DwmEnableBlurBehindWindow` + `softbuffer`
  window every 2s (hiding the surface for several seconds on launch), and
  `mic_grow`/`upper_op` started at `1.0` instead of `0.0`. All of these are
  now synchronized before showing the widget and on the first HWND tick, and
  `pill.slint` hides the side status pill immediately on collapse start.
- **Wakewords never fired on real speech (0.000 despite community heads).**
  Three stacked pipeline defects killed detection even after loading the
  community heads: (1) `WakeWordEngine` skipped `push_audio` on 3 out of 4
  quiet chunks (`quiet_ticks % 4`), starving and time-warping the mel cache;
  (2) the energy/VAD gate snapped shut on the very first quiet chunk after
  saying the wakeword — wiping hit counts before openWakeWord's causal
  76-frame/16-embedding peak (~200–700ms post-speech) ever scored; and
  (3) `apply_input_gain` multiplied by `3x` and hard-clipped to `i16` before
  attenuating loud inputs, square-waving normal/Mint speech. `push_audio` now
  runs losslessly on every chunk, a 960ms post-speech hangover keeps scoring
  across the causal peak, and `apply_input_gain` scales cleanly in `f32`.
- **Wakeword ON/OFF setting now strictly honored.**
  Removed the startup hack that forcibly overwrote `"Off"` back to `"Always On"`;
  synced `AppState::wakeword_active` with `wake_word_mode` on startup and
  toggle (fixing 20Hz UI toggle oscillation); added `SetWakewordsEnabled` so
  turning wakewords OFF shuts down all wakeword inference and triggers even
  when clap detection keeps the background mic alive; re-applies all saved
  preferences on every OFF → ON respawn; and persists `showWidget` on Windows.

## [2.0.0-alpha.27] — 2026-11-14

Two shipped fixes: the blank pill on fresh launch, and the dead wakeword.

### Fixed
- **Windows: the mini pill rendered as a see-through outline on every fresh
  launch.** The pill spawns expanded, then collapses via a native resize; the
  software renderer rebuilt the window surface blank and Slint's damage
  tracker marked only the changed item (the collapsed bar) dirty — everything
  else stayed invisible and whatever was behind (browser, editor, Obsidian's
  Vault/Graph menu) showed straight through it. Dragging the pill forced a
  repaint, which is why "move it in any direction" normalized it. The pill's
  root rectangle now re-emits a full-scene damage on every footprint change,
  painting the entire window each resize. Verified on a fresh launch: opaque
  capsule on a dark background.
- **The wakeword was dead on every inference — 511,000+ identical failures,
  zero wake detections.** Two defects combined: the model-dir resolver
  preferred the legacy custom heads (`[batch, 40, time]` LSTM) over the
  community openWakeWord heads the app is built to run, feeding the wrong
  tensor contract into tract; and `OwwHead::load` never checked the input
  shape, so the mismatch failed silently forever. The resolver now prefers
  the repo-root community layout (the installed Windows layout was
  always correct); heads with a non-[1, 16, 96] input shape are refused at
  load with one clear error. Verified: live scores flow from the community
  heads on both Windows and Linux.

## [2.0.0-alpha.26] — 2026-11-14

Desktop pass on top of alpha.25 (Windows + Linux), plus the first release
where text is grid-fitted by the rasterizer itself.

### Fixed
- **Linux + Windows: native app windows could paint before their final
  geometry applied** (windows re-shown after hide are parked at −25600 and
  re-shown via SW_RESTORE), leaving an unpainted strip at the left edge with
  wallpaper showing through an opaque APPWINDOW surface. A full-frame repaint
  is now queued before the window is shown, in addition to the existing
  post-restore redraw.
- **Dashboard tabs were clipped at the right window edge.** The content
  column sized to its longest single-line child instead of the window;
  `horizontal-stretch: 1` fills the row properly.
- **"System & Info" showed a hard-coded stale version** ("2.0.0-alpha.18").
  It now reports `CARGO_PKG_VERSION` at runtime — it cannot drift again.
- **Linux: release-stage dist artifacts accidentally tracked in git**
  (installers at 40+ MB) — removed and git-ignored.

### Added
- **Native-level text crispness via TrueType stem hinting.** The software
  rasterizer shipped by Slint (`i-slint-renderer-software` 1.17.0) draws
  un-hinted outlines: stems thin and AA darkens at UI text sizes — the
  "soft" look versus Windows ClearType. The crate is now vendored under
  `tools/slint-software` identical except `swash`'s `hint(true)`, and
  `[patch.crates-io]` binds the build to it. Inter already ships TrueType
  hinting, so stems now snap to the pixel grid. (LCD sub-pixel rendering
  remains future work — see `docs/` notes.)

### Changed
- Nothing user-visible beyond the fixes above; the pill/widget and Android
  behavior are unchanged.

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
