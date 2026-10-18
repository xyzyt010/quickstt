# Models — Vosk, Parakeet, Nemotron (offline, local)

QuickSTT is **100% offline** by default. All three models run on-device via `quickstt-core/src/models/catalog.rs:15` `EngineFamily` and `models/engine.rs` dispatch.

## Catalog

| Model | EngineFamily | Display | Path Linux Mint (`~/.local/share/QuickSTT/models/…`) | Path Windows (`%APPDATA%\QuickSTT\models\…`) | Size | Best for |
|---|---|---|---|---|---|
| **Vosk small EN 0.15** | `Vosk` (`libvosk.so` / `libvosk.dll` via `vosk_api.h` `dlopen`) | `vosk/small_en_us_0.15/` | same | 50 MB | Fastest CPU, dictation |
| **Parakeet TDT 0.6B v3 INT8** | `ParakeetRust` (`transcribe-rs 0.3.3 onnx` `ort` `libonnxruntime.so`) | `nemo/tdt_0_6b_v3_int8/` | same | 640 MB | Highest accuracy offline |
| **Phonon-2 0.6B INT8** | `Phonon2` (same persistent `parakeet_engine` worker as Parakeet) | `phonon2/phonon-2-0.6b-int8/` | same | 688 MB | Best compact English accuracy (Fermion Research) |
| **Nemotron 3.5 Streaming 0.6B Q8_0** | `Nemotron` (`tools/nemotron/nemotron_engine` `transcribe.cpp` Handy GGML) | `nemotron/streaming_0.6b_q8_0/*.gguf` | same | 716 MB | Streaming + VAD, low latency |

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

# Nemotron 3.5 Q8_0 — Handy GGML (716M)
python3 tools/nemotron/fetch_and_convert.py --q 8_0
# → tools/nemotron/nemotron-3.5-*.gguf then install to ~/.local/share/QuickSTT/models/nemotron/streaming_0.6b_q8_0/
mkdir -p ~/.local/share/QuickSTT/models/nemotron/streaming_0.6b_q8_0
cp tools/nemotron/*.gguf ~/.local/share/QuickSTT/models/nemotron/streaming_0.6b_q8_0/
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

## Engines — how they run

- **Vosk:** `Source/native/vosk_api.h` `VoskAPI::load` `dlopen("libvosk.so")`, Windows `libvosk.dll`. Native `stt_service` loads it directly; Rust fallback `transcribe_vosk()` tries `vosk_transcriber` helper then `libvosk.so` via `stt_service`.
- **Parakeet:** `transcribe-rs` Rust ONNX, persistent `ParakeetPipe` child process (`engine.rs:276` `ParakeetSession` line-delimited JSON `{"action":"load","model_path":…}` / `{"action":"transcribe","audio_path":…}`) — zero file-I/O, `CloseHandle` vs `pid_t/pipe/poll`.
- **Phonon-2:** same worker as Parakeet (`Phonon2 → transcribe_parakeet_rust`); files live in `phonon2/phonon-2-0.6b-int8/` and load on demand.
- **Nemotron:** `nemotron_engine` (built from `tools/nemotron` `transcribe.cpp` Handy) invoked `nemotron_engine --model *.gguf --audio *.wav --vad` (`engine.rs:find_gguf_file`).

At runtime `RUST_LOG=info quickstt 2>&1 | grep -E "Whisper|Parakeet|Vosk|Nemotron|libvosk|libonnx"` shows which engine fired.

## Verifying

```bash
ls -R ~/.local/share/QuickSTT/models | head -40
cat ~/.config/QuickSTT/config.toml | grep -E "selectedModel|widgetModels"
RUST_LOG=info quickstt 2>&1 | grep -i "model not found"
```

If a model shows `model not found`, check the exact `model_dir` string in `catalog.rs:40` matches the folder you created.

