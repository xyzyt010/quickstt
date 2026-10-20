# Models — Vosk, Parakeet, Phonon-2, SenseVoice, Nemotron, Qwen3-ASR (offline, local)

QuickSTT is **100% offline** by default. Every model runs on-device via `quickstt-core/src/models/catalog.rs:15` `EngineFamily` and `models/engine.rs` dispatch.

## Catalog

| Model | EngineFamily | Display | Path Linux Mint (`~/.local/share/QuickSTT/models/…`) | Path Windows (`%APPDATA%\QuickSTT\models\…`) | Size | Best for |
|---|---|---|---|---|---|
| **Vosk small EN 0.15** | `Vosk` (`libvosk.so` / `libvosk.dll` via `vosk_api.h` `dlopen`) | `vosk/small_en_us_0.15/` | same | same | 50 MB | Fastest CPU, dictation |
| **Parakeet TDT 0.6B v3 INT8** | `ParakeetRust` (`transcribe-rs 0.3.3 onnx` `ort` `libonnxruntime.so`) | `nemo/tdt_0_6b_v3_int8/` | same | same | 640 MB | Highest accuracy offline |
| **Phonon-2 0.6B INT8** | `Phonon2` (same persistent `parakeet_engine` worker as Parakeet) | `phonon2/phonon-2-0.6b-int8/` | same | same | 688 MB | Best compact English accuracy (Fermion Research) |
| **SenseVoice Small Multilingual INT8** | `SenseVoice` (ONNX Runtime CTC, CPU) | `sensevoice/small_multilingual/` | same | same | 229 MB | Fast multilingual (zh/en/ja/ko/yue) with auto language detect |
| **Nemotron 3.5 Streaming 0.6B Q4_K_M** | `Nemotron` (`tools/nemotron/nemotron_engine` `transcribe.cpp` Handy GGML) | `nemotron/nemotron-3.5-asr-streaming-0.6b/` | same | same | 473 MB | Streaming + VAD, low latency |
| **Qwen3-ASR 0.6B Q4_K_M** | `Qwen3Asr` (same `transcribe.cpp` worker, offline) | `qwen3_asr/qwen3-0.6b-q4km/` | same | same | 562 MB | 30-language offline ASR with auto language detect |
| **Cactus Whistle 16M** | `Whistle` (Cactus `needle` CLI, `.cact`) | `whistle/whistle-16m/` | same | same | 17 MB | Tiny 7-language CPU ASR, one-shot |

Also in catalog (Whisper/Moonshine for fallback): `whisper_cpp/tiny_en_q5` 32M, `base_en_q8` 74M, `small_en_q8` 264M, `large_v3_turbo_q8` 1500M, `moonshine_v2/*`.

The pill dropdown shows `● Installed • 190MB` vs `○ Not Installed` via `catalog::display_name` + `is_model_installed()` checking `Path::exists`.

## Paths — XDG vs Registry

- Linux Mint: `catalog::models_root()` → `dirs::data_dir()` → `~/.local/share/QuickSTT/models/` (fallback `exe/data/models` for portable). Same for `settings::data_root()` + `recording_dir()`.
- Windows: `APPDATA\QuickSTT\models\` else `QuickSTT_App\data\models\`.

`SttEngineConfig::detect()` searches **both** `models_root` and `exe_dir/tools/...` with correct extension (`ext = if cfg!(windows){".exe"}else{""}`) so portable installs work on both.

## Installing

### Via app (recommended)

1. `quickstt &` → click pill gear → **Dashboard → Models**
2. Counts `Installed/Available`, `Widget Dropdown Models` chips, `Download` / `Uninstall`
3. Pill dropdown updates instantly; `selectedModel` persisted to `config.toml` / Registry.

### Manual (headless / CI)

```bash
# Vosk small en (50M) — from alphacephei
mkdir -p ~/.local/share/QuickSTT/models/vosk
wget -O /tmp/vosk-small-en.zip https://alphacephei.com/vosk/models/vosk-model-small-en-us-0.15.zip
unzip /tmp/vosk-small-en.zip -d /tmp && mv /tmp/vosk-model-small-en-us-0.15 ~/.local/share/QuickSTT/models/vosk/small_en_us_0.15

# Parakeet TDT 0.6B v3 INT8 — via huggingface (nvidia)
# Place under nemo/tdt_0_6b_v3_int8/ with encoder/decoder *.onnx
# See parakeet_engine/README for convert steps

# Nemotron 3.5 Q4_K_M — Handy GGML (473M, the working CPU quant)
mkdir -p ~/.local/share/QuickSTT/models/nemotron/nemotron-3.5-asr-streaming-0.6b
curl -L -o ~/.local/share/QuickSTT/models/nemotron/nemotron-3.5-asr-streaming-0.6b/nemotron-3.5-asr-streaming-0.6b-Q4_K_M.gguf \
  https://huggingface.co/handy-computer/nemotron-3.5-asr-streaming-0.6b-gguf/resolve/main/nemotron-3.5-asr-streaming-0.6b-Q4_K_M.gguf

# Qwen3-ASR 0.6B Q4_K_M — Handy transcribe.cpp GGUF (562M)
mkdir -p ~/.local/share/QuickSTT/models/qwen3_asr/qwen3-0.6b-q4km
curl -L -o ~/.local/share/QuickSTT/models/qwen3_asr/qwen3-0.6b-q4km/Qwen3-ASR-0.6B-Q4_K_M.gguf \
  https://huggingface.co/handy-computer/Qwen3-ASR-0.6B-gguf/resolve/main/Qwen3-ASR-0.6B-Q4_K_M.gguf

# Cactus Whistle (17M) + its needle runtime
mkdir -p ~/.local/share/QuickSTT/models/whistle/whistle-16m ~/.local/share/QuickSTT/models/runtimes/whistle
curl -L -o ~/.local/share/QuickSTT/models/whistle/whistle-16m/whistle.cact \
  https://huggingface.co/Cactus-Compute/whistle/resolve/main/whistle.cact
curl -L -o ~/.local/share/QuickSTT/models/runtimes/whistle/needle \
  https://huggingface.co/Cactus-Compute/needle3/resolve/main/linux-x86_64/needle
chmod +x ~/.local/share/QuickSTT/models/runtimes/whistle/needle
```

DeepFilter frontend (`DeepFilterNet3.tar.gz`) is optional; `engine.rs:198` skips it for `Nemotron|Vosk` (they have internal VAD). For Whisper/Sherpa it can run if `QUICKSTT_DEEPFILTER_FRONTEND=1`.

## Phonon-2 (Fermion Research)

`EngineFamily::Phonon2` rides the exact same persistent `parakeet_engine`
worker as `ParakeetRust` (`{"action":"load","model_path":…}` line-JSON; the
worker hot-swaps models when the dir changes), fed by the community ONNX
conversion of Fermion Research's **Phonon-2** in the onnx-asr
`nemo-conformer-tdt` layout (pinned revisions):

| File | Source |
|---|---|
| `encoder-model.int8.onnx` (614 MB) | `tiyuvta/Phonon-2-ONNX` @ `12c9688b…` |
| `decoder_joint-model.int8.onnx` (72 MB) | same |
| `nemo128.onnx` (1.2 MB; upstream name `preprocessor-model.onnx`) | same |
| `vocab.txt` | same |
| `NOTICE-Phonon-2.txt`, `LICENSE-WEIGHTS-CC-BY-4.0.txt` | `FermionResearch/Phonon-2` @ `7e153e40…` |

Identical artifacts on Windows/Linux/macOS (CPU engine via ONNX Runtime);
on Linux the shared `runtimes/parakeet/libonnxruntime.so` is provisioned by
the installer. Fermion's own 164 MB `phonon-2.bps.tar.zst` is a custom
five-value container that only their Python/torch runtime decodes — the
ONNX route is the one this app's runtime executes.

Weights are **CC-BY-4.0**; attribution chain: NVIDIA Parakeet TDT 0.6B v3 →
Fermion Research Phonon-2 → ONNX conversion by Tiyuvta. The NOTICE and
license text are fetched into the model directory next to the weights.

Headless install / scripting (the app instance must be running — the
command travels over the single-instance IPC socket):

```bash
printf 'DOWNLOAD Phonon-2\n' | timeout 2 bash -c 'cat > /dev/tcp/127.0.0.1/47631'
```

## SenseVoice Small Multilingual (FunAudioLLM)

`EngineFamily::SenseVoice` runs the sherpa-onnx INT8 export of FunAudioLLM /
Xiaomi **SenseVoice Small** directly on ONNX Runtime — a single CTC forward
pass, no autoregressive decoder, so it is the fastest multilingual option here
(and the only one that detects the spoken language itself).

| File | Source |
|---|---|
| `model.int8.onnx` (229 MB) | `csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17` @ `2365baea…` |
| `tokens.txt` | same |
| `LICENSE` | same |

Languages: Chinese, English, Japanese, Korean, Cantonese (plus emotion/event
tags in the model, which this app ignores). The dashboard exposes the model's
own language tokens — `Auto`, `zh`, `en`, `ja`, `ko`, `yue` — and `Auto` is the
default, so no per-recording language picker is required.

Execution provider: **CPU only** on every OS. The DirectML EP crashes with
`0xC0000005` on the sense_voice graph, so the Windows worker pins this model to
`CpuOnly` while Parakeet/Phonon keep DirectML. Linux uses the shared
`runtimes/parakeet/libonnxruntime.so` via `ORT_DYLIB_PATH`; Windows and Android
link/resolve their own ONNX Runtime.

Downloaded on demand (never bundled):

```bash
printf 'DOWNLOAD SenseVoice\n' | timeout 2 bash -c 'cat > /dev/tcp/127.0.0.1/47631'
```

## Qwen3-ASR 0.6B (Alibaba)

`EngineFamily::Qwen3Asr` runs Alibaba's **Qwen3-ASR 0.6B** through the same
Handy `transcribe.cpp` worker as Nemotron/Canary — an 18-layer audio encoder
feeding a 28-layer Qwen3 LM. It is **offline/per-utterance** (no streaming),
runs on CPU, and auto-detects the spoken language among ~30 languages.

| File | Source |
|---|---|
| `Qwen3-ASR-0.6B-Q4_K_M.gguf` (562 MiB) | `handy-computer/Qwen3-ASR-0.6B-gguf` @ `7e7c69a5…` |

Q4_K_M is the app default (2.26% WER on LibriSpeech test-clean, 562 MiB);
Q5/Q6/Q8/F16 files dropped into the same directory are also accepted.
The upstream port rejects explicit language hints
(`TRANSCRIBE_ERR_UNSUPPORTED_LANGUAGE`), so the dashboard exposes **Auto**
only — no per-recording language picker. Model loads and unloads through the
shared Nemotron worker session, so idle offload frees its RAM like the other
GGUF families.

Downloaded on demand (never bundled):

```bash
printf 'DOWNLOAD Qwen3-ASR\n' | timeout 2 bash -c 'cat > /dev/tcp/127.0.0.1/47631'
```

## Cactus Whistle 16M (Cactus Compute)

`EngineFamily::Whistle` runs **Cactus Whistle** from a single 16.9 MB
`.cact` container on the Cactus `needle` CPU engine. Seven languages
(en/de/fr/es/it/nl/pl) with in-model detection, no GPU, no Python. It is
per-utterance: the app invokes `needle --model whistle.cact --audio <wav>`
once per turn, so the process exits and releases its RAM immediately.

| File | Source |
|---|---|
| `whistle.cact` (16.9 MB) | `Cactus-Compute/whistle` |
| `config.json` | same |
| `runtimes/whistle/needle` | `Cactus-Compute/needle3` → `linux-x86_64`, `windows-x86_64`, `linux-arm64` |

Output is one JSON object (`text`, detected `language`, `ttft_ms`,
`decode_tps`). The dashboard language list is `Auto` + the seven languages;
`Auto` lets the model detect. The runtime binary is downloaded with the model
(never bundled) into `models/runtimes/whistle/`, and engine detection also
accepts `tools/whistle/needle` next to the executable for portable builds.

Downloaded on demand:

```bash
printf 'DOWNLOAD Cactus Whistle\n' | timeout 2 bash -c 'cat > /dev/tcp/127.0.0.1/47631'
```

## Engines — how they run

- **Vosk:** `Source/native/vosk_api.h` `VoskAPI::load` `dlopen("libvosk.so")`, Windows `libvosk.dll`. Native `stt_service` loads it directly; Rust fallback `transcribe_vosk()` tries `vosk_transcriber` helper then `libvosk.so` via `stt_service`.
- **Parakeet:** `transcribe-rs` Rust ONNX, persistent `ParakeetPipe` child process (`engine.rs:276` `ParakeetSession` line-delimited JSON `{"action":"load","model_path":…}` / `{"action":"transcribe","audio_path":…}`) — zero file-I/O, `CloseHandle` vs `pid_t/pipe/poll`.
- **Phonon-2:** same worker as Parakeet (`Phonon2 → transcribe_parakeet_rust`); files live in `phonon2/phonon-2-0.6b-int8/` and load on demand.
- **SenseVoice:** `transcribe-rs` Rust ONNX CTC session (`SenseVoice` family), CPU EP; `model.int8.onnx` + `tokens.txt` from `sensevoice/small_multilingual/`, language token chosen in the dashboard (`Auto` by default).
- **Nemotron:** `nemotron_engine` (built from `tools/nemotron` `transcribe.cpp` Handy) invoked `nemotron_engine --model *.gguf --audio *.wav --vad` (`engine.rs:find_gguf_file`).
- **Qwen3-ASR:** same persistent Handy worker (`nemotron_engine`), offline `{"action":"load","language":"auto"}` + `{"action":"transcribe"}`; Linux falls back to `tools/nemotron/transcribe-cli -m *.gguf`.
- **Cactus Whistle:** one-shot Cactus `needle --model whistle.cact --audio *.wav [--audio-language xx]`; the JSON `text` field is the transcript, and the process exits after each turn (no idle RAM).

At runtime `RUST_LOG=info quickstt 2>&1 | grep -E "Whisper|Parakeet|Vosk|Nemotron|libvosk|libonnx"` shows which engine fired.

## Verifying

```bash
ls -R ~/.local/share/QuickSTT/models | head -40
cat ~/.config/QuickSTT/config.toml | grep -E "selectedModel|widgetModels"
RUST_LOG=info quickstt 2>&1 | grep -i "model not found"
```

If a model shows `model not found`, check the exact `model_dir` string in `catalog.rs:40` matches the folder you created.

