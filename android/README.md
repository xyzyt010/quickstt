# QuickSTT for Android

Touch-first port of QuickSTT. The dashboard is a **new** Slint scene
(`ui/android.slint`) designed for phones — bottom navigation, a thumb-reachable
record orb, 48 DIP minimum targets — not a shrunken copy of the desktop window.
It shares the desktop palette and the same model engines.

## Status

| Piece | State |
|---|---|
| Rust + Slint native Android app (`android_main`) | ✅ builds / installs |
| Touch-first dashboard (Listen, History, Models, Settings) | ✅ implemented |
| Desktop preview of the Android scene | ✅ `cargo run --bin quickstt-android-preview` |
| Audio capture + transcription wiring | ⏳ next (port `quickstt-core` engine calls) |
| Always-on floating pill (overlay + foreground service) | ⏳ next (Kotlin service + `WindowManager`) |
| SenseVoice / Phonon-2 / Parakeet ONNX on device | ⏳ needs the ONNX Runtime AAR for each ABI |

## Build

Toolchain used and verified on this machine:

- JDK 21 (`JAVA_HOME`)
- Android SDK + NDK **27.3.13750724**
- Rust targets: `aarch64-linux-android`, `armv7-linux-androideabi`, `x86_64-linux-android`
- `cargo-apk 0.10`

`ANDROID_HOME` must point at the **SDK root** (not `platform-tools`):

```powershell
$env:ANDROID_HOME     = "$env:LOCALAPPDATA\Android\Sdk"
$env:ANDROID_NDK_ROOT = "$env:LOCALAPPDATA\Android\Sdk\ndk\27.3.13750724"
cd android
cargo apk build -t aarch64-linux-android
# → target/release/apk/quickstt-android.apk
```

Install on a device (USB debugging):

```powershell
adb install -r target\release\apk\quickstt-android.apk
```

Preview the exact same UI on Windows/Linux without a device:

```powershell
cd android
cargo run --bin quickstt-android-preview
```

## Why the UI differs from the desktop

- **Bottom navigation** instead of a collapsible sidebar: one-handed reach.
- **Record orb in the lower third**: the thumb rests there; the top of a phone
  is out of reach.
- **No dense tables**: model rows are 86 DIP cards, settings rows 62 DIP.
- **No hover/pressed micro-states**: touch has no hover, so state is carried by
  colour, fill and the breathing ring.

## Next steps (in order)

1. **Runtime**: move `quickstt-core`'s engine dispatch behind a small Android
   facade; request `RECORD_AUDIO` at runtime; stream 16 kHz mono into the
   selected engine.
2. **ONNX Runtime for Android**: add `onnxruntime-android` (AAR) via Gradle, or
   link the prebuilt `libonnxruntime.so` per ABI; point `ORT_DYLIB_PATH` at it.
   SenseVoice and Parakeet/Phonon share this.
3. **Model downloads**: reuse `quickstt-core::models::downloader` (it already
   handles SenseVoice, Phonon-2 and Parakeet with pinned revisions); store under
   the app's private `files/models/` (no external storage permission needed).
4. **Floating pill**: a `ForegroundService` (type `microphone`) plus
   `WindowManager.addView` with `TYPE_APPLICATION_OVERLAY`, gated on
   `Settings.canDrawOverlays()`. Kotlin lives in a Gradle module; the Rust
   cdylib stays untouched. The pill expands/collapses like the desktop one.
5. **Offload**: mirror the desktop idle-unload (`auto_offload`, 60 s) by
   dropping the engine session from the service when idle.
