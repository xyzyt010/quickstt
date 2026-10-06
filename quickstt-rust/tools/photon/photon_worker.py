#!/usr/bin/env python3
"""Persistent Photon (Moondream Parakeet) transcription worker for QuickSTT.

Mirrors the parakeet_engine JSON-lines protocol so the Rust side
(`quickstt-core/src/models/engine.rs`, Photon family) can drive it with the
same load/transcribe actions:

    {"action": "load", "repo_id": "moondream/parakeet-ultra", "isa": null}
      -> {"status": "ok"}
    {"action": "transcribe", "audio_path": "C:/.../stt_123.wav"}
      -> {"status": "ok", "text": "..."}
    {"status": "error", "error": "..."} on any failure (never exits).

The Photon context stays open across turns, so the (slow) weight load —
~2.5 min for the 1.26 GB Ultra checkpoint on CPU — is paid once per model,
not per utterance. Model weights come from the shared HuggingFace cache
(repo id, not a local path: Photon only accepts registered ids).

`isa` forces the ternary int8 GEMM path (e.g. "avxvnni") on machines whose
kestrel build misdetects the CPU. Full-precision models (Ultra) ignore it.
Requires: pip install "moondream>=2.4"  (+ Python 3.10+).
"""
import argparse
import json
import sys
import traceback


def log(msg: str) -> None:
    print(f"[photon-worker] {msg}", file=sys.stderr, flush=True)


def respond(payload: dict) -> None:
    sys.stdout.write(json.dumps(payload, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def apply_isa(isa: str | None) -> None:
    # set_gemm_isa() never rejects (validation happens at GEMM time), so only
    # apply a value that was verified end-to-end (see bench_photon.py).
    if not isa:
        return
    try:
        from kestrel_kernels import ternary

        ternary.set_gemm_isa(isa)
        log(f"ISA override applied: {isa}")
    except Exception as e:  # noqa: BLE001 - keep serving without it
        log(f"ISA override {isa!r} unavailable ({e}); continuing without it")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--isa", default="", help="forced ternary GEMM ISA (rarely needed)")
    args = ap.parse_args()

    try:
        import moondream as md
    except Exception as e:  # noqa: BLE001
        log(f"FATAL: cannot import moondream ({e}); need pip install 'moondream>=2.4'")
        respond({"status": "error", "error": f"moondream import failed: {e}"})
        return 1

    apply_isa(args.isa or None)

    speech = None
    active_repo: str | None = None

    def ensure_loaded(repo_id: str) -> None:
        nonlocal speech, active_repo
        if speech is not None and active_repo == repo_id:
            return
        unload_model()
        log(f"loading {repo_id} (first load is slow: weights + kernels) ...")
        speech = md.photon(repo_id, device="cpu").__enter__()
        active_repo = repo_id
        log(f"loaded {repo_id}")

    def unload_model() -> None:
        nonlocal speech, active_repo
        if speech is not None:
            try:
                speech.close()
            except Exception:  # noqa: BLE001
                pass
            speech = None
            active_repo = None
            log("model unloaded (weights released, process stays for fast reload)")

    log("ready, waiting for JSON-lines on stdin")
    for raw in sys.stdin:
        # Tolerate a UTF-8 BOM (some launchers/pipes prepend one) and stray
        # whitespace; the Rust side always sends plain ASCII.
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
                repo_id = req.get("repo_id") or ""
                if not repo_id:
                    respond({"status": "error", "error": "load needs repo_id"})
                    continue
                # Ternary ISA override must land before the context opens
                # (resident weight form is decided at load).
                apply_isa(req.get("isa"))
                ensure_loaded(repo_id)
                respond({"status": "ok"})
            elif action == "transcribe":
                audio_path = req.get("audio_path") or ""
                repo_id = req.get("repo_id") or active_repo or ""
                if not audio_path:
                    respond({"status": "error", "error": "transcribe needs audio_path"})
                    continue
                if not repo_id:
                    respond({"status": "error", "error": "no model loaded (send load first)"})
                    continue
                ensure_loaded(repo_id)
                assert speech is not None
                result = speech.transcribe(audio=audio_path)
                text = result["text"] if isinstance(result, dict) else str(result)
                respond({"status": "ok", "text": text.strip()})
            elif action == "ping":
                respond({"status": "ok"})
            elif action == "unload":
                # Idle offload: drop the weights (1.26 GB for Ultra) but keep
                # serving — next transcribe reloads on demand.
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
