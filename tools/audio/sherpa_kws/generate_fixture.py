#!/usr/bin/env python3
"""Generate a local synthetic Mandarin fixture, never a human accuracy benchmark."""

import argparse
from pathlib import Path
import struct
import wave


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-dir", required=True, type=Path,
                        help="Local vits-icefall-zh-aishell3 model directory")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--text", default="你好小维。")
    parser.add_argument("--speaker", type=int, default=21)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("Output already exists")
    if not args.text.strip():
        parser.error("Text must not be empty")
    for name in ("model.onnx", "tokens.txt", "lexicon.txt"):
        if not (args.model_dir / name).is_file():
            parser.error("Missing local TTS file: " + name)
    import sherpa_onnx

    vits = sherpa_onnx.OfflineTtsVitsModelConfig(
        model=str(args.model_dir / "model.onnx"),
        tokens=str(args.model_dir / "tokens.txt"),
        lexicon=str(args.model_dir / "lexicon.txt"),
        noise_scale=0, noise_scale_w=0)
    tts = sherpa_onnx.OfflineTts(sherpa_onnx.OfflineTtsConfig(
        model=sherpa_onnx.OfflineTtsModelConfig(vits=vits, num_threads=1, provider="cpu")))
    if not 0 <= args.speaker < tts.num_speakers:
        parser.error("Speaker is out of range")
    audio = tts.generate(args.text, sid=args.speaker, speed=1.0)
    if len(audio.samples) == 0:
        raise RuntimeError("TTS returned empty audio")
    pcm = bytearray()
    for sample in audio.samples:
        pcm.extend(struct.pack("<h", max(-32768, min(32767, round(float(sample) * 32768)))))
    with args.output.open("xb") as output:
        with wave.open(output, "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(audio.sample_rate)
            wav.writeframes(pcm)
    print("Synthetic fixture:", args.output, "sherpa_onnx=" + sherpa_onnx.__version__,
          "speaker=" + str(args.speaker), "NOT human accuracy evidence")


if __name__ == "__main__":
    main()
