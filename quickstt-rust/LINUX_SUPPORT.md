# QuickSTT on Linux — what works and what doesn't

Verified against the engine probes in `quickstt-core/src/models/engine.rs`, the
downloaders in `models/downloader.rs`, and the Desktop Release CI pipeline
(`.github/workflows/release-desktop.yml`).

## Engine support matrix

| Engine | Models using it | Linux runtime | How it gets there | Status |
|---|---|---|---|---|
| `whisper.cpp` | Whisper Tiny EN, Whisper Large v3 Turbo | `whisper-cli` | built from source by CI | ✅ works |
| `parakeet-rust` | Parakeet TDT 0.6B v3 | `parakeet_engine` | built from source by CI | ✅ works |
| `phonon2` | Phonon-2 0.6B | same `parakeet_engine` worker | CI | ✅ works |
| `sensevoice` | SenseVoice Small | same `parakeet_engine` worker | CI | ✅ works |
| `nemotron` | Nemotron 3.5 ASR Streaming | `transcribe-cli`/`nemotron_engine` (transcribe.cpp) | built from source by CI | ✅ works |
| `canary` | Canary 180M Flash | the same transcribe.cpp worker | CI | ✅ works |
| `qwen3-asr` | Qwen3-ASR 0.6B | the same transcribe.cpp worker | CI | ✅ works |
| `vosk` | Vosk Small EN (50M) | `libvosk.so` | downloaded on demand | ✅ works |
| `cactus-whistle` | Cactus Whistle 16M | `needle` CLI | per-OS release download (linux-x86_64 / linux-arm64) | ✅ works |
| `photon` | Parakeet Ultra / Redux | `photon_worker.py` + **Python 3 + `pip install moondream`** | script ships, deps are the user's | ⚠️ needs Python |
| `canary-qwen` | Canary-Qwen 2.5B | `canary_worker.py` + **Python 3 + CUDA torch + nemo_toolkit**, NVIDIA GPU (8 GB+) | script ships, deps are the user's | ⚠️ needs GPU + Python |
| `sherpa-onnx` | Moonshine Medium Streaming (245M) | `sherpa-onnx-offline` | **nothing builds or downloads it for Linux** | ❌ Windows only |

### The one real gap: sherpa-onnx (Moonshine)

`SttEngineConfig::detect()` probes `tools/sherpa_onnx/bin/sherpa-onnx-offline`
(no extension on Linux) and `models/runtimes/sherpa_onnx/...`, but:

* the Windows tree ships the binary in `tools/`;
* the CI Linux job builds whisper.cpp, transcribe.cpp and parakeet_engine only;
* no downloader fetches a sherpa-onnx runtime — `install_moonshine()` grabs the
  ONNX **model** tarball, not the CLI.

So on Linux Moonshine would install and then never transcribe. The catalog now
marks `EngineFamily::SherpaOnnx` as Linux-unsupported
(`unsupported_on_linux()` / `supported_on_this_os()`), the orchestrator filters
those models out of the list on Linux (`AppState::platform_hidden_models`), and
the dashboard's Speech Models info line states what was hidden and why. On
Windows nothing changes — every engine is available.

**To bring Moonshine to Linux** (not done here, needs a build decision): add a
sherpa-onnx build step to the Linux CI job and ship the binary in
`tools/sherpa_onnx/bin/`, or extend `install_moonshine()` to pull a
`sherpa-onnx-*-linux-x86_64` release tarball. The model tarball already comes
from the k2-fsa GitHub releases, which also publish prebuilt CLI binaries.

## Text injection (typing into text boxes)

`deliver_transcription_output()` is one code path for every engine; the OS half
is `main.rs`:

* **Windows** — Win32 clipboard write, then `restore_typing_focus()` puts
  `hwndFocus` back (captured at turn end via `GetGUIThreadInfo`) and injects
  Ctrl+V with `SendInput`. If focus cannot be restored the keystroke is skipped
  and the text stays on the clipboard.
* **Linux / X11** — `xclip` (then `xsel`) writes the clipboard, `xdotool
  windowactivate` restores the captured window (`xdotool getwindowfocus`, with
  our own windows filtered out so dictation is never typed back into the pill),
  then `xdotool key ctrl+v`.
* **Linux / Wayland** — `wl-copy` writes the clipboard, `wtype -M ctrl -k v`
  types the paste into whatever holds focus (no global focus query exists on
  Wayland; the hands-free flow keeps focus where it is).

Required tools: `xclip`/`xsel`/`xdotool` on X11, `wl-clipboard` + `wtype` on
Wayland. `scripts/install.sh` installs them; the deb declares them as deps.

Everything else — the pill, TextBoard, dashboard, History, onboarding, tray
(StatusNotifierItem via `ksni`), global hotkeys (X11), model picker, language
forcing, idle offload — is the same code on both platforms; only the OS halves
above are `#[cfg]`'d.
