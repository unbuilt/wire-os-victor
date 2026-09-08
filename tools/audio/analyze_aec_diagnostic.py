#!/usr/bin/env python3
"""Integrity-gated waveform/timestamp diagnostics. Never an AEC-effectiveness measurement."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import wave

import numpy as np
from scipy import signal


FILES = {"raw.wav", "reference.wav", "mic.csv", "reference.csv", "clocks.csv"}
RATE = 15625
TRACE_FIELDS = {"observed_ns", "predicted_ns", "predicted_before_ns", "elapsed_ns", "duration_ns", "residual_ns",
                "min_ns", "min_at_ns", "max_ns", "window_start_ns", "span_ns", "previous_min_ns",
                "rate_before_ppb", "rate_after_ppb", "windows", "updates", "reason", "closed", "ready"}
RECORD_FIELDS = {"index", "file_offset", "count", "received_ns", "source_first", "source_last",
                 "source_valid", "sink_errors", "ring_drops"} | TRACE_FIELDS


def fnv1a64(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return format(value, "016x")


def read_records(path, trial, history=False):
    with path.open(newline="") as source:
        reader = csv.DictReader(source)
        expected = (TRACE_FIELDS | {"trial", "stream"}) if history else (RECORD_FIELDS | {"trial"})
        if set(reader.fieldnames or []) != expected:
            raise ValueError("CSV header/schema mismatch: " + path.name)
        rows = list(reader)
    result = []
    for row in rows:
        if row.pop("trial", None) != trial or None in row:
            raise ValueError("CSV trial/schema mismatch: " + path.name)
        stream = row.pop("stream", None)
        try:
            record = {key: int(value) for key, value in row.items()}
        except (ValueError, TypeError):
            raise ValueError("Invalid numeric metadata: " + path.name)
        if history:
            if stream not in ("mic", "reference", "mic_first_fault", "reference_first_fault"):
                raise ValueError("Invalid history stream")
            record["stream"] = stream
        result.append(record)
    return result


def load_trial(directory):
    directory = Path(directory)
    manifest = json.loads((directory / "manifest.json").read_text())
    if (manifest.get("schema") != 1 or manifest.get("revision") != 110 or
            manifest.get("mode") != "reference" or manifest.get("cancel_enabled") is not False or
            manifest.get("adaptation_enabled") is not False or manifest.get("diagnostic_only") is not True or
            manifest.get("write_complete") is not True or set(manifest.get("files", {})) != FILES):
        raise ValueError("Not a complete reference-only schema-1 diagnostic trial")
    if manifest.get("errors") != 0:
        raise ValueError("Recorder integrity errors: " + str(manifest.get("errors")))
    if not 1 <= manifest["seconds"] <= 15 or manifest["end_ns"] - manifest["start_ns"] != manifest["seconds"] * 10**9:
        raise ValueError("Invalid capture interval")
    hashes = {}
    for name, expected in manifest["files"].items():
        path = directory / name
        if path.is_symlink() or path.stat().st_size > 8 * 1024 * 1024:
            raise ValueError("Unsafe/oversize artifact: " + name)
        data = path.read_bytes()
        if len(data) != expected["bytes"] or fnv1a64(data) != expected["fnv1a64"]:
            raise ValueError("Artifact checksum/length mismatch: " + name)
        hashes[name] = hashlib.sha256(data).hexdigest()
    streams = {}
    for name, wav_name, csv_name, channels, rate in (
            ("mic", "raw.wav", "mic.csv", 4, RATE),
            ("reference", "reference.wav", "reference.csv", 1, 32000)):
        with wave.open(str(directory / wav_name), "rb") as audio:
            if (audio.getnchannels(), audio.getsampwidth(), audio.getframerate(), audio.getcomptype()) != (channels, 2, rate, "NONE"):
                raise ValueError("Unexpected diagnostic WAV format; do not substitute legacy 16 kHz raw captures")
            frames = audio.getnframes()
            pcm = audio.readframes(frames)
            if len(pcm) != frames * channels * 2 or frames != manifest[name + "_frames"]:
                raise ValueError("Truncated WAV or frame-count mismatch")
            if (directory / wav_name).stat().st_size != 44 + frames * channels * 2:
                raise ValueError("Diagnostic WAV container length mismatch")
        rows = read_records(directory / csv_name, manifest["trial"])
        if not rows or len(rows) != manifest[name + "_records"]:
            raise ValueError("Missing or mismatched observation records")
        offset = 0
        previous = None
        for row in rows:
            if set(row) != RECORD_FIELDS or row["file_offset"] != offset or not 1 <= row["count"] <= 32000:
                raise ValueError("Sample metadata schema/length mismatch")
            if not manifest["start_ns"] <= row["received_ns"] < manifest["end_ns"] or row["observed_ns"] <= 0:
                raise ValueError("Invalid source observation/capture association")
            if name == "mic" and (row["count"] != 160 or row["source_valid"] != 1 or
                                  row["observed_ns"] > row["received_ns"] or
                                  (row["source_last"] - row["source_first"]) % 2**32 != 1):
                raise ValueError("Invalid transmitted microphone frame pair")
            if previous:
                if row["index"] != previous["index"] + previous["count"]:
                    raise ValueError("Sample gap: cross-gap correlation forbidden")
                if name == "mic" and (row["source_first"] - previous["source_last"]) % 2**32 != 1:
                    raise ValueError("Source frame gap: cross-gap correlation forbidden")
                observation_step = row["observed_ns"] - previous["observed_ns"]
                if observation_step < 0 or abs(observation_step - previous["count"] * 1e9 / rate) > 100_000_000:
                    raise ValueError("Observation discontinuity; cannot assume continuous acquisition")
                if row["sink_errors"] != previous["sink_errors"] or row["ring_drops"] != previous["ring_drops"]:
                    raise ValueError("Error/drop counter changed within trial")
            offset += row["count"]
            previous = row
            if row["duration_ns"] != row["count"] * (64000 if name == "mic" else 31250):
                raise ValueError("Nominal sample interval mismatch")
        if offset != frames:
            raise ValueError("PCM and observation lengths disagree")
        streams[name] = (np.frombuffer(pcm, dtype="<i2").reshape(-1, channels).astype(np.float64), rows)
    history = read_records(directory / "clocks.csv", manifest["trial"], history=True)
    for name in ("mic", "reference"):
        if sum(row["stream"] == name for row in history) != manifest[name + "_history_records"]:
            raise ValueError("Clock history length mismatch")
    return manifest, streams, history, hashes


def correlate_window(query, search, rate=RATE, min_query_rms=100.0):
    """Normalized local search; refuse silence, clipping, weak or repeated peaks."""
    if not 1 <= min_query_rms <= 1000:
        raise ValueError("Minimum microphone RMS must be 1..1000 raw PCM counts")
    if len(search) < len(query) + 4:
        return {"status": "insufficient_overlap"}
    if np.mean(np.abs(query) >= 32760) > 0.001 or np.mean(np.abs(search) >= 32760) > 0.001:
        return {"status": "clipped"}
    centered = query - np.mean(query)
    norm = np.linalg.norm(centered)
    rms = norm / np.sqrt(len(query))
    if rms < min_query_rms or np.std(search) < 100:
        return {"status": "silence_or_low_energy", "rms_pcm": float(rms)}
    n = len(query)
    sums = np.concatenate(([0.0], np.cumsum(search)))
    squares = np.concatenate(([0.0], np.cumsum(search * search)))
    energy = squares[n:] - squares[:-n] - (sums[n:] - sums[:-n])**2 / n
    scores = signal.correlate(search, centered, mode="valid", method="fft") / (
        norm * np.sqrt(np.maximum(energy, 1)))
    absolute = np.abs(scores)
    peak = int(np.argmax(absolute))
    score = float(absolute[peak])
    result = {"score": score, "polarity": int(np.sign(scores[peak])), "rms_pcm": float(rms)}
    if score < 0.35:
        return dict(result, status="weak_correlation")
    candidates = signal.find_peaks(absolute, distance=max(1, int(rate * 0.010)))[0]
    # find_peaks omits endpoints; a competing match may be clipped by either search boundary.
    candidates = np.concatenate(([0], candidates, [len(absolute) - 1]))
    alternatives = [float(absolute[i]) for i in candidates if abs(i - peak) >= rate * 0.010]
    second = max(alternatives, default=0.0)
    result["second_peak_score"] = second
    if second >= score * 0.95:
        return dict(result, status="ambiguous_repeated_peaks")
    if peak < 2 or peak >= len(scores) - 2:
        return dict(result, status="search_boundary")
    left, right = peak, peak
    while left > 0 and absolute[left - 1] >= score * 0.95:
        left -= 1
    while right + 1 < len(scores) and absolute[right + 1] >= score * 0.95:
        right += 1
    return dict(result, status="ok", index=peak,
                peak_width_ms=max(1, right - left) * 1000 / rate)


def timestamp_at(rows, offset, field):
    starts = np.array([r["file_offset"] for r in rows], dtype=float)
    position = int(np.searchsorted(starts, offset, side="right") - 1)
    if position < 0 or offset >= rows[-1]["file_offset"] + rows[-1]["count"]:
        raise ValueError("Timestamp interpolation outside recorded sample coverage")
    row = rows[position]
    nominal_ns = row["duration_ns"] / row["count"]
    scale = 1 + row["rate_after_ppb"] / 1e9 if field == "predicted_ns" else 1
    return row[field] + (offset - row["file_offset"]) * nominal_ns * scale


def analyze(directory, window_seconds=0.75, step_seconds=0.5, search_seconds=5.0,
            min_mic_rms_pcm=100.0):
    if not 0.25 <= window_seconds <= 3 or not 0.1 <= step_seconds <= 3 or not 0.1 <= search_seconds <= 5:
        raise ValueError("Window/step/search outside bounded analysis limits")
    if not 1 <= min_mic_rms_pcm <= 1000:
        raise ValueError("Minimum microphone RMS must be 1..1000 raw PCM counts")
    manifest, streams, history, hashes = load_trial(directory)
    microphone, mic_rows = streams["mic"]
    reference, ref_rows = streams["reference"]
    ref = signal.resample_poly(reference[:, 0], 125, 256)
    # Only a search prior. HAL receipt minus nominal block age is not ADC truth.
    coarse = (mic_rows[0]["observed_ns"] - 160 * 64000 - ref_rows[0]["observed_ns"]) / 1e9 * RATE
    size, step, radius = int(window_seconds * RATE), int(step_seconds * RATE), int(search_seconds * RATE)
    windows = []
    for start in range(0, len(microphone) - size + 1, step):
        center = int(round(start + coarse))
        low = min(len(ref), max(0, center - radius))
        high = max(0, min(len(ref), center + radius + size))
        for channel in range(4):
            result = correlate_window(microphone[start:start + size, channel], ref[low:high],
                                      min_query_rms=min_mic_rms_pcm)
            result.update(channel=channel, mic_offset=start, mic_index=mic_rows[0]["index"] + start,
                          time_seconds=start / RATE, search_ref_start=low, search_ref_end=high)
            if result["status"] == "ok":
                matched = low + result.pop("index")
                ref_offset = matched * 256 / 125
                midpoint_mic = start + size / 2
                midpoint_ref = ref_offset + size / 2 * 256 / 125
                if midpoint_ref >= len(reference):
                    result["status"] = "insufficient_overlap"
                else:
                    age = (manifest.get("mic_age_ms", 10) + manifest.get("ref_delay_ms", 0)) * 1e6
                    result.update(
                        ref_index=ref_rows[0]["index"] + ref_offset,
                        sample_axis_lag_ms=(start - matched) * 1000 / RATE,
                        observation_lag_ms=(timestamp_at(mic_rows, midpoint_mic, "observed_ns") -
                                            160 * 64000 - timestamp_at(ref_rows, midpoint_ref, "observed_ns")) / 1e6,
                        model_lag_ms=(timestamp_at(mic_rows, midpoint_mic, "predicted_ns") -
                                      age - timestamp_at(ref_rows, midpoint_ref, "predicted_ns")) / 1e6)
            windows.append(result)
    trends = []
    for channel in range(4):
        good = [row for row in windows if row["channel"] == channel and row["status"] == "ok"]
        if len(good) < 3 or good[-1]["time_seconds"] - good[0]["time_seconds"] < 2:
            trends.append({"channel": channel, "status": "insufficient_unambiguous_windows"})
            continue
        x = np.array([row["time_seconds"] for row in good])
        y = np.array([row["sample_axis_lag_ms"] for row in good])
        slope, intercept = np.polyfit(x, y, 1)
        residual = y - (slope * x + intercept)
        uncertainty = max(1000 / RATE, float(np.std(residual)))
        trends.append({"channel": channel, "status": "ok" if uncertainty <= 2 else "nonstationary_lag",
                       "windows": len(good),
                       "sample_axis_lag_slope_ppm": float(slope * 1000),
                       "fit_residual_ms": uncertainty,
                       "lag_slope_standard_error_ppm": uncertainty / np.sqrt(np.sum((x - np.mean(x))**2)) * 1000})
    coverage = {}
    first_faults = {}
    for name in ("mic", "reference"):
        numbers = {row["windows"] for row in history if row["stream"] == name}
        coverage[name] = {"first_window": min(numbers) if numbers else None,
                          "last_window": max(numbers) if numbers else None,
                          "contains_startup_and_independent_window": set(range(1, 14)) <= numbers}
        retained = [row for row in history if row["stream"] == name + "_first_fault"]
        if retained:
            first_faults[name] = {"location": "retained_before_capture", "record": retained[0]}
        else:
            rows = streams[name][1]
            fault = next(((i, row) for i, row in enumerate(rows) if row["reason"]), None)
            first_faults[name] = ({"location": "captured_transition" if fault[0] else "already_faulted_onset_unknown",
                                  "record": fault[1]} if fault else None)
    has_lag = any(row["status"] == "ok" for row in windows)
    has_trend = any(row["status"] == "ok" for row in trends)
    return {"schema": 1, "trial": manifest["trial"], "diagnostic_only": True, "artifact_sha256": hashes,
            "analysis_parameters": {"window_seconds": window_seconds, "step_seconds": step_seconds,
                                    "search_seconds": search_seconds, "min_mic_rms_pcm": min_mic_rms_pcm},
            "result_status": "lag_and_trend" if has_trend else "lag_only" if has_lag else "inconclusive",
            "history_coverage": coverage,
            "first_faults": first_faults,
            "windows": windows, "trends": trends, "clock_history": history,
            "captured_clock_observations": {"mic": mic_rows, "reference": ref_rows},
            "caveat": "Actual accepted PCM, not the input fixture. Lag includes clip-edge offset and acoustic/processing delay. "
                      "Positive slope means increasing sample-axis lag; it is not a physical oscillator measurement. "
                      "HAL receipt and ALSA head estimates are observations, not acquisition/DAC truth. "
                      "Peak width and fit error are diagnostic uncertainty, not calibrated confidence intervals. "
                      "Repeated peaks, silence, clipping and gaps must not be reported as zero delay or AEC success."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trial", type=Path)
    parser.add_argument("--window-seconds", type=float, default=0.75)
    parser.add_argument("--step-seconds", type=float, default=0.5)
    parser.add_argument("--search-seconds", type=float, default=5.0)
    parser.add_argument("--min-mic-rms-pcm", type=float, default=100.0,
                        help="DC-removed raw microphone RMS floor in PCM counts (1..1000); correlation guards unchanged")
    args = parser.parse_args()
    try:
        result = analyze(args.trial, args.window_seconds, args.step_seconds, args.search_seconds,
                         args.min_mic_rms_pcm)
    except (ValueError, KeyError, OSError, wave.Error) as error:
        parser.exit(2, "INVALID DIAGNOSTIC TRIAL: " + str(error) + "\n")
    print(json.dumps(result, indent=2, allow_nan=False))
    if result["result_status"] == "inconclusive":
        parser.exit(3, "NO UNAMBIGUOUS LAG: inspect window rejection reasons; this is not zero delay or success.\n")


if __name__ == "__main__":
    main()
