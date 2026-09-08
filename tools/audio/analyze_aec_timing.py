#!/usr/bin/env python3
"""Offline timing evidence only; never infer AEC effectiveness from a WAV header."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics
import wave


MIC_RATE = 48000000 // (16 * 2 * 96)


def journal_stats(text, marker="AEC_EXPERIMENT.Stats:"):
    rows = []
    for line in text.splitlines():
        if marker not in line:
            continue
        fields = {key: float(value) if "." in value else int(value)
                  for key, value in re.findall(r"(\w+)=(-?\d+(?:\.\d+)?)", line)}
        tick = re.search(r"\(tc(\d+)\)", line)
        if tick:
            fields["anim_tick"] = int(tick.group(1))
        rows.append(fields)
    return rows


def summarize(rows):
    blocks = sum(row["blocks"] for row in rows)
    result = {
        "windows": len(rows),
        "blocks": blocks,
        "complete_reference_percent": 100 * sum(row["valid"] for row in rows) / blocks if blocks else None,
        "missing_samples": sum(row["missing_samples"] for row in rows),
        "nominal_divider_mic_rate_hz": MIC_RATE,
        "nominal_divider_block_ms": 160000 / MIC_RATE,
        "legacy_error_us_per_block": 160000000 / MIC_RATE - 10000,
        "legacy_100ms_reset_period_seconds_unbatched": 0.1 / (1 - MIC_RATE / 16000),
        "timing_invalid_blocks": sum(row.get("timing_invalid", 0) for row in rows),
        "clock_fault_windows": sum(bool(row.get("mic_clock_fault", 0) or row.get("ref_clock_fault", 0))
                                   for row in rows),
        "caveat": "Divider rate is nominal, not an oscillator measurement. Aggregate minima cannot distinguish source loss, phase steps and oscillator drift; complete coverage is not alignment.",
    }
    for name in ("mic", "ref"):
        field = name + "_drift_us"
        changes = [(b[field] - a[field]) / (b["blocks"] * 160 / MIC_RATE)
                   for a, b in zip(rows, rows[1:]) if field in a and field in b]
        if changes:
            result[name + "_minimum_change_per_nominal_second_ppm"] = {
                "median": statistics.median(changes), "min": min(changes), "max": max(changes),
            }
    if len(rows) > 1 and all("anim_tick" in row for row in rows):
        # Anim's nominal 33 ms tick is a coarse cross-check, not a hardware clock.
        delta = rows[-1]["anim_tick"] - rows[0]["anim_tick"]
        subsequent_blocks = sum(row["blocks"] for row in rows[1:])
        result["coarse_anim_tick_ms_per_block"] = delta * 33 / subsequent_blocks
    return result


def wav_info(path):
    with wave.open(str(path)) as audio:
        info = {
            "channels": audio.getnchannels(), "frames": audio.getnframes(),
            "header_rate_hz": audio.getframerate(),
            "header_seconds": audio.getnframes() / audio.getframerate(),
        }
        if audio.getnchannels() == 4:
            info["nominal_divider_seconds_if_contiguous"] = audio.getnframes() / MIC_RATE
    info["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
    return info


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path, help="local directory containing journal.txt and WAV files")
    args = parser.parse_args()
    text = (args.capture / "journal.txt").read_text()
    rows = journal_stats(text)
    if not rows:
        parser.error("no AEC stats in journal.txt")
    print(json.dumps({"timing": summarize(rows), "windows": rows,
                      "clock_diagnostics": journal_stats(text, "AEC_EXPERIMENT.Clock:"),
                      "tracking_diagnostics": journal_stats(text, "AEC_EXPERIMENT.Tracking:"),
                      "wav": {path.name: wav_info(path) for path in sorted(args.capture.glob("*.wav"))}},
                     indent=2))


if __name__ == "__main__":
    main()
