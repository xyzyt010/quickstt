#!/usr/bin/env python3
"""Persistent transcript-cleanup worker for QuickSTT.

Two interchangeable backends, one JSON-lines protocol:

1. "fdt" — stillerman/fdt-disfluency-distilbert-66m-v5 (int8 ONNX, 66M).
   A deletion-only word tagger (KEEP / DELETE / KEEP_STRIP_COMMA /
   KEEP_CAPITALIZE) plus the retraction repairs below. Fast (~15ms) and it
   cannot rephrase or hallucinate, but it only removes words — it does not
   rewrite numbers, dates or self-corrections into written form.

2. "s1mini" — "S1-mini by Superwhisper" (superwhisper/s1-mini, a Qwen3-0.6B
   fine-tune specialised as an ASR transcript normalizer), served from the
   INT4 ONNX conversion (elbruno/s1-mini-onnx) via onnxruntime-genai. It
   removes fillers AND resolves self-corrections to the value the speaker
   landed on ("nine am no sorry ten thirty" -> "10:30"), and writes spoken
   numbers, dates, times and currency in written form. ~94.8% token accuracy.
   Slower per turn (0.6B on CPU) but strictly better text.
   License: Apache-2.0 + naming clause — the name "S1-mini" by "Superwhisper"
   must be preserved wherever it is used (see the dashboard card / NOTICE).

Protocol (same shape as photon_worker.py):

    {"action": "load", "backend": "fdt", "model_dir": "<dir>"}
      -> {"status": "ok"}
    {"action": "clean", "backend": "fdt", "text": "um I I think uh we go"}
      -> {"status": "ok", "text": "I think we go"}
    {"action": "unload"} -> {"status": "ok"}
    {"action": "ping"}   -> {"status": "ok"}
    {"status": "error", "error": "..."} on any failure (never exits).

MEMORY POLICY: the caller sends `unload` right after every `clean` — the
model is loaded, used for one turn, then dropped, so a cleanup model never
sits resident in RAM between turns. Only this interpreter stays alive.

Requires: pip install onnxruntime tokenizers      (fdt backend)
          pip install onnxruntime-genai           (s1mini backend)

Model files (NOT bundled — the app's downloader fetches them on first use):
    <models>/disfluency/fdt-disfluency-distilbert-66m-v5/
        model_quantized.onnx  tokenizer.json  config.json  vocab.txt
    <models>/disfluency/s1-mini-int4/
        model.onnx  model.onnx.data  genai_config.json
        tokenizer.json  tokenizer_config.json  chat_template.jinja
"""
import argparse
import gc
import json
import os
import re
import sys
import traceback

LABELS = ("KEEP", "DELETE", "KEEP_STRIP_COMMA", "KEEP_CAPITALIZE")

# Byte-exact prompt contract for S1-mini (model card). Changing any of this
# makes the model hallucinate or return nothing at all.
S1_SYSTEM = (
    "You are a text normalizer for speech-to-text transcripts. The input begins "
    "with a control line specifying the styling, structure, and context settings; "
    "clean the transcript to match those settings and output only the cleaned text."
)
S1_CONTROL = "[Styling: {styling}] [Structure: {structure}] [Context: {context}]"
# The literal chat template the model was trained with, thinking disabled:
# an EMPTY think block must open the assistant turn or Qwen3 emits nothing.
S1_ASSISTANT_PREFIX = "<|im_start|>assistant\n<think>\n\n</think>\n\n"


def log(msg: str) -> None:
    print(f"[disfluency-worker] {msg}", file=sys.stderr, flush=True)


def respond(payload: dict) -> None:
    sys.stdout.write(json.dumps(payload, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def reconstruct(words: list[str], tags: list[str]) -> str:
    """Tags -> cleaned text (~15 lines, per the model card)."""
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


NUM_WORDS = {
    "zero", "one", "two", "three", "four", "five",
    "six", "seven", "eight", "nine", "ten",
}

# Deterministic repairs the 66M tagger cannot learn (it is deletion-only).
# These run on both backends' OUTPUT, never on the tagger's input.
_PAT_NOT = re.compile(
    r"\b([A-Za-z]+(?:\s+[A-Za-z]+){0,3})[?.,!]*\s+(?:oh\s*,?\s*)?(?:sorry|wait|no)\s*,?\s*not\s+\1\s*[.,!?;]*\s+([A-Za-z]+(?:\s+[A-Za-z]+){0,3})([.!?]?)",
    re.IGNORECASE,
)
_PAT_IMEAN = re.compile(
    r"\b((?:a|an|the|my|your|his|her|our|their)\s+[A-Za-z]+|[A-Za-z]+),\s*I\s+mean,\s*((?:a|an|the|my|your|his|her|our|their)\s+[A-Za-z]+|[A-Za-z]+)",
    re.IGNORECASE,
)


def apply_repairs(text: str) -> str:
    """'<X>? Oh, sorry, not <X>. <Y>' -> '<Y>' and '<NP>, I mean, <NP>' -> <NP>."""

    def _sub_not(m: "re.Match[str]") -> str:
        orig, repl, punct = m.group(1), m.group(2), m.group(3)
        if (
            orig[0].islower()
            and repl[0].isupper()
            and repl.lower().split()[0] in {"a", "an", "the", "my", "your", "his", "her", "our", "their"}
        ):
            repl = repl[0].lower() + repl[1:]
        return repl + (punct or ".")

    text = _PAT_NOT.sub(_sub_not, text)

    def _sub_imean(m: "re.Match[str]") -> str:
        lhs, rhs = m.group(1), m.group(2)
        lw, rw = lhs.split(), rhs.split()
        if rw[0].lower() in {
            "i", "you", "we", "they", "he", "she", "it", "that", "this",
            "could", "can", "should", "would",
        }:
            return m.group(0)
        return rhs if len(lw) == len(rw) else m.group(0)

    return _PAT_IMEAN.sub(_sub_imean, text)


class Backend:
    """One loaded cleanup model. Loading is cheap, so callers load/unload per turn."""

    def __init__(self, kind: str, model_dir: str) -> None:
        self.kind = kind
        self.dir = model_dir
        self.session = None      # fdt
        self.tokenizer = None    # fdt (HF tokenizers)
        self.model = None        # s1mini (ORT-genai)
        self.s1_tok = None       # s1mini
        self.load()

    def load(self) -> None:
        if self.kind == "fdt":
            self._load_fdt()
        elif self.kind == "s1mini":
            self._load_s1()
        else:
            raise ValueError(f"unknown cleanup backend {self.kind!r}")

    def _load_fdt(self) -> None:
        import onnxruntime as ort
        from tokenizers import Tokenizer

        log(f"loading FDT 66M tagger from {self.dir} ...")
        self.tokenizer = Tokenizer.from_file(os.path.join(self.dir, "tokenizer.json"))
        self.session = ort.InferenceSession(
            os.path.join(self.dir, "model_quantized.onnx"),
            providers=["CPUExecutionProvider"],
        )
        log("FDT 66M tagger loaded")

    def _load_s1(self) -> None:
        import onnxruntime_genai as og

        log(f"loading S1-mini INT4 from {self.dir} (0.6B, first load reads ~375MB) ...")
        self.model = og.Model(self.dir)
        self.s1_tok = og.Tokenizer(self.model)
        log("S1-mini INT4 loaded")

    def unload(self) -> None:
        self.session = None
        self.tokenizer = None
        self.s1_tok = None
        self.model = None
        gc.collect()
        log(f"{self.kind} model dropped (RAM released)")

    def clean(self, text: str, styling: str, structure: str, context: str) -> str:
        norm = " ".join(text.split())
        if not norm:
            return text
        out = self._clean_fdt(norm) if self.kind == "fdt" else self._clean_s1(norm, styling, structure, context)
        out = apply_repairs(out)
        # Never blank a real turn: an all-filler input legitimately normalizes
        # to "", but so does a broken decode — keep the words if nothing is left.
        return out if out.strip() else norm

    def _clean_fdt(self, norm_text: str) -> str:
        import numpy as np

        words = norm_text.split()
        # is_pretokenized=True so [CLS]/[SEP] map to None and punctuation stays
        # glued to its word (matching how the tagger was trained).
        enc = self.tokenizer.encode(words, is_pretokenized=True)
        ids = np.array([enc.ids], dtype=np.int64)
        logits = self.session.run(
            None, {"input_ids": ids, "attention_mask": np.ones_like(ids)}
        )[0][0]
        tags = ["KEEP"] * len(words)
        seen: set[int] = set()
        for tok_pos, wi in enumerate(enc.words):
            if wi is None or wi in seen:
                continue
            seen.add(wi)
            row = logits[tok_pos]
            best = int(np.argmax(row))
            tag = LABELS[best] if best < len(LABELS) else "KEEP"
            bare = re.sub(r"[^A-Za-z0-9]", "", words[wi]).lower()
            # Counting numbers are content ("one two three"), not stutters —
            # only an immediate repeat ("one one") may be deleted.
            if tag == "DELETE" and (bare in NUM_WORDS or bare.isdigit()):
                nxt = (
                    re.sub(r"[^A-Za-z0-9]", "", words[wi + 1]).lower()
                    if wi + 1 < len(words)
                    else ""
                )
                if bare != nxt:
                    tag = "KEEP"
            tags[wi] = tag
        # Keep sentence-initial capitalization when a leading filler is cut.
        pending_cap = False
        for i, (w, t) in enumerate(zip(words, tags)):
            starts = i == 0 or words[i - 1].endswith((".", "!", "?"))
            if t == "DELETE":
                if starts and w and w[0].isupper():
                    pending_cap = True
            else:
                if pending_cap and t == "KEEP":
                    tags[i] = "KEEP_CAPITALIZE"
                pending_cap = False
        return reconstruct(words, tags)

    def _clean_s1(self, norm_text: str, styling: str, structure: str, context: str) -> str:
        import onnxruntime_genai as og

        control = S1_CONTROL.format(styling=styling, structure=structure, context=context)
        prompt = (
            f"<|im_start|>system\n{S1_SYSTEM}<|im_end|>\n"
            f"<|im_start|>user\n{control}\n{norm_text}<|im_end|>\n"
            f"{S1_ASSISTANT_PREFIX}"
        )
        tokens = self.s1_tok.encode(prompt)
        # Greedy, no sampling. Never pass temperature=0 — ORT-genai divides by
        # zero on that in the native sampler (a hard crash, not an error), so
        # we rely on the shipped do_sample=False default instead.
        params = og.GeneratorParams(self.model)
        budget = int(len(tokens) + max(48, int(len(norm_text) * 0.7)) + 64)
        params.set_search_options(max_length=min(budget, 1200))
        gen = og.Generator(self.model, params)
        gen.append_tokens(tokens)
        while not gen.is_done():
            gen.generate_next_token()
        seq = gen.get_sequence(0)
        generated = self.s1_tok.decode(seq[len(tokens):])
        return generated.strip()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--model-dir", default="", help="default model dir")
    ap.add_argument("--backend", default="fdt", help="default backend (fdt | s1mini)")
    args = ap.parse_args()

    backend: Backend | None = None
    active_dir: str | None = None
    active_kind: str | None = None

    def ensure_loaded(kind: str, model_dir: str) -> Backend:
        nonlocal backend, active_dir, active_kind
        if backend is not None and active_kind == kind and active_dir == model_dir:
            return backend
        if backend is not None:
            backend.unload()
        backend = Backend(kind, model_dir)
        active_dir = model_dir
        active_kind = kind
        return backend

    def unload_backend() -> None:
        nonlocal backend, active_dir, active_kind
        if backend is None:
            return
        backend.unload()
        backend = None
        active_dir = None
        active_kind = None

    log(f"ready (default backend={args.backend}), waiting for JSON-lines on stdin")
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
                kind = req.get("backend") or args.backend
                model_dir = req.get("model_dir") or args.model_dir
                if not model_dir:
                    respond({"status": "error", "error": "load needs model_dir"})
                    continue
                ensure_loaded(kind, model_dir)
                respond({"status": "ok"})
            elif action == "clean":
                kind = req.get("backend") or active_kind or args.backend
                model_dir = req.get("model_dir") or active_dir or args.model_dir
                if not model_dir:
                    respond({"status": "error", "error": "no model available (send load first)"})
                    continue
                be = ensure_loaded(kind, model_dir)
                text = req.get("text") or ""
                cleaned = be.clean(
                    text,
                    req.get("styling") or "semi-formal",
                    req.get("structure") or "prose",
                    req.get("context") or "general",
                )
                respond({"status": "ok", "text": cleaned})
            elif action == "ping":
                respond({"status": "ok"})
            elif action == "unload":
                unload_backend()
                respond({"status": "ok"})
            else:
                respond({"status": "error", "error": f"unknown action: {action!r}"})
        except ModuleNotFoundError as e:
            # e.g. onnxruntime-genai missing: report the pip line, stay alive.
            respond({"status": "error", "error": f"missing module {e} (pip install onnxruntime-genai)"})
        except Exception:  # noqa: BLE001
            respond({"status": "error", "error": traceback.format_exc(limit=3)})
    log("stdin closed, exiting")
    return 0


if __name__ == "__main__":
    sys.exit(main())