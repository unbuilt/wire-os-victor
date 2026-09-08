#!/usr/bin/env python3
"""Transcribe short processed AEC recordings on CPU using local Whisper weights."""

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import wave

import numpy as np


def load_recording(path):
    data = path.read_bytes()
    with wave.open(io.BytesIO(data), "rb") as source:
        if (source.getnchannels(), source.getsampwidth(), source.getframerate(),
                source.getcomptype()) != (1, 2, 16000, "NONE"):
            raise ValueError(str(path) + ": requires processed mono PCM16 at 16000 Hz")
        frames = source.getnframes()
        if not 0 < frames <= 30 * 16000:
            raise ValueError(str(path) + ": requires more than zero and at most 30 seconds")
        pcm = source.readframes(frames + 1)
        if len(pcm) != frames * 2:
            raise ValueError(str(path) + ": truncated or partial-frame PCM payload")
    samples = np.frombuffer(pcm, dtype="<i2").astype(np.float32) / 32768.0
    return samples, {"path": str(path), "sha256": hashlib.sha256(data).hexdigest(),
                     "frames": frames, "sample_rate": 16000,
                     "nominal_seconds": frames / 16000}


def model_hashes(directory):
    if not (directory / "model.safetensors").is_file():
        raise ValueError("Local model.safetensors is required; no downloads or pickle weights")
    hashes = {}
    for path in sorted(directory.iterdir()):
        if path.is_file() and path.suffix in (".json", ".txt", ".safetensors"):
            digest = hashlib.sha256()
            with path.open("rb") as source:
                for chunk in iter(lambda: source.read(1024 * 1024), b""):
                    digest.update(chunk)
            hashes[path.name] = digest.hexdigest()
    return hashes


def transcribe(model_directory, paths, threads=2):
    if not 1 <= threads <= 8:
        raise ValueError("CPU threads must be 1..8")
    recordings = [load_recording(path) for path in paths]
    if not recordings:
        raise ValueError("At least one recording is required")
    hashes = model_hashes(model_directory)
    # Enforce offline loading before importing the optional inference libraries.
    os.environ["HF_HUB_OFFLINE"] = "1"
    os.environ["TRANSFORMERS_OFFLINE"] = "1"
    os.environ["HF_HUB_DISABLE_TELEMETRY"] = "1"
    import torch
    import transformers
    from transformers import WhisperForConditionalGeneration, WhisperProcessor

    if not transformers.utils.is_safetensors_available():
        raise RuntimeError("safetensors is required; refusing a pickle-loader fallback")
    torch.set_num_threads(threads)
    processor = WhisperProcessor.from_pretrained(str(model_directory), local_files_only=True)
    model = WhisperForConditionalGeneration.from_pretrained(
        str(model_directory), local_files_only=True, use_safetensors=True,
        torch_dtype=torch.float32)
    model.to("cpu")
    model.eval()
    results = []
    for samples, metadata in recordings:
        features = processor(samples, sampling_rate=16000, return_tensors="pt").input_features
        with torch.inference_mode():
            tokens = model.generate(features, do_sample=False, num_beams=1,
                                    max_new_tokens=256)
        if tokens[0, -1].item() != model.config.eos_token_id:
            raise RuntimeError(metadata["path"] + ": decoding did not finish within token limit")
        metadata["transcript"] = processor.batch_decode(tokens, skip_special_tokens=True)[0].strip()
        results.append(metadata)
    return {
        "schema": 1, "model_directory": str(model_directory), "model_sha256": hashes,
        "versions": {"torch": torch.__version__, "transformers": transformers.__version__},
        "device": "cpu", "threads": threads, "decoding": "greedy",
        "expected_phrase_prompt": None, "vad": False, "audio_uploaded": False,
        "limitations": [
            "Recognizer hypotheses are not verified ground truth or a word-error-rate measurement.",
            "No VAD or speaker attribution: silence and residual robot speech can produce text.",
            "Saved-file recognition does not establish live streaming, barge-in, or AEC-ON efficacy."
        ],
        "recordings": results
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-dir", required=True, type=Path,
                        help="Existing local Whisper snapshot with safetensors; never downloaded here")
    parser.add_argument("--output", required=True, type=Path, help="New JSON report; never overwritten")
    parser.add_argument("--threads", type=int, default=2, help="CPU threads, 1..8 (default 2)")
    parser.add_argument("wav", type=Path, nargs="+", help="Processed mono PCM16 16 kHz WAVs, up to 30 s each")
    args = parser.parse_args()
    if args.output.exists():
        parser.error("Output already exists; choose a new report path")
    report = transcribe(args.model_dir, args.wav, args.threads)
    with args.output.open("x") as output:
        json.dump(report, output, indent=2)
        output.write("\n")
    for recording in report["recordings"]:
        print("{}: {}".format(recording["path"], recording["transcript"]))
    print("Offline hypotheses saved to " + str(args.output) + "; not a live recognition result.")


if __name__ == "__main__":
    main()
