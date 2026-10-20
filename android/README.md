# QuickSTT for Android

Touch-first port of QuickSTT. The dashboard is a **new** Slint scene
(`ui/android.slint`) designed for phones — bottom navigation, a thumb-reachable
record orb, 48 DIP minimum targets — not a shrunken copy of the desktop window.

## Status

| Piece | State |
|---|---|
| Rust + Slint native Android app (`android_main`) | ✅ builds / installs |
| Touch-first dashboard (Listen, History, Models, Settings) | ✅ implemented |
| Desktop preview of the Android scene | ✅ `cargo run --example preview` |
| **Microphone capture** (NDK AAudio, 16 kHz mono) | ✅ implemented |
| **Input conditioning** (DC removal, soft limiting, clip/dead watch) | ✅ implemented |
| **Microphone self-test** ("Test mic" in the setup card) | ✅ implemented |
| **Runtime permission flow** (system dialog + result pickup) | ✅ implemented |
| **On-device transcription** (whisper.cpp for arm64-v8a) | ✅ implemented |
| **Model download in-app** (Whisper Tiny EN, 32 MB) | ✅ implemented |
| **Clipboard copy** of the finished transcript | ✅ implemented |
| **Floating bubble over other apps** (WindowManager overlay) | ✅ implemented (read-out bubble) |
| Setup steps / how-to-use card in the Listen tab | ✅ implemented |
| Activity log + status line (every action says what it did) | ✅ implemented |
| Tap-to-record on the floating bubble | ⏳ needs a Java service (cargo-apk cannot declare one) |
| Always-on wake word (background mic) | ⏳ needs a Java foreground service |
| SenseVoice / Phonon-2 / Parakeet ONNX on device | ⏳ needs the ONNX Runtime AAR for each ABI |

## Two traps that made every button look dead

Both were found by reading the platform crates and fixed in `src/platform.rs`:

1. **`ndk-context` hands out the `Application`, not the `Activity`.**
   `android-activity` 0.6 initialises the ndk context with
   `Application.getApplication()`. An `Application` has **no**
   `requestPermissions()`, **no** `startActivity()` and **no** `window` system
   service — calling them raises a Java `NoSuchMethodError` that the JNI layer
   swallows, so the button did nothing at all. The Activity now comes from
   `AndroidApp::activity_as_ptr()`.
2. **`requestPermissions()` and `startActivity()` must run on the UI thread.**
   The Slint event loop runs on android-activity's own `android_main` thread,
   not the Java UI thread. Every such call is dispatched with
   `AndroidApp::run_on_java_main_thread()`.

The app also polls `checkSelfPermission` while a dialog is open, so the setup
card advances to "Test mic" as soon as the user grants the permission — and
the overlay permission is picked up when the user comes back from the system
settings.


## How it works

1. **Capture** — `src/aaudio.c` wraps the NDK's AAudio recording API (cpal has
   no Android backend). It records 16 kHz mono float, which is exactly what
   whisper.cpp wants, so no resampling is needed. `src/audio.rs` is the Rust
   side (start / read / stop + level metering + WAV writing).
2. **Conditioning** — `src/conditioner.rs` (the same algorithm as the
   desktop) removes the DC offset with a 20 Hz high-pass, soft-limits the
   signal and watches the raw rail fraction. A clipped microphone is
   reported ("Input clipped — lower the mic gain") and a silent one ("No
   signal from the microphone") instead of producing a bogus transcript.
   The **"Test mic"** button in the setup card opens the mic for a second
   and reports the level, so the user can check the input without
   recording.
3. **Transcribe** — whisper.cpp is cross-compiled for `arm64-v8a` with the
   Android NDK (`cmake` + the NDK toolchain file, portable x86-64 baseline so
   it never SIGILLs), stripped and shipped inside the APK
   (`prebuilt/whisper-cli-arm64-v8a`). At first run it is written to the app's
   private files directory and executed once per turn. A silent take is
   rejected *before* the engine runs (whisper hallucinates short words on
   noise-only audio).
4. **Model** — never bundled. The Listen tab's step 2 downloads
   `ggml-tiny.en-q5_1.bin` (32 MB) from the same HuggingFace source the
   desktop app uses, with a progress bar.
5. **Result** — the transcript is shown in the app, pushed into History and
   copied to the system clipboard, so it can be pasted into any app.
6. **Floating bubble** — a `WindowManager` overlay (`TYPE_APPLICATION_OVERLAY`)
   shows the mic state and the last transcript above other apps. The user
   grants "Display over other apps" in step 3 of the setup card.

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
cargo apk build --target aarch64-linux-android --lib
# → target\debug\apk\quickstt-android.apk (debug-signed)
```

`--lib` is required: the GUI is the cdylib's `android_main`, and cargo-apk
otherwise also tries to package the desktop `examples/preview.rs`.

Install on a device (USB debugging):

```powershell
adb install -r target\debug\apk\quickstt-android.apk
```

A **release** APK additionally needs a keystore (`cargo apk` refuses to sign
release builds with the debug key):

```toml
[package.metadata.android.signing.release]
path = "quickstt-release.jks"
keystore_password = "…"
```

The manifest is generated from `[package.metadata.android]` in `Cargo.toml` —
cargo-apk 0.10 ignores a hand-written `AndroidManifest.xml`. All eight
permissions (mic, foreground service, overlay, notifications, wake lock,
vibrate) are declared there and verified with `aapt dump permissions`.

Preview the exact same UI on Windows/Linux without a device:

```powershell
cd android
cargo run --example preview
```

### Rebuilding the embedded engine

The APK ships `prebuilt/whisper-cli-arm64-v8a`. To rebuild it from a fresh
whisper.cpp checkout (portable baseline, no `-march=native` — a native build
crashes with SIGILL on many phones):

```powershell
$ndk = "$env:LOCALAPPDATA\Android\Sdk\ndk\27.3.13750724"
git clone --depth 1 https://github.com/ggerganov/whisper.cpp $env:TEMP\whisper.cpp
cmake -S $env:TEMP\whisper.cpp -B $env:TEMP\whisper-android -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE="$($ndk -replace '\\','/')/build/cmake/android.toolchain.cmake" `
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release `
  -DWHISPER_BUILD_TESTS=OFF -DWHISPER_BUILD_EXAMPLES=ON -DBUILD_SHARED_LIBS=OFF `
  -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF
cmake --build $env:TEMP\whisper-android --target whisper-cli
& "$ndk\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe" --strip-all "$env:TEMP\whisper-android\bin\whisper-cli"
Copy-Item "$env:TEMP\whisper-android\bin\whisper-cli" android\prebuilt\whisper-cli-arm64-v8a -Force
```

## Why the UI differs from the desktop

- **Bottom navigation** instead of a collapsible sidebar: one-handed reach.
- **Record orb in the lower third**: the thumb rests there; the top of a phone
  is out of reach.
- **No dense tables**: model rows are 86 DIP cards, settings rows 62 DIP.
- **No hover/pressed micro-states**: touch has no hover, so state is carried by
  colour, fill and the breathing ring.

### Slint layout rules the scene follows

Getting these wrong is what made the first phone build look broken (words in
the wrong place, cards hanging off the screen):

- A layout inside a plain `Rectangle` does **not** stretch to the rectangle —
  it keeps its own size and floats in the middle. Nested layouts bind
  `width: parent.width` (or `100%` inside a `ScrollView`).
- A row inside a **padded** layout must **not** bind its width: the layout
  already hands out the padded width, and an explicit width overrides the
  padding and overflows the card.
- `Text` is centred inside its slot by default → every piece of body text sets
  `horizontal-alignment: left`.
- A layout's `alignment` is the **main axis**: for a `HorizontalLayout` that
  is horizontal, so `alignment: center` centres the row. Use
  `cross-axis-alignment: center` to centre the items vertically.
- Long texts wrap or elide — a long text must never widen the layout.


## Next steps (in order)

1. **Tap-to-record on the floating bubble.** `cargo-apk` cannot declare a
   `Service` (it only generates a single activity), so the bubble is currently
   a read-out. Making it interactive needs a small Kotlin/Java service (or a
   Gradle build instead of cargo-apk) that owns a `ForegroundService` and
   forwards taps into the Rust cdylib.
2. **ONNX Runtime for Android**: add `onnxruntime-android` (AAR) via Gradle, or
   link the prebuilt `libonnxruntime.so` per ABI; point `ORT_DYLIB_PATH` at it.
   SenseVoice and Parakeet/Phonon share this.
3. **More models**: the engine layer is model-agnostic — adding the Qwen3-ASR
   GGUF (via the Handy transcribe.cpp worker built for arm64) only needs a
   second embedded binary and a catalog entry.
4. **Offload**: mirror the desktop idle-unload (`auto_offload`, 60 s) by
   dropping the engine session when idle.
