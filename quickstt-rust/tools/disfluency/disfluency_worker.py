#!/usr/bin/env python3
"""Persistent disfluency-cleanup worker for QuickSTT.

Runs stillerman/fdt-disfluency-distilbert-66m-v5 (int8 ONNX, 66M params) —
a deletion-only tagger: every whitespace word is tagged KEEP / DELETE /
KEEP_STRIP_COMMA / KEEP_CAPITALIZE and the reconstruction below turns tags
into cleaned text. Deletion-only by construction: it cannot rephrase,
hallucinate, or alter names and numbers.

Same JSON-lines protocol as photon_worker.py:

    {"action": "load", "model_dir": "<dir with model_quantized.onnx + tokenizer.json>"}
      -> {"status": "ok"}
    {"action": "clean", "text": "um I I think uh we should go"}
      -> {"status": "ok", "text": "I think we should go"}
    {"action": "unload"} -> {"status": "ok"}
    {"action": "ping"} -> {"status": "ok"}
    {"status": "error", "error": "..."} on any failure (never exits).

Tag aggregation: first subword token decides its whitespace word (standard
for token-classification taggers). A word is DELETE only when DELETE is the
strict argmax — ties fall back to KEEP (conservative: never eat a word on
a coin flip).

Empty-guard: if every word tags DELETE (or input is empty), the ORIGINAL
text is returned — cleanup must never blank a real turn.

Requires: pip install onnxruntime tokenizers  (CPU only, ~10-50ms/turn).
Model files (NOT bundled — QuickSTT downloader fetches them on first use):
    <models>/disfluency/fdt-disfluency-distilbert-66m-v5/
        model_quantized.onnx  tokenizer.json  config.json  vocab.txt
"""
import argparse
import json
import sys
import traceback

LABELS = ("KEEP", "DELETE", "KEEP_STRIP_COMMA", "KEEP_CAPITALIZE")


def log(msg: str) -> None:
    print(f"[disfluency-worker] {msg}", file=sys.stderr, flush=True)


def respond(payload: dict) -> None:
    sys.stdout.write(json.dumps(payload, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def reconstruct(words: list[str], tags: list[str]) -> str:
    """~15-line reconstruction per the model card: tags -> cleaned text."""
    out: list[str] = []
    for word, tag in zip(words, tags):
        if tag == "DELETE":
            continue
        if tag == "KEEP_STRIP_COMMA":
            word = word.replace(",", "")
            if not word:
                continue
        elif tag == "KEEP_CAPITALIZE" and word:
            word = word[0].upper() + word[1:]
        out.append(word)
    return " ".join(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--model-dir", default="", help="default model dir")
    args = ap.parse_args()

    try:
        import numpy as np
    except Exception as e:  # noqa: BLE001
        log(f"FATAL: cannot import numpy ({e})")
        respond({"status": "error", "error": f"numpy import failed: {e}"})
        return 1
    try:
        import onnxruntime as ort
    except Exception as e:  # noqa: BLE001
        log(f"FATAL: cannot import onnxruntime ({e}); need pip install onnxruntime")
        respond({"status": "error", "error": f"onnxruntime import failed: {e}"})
        return 1
    try:
        from tokenizers import Tokenizer
    except Exception as e:  # noqa: BLE001
        log(f"FATAL: cannot import tokenizers ({e}); need pip install tokenizers")
        respond({"status": "error", "error": f"tokenizers import failed: {e}"})
        return 1

    session = None
    tokenizer = None
    active_dir: str | None = None

    def ensure_loaded(model_dir: str) -> None:
        nonlocal session, tokenizer, active_dir
        if session is not None and active_dir == model_dir:
            return
        unload_model()
        import os

        onnx_path = os.path.join(model_dir, "model_quantized.onnx")
        tok_path = os.path.join(model_dir, "tokenizer.json")
        log(f"loading disfluency tagger from {model_dir} ...")
        tokenizer = Tokenizer.from_file(tok_path)
        session = ort.InferenceSession(onnx_path, providers=["CPUExecutionProvider"])
        active_dir = model_dir
        log("disfluency tagger loaded")

    def unload_model() -> None:
        nonlocal session, tokenizer, active_dir
        session = None
        tokenizer = None
        active_dir = None
        log("tagger unloaded")

    def clean(text: str) -> str:
        assert session is not None and tokenizer is not None
        import re

        import numpy as _np

        # Whitespace-word spans: the WordPiece pre-tokenizer splits
        # punctuation ("should," -> "should" + ","), so token word-ids do
        # NOT match whitespace words. Map every token to its whitespace
        # word via character offsets; the FIRST token of each whitespace
        # word decides its tag (standard for token-classification taggers).
        spans = [(m.start(), m.end()) for m in re.finditer(r"\S+", text)]
        words = [text[a:b] for a, b in spans]
        if not words:
            return text

        def word_of(char_pos: int) -> int | None:
            for i, (a, b) in enumerate(spans):
                if a <= char_pos < b:
                    return i
            return None

        enc = tokenizer.encode(text)
        ids = _np.array([enc.ids], dtype=_np.int64)
        mask = _np.ones_like(ids)
        logits = session.run(None, {"input_ids": ids, "attention_mask": mask})[0][0]
        tags = ["KEEP"] * len(words)
        seen: set[int] = set()
        for tok_pos, (tok_start, _tok_end) in enumerate(enc.offsets):
            wi = word_of(tok_start)
            if wi is None or wi in seen:
                continue
            seen.add(wi)
            row = logits[tok_pos]
            best = int(_np.argmax(row))
            tags[wi] = LABELS[best] if best < len(LABELS) else "KEEP"
        cleaned = reconstruct(words, tags)
        # Never blank a turn: all-DELETE (or punctuation-only loss) falls
        # back to the original text.
        return cleaned if cleaned.strip() else text

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
                model_dir = req.get("model_dir") or args.model_dir
                if not model_dir:
                    respond({"status": "error", "error": "load needs model_dir"})
                    continue
                ensure_loaded(model_dir)
                respond({"status": "ok"})
            elif action == "clean":
                text = req.get("text") or ""
                model_dir = req.get("model_dir") or active_dir or args.model_dir
                if not model_dir:
                    respond({"status": "error", "error": "no model loaded (send load first)"})
                    continue
                ensure_loaded(model_dir)
                respond({"status": "ok", "text": clean(text)})
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
