"""Download (via HuggingFace) + benchmark Moondream Photon ASR models on this CPU.

Usage:
    python bench_photon.py --model redux|ultra|both [--wav PATH] [--models-root DIR]

Downloads go into the app's models_root (installed-flag + offline story);
benchmarks run via repo id against the shared HF cache. Ternary models get
the avx2 ISA override (this box's kestrel build misdetects the CPU).
"""
import argparse
import json
import sys
import time
import wave
from pathlib import Path


def wav_seconds(path: str) -> float:
    with wave.open(path, "rb") as w:
        return w.getnframes() / w.getframerate()


def ensure_model(repo_id: str, dest: Path, patterns: list[str]) -> Path:
    from huggingface_hub import snapshot_download

    dest.mkdir(parents=True, exist_ok=True)
    print(f"[dl] {repo_id} -> {dest}", flush=True)
    snap = snapshot_download(
        repo_id=repo_id,
        local_dir=str(dest),
        allow_patterns=patterns,
    )
    files = sorted(p.name for p in dest.iterdir())
    print(f"[dl] done: {files}", flush=True)
    return Path(snap)


def bench(model_src: str, wav: str, device: str = "cpu") -> dict:
    import moondream as md

    dur = wav_seconds(wav)
    t0 = time.perf_counter()
    with md.photon(model_src, device=device) as speech:
        load_s = time.perf_counter() - t0
        t1 = time.perf_counter()
        result = speech.transcribe(audio=wav)
        infer_s = time.perf_counter() - t1
    text = result["text"] if isinstance(result, dict) else str(result)
    return {
        "model": model_src,
        "audio_s": round(dur, 2),
        "load_s": round(load_s, 1),
        "infer_s": round(infer_s, 1),
        "rtf": round(dur / infer_s, 1) if infer_s > 0 else 0.0,
        "text": text[:300],
    }


def maybe_force_avx2(dest: Path) -> None:
    """This box's kestrel build misdetects the CPU as 'scalar' (no int8
    kernels) even though the Windows binary ships them — force the path
    explicitly. Verified on this machine: 'avxvnni' loads AND transcribes
    (Meteor Lake has AVX-VNNI); bare 'avx2' raises 'no path on this machine'
    at GEMM time. Order matters: most-capable first."""
    if (dest / "ternary.json").exists():
        # NOTE: set_gemm_isa() never rejects (validation happens at GEMM
        # time), so there is no point trying names in a loop — hardcode the
        # one verified end-to-end on this machine.
        from kestrel_kernels import ternary

        ternary.set_gemm_isa("avxvnni")
        print("[info] ternary weights detected -> forced ISA: avxvnni", flush=True)


MODELS = {
    "redux": (
        "moondream/parakeet-redux",
        ["model.safetensors", "config.json", "ternary.json", "tokenizer.json"],
    ),
    "ultra": (
        "moondream/parakeet-ultra",
        ["model.safetensors", "config.json", "tokenizer.json"],
    ),
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default="both", choices=["redux", "ultra", "both"])
    ap.add_argument("--wav", default="")
    ap.add_argument("--models-root", default="")
    args = ap.parse_args()

    if not args.wav:
        # repo test clip if present
        here = Path(__file__).resolve()
        cand = here.parents[3] / "test_16k.wav"
        args.wav = str(cand)
    if not Path(args.wav).exists():
        print(f"[err] wav not found: {args.wav}")
        return 2
    print(f"[info] wav={args.wav} dur={wav_seconds(args.wav):.1f}s", flush=True)

    if args.models_root:
        root = Path(args.models_root)
    else:
        appdata = Path.home() / "AppData" / "Roaming"
        root = appdata / "QuickSTT" / "models"

    want = ["redux", "ultra"] if args.model == "both" else [args.model]
    for name in want:
        repo_id, patterns = MODELS[name]
        dest = root / f"parakeet_{name}"
        try:
            ensure_model(repo_id, dest, patterns)
        except Exception as e:  # noqa: BLE001
            print(f"[err] download failed for {repo_id}: {e}")
            return 3
        # Prefer the local snapshot (no re-download); fall back to repo id.
        # NOTE: Photon only accepts registered repo ids (not local paths),
        # so bench via repo id against the shared HF cache; the local copy
        # serves the app's installed-flag + offline story.
        maybe_force_avx2(dest)
        for src in (repo_id,):
            try:
                print(f"[bench] trying src={src}", flush=True)
                out = bench(src, args.wav)
                print("[result] " + json.dumps(out, ensure_ascii=False), flush=True)
                break
            except Exception as e:  # noqa: BLE001
                print(f"[warn] src {src} failed: {type(e).__name__}: {e}", flush=True)
        else:
            print(f"[err] all sources failed for {name}")
            return 4
    return 0


if __name__ == "__main__":
    sys.exit(main())
