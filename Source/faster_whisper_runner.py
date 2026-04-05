import argparse
import sys
from pathlib import Path

from faster_whisper import WhisperModel
from faster_whisper.utils import download_model


def run_download(args: argparse.Namespace) -> int:
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    model_dir = download_model(args.model, output_dir=str(output_dir))
    print(str(Path(model_dir).resolve()), flush=True)
    return 0


def run_transcribe(args: argparse.Namespace) -> int:
    model = WhisperModel(
        args.model_dir,
        device=args.device,
        device_index=args.device_index,
        compute_type=args.compute_type,
    )
    segments, _ = model.transcribe(
        args.audio,
        language=None if args.language == "auto" else args.language,
        beam_size=1,
        vad_filter=False,
        condition_on_previous_text=False,
    )
    text = "".join(segment.text for segment in segments).strip()
    print(text, flush=True)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    download_parser = subparsers.add_parser("download")
    download_parser.add_argument("--model", required=True)
    download_parser.add_argument("--output-dir", required=True)
    download_parser.set_defaults(func=run_download)

    transcribe_parser = subparsers.add_parser("transcribe")
    transcribe_parser.add_argument("--model-dir", required=True)
    transcribe_parser.add_argument("--audio", required=True)
    transcribe_parser.add_argument("--device", default="cpu")
    transcribe_parser.add_argument("--device-index", type=int, default=0)
    transcribe_parser.add_argument("--compute-type", default="int8")
    transcribe_parser.add_argument("--language", default="auto")
    transcribe_parser.set_defaults(func=run_transcribe)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(str(exc), file=sys.stderr, flush=True)
        raise SystemExit(1)
