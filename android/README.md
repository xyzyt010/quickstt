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
| **On-device transcription** (three engines for arm64-v8a) | ✅ implemented |
| **Clipboard copy** of the finished transcript | ✅ implemented |
| **Floating bubble over other apps** (`TYPE_APPLICATION_OVERLAY`) | ✅ implemented |
| **Interactive bubble** (REC / TYPE / close, draggable) | ✅ implemented (Java helper in a dex) |
| **Foreground service** (the bubble keeps working in other apps) | ✅ implemented (`SpeechService`) |
| **Battery optimisation exemption** (the app is not put to sleep) | ✅ implemented |
| **Model download in-app** (8 models, real byte progress) | ✅ implemented (Android HTTP stack) |
| **Model self-test** ("Test this model" — records 4 s, runs the engine) | ✅ implemented |
| **Typing into the app in front** (real KeyEvents) | ✅ implemented, needs the INJECT_EVENTS permission |
| **Share sheet** for the last transcript | ✅ implemented |
| Setup steps / how-to-use card in the Listen tab | ✅ implemented |
| Activity log + status line (every action says what it did) | ✅ implemented |
| Tab bar with real icons and an active marker | ✅ implemented |
| Always-on wake word (background mic) | ⏳ needs the foreground service to record |

## The engines are real

Three engines are cross-compiled for `arm64-v8a` with the Android NDK and
ship **inside the APK**, in the APK's `lib/arm64-v8a/` directory:

| Engine | Binary in the APK | Models it runs |
|---|---|---|
| whisper.cpp | `libwhisper-cli.so` (2.1 MB) | Whisper Tiny EN |
| Cactus `needle` | `libneedle.so` (1.6 MB) | Cactus Whistle 16M |
| transcribe.cpp | `libtranscribe-cli.so` (3.0 MB) | the GGUF family |

### They were run, not assumed

The desktop builds of the same three engines were run here with **the exact
model files the app downloads** and **the exact command line the app
passes**, on a spoken sample (`jfk.wav`):

| Engine | Command | Result |
|---|---|---|
| whisper.cpp | `-m ggml-tiny.en-q5_1.bin -f jfk.wav -l en --no-timestamps --suppress-nst -nth 0.85 -t 4` | "And so my fellow Americans ask not what your country can do for you. Ask what you can do for your country." (1.7 s) |
| Cactus `needle` | `--model whistle.cact --audio jfk.wav --audio-language en` | the same sentence as JSON (4.1 s) |
| transcribe.cpp | `-m moonshine-base-Q8_0.gguf --language en --threads 4 --backend cpu jfk.wav` | the same sentence (1.2 s) |

The transcribe.cpp output is the metadata block the app skips plus the
`text:` line the app takes — the two match exactly.

Three tests keep it that way (`cargo test`):

* `the_shipped_engines_know_our_options` — every option the app passes
  has to exist in the shipped binary. An option that does not exist makes
  the engine die with "unknown argument" and the transcript stays empty —
  on the phone, with no way to see why.
* `the_shipped_engines_are_android_binaries` — ELF64, ARM64 and the Android
  linker. A Linux build can never be started on a phone.
* `every_model_url_is_downloadable` (needs the network:
  `cargo test -- --ignored`) — every model URL has to deliver a real model
  file: the right size **and** the right header (GGML, GGUF or the Cactus
  container), not an HTML error page.


They are named `lib*.so` and stored compressed on purpose: that is what the
Android installer looks for, and it extracts them into the app's **native
library directory** with the execute permission. That directory is
read-only but exec-mounted — the only place where a modern Android lets an
app run a program. (The app's home directory is mounted `noexec` for apps
that target API 29+, so a binary unpacked there cannot be started at all —
that is why the engines are *not* written out at runtime.)

If the installer did not extract an engine, the app copies it out of the APK
(`ai.quickstt.app.Engine`) and starts it through the system linker
(`/system/bin/linker64`), which is the other way around the W^X rule.

## The models are real

`stt::MODELS` is the single source of truth and it only contains models one of
the bundled engines can really load on a phone:

| Model | Engine | File | Size |
|---|---|---|---|
| Whisper Tiny EN | whisper.cpp | `ggml-tiny.en-q5_1.bin` | 32 MB |
| Cactus Whistle 16M | Cactus needle | `whistle.cact` | 16.9 MB |
| Moonshine Base EN | transcribe.cpp | `moonshine-base-Q8_0.gguf` | 74 MB |
| SenseVoice Small | transcribe.cpp | `SenseVoiceSmall-Q4_K_M.gguf` | 139 MB |
| Canary 180M Flash | transcribe.cpp | `canary-180m-flash-Q5_K_M.gguf` | 151 MB |
| Parakeet Unified EN 0.6B | transcribe.cpp | `parakeet-unified-en-0.6b-Q4_K_M.gguf` | 455 MB |
| Nemotron 3.5 ASR Streaming 0.6B | transcribe.cpp | `nemotron-3.5-asr-streaming-0.6b-Q4_K_M.gguf` | 473 MB |
| Qwen3-ASR 0.6B | transcribe.cpp | `Qwen3-ASR-0.6B-Q4_K_M.gguf` | 562 MB |

The models are the same files the desktop app uses (the same HuggingFace
sources), so one download serves both. The Models tab renders the list with
the real download state — there is no placeholder row and no button that
cannot work. `cargo test -- --ignored` checks that every URL is alive and
delivers the announced size.

## The floating bubble

The bubble is a `WindowManager` overlay (`TYPE_APPLICATION_OVERLAY`), so it
floats above **every** app once the user grants "Display over other apps".

### The permission

The **Enable** button opens the system page for QuickSTT itself
(`Settings.ACTION_MANAGE_OVERLAY_PERMISSION` with a `package:` URI). The
Java helper (`java/ai/quickstt/app/Overlay.java`) falls back to the list of
all apps and then to the app-info page, because several phone makers do not
implement that action the same way — and it returns the reason when no page
could be opened, so the app never pretends that a button worked.

The permission is granted *in another app*, so the app has to notice it
coming back:

* the Java helper registers an `ActivityLifecycleCallbacks` and tells Rust
  when the activity is resumed,
* and the permission is polled once a second as a fallback.

The moment the permission appears, the bubble is shown — without the user
having to press the button a second time. The button itself shows the state
of the bubble (**Enable** / **Disable**), not the state of the permission.

### The bubble itself

It is built by a small Java helper (`java/ai/quickstt/app/Bubble.java`),
because a `View.OnClickListener` cannot be created from JNI. The helper:

* draws a compact card with the mic state and the last transcript,
* has three buttons — **REC** (start/stop a recording), **TYPE** (type the
  last transcript into the app in front) and **X** (hide it),
* can be **dragged** anywhere on the screen (by its text — a touch listener
  on the row would swallow the taps before they reach the buttons),
* uses the *application* context, so it survives the activity that made it,
* reports every failure back to Rust, so the app can say what went wrong
  instead of showing a bubble that is not there.

The helper is looked up through the **Activity's class loader**, not with
`FindClass`: on a thread that was attached with `AttachCurrentThread`,
`FindClass` uses the *system* class loader and the app's own classes are
invisible to it.

The class is compiled with `javac`, turned into a `classes.dex` with `d8` and
added to the APK by `build-apk.ps1` (cargo-apk cannot put a dex into an APK).
The manifest therefore needs `android:hasCode="true"`. When the dex is
missing, the app falls back to a plain read-out bubble — it never breaks.

The bubble is *persistent*: the Settings tab has a switch that keeps it on
across apps and across launches. It is also shown automatically while a
recording runs and when the transcript arrives.

## Testing it on the phone

The app has to be able to prove that it works, otherwise "it looks fine" is
the only thing that can be said about it.

* **Models → Test this model** records four seconds, runs them through the
  selected engine and shows the transcript, the engine binary, the model
  file and the time the engine needed. When it fails, the reason is shown.
* **Settings → Run the self-test** reports the microphone and overlay
  permissions, all three engines and the model files with their paths.
  **Copy** puts the report on the clipboard.

Both write to logcat as well (`adb logcat | grep QuickSTT`).

The engines have to be *runnable* for any of this to work: they live in the
APK's `lib/arm64-v8a/` directory and the installer unpacks them into the
app's native library directory — the only place where Android lets an app
run a program. That needs `android:extractNativeLibs="true"` in the
manifest (it is false by default for apps that target API 23 or newer, and
then the engines stay inside the APK and can never be started). When the
installer did not unpack them, `Engine.java` unpacks one itself.

## Typing into other apps

`platform::inject_text()` types with **real key events**: the characters are
turned into `KeyEvent`s by `KeyCharacterMap.getEvents()` (so capitals and
punctuation come out right) and pushed into the input stack with
`InputManager.injectInputEvent` — the same path a hardware keyboard uses.

The flow in the app:

1. tap the orb (the bubble appears over everything),
2. switch to the app you want to write in,
3. speak — after 1.4 s of silence the recording ends by itself, the text is
   transcribed and **typed into the app in front**.

The text is *always* copied to the clipboard as well, so a phone that refuses
the injection still gives the user the text (long-press → Paste), and the
bubble says which of the two happened.

`INJECT_EVENTS` is a system permission, so Android only allows the injection
for apps that hold it. On a rooted phone or a userdebug build:

```bash
adb shell pm grant ai.quickstt.app android.permission.INJECT_EVENTS
```

Without it the app falls back to the clipboard and says so — the Settings tab
explains this as well.

## Four traps that made every button look dead

All of them were found by reading the platform crates and fixed in
`src/platform.rs` / `src/stt.rs`:

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
3. **The manifest had no `INTERNET` permission.** Without it every socket
   fails, so the model download could never start — the button looked dead
   even though the code was right. It is declared in `Cargo.toml` now, next
   to `ACCESS_NETWORK_STATE`.
4. **The app's home directory is `noexec`.** Since Android 10 the files
   directory of an app that targets API 29+ is mounted `noexec`, so an
   engine binary unpacked there cannot be started ("Permission denied").
   The engines are therefore *shipped* in the APK's `lib/arm64-v8a/`
   directory: the installer extracts them into the app's **native library
   directory**, which is read-only but exec-mounted. (Unpacking them at
   runtime into the files directory would have produced exactly the
   "Permission denied" the transcription kept failing with.)

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
3. **Transcribe** — the engine for the chosen model runs out of the app's
   native library directory (where the installer put it) and is executed
   once per turn, with a 120 s deadline, so a broken model cannot freeze the
   app. A silent take is rejected *before* the engine runs (whisper
   hallucinates short words on noise-only audio).
4. **Model** — never bundled. The Models tab lists the models the phone can
   run and downloads the chosen one from the same HuggingFace source the
   desktop app uses. The transfer is done by the **Android HTTP stack**
   (`ai.quickstt.app.ModelDownloader`, `HttpURLConnection`): the number of
   bytes that really arrived is reported into the app, an interrupted
   download continues where it stopped (a `Range` request), and only a
   **complete** file is moved into place — a truncated model is worse than
   no model at all, because the engine would fail on it. The choice is
   remembered between launches.
5. **Result** — the transcript is shown in the app, pushed into History and
   copied to the system clipboard, so it can be pasted into any app. If
   another app is in front, the text is also *typed* into it (see above).
6. **Floating bubble** — the overlay described above.
7. **Background** — the bubble is started together with a **foreground
   service** (`ai.quickstt.app.SpeechService`, type `microphone`): it keeps
   the process alive while the user works in another app and shows a small
   notification that says what it is for. The service needs the app to be
   exempt from the battery restrictions, which the Settings tab offers
   (`Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS`, the app's own
   page first and the list of all apps as the fallback — the same flow the
   reference app uses).

## The manifest

cargo-apk generates a manifest that can only describe an activity, and
QuickSTT needs a **service** (the foreground service above). So the APK is
finished by hand:

- `AndroidManifest.xml` in this folder is the real manifest — the
  permissions, the activity with `android.app.lib_name`, and the service
  with its type. It is compiled with `aapt2 link` and the result replaces
  the manifest in the APK.
- `build-apk.ps1` (and the CI) do that: javac → d8 → `aapt2 link` → the dex,
  the manifest and the engines go into the APK → zipalign → apksigner.

The manifest is also where `android:extractNativeLibs="true"` lives: it is
false by default for apps that target API 23 or newer, and then the speech
engines stay inside the APK where they can never be started.

## Build

Toolchain used and verified on this machine:

- JDK 21 (`JAVA_HOME`)
- Android SDK + NDK **27.3.13750724**
- Rust targets: `aarch64-linux-android`, `armv7-linux-androideabi`, `x86_64-linux-android`
- `cargo-apk 0.10`

### The release APK

`build-apk.ps1` does the whole thing — cargo-apk cannot add a dex to the
APK, so the APK is finished by hand:

```powershell
cd android
.\build-apk.ps1            # release APK, signed with quickstt-release.jks
.\build-apk.ps1 -Debug     # debug build, unsigned
```

The steps are: `javac` → `d8` → `cargo apk build` → add `classes.dex` →
`zipalign -p 4` → `apksigner sign`.

`ANDROID_HOME` must point at the **SDK root** (not `platform-tools`); the
script finds the SDK itself and only uses the environment variable when it
really is an SDK.

`--lib` is required: the GUI is the cdylib's `android_main`, and cargo-apk
otherwise also tries to package the desktop `examples/preview.rs`.

Install on a device (USB debugging):

```powershell
adb install -r target\release\apk\quickstt-android.apk
```

The manifest is generated from `[package.metadata.android]` in `Cargo.toml` —
cargo-apk 0.10 ignores a hand-written `AndroidManifest.xml`. All eleven
permissions (internet, mic, foreground service, overlay, notifications, wake
lock, vibrate, input injection) are declared there and verified with
`aapt2 dump badging`.

Preview the exact same UI on Windows/Linux without a device:

```powershell
cd android
cargo run --example preview
```

### Rebuilding the embedded engines

The APK ships three binaries in `prebuilt/`. All of them are cross-compiled
with the NDK toolchain file, with `GGML_NATIVE=OFF` (a native build crashes
with SIGILL on many phones) and stripped.

```powershell
$ndk = "$env:LOCALAPPDATA\Android\Sdk\ndk\27.3.13750724"
$toolchain = "$ndk/build/cmake/android.toolchain.cmake"

# whisper.cpp (the Whisper models)
cmake -S $env:TEMP\whisper.cpp -B $env:TEMP\whisper-android -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE=$toolchain `
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release `
  -DWHISPER_BUILD_TESTS=OFF -DWHISPER_BUILD_EXAMPLES=ON -DBUILD_SHARED_LIBS=OFF `
  -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF
cmake --build $env:TEMP\whisper-android --target whisper-cli

# transcribe.cpp (the GGUF family) — the source lives in third_party/
cmake -S ..\third_party\transcribe.cpp -B $env:TEMP\tc-android -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE=$toolchain `
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DCMAKE_BUILD_TYPE=Release `
  -DGGML_NATIVE=OFF -DTRANSCRIBE_USE_OPENMP=OFF -DTRANSCRIBE_BUILD_TESTS=OFF `
  -DTRANSCRIBE_BUILD_TOOLS=OFF -DTRANSCRIBE_VULKAN=OFF -DGGML_CURL=OFF
cmake --build $env:TEMP\tc-android --target transcribe-cli

# strip and copy
$strip = "$ndk/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe"
& $strip --strip-all "$env:TEMP\whisper-android\bin\whisper-cli"
& $strip --strip-all "$env:TEMP\tc-android\bin\transcribe-cli"
```

The Cactus engine is downloaded from the Cactus Compute release
(`Cactus-Compute/needle3`, `android-arm64/needle`).

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
- A layout whose children all have `vertical-stretch: 0` **shares the leftover
  space equally between them** — a short page would blow its cards up to
  full height. Every scrolling page therefore ends with a
  `Rectangle { vertical-stretch: 1; }` sponge, and the page itself uses
  `alignment: start` so the content starts at the top.
- Long texts wrap or elide — a long text must never widen the layout.


## Next steps (in order)

1. **Always-on wake word**: a Java foreground service that keeps the mic open
   in the background. cargo-apk can only declare one activity, so the service
   has to be added to the manifest by hand (the dex pipeline already makes
   that possible).
2. **More whisper models**: the engine is model-agnostic — adding Whisper Base
   or the multilingual Whisper models only needs a catalog entry.
3. **Offload**: mirror the desktop idle-unload (`auto_offload`, 60 s) by
   dropping the engine session when idle.
