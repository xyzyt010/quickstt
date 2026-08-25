#!/usr/bin/env python3
"""Download Handy's Nemotron 3.5 ASR Streaming 0.6B GGUF from Hugging Face.

This is the EXACT model Handy ships:
  repo: handy-computer/nemotron-3.5-asr-streaming-0.6b-gguf
  runtime: transcribe.cpp (tools/nemotron/transcribe.dll + ggml)

Default quant: Q8_0 (716 MB) — Handy's accuracy/size sweet spot.

Usage:
  python fetch_and_convert.py --dest <install_dir>
  python fetch_and_convert.py --dest %LOCALAPPDATA%\\QuickSTT\\models\\nemotron\\nemotron-3.5-asr-streaming-0.6b

Progress lines (for LocalModelManager):
  PROGRESS <0-100> <message>
  STATUS <message>
  ERROR <message>
  DONE <dest>
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO_ID = "handy-computer/nemotron-3.5-asr-streaming-0.6b-gguf"
DEFAULT_QUANT = "Q8_0"
FILENAME_TMPL = "nemotron-3.5-asr-streaming-0.6b-{quant}.gguf"
HF_URL_TMPL = (
    f"https://huggingface.co/{REPO_ID}/resolve/main/" + FILENAME_TMPL
)


def emit(kind: str, msg: str) -> None:
    print(f"{kind} {msg}", flush=True)


def progress(pct: int, msg: str) -> None:
    pct = max(0, min(100, int(pct)))
    emit("PROGRESS", f"{pct} {msg}")


def status(msg: str) -> None:
    emit("STATUS", msg)


def die(msg: str, code: int = 1) -> None:
    emit("ERROR", msg)
    sys.exit(code)


def already_installed(dest: Path, filename: str) -> bool:
    gguf = dest / filename
    return gguf.is_file() and gguf.stat().st_size > 50_000_000


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Download Handy Nemotron 3.5 ASR Streaming GGUF for QuickSTT"
    )
    parser.add_argument("--dest", required=True, help="Install directory")
    parser.add_argument(
        "--quant",
        default=DEFAULT_QUANT,
        choices=["F32", "F16", "Q8_0", "Q6_K", "Q5_K_M", "Q4_K_M"],
        help=f"GGUF quant (default {DEFAULT_QUANT})",
    )
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    dest = Path(args.dest).expanduser().resolve()
    dest.mkdir(parents=True, exist_ok=True)
    filename = FILENAME_TMPL.format(quant=args.quant)
    target = dest / filename

    if already_installed(dest, filename) and not args.force:
        status(f"Already installed at {target}")
        progress(100, "Already installed")
        emit("DONE", str(dest))
        return 0

    try:
        from huggingface_hub import hf_hub_download
    except ImportError:
        die("Missing huggingface_hub. Install with: pip install huggingface_hub")

    progress(5, f"Downloading {filename} from Hugging Face ({REPO_ID})")
    status(f"Fetching {filename} …")

    try:
        path = hf_hub_download(
            repo_id=REPO_ID,
            filename=filename,
            local_dir=str(dest),
            local_dir_use_symlinks=False,
            resume_download=True,
        )
    except Exception as ex:
        die(f"Download failed: {ex}")

    gguf = Path(path)
    if not gguf.is_file() or gguf.stat().st_size < 50_000_000:
        die(f"Downloaded file looks invalid: {gguf}")

    # Ensure canonical name in dest root (hf may nest).
    if gguf.resolve() != target.resolve():
        if target.exists():
            target.unlink()
        gguf.replace(target)
        gguf = target

    size_mb = gguf.stat().st_size / (1024 ** 2)
    (dest / "SOURCE.txt").write_text(
        f"repo={REPO_ID}\n"
        f"file={filename}\n"
        f"url={HF_URL_TMPL.format(quant=args.quant)}\n"
        f"runtime=transcribe.cpp (Handy)\n"
        f"base_model=nvidia/nemotron-3.5-asr-streaming-0.6b\n",
        encoding="utf-8",
    )

    progress(100, f"Installed {filename} ({size_mb:.0f} MB)")
    status(f"Installed to {dest}")
    emit("DONE", str(dest))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        die("Interrupted", 130)
