#!/usr/bin/env python3
"""Persistent Canary-Qwen 2.5B transcription worker for QuickSTT.

NVIDIA NeMo SALM (FastConformer encoder + Qwen3-1.7B) behind the same
JSON-lines protocol as photon_worker.py so the Rust side
(`quickstt-core/src/models/engine.rs`, CanaryQwen family) drives it
identically:

    {"action": "load", "repo_id": "nvidia/canary-qwen-2.5b"}
      -> {"status": "ok"}
    {"action": "transcribe", "audio_path": "C:/.../stt_123.wav"}
      -> {"status": "ok", "text": "..."}
    {"action": "unload"} -> {"status": "ok"}  (weights released, serving stays)
    {"action": "ping"} -> {"status": "ok"}
    {"status": "error", "error": "..."} on any failure (never exits).

Requirements: NVIDIA discrete GPU (8GB+ VRAM recommended: 2.5B BF16 weights
are ~5GB before encoder state + CUDA overhead), Python 3.10+, CUDA torch,
and NeMo 2.5+::

    pip install "nemo_toolkit[asr]"          # trunk build per model card
    # torch with CUDA comes with the [asr] extra

Weights auto-download from HuggingFace into the shared HF cache on first
load (several GB — NOT bundled, NOT fetched by the QuickSTT downloader).

Utterances here are <= 12s; the model supports up to ~40s per call, so one
transcribe call per turn with no chunking. 16kHz mono wav in (the app's
native segment format); no pre-processing needed.
"""
import argparse
import gc
import json
import sys
import traceback

DEFAULT_REPO = "nvidia/canary-qwen-2.5b"
# Short-turn ceiling: 12s of speech is a few dozen tokens; 256 leaves wide
# headroom without letting a confused decode ramble.
MAX_NEW_TOKENS = 256


def log(msg: str) -> None:
    print(f"[canary-worker] {msg}", file=sys.stderr, flush=True)


def respond(payload: dict) -> None:
    sys.stdout.write(json.dumps(payload, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=DEFAULT_REPO, help="default HF repo id")
    args = ap.parse_args()

    try:
        import torch
    except Exception as e:  # noqa: BLE001
        log(f"FATAL: cannot import torch ({e})")
        respond({"status": "error", "error": f"torch import failed: {e}"})
        return 1
    if not torch.cuda.is_available():
        log("FATAL: torch has no CUDA device — Canary-Qwen needs an NVIDIA GPU")
        respond({"status": "error", "error": "no CUDA device (Canary-Qwen needs an NVIDIA GPU)"})
        return 1
    try:
        from nemo.collections.speechlm2.models import SALM
    except Exception as e:  # noqa: BLE001
        log(f"FATAL: cannot import NeMo SALM ({e}); need pip install 'nemo_toolkit[asr]'")
        respond({"status": "error", "error": f"NeMo SALM import failed: {e}"})
        return 1

    model = None
    active_repo: str | None = None

    def ensure_loaded(repo_id: str) -> None:
        nonlocal model, active_repo
        if model is not None and active_repo == repo_id:
            return
        unload_model()
        log(f"loading {repo_id} (first load downloads ~5GB + inits CUDA) ...")
        model = SALM.from_pretrained(repo_id)
        active_repo = repo_id
        log(f"loaded {repo_id}")

    def unload_model() -> None:
        nonlocal model, active_repo
        if model is not None:
            try:
                del model
            except Exception:  # noqa: BLE001
                pass
            model = None
            active_repo = None
            gc.collect()
            try:
                torch.cuda.empty_cache()
            except Exception:  # noqa: BLE001
                pass
            log("model unloaded (weights released, process stays for fast reload)")

    def transcribe(audio_path: str) -> str:
        assert model is not None
        tag = model.audio_locator_tag
        answer_ids = model.generate(
            prompts=[
                [
                    {
                        "role": "user",
                        "content": f"Transcribe the following: {tag}",
                        "audio": [audio_path],
                    }
                ]
            ],
            max_new_tokens=MAX_NEW_TOKENS,
        )
        return model.tokenizer.ids_to_text(answer_ids[0].cpu()).strip()

    log("ready, waiting for JSON-lines on stdin")
    for raw in sys.stdin:
        line = raw.lstrip("\ufeff").strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except Exception as e:  # noqa: BLE001
            respond({"status": "error", "error": f"bad JSON: {e}"})
            continue
        action = req.get("action")
        try:
            if action == "load":
                repo_id = req.get("repo_id") or args.repo
                if not repo_id:
                    respond({"status": "error", "error": "load needs repo_id"})
                    continue
                ensure_loaded(repo_id)
                respond({"status": "ok"})
            elif action == "transcribe":
                audio_path = req.get("audio_path") or ""
                repo_id = req.get("repo_id") or active_repo or args.repo
                if not audio_path:
                    respond({"status": "error", "error": "transcribe needs audio_path"})
                    continue
                if not repo_id:
                    respond({"status": "error", "error": "no model loaded (send load first)"})
                    continue
                ensure_loaded(repo_id)
                respond({"status": "ok", "text": transcribe(audio_path)})
            elif action == "ping":
                respond({"status": "ok"})
            elif action == "unload":
                unload_model()
                respond({"status": "ok"})
            else:
                respond({"status": "error", "error": f"unknown action: {action!r}"})
        except Exception:  # noqa: BLE001
            respond({"status": "error", "error": traceback.format_exc(limit=3)})
    log("stdin closed, exiting")
    return 0


if __name__ == "__main__":
    sys.exit(main())
