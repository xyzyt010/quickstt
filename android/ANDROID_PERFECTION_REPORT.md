# QuickSTT Android — Perfection Report

Scope of this pass: `android/` only. Desktop (`quickstt-rust/**`) untouched,
no version bump, nothing committed. Windows GUI was never launched or
interacted with; all verification is build/test/badging level (no device
was attached — `adb devices` is empty — so no on-device run is claimed).

## 1. Research conclusions (encoded in code comments)

**Android cannot commit text into another app's focused field unless
QuickSTT is the active IME.** The `InputConnection` for an editor is bound
by the system only to the currently selected input method. The alternatives
are all rejected, and are now absent from the APK (proven by badging, §5):

- **Accessibility** (`BIND_ACCESSIBILITY_SERVICE`): Play Protect Enhanced
  Fraud Protection blocks sideloaded APKs declaring it; also a Play policy
  restricted permission. **Never added.**
- **`INJECT_EVENTS`**: signature permission — impossible without
  root/userdebug/signature. **Removed** from `Cargo.toml` and from Rust
  (`platform.rs:62` — `inject_text` now only delegates to the IME commit
  and otherwise errors; callers fall back to clipboard + an explicit
  "tap the field and paste" status, never fake-typed).
- **Overlay**: can draw/touch above apps but cannot commit text. The pill
  therefore only *routes*: result → active IME if bound, else clipboard.
- **IME is the sanctioned path**: `QuickSttInputMethodService`
  (`BIND_INPUT_METHOD`, the API Gboard voice typing uses).

IME rules implemented (`QuickSttInputMethodService.java`):
- Voice bar appears **only** in the keyboard area while an editable
  `EditorInfo` is bound: `onEvaluateInputViewShown` now returns
  `isEditableEditor(currentEditor)` (strictly `inputType & TYPE_MASK_CLASS
  != TYPE_NULL`); the previous field-name/hint fallback that treated
  `TYPE_NULL` editors as typable was removed.
- Password / no-save editors (text/number password variations, web
  password, incognito `IME_FLAG_NO_PERSONALIZED_LEARNING`): commit via
  `commitText` is allowed, but the turn is flagged sensitive end-to-end —
  **never** written to history, clipboard, last-text or logs
  (`isSensitiveEditor` / `isCurrentFieldSensitive`, consumed in
  `lib.rs` delivery path).
- Live partials use `setComposingText`, final uses `commitText`,
  backspace uses `deleteSurroundingText`/selection-clear, the new **↵**
  button runs `performEditorAction` (or sends Enter), and **⌨ Keyboard**
  uses `switchToPreviousInputMethod()` with the system picker as fallback.

## 2. Bugs found & fixed

| # | Bug | Fix |
|---|---|---|
| 1 | History was memory-only (`VecModel`), lost on force-stop; Clear deleted nothing on disk | `src/store.rs:66` versioned JSONL store (`history.jsonl`, v1 field per record), atomic tmp+rename save, cap 200 with evicted-audio cleanup, per-turn delete + clear delete records **and** retained WAVs (`history_audio/`). Turns record text, ts, duration, model, audio path. Loaded at startup in `build_ui`. |
| 2 | History toggle didn't exist; sensitive fields would have been saved/copied | `history_enabled` setting (Settings toggle); sensitive turns commit-only (see §1). |
| 3 | Auto-offload was a hard-coded, unpersisted 30 s in three places | Persisted `auto_offload` + `offload_idle_secs` (5–3600, picker 15/30/60/120/300/900) in `settings.json`; both the 1 s UI watchdog and the background watchdog use `check_idle_offload(offload_idle_secs())`. Cancelling happens naturally: recording/transcribing state gates the check and `last_used` is touched on use. Manual Onload/Offload already reflected the real worker; RAM status text unchanged and accurate (worker/pinned bytes + load ms + idle s). |
| 4 | Settings scattered/unpersisted (`selected_model`, pill and onboarding flag files only) | `src/store.rs` `Settings` (auto-offload, idle secs, live mode, history, pill, haptics flag, selected model, language, API provider/model, onboarding) loaded on start, saved atomically on every change and reapplied to the live atomics. Legacy files still written where Java reads them (`pill_on_focus_v2`, `selected_model`). |
| 5 | No cloud API at all | Groq **and** OpenAI (OpenAI-compatible multipart) **and** Deepgram (raw WAV POST) end-to-end via `java/ai/quickstt/app/ApiClient.java`: keys encrypted with an AndroidKeyStore AES/GCM key, only ciphertext in prefs, never logged/returned to Rust; HTTPS via `HttpURLConnection`, 15 s connect / 120 s read, HTTP errors surfaced with body snippet, offline failure is a readable status. Rust catalog+rules in `src/api.rs:52`: API model selectable **only** with a stored key, local **only** when downloaded (desktop model-menu rule), enforced in the Listen dropdown (`refresh_model_options`, `lib.rs:107`) and in `pick-api-model`. Connect/Disconnect + model dropdown in Models → Cloud API; key field is a password `TextInput`. |
| 6 | Downloads lacked header validation and free-space precheck | `stt.rs:689` `header_is_model` + post-download validation (HTML error pages / stubs are deleted with a reason); storage precheck (`StatFs`, needs model size + 32 MB) before a download starts. Existing strengths kept: Range resume, Content-Range totals, one-at-a-time guard, per-model % + MB/s (`download-info` now actually rendered, see #10), delete frees space, bundled restore. |
| 7 | IME voice bar: **any tap** on the bar (even the status label) started recording (`recBtnRect.contains(x,y) || (!liveBusy || liveRecording)`) | Only the record button toggles recording; ⌫ and the new ↵ have their own hit rects. |
| 8 | IME bar START button was blue `#2563EB`; Bubble ✓ chip was blue | Desktop monochrome: white chip/button with black glyph/text; recording keeps only a red border/state colour. |
| 9 | `INJECT_EVENTS` declared in `Cargo.toml`, injection implementation in Rust | Removed both (see §1). Final badging proves absence (§5). Misleading `accessibility_enabled`/`openAccessibilitySettings` names (they always meant the IME) renamed to `ime_ready`/`openImeSettings` across Rust+Java. |
| 10 | `status-text`, `download-info`, `mic-health` were set by Rust but **never rendered** — actions appeared silent | One-line status on Listen (mic-health overrides while a mic fault lasts), download line shows `MB · MB/s · %`, model cards show `ram-status`. |
| 11 | SpeechService kept running after pill disabled | Disabling the pill now calls `stop_service()` (service `onDestroy` hides the bubble); service still starts only with pill+permission, never at launch. |
| 12 | Model cards: duplicate active markers (selected fill + badge + "Selected"), no explicit downloaded state | Active state exists **only** as the right-edge **Use/Active** button; name row carries `Downloaded` / `Not downloaded`; size/langs in the detail line; selected-card highlight removed. |
| 13 | Desktop-only engines implied as shippable | Catalog contains only engines actually bundled (`needle`, `whisper-cli`, `transcribe-cli`) — unchanged, verified by the engine tests; no fake rows/buttons added. Python-worker desktop models are not listed on Android at all. |

## 3. UI (desktop theme, mobile-adapted)

- Palette mapped to desktop: background `#000`, cards `#0E0E0E`, raised
  `#141414`, border `#262626`, secondary `#9A9A9A`, primary = white on
  black. Zero blue remains in `android.slint` / Java (grep-verified).
  Card/button radius 10 (desktop 8–10), bottom nav kept as the mobile
  adaptation, all main targets ≥48dp.
- New reusable `ChevronDropdown` (`android.slint:253`): chevron rotates
  0°→180° with `animate transform-rotation { 160ms; ease-out }`; used for
  the Listen model picker, Settings language, Settings offload-seconds.
- Models: Download button + progress bar (`%` + bytes/speed) for
  not-downloaded; Use/Active/Test/Delete at 48dp for downloaded.
- Onboarding keyboard page states the real rule: the voice bar shows only
  when a text field is tapped **and** QuickSTT is the selected keyboard;
  overlay is optional and never shown at launch; no accessibility claims
  anywhere in UI text.
- Bubble keeps the desktop pill look (dark, waveform, ✕/✓, spinner on
  load/offload, draggable, only when enabled + overlay permission).

## 4. Architecture (typing path)

```
text field tapped in any app
  → system binds EditorInfo to the *selected* IME
  → QuickSTT IME selected: voice bar in keyboard area (START/STOP/⌫/↵/⌨)
    overlay pill (optional) can also start a turn
  → AAudio 16kHz → conditioner (HP + soft-limit + clip/dead gates)
  → local engine (persistent transcribe worker / pinned weights)
    or cloud API (Keystore key, Java HTTPS, 16k WAV)
  → live: setComposingText · final: commitText (same InputConnection)
  → not the active IME: clipboard + "tap the field and paste" status
password/no-save field: commit only — no history, clipboard or logs
```

## 5. Verification evidence

- `cargo test` (host): **18 passed, 1 ignored** (network URL test),
  including new suites: history JSON round-trip/cap/reload/delete+clear
  audio return, settings round-trip + idle clamp, API selectability
  rules, download-header rejection, plus `tests/ui_wiring.rs` **1 passed**
  (every Slint callback has a Rust handler; tap + nav reach Rust).
- `cargo build --example preview`: compiles (not launched — Windows GUI
  is the user's working machine).
- Java: `javac --release 8` clean inside `build-apk.ps1`.
- **APK built**: `.\build-apk.ps1` (JDK 21, NDK 27.3.13750724) →
  signed release `android/target/release/apk/quickstt-android.apk`
  (26,030,939 bytes at first final build; rebuilt after the last edit).
- `aapt2 dump badging` permissions — exactly: RECORD_AUDIO, INTERNET,
  ACCESS_NETWORK_STATE, SYSTEM_ALERT_WINDOW, FOREGROUND_SERVICE,
  FOREGROUND_SERVICE_MICROPHONE, FOREGROUND_SERVICE_SPECIAL_USE,
  POST_NOTIFICATIONS, WAKE_LOCK, VIBRATE,
  REQUEST_IGNORE_BATTERY_OPTIMIZATIONS. **No** INJECT_EVENTS, **no**
  SMS, **no** BIND_ACCESSIBILITY_SERVICE, **no**
  BIND_NOTIFICATION_LISTENER_SERVICE. Manifest services: SpeechService
  (FGS) + QuickSttInputMethodService (`BIND_INPUT_METHOD` only).
- No device attached (`adb devices` empty): no install/interaction
  claimed. Engines/models were previously verified on-device-class
  binaries by the shipped ELF/option tests (unchanged, still passing).

## 6. Blockers / not verified

- No physical device or emulator session was available to this pass, so
  live IME commit, Keystore encryption and API calls are compile- and
  logic-verified only, not exercised against a real Groq/OpenAI/Deepgram
  account (no keys on this machine).
- `selected_model` is persisted both by the legacy `selected_model` file
  (read by `stt::selected()`) and `settings.json`; they are written
  together on every pick, but the legacy file remains the startup source.
- History timestamps render as relative labels ("5m ago") recomputed at
  load; they do not tick live while the History tab stays open.

## 7. How to enable the keyboard (user steps)

1. Open QuickSTT once (bundled model works offline immediately).
2. Settings → Keyboard → **Enable QuickSTT Voice Auto-Type** → turn it on
   in Android's *On-screen keyboards* page, return (the picker opens).
3. Tap any text field, open the keyboard switcher and pick **QuickSTT
   Voice Auto-Type**. The voice bar now replaces the keyboard **only**
   while that field is focused: ● START speaks (live text composes in the
   field), ■ STOP commits, ⌫ deletes, ↵ runs search/send, ⌨ switches back.
4. Optional: Settings → *Pill on text fields* + overlay permission for the
   floating pill (for starting turns while another keyboard is selected —
   results then go to the clipboard with a paste instruction).
5. Optional cloud: Models → Cloud API → paste a Groq/OpenAI/Deepgram key
   → Connect. API models then appear in the Listen model picker.
