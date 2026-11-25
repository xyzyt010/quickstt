# Changelog

## [2.0.0-alpha.50] — 2026-11-25

Verification hooks, slimmer sidebar, tour row centre (from the Mint audit loop).

### Fixed
- **"Show the intro tour again" row is truly centred** — its layout now
  fills the 40px pill height so the text sits dead-centre (it rode high).
- **Sidebar slimmed back down.** The 240px minimum overcompensated for the
  +2px text: it is 205px everywhere (default, drag and persisted clamps
  205–280), and nav hover boxes are 48px (was 54px), chevron 36px. The
  largest label ("Wake Words & VAD") still fits with margin, and the
  expanded sidebar no longer dominates smaller windows. The 56px rail and
  the 820-DIP adaptive threshold are unchanged, so narrow windows still
  collapse to the minimal rail instead of breaking.

### Added
- `FLYOUTMODEL` / `DASHMOVE` Mint verification IPC: deterministic Model
  flyout photo (logs the exact row list) and cursor-free hover proof for
  Delete-all / sidebar text without any physical mouse input.

## [2.0.0-alpha.49] — 2026-11-25

Right-click Model menu filtering, sidebar expand, button alignment audit.

### Fixed
- **Right-click → Model menu now lists only usable models.** Rebuilt on
  every open from live state: Local models appear only when actually
  downloaded; hosted API models (Groq/OpenAI/DeepGram) appear only when
  that provider has an API key entered (any of its models carrying a
  non-empty key counts the provider as configured). Providers without a
  key contribute no rows. Entering a key or finishing a download updates
  the menu on its next open — no restart. The dashboard catalog remains
  the place to discover, download and connect everything else.
- **Sidebar expands again at narrow windows (Mint report).** The adaptive
  rail (`rail-mode` below 820 DIP) swallowed the expand toggle: it flipped
  `sidebar-collapsed` but the rail stayed. An explicit expand now wins
  over the adaptive rail (`sidebar-force-open`), the toggle glyph follows
  the actual rail state, and expanding at a narrow window grows the
  window to 960 DIP so sidebar + content both fit.
- **Delete all was oversized and top-aligned.** It is now a slim 138×40
  pill (196px armed) matching the neighbouring toggles' height, with its
  icon + label vertically centred; Export/Delete-selected pills got the
  same centring.
- **Sidebar nav text ("Wake Words & VAD" etc.) is pixel-centred** in its
  button: icon and label are absolutely centred vertically instead of
  layout-placed, identical at rest and on hover (all six nav items;
  `ToggleRow` got the same treatment).
- **History rows redesigned to minimal chrome.** No permanent grey plates:
  the copy/trash/⋮ actions are bare 16px icons (30px targets) with a soft
  highlight only on hover/active; the trash artwork is now white. The
  play/pause control moved into the timeline row, left of the slider
  (using the previously empty width), and morphs play→pause with a
  crossfade — again no permanent circle, just a hover/playing highlight.
  The transcript now sits at the bottom of the block, in full (the
  240-char truncation is gone), scrolling inside an 86px-capped viewport
  when it does not fit.

## [2.0.0-alpha.48] — 2026-11-25

Screenshot-loop fixes from Mint verification of alpha.47.

### Fixed
- **History rows: the left-side empty space is gone.** The selection
  checkbox and play circle (and the copy/⋮ buttons) were swapped with
  `visible:` — an invisible element still reserves its layout slot, so
  the play button sat 53px in from the row's left edge while the right
  side was flush. They are now conditional (`if`) elements that occupy
  no space when absent; play, transcript and timeline share one left
  edge. Verified in Mint screenshots at small and large sizes.
- **"Best fit" no longer recommends a hosted API model.** API entries
  (size 0, top paper accuracy) outscored every local model, so the
  banner recommended DeepGram Nova-3 as the hardware fit. The
  recommendation now scores local engines only (regression test
  `api_models_are_never_the_best_fit`; 34 core tests pass).

### Added
- `DASHSCROLL px` Mint verification IPC (programmatic dashboard scroll)
  so the model catalog can be screenshot-verified without a mouse;
  `DASHTAB` resets the scroll.

## [2.0.0-alpha.47] — 2026-11-25

Catalog/sidebar/history alignment, discoverable offload, terminating offload.

### Fixed
- **Catalog cards are now deterministically middle-aligned.** The card row
  is a fixed 64px zone: the model name + languages/size sit on the LEFT,
  vertically centred, and the Active/Select/Use/Connect/Download button sits
  on the RIGHT, vertically centred (`y = (64 − 34) / 2`) — absolute centring
  instead of layout cross-axis defaults, so it holds at every window size.
- **Sidebar highlight can no longer be smaller than its label.** With the
  +2px text, "Wake Words & VAD" could reach past the nav highlight at the
  old 150–280px sidebar range. The sidebar minimum is now 240px (default
  240, drag clamp 240–320, persisted values re-clamped on sync) and nav
  labels elide inside the highlight instead of overflowing it.
- **History header no longer overflows small windows.** Title/count and the
  Select/Delete-all/Backup controls are stacked in two rows, transcript and
  timeline are flush-left (the 53px gutter made the left side emptier than
  the right), the ⋮ menu grew to fit its 48px items, and the copy toast is
  wide enough for its text.
- **Offload now TERMINATES the worker process.** A polite JSON "unload"
  only helps if the helper truly frees its model context — unverifiable.
  `parakeet/nemotron/canary/photon/canary-qwen_unload()` now kill + reap
  the child (via new `Drop` impls for Photon/Canary-Qwen, which previously
  could be orphaned like Parakeet was), so after the configured idle the
  helper (e.g. `nemotron_engine`) is gone from the process list and its
  RAM is provably released. The next turn respawns + reloads lazily.
  Your setting was already ON at 5s (verified in the startup log:
  `offload=true/5s`) — the release mechanism itself is what changed.

### Added
- **Model offloading controls on the Models tab** (same Auto-offload toggle
  + delay slider as General → Memory Management, with a plain-language
  status line), because that is where users look for it. General keeps its
  card too, now with the same status line.
- Mint screenshot-verification IPC: `DASHTAB n`, `DASHSIZE w h`,
  `HISTPLAY n` (open dashboard on a tab / at an exact size / play a history
  row) so GUI verification runs over SSH with `scrot`, no mouse input.

## [2.0.0-alpha.46] — 2026-11-25

Settings rename, +2 text everywhere, History rebuild (icons, playback, scroll).

### Fixed
- **Tray menu is now "Settings" everywhere.** The Windows native tray menu
  (the owner-drawn overlay), the Linux SNI menu and the legacy reference GUI
  all said "Dashboard"; they now say "Settings". The Slint window title is
  "QuickSTT Settings" too.
- **History playback now actually sounds right.** The output stream was
  written as mono F32 regardless of device format/rate: on Windows (stereo
  I16 WASAPI defaults) it built a stream whose clock was the *device* rate
  while the playhead advanced per interleaved sample — effectively 2x fast
  and chipmunk-pitched, i.e. unrecognisable. The player now prefers an F32
  config (falling back to I16/U16 conversion), duplicates mono to every
  channel per-frame, and linearly resamples 16 kHz recordings to the device
  rate so position, duration, pitch and progress all track real time.
- **History progress bar fills strictly left → right.** The played fill is
  explicitly anchored at `x: 0` and the track itself is `x: 0` (previously
  left implicit), with track-height blinking removed so the bar cannot
  shimmer. The elapsed/total clock uses the decoder's true length while
  playing, so the time now advances in step with the bar.
- **History rows no longer block scrolling.** Each history card carried a
  full-area `TouchArea` (for hover feedback) that swallowed drag-pan —
  the "laggy scrollbar". It is removed; the row selection set is cloned
  once per sync instead of once per row (60 → 1 clones per tick).
- **All button icons are centred.** Copy/delete/more glyphs were parked at
  `6,6` in a 44 px target (top-left), the play/pause triangle at `8,8` in
  the 44 px circle, history-menu icons at `8,8` in 48 px rows, and the toast
  tick at `10,6`. They are now centred with `(parent - self) / 2` on both
  axes — across History, the pill menus and the tray overlay.

### Changed
- **All UI text is exactly +2 px throughout.** Every `font-size` in every
  Slint screen (dashboard, pill, menus, textboard, onboarding, dock, shared
  components) grew by exactly 2 px (e.g. 12 → 14, 13 → 15, 11.5 → 13.5),
  with row/menu/card heights enlarged in step so nothing clips. The pill
  shrink-wrap measurement matches the new sizes (17/14 px).
- **History actions are icon-first.** Delete uses your supplied trash
  artwork (`delete_red.svg`, recoloured red) for the row delete button,
  the ⋮ menu "Delete turn", and both bulk deletes ("Delete all",
  "Delete selected"); "Export audio" gains a download glyph. Bulk controls
  are now centred icon + label pills instead of bare text.
- The "Best fit" banner is always visible (previously hidden when no
  recommendation resolved), so the Models tab never looks empty; catalog
  cards and action pills grew to fit the +2 text without clipping.

## [2.0.0-alpha.45] — 2026-11-25

Worker orphan leak fixed; offload always eventually fires; UI text hierarchy.

### Fixed
- **Worker processes were orphaned and leaked memory.** `ParakeetSession`
  held a raw `Child` with no `Drop` impl; every worker respawn (model
  switch, params change) dropped the old session and detached the old
  `nemotron_engine.exe` / `parakeet_engine.exe` / python worker — it kept
  running with its model in RAM forever. That's the source of the stray
  processes in Task Manager and the "Nemotron never offloads" / "local
  models get slow" reports. The child is now terminated + reaped on drop
  (root fix, covers every worker family). Your machine also has a leftover
  `python3 (ppid=1)` orphan from an older build — safe to kill.
- **Auto-offload now always eventually fires.** It previously anchored only
  on turn ends, so a worker loaded by a path that never armed a turn end
  (or a fresh boot) never unloaded even with the setting on. The anchor is
  now boot-based as well, and re-sends are gated on `model_offloaded`.
- **Custom phrases note removed** from Wakewords (the retired-phrases text
  is gone; the Hey Jarvis / Alexa toggles remain).

### Changed
- Grey helper/hint text scaled up one step (10–11.5px → 11–12px) across the
  dashboard — more readable while staying visually subordinate to 12.5–13px
  body text. Stale "cloud engines removed" comment corrected in engine.rs.
- **OpenAI catalog now has 2 STT models**: Whisper Large v3 Turbo and
  GPT-4o Transcribe.
- Bump version to 2.0.0-alpha.45.

## [2.0.0-alpha.44] — 2026-11-24

Settings overhaul: API providers, hardware-fit recommendation, mic list fix,
menu show/hide symmetry.

### Fixed
- **Microphone list on Linux no longer lists unrelated sources.** Monitor
  sources, loopback cables, virtual/placeholder devices (PipeWire/ALSA
  enumerate them as inputs) are filtered out; only real microphones remain,
  with the OS default first.
- **Tray Show/Hide vs pill menu symmetry.** Hiding the widget now also takes
  the pill menu + flyout down; showing it (tray click, tray Show/Hide,
  double-click, SNI) immediately re-opens the right-click menu anchored at
  the pill — you land back on the menu you left instead of needing a second
  right-click.
- **Startup log trigger slot corrected** (`offload=false/300s trigger=hold|toggle`)
  — shipped here from alpha.43's fix.

### Added
- **Providers: Groq / OpenAI / DeepGram.** A Provider dropdown (Local by
  default) sits above the Model dropdown and filters the model list;
  switching a provider activates its last-used model. API models: Groq
  Whisper Large v3 Turbo, OpenAI Whisper Large v3 Turbo, and five DeepGram
  Nova models (nova-3, nova-3-medical, nova-3-conversational-ai, nova-2,
  nova-2-medical). The UI lists them in the catalog after the offline
  models; with a key set they show **Use**, without one they show **Connect**
  which expands an inline, themed API-key box with where to get the key.
- **Catalog collapse.** The catalog header now has a chevron that rotates
  (▼ open → ▲ closed); collapsed it shows just the rounded header. Missing
  local models render their names in muted grey. Download / Select / Use /
  Connect buttons are middle-aligned in their cards.
- **Hardware-fit "Best fit" recommendation.** The old top banner is now a
  RAM/VRAM/GPU-aware best-fit pick (same scoring as before, strengthened:
  GPU-checkpoint fit vs VRAM, half-RAM thrash guard), with a long-form reason
  naming the measured hardware. The same fit logic recommends the cleanup
  engine (FDT / S1-mini / SmolLM2) by RAM with a green note under the
  Cleanup model dropdown.
- **System tray dropdown entry renamed Dashboard → Settings.**

### Changed
- Helper/hint text stepped up one size (10–10.5px → 11px) for readability
  while staying visually subordinate to 12–13px body text; dead duplicate
  text trimmed.
- Bump version to 2.0.0-alpha.44.

## [2.0.0-alpha.43] — 2026-11-24

Startup log line fix.

### Fixed
- **Startup "settings restored" line printed the trigger mode in the wrong
  slot** (`offload=false/holds trigger=300` — `trigger=` showed the offload
  delay, not the mode). Argument order corrected; the line now ends with
  `offload=false/300s trigger=hold|toggle`.

### Changed
- Bump version to 2.0.0-alpha.43.

## [2.0.0-alpha.42] — 2026-11-24

Menu trace scaffold + menu re-creation churn removed; Trigger Mode hardening.

### Fixed
- **Linux: menu vanishing on hover — second round of root-cause removal.**
  The pill menu itself was still calling the 120 ms `hide_show_cycle` (native
  X window destroy + recreate) on *every* right-click open; it is now
  first-presentation-only, same as the flyout. Every menu tear-down is now
  caller-tagged in the new trace.
- **Pixel-level menu trace (`quickstt-menu-trace.log`).** Every right-click
  arms a 10-second trace that logs, per 50 ms tick: cursor `cur=(x,y)`,
  held-button state, pointer-query failures, menu position/scale, all five
  inside-rect flags (`in_menu`, `in_fly`, `in_pill`, `in_corridor`, both row
  bands), `want`/`active`/`pin` hover state, both grace timers, both dismiss
  decisions, and window-visibility — plus call-tagged hide/show/open events.
  Reproduce once on Mint, then share the trace file; it names the exact tick
  and cause.
- **Trigger Mode (hold vs toggle): autorepeat guard + observability.** Repeat
  `Pressed` events while the key is held are now ignored (in toggle mode a
  repeat previously looked like a second press and instantly stopped the
  session it had just started). The trigger mode is now printed in the
  startup "settings restored" line and whenever the dashboard changes it, and
  toggle starts are logged. Windows logs of the full hold-mode cycle were
  reviewed — press/hold/release/transcribe/paste is intact.

### Changed
- Bump version to 2.0.0-alpha.42.

## [2.0.0-alpha.41] — 2026-11-24

Menu hover/dismissal parity, outside-click on Linux, Groq cloud Whisper.

### Fixed
- **Right-click menu / flyout hover glitches on Linux.** The shared
  Microphone/Model flyout used to re-create its native X window (and sleep
  120 ms on the UI thread) on *every* hover switch between the adjacent rows,
  leaving a stale cursor snapshot that closed the flyout, hid the whole menu,
  or collapsed the pill. The re-present now happens once on first open only.
  A failed X cursor query reuses the last good position instead of a bogus
  (0,0) that looked like "pointer left the cluster". Opening/switching the
  flyout restarts the dismiss grace window, and the grace is now applied to
  the hover-leave timer (it was computed but ignored on Linux).
- **Pressing outside the menu now dismisses it on Linux too.** Previously
  `clicked_outside` was hardcoded `false` on non-Windows, so only a 650 ms
  hover timer could close the menu. Both platforms now use the same pointer
  query (position + held buttons) and the same dismissal rule.

### Added
- **Groq cloud transcription model.** Settings → Models now lists
  *Groq Whisper Large v3 Turbo (API)*. Select it, paste your Groq API key in
  its "Model parameters" field, and dictation is transcribed through
  `whisper-large-v3-turbo` on Groq's OpenAI-compatible endpoint. No local
  weights or download; the key persists per-model on Windows and Linux and
  takes effect on the next utterance. Other engines remain fully offline.
  Language names from the UI are mapped to ISO-639-1 codes; unknown values
  fall back to auto-detect. Added a catalog regression test.

### Changed
- Bump version to 2.0.0-alpha.41.

## [2.0.0-alpha.40] — 2026-11-23

Revert menu heading sizing; uniform quiet waveform on Linux.

### Fixed
- **Right-click menu "Model" heading is back to its original size.** The
  alpha.39 2× enlargement (13px → 26px) and the flyout header enlargement
  (11px → 22px) are reverted — both render exactly as the other menu rows
  again. The red mic icon / black active pill from alpha.39 is kept.
- **Linux: waveform is uniform while quiet, matching Windows.** Linux's
  ALSA/PipeWire capture reports low-level room noise as a fluctuating
  5–20/100, which snapped to 1/6 steps and rendered as a mix of 2px dots and
  taller bars. A quiet deadzone (0.20, aligned with the VAD's ~25/100 speech
  floor) now collapses that noise to the same minimal height on every bar,
  so the idle waveform is a clean, even row — as on Windows.

### Changed
- Bump version to 2.0.0-alpha.40.

## [2.0.0-alpha.39] — 2026-11-23

UI polish: menu heading and active-pill colours.

### Fixed
- **Right-click menu "Model" heading looked loosely spaced / had a visible
  gap, and was too small.** Both places that render the "Model" heading were
  tightened up and doubled in size:
  - the `PillMenu` "Model" row now has explicit geometry (x/y/width) instead
    of relying on Slint's auto-sized Text (which shifted oddly under
    DirectWrite on Windows), and its font goes 13px → 26px;
  - the `MenuFlyout` submenu header (shown when you open the Model or
    Microphone submenu) goes 11px → 22px and now uses a bright white colour
    instead of dim gray, so it reads as a heading instead of a label floating
    in a 32px box with ~10px of dead space above and below.
  - Both headings pin `letter-spacing: 0px` for deterministic spacing.
- **Active/transcribing mini pill colours inverted.** The mic capsule used to
  turn red (`#FF334B`) with a white icon while recording. Now the capsule
  stays pitch black (`#000000`) and the mic icon itself turns red, on all
  three docks (bottom / right edge / left edge). Added a red mic SVG asset
  (`mic_inactive_red.svg`); Slint embeds it into the binary at compile time.

### Changed
- Bump version to 2.0.0-alpha.39.

## [2.0.0-alpha.38] — 2026-11-17

Linux packaging refinement.

### Changed
- The `.deb` no longer hard-depends on `libgtk-3-0` and
  `libayatana-appindicator3-1`. The shipping Slint app links no GTK at all
  (software renderer for icons/cards) and the Linux tray is SNI over
  D-Bus — pulling GTK into every fresh install bought nothing.
  Verified live: the app starts and paints the pill + dashboard correctly
  on the Mint without these packages resolved by this release.

### Changed
- Bump version to 2.0.0-alpha.38.

## [2.0.0-alpha.37] — 2026-11-17

Release-engineering + Linux menu placement.

### Fixed
- **Linux: first right-click on the pill menu lands at the wrong position.**
  The hide/show re-creation destroys the native window; the previously set
  position was lost with it. The menu position is now also re-applied after
  `hide_show_cycle`, so the first click opens the menu flush against the
  pill just like the second and later ones.
- **Linux: menu z-order keeps the menu above the expanded pill.** Menus are
  override-redirect (same true-widget layer as the pill), raised after the
  hide/show cycle — verified with xwininfo stacking output.
- **Right-click menu opens flush to the expanded pill's edges on all three
  docks** (bottom: above, right-to-left, left-to-right). Verified
  placements from a real run for all three docks.

### Changed / packaging
- **Dropped the legacy Qt/C++ build from the Linux release entirely.** The
  Rust/Slint app is the only one exec'd; the Qt `quickstt-app` and C++
  `stt_service` build steps, the CMake dependency in the Linux release, the
  bundled deb `Recommends: libqt6*`, and the tarball payload entries are all
  removed. Removes ~5 MB of dead weight from the Linux package and removes
  the Qt toolchain from the Linux CI for good.
- Bump version to 2.0.0-alpha.37.

## [2.0.0-alpha.36] — 2026-11-16

Right-click menu dock-station placement.

### Changed
- **The mini-pill right-click menu now opens flush against the expanded pill's
  edge — never below it.** Placement follows the pill's dock station: bottom
  dock → menu directly above (bottom edge touching the pill), right dock →
  menu to the pill's left (right edge touching), left dock → menu to the
  pill's right (left edge touching). All three candidates clamp inside the
  work area; if the above-pill slot can't fit the menu height it drops to
  the pill's side instead of half-off-screen. Explicit positions passed by
  callers (tests / synthetic clicks) keep the old cursor-adjacent behaviour.

### Changed
- Bump version to 2.0.0-alpha.36.

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
