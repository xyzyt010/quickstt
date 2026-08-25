# Models — Vosk, Parakeet, Nemotron (offline, local)

QuickSTT is **100% offline** by default. All three models run on-device via `quickstt-core/src/models/catalog.rs:15` `EngineFamily` and `models/engine.rs` dispatch.

## Catalog

| Model | EngineFamily | Display | Path Linux Mint (`~/.local/share/QuickSTT/models/…`) | Path Windows (`%APPDATA%\QuickSTT\models\…`) | Size | Best for |
|---|---|---|---|---|---|
| **Vosk small EN 0.15** | `Vosk` (`libvosk.so` / `libvosk.dll` via `vosk_api.h` `dlopen`) | `vosk/small_en_us_0.15/` | same | 50 MB | Fastest CPU, dictation |
| **Parakeet TDT 0.6B v3 INT8** | `ParakeetRust` (`transcribe-rs 0.3.3 onnx` `ort` `libonnxruntime.so`) | `nemo/tdt_0_6b_v3_int8/` | same | 640 MB | Highest accuracy offline |
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

## Engines — how they run

- **Vosk:** `Source/native/vosk_api.h` `VoskAPI::load` `dlopen("libvosk.so")`, Windows `libvosk.dll`. Native `stt_service` loads it directly; Rust fallback `transcribe_vosk()` tries `vosk_transcriber` helper then `libvosk.so` via `stt_service`.
- **Parakeet:** `transcribe-rs` Rust ONNX, persistent `ParakeetPipe` child process (`engine.rs:276` `ParakeetSession` line-delimited JSON `{"action":"load","model_path":…}` / `{"action":"transcribe","audio_path":…}`) — zero file-I/O, `CloseHandle` vs `pid_t/pipe/poll`.
- **Nemotron:** `nemotron_engine` (built from `tools/nemotron` `transcribe.cpp` Handy) invoked `nemotron_engine --model *.gguf --audio *.wav --vad` (`engine.rs:find_gguf_file`).

At runtime `RUST_LOG=info quickstt 2>&1 | grep -E "Whisper|Parakeet|Vosk|Nemotron|libvosk|libonnx"` shows which engine fired.

## Verifying

```bash
ls -R ~/.local/share/QuickSTT/models | head -40
cat ~/.config/QuickSTT/config.toml | grep -E "selectedModel|widgetModels"
RUST_LOG=info quickstt 2>&1 | grep -i "model not found"
```

If a model shows `model not found`, check the exact `model_dir` string in `catalog.rs:40` matches the folder you created.

