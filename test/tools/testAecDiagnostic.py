import csv
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import unittest
from unittest import mock
import wave

import numpy as np
from scipy import signal

tools = Path(__file__).resolve().parents[2] / "tools/audio"


def module(name, file):
    spec = importlib.util.spec_from_file_location(name, tools / file)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


analysis = module("diagnostic_analysis", "analyze_aec_diagnostic.py")
operator = module("diagnostic_operator", "aec_experiment_capture.py")


class DiagnosticAnalysisTest(unittest.TestCase):
    def setUp(self):
        # Existing validation runner supplies a project-local artifact directory.
        self.directory = Path(os.environ["TMPDIR"]) / ("python-diagnostic-" + self._testMethodName)
        self.directory.mkdir(exist_ok=False)
        self.addCleanup(shutil.rmtree, self.directory)

    def checksum(self, name, manifest):
        data = (self.directory / name).read_bytes()
        manifest["files"][name] = {"bytes": len(data), "fnv1a64": analysis.fnv1a64(data)}

    def save_manifest(self, manifest):
        (self.directory / "manifest.json").write_text(json.dumps(manifest))

    def fixture(self, ppm=0, delay=0.030, seconds=10):
        rng = np.random.RandomState(90210)
        reference = signal.lfilter(signal.firwin(31, 3000, fs=32000), [1],
                                   rng.normal(0, 9000, seconds * 32000))
        nmic = int((seconds - 1) * 15625) // 160 * 160
        time = 0.2 + np.arange(nmic) / 15625 * (1 + ppm / 1e6) - delay
        microphone = np.interp(time * 32000, np.arange(len(reference)), reference)
        microphone = np.column_stack([microphone * gain for gain in (0.6, 0.7, 0.8, 0.9)])
        trial, base = "synthetic-not-recorded", 10**12
        manifest = {"schema": 1, "revision": 110, "trial": trial, "mode": "reference",
                    "cancel_enabled": False, "adaptation_enabled": False, "write_complete": True,
                    "diagnostic_only": True, "errors": 0, "seconds": seconds,
                    "start_ns": base, "end_ns": base + seconds * 10**9, "files": {},
                    "mic_age_ms": 10, "ref_delay_ms": 0, "mic_history_records": 0, "reference_history_records": 0}
        for name, wav_name, csv_name, values, rate, channels, block in (
                ("mic", "raw.wav", "mic.csv", microphone, 15625, 4, 160),
                ("reference", "reference.wav", "reference.csv", reference, 32000, 1, 1024)):
            with wave.open(str(self.directory / wav_name), "wb") as audio:
                audio.setnchannels(channels)
                audio.setsampwidth(2)
                audio.setframerate(rate)
                audio.writeframes(values.astype("<i2").tobytes())
            rows = []
            for offset in range(0, len(values), block):
                count = min(block, len(values) - offset)
                observed = base + round(offset * 10**9 / rate) + (210240000 if name == "mic" else 0)
                row = {key: 0 for key in analysis.RECORD_FIELDS}
                row.update(index=10000 + offset, file_offset=offset, count=count, received_ns=observed + 1000,
                           observed_ns=observed, predicted_ns=observed, duration_ns=round(count * 10**9 / rate),
                           elapsed_ns=round(offset * 10**9 / rate), source_valid=1, reason=5, ready=0,
                           source_first=50 + 2 * offset // block, source_last=51 + 2 * offset // block,
                           trial=trial)
                rows.append(row)
            with (self.directory / csv_name).open("w", newline="") as source:
                writer = csv.DictWriter(source, fieldnames=["trial"] + sorted(analysis.RECORD_FIELDS))
                writer.writeheader()
                writer.writerows(rows)
            manifest[name + "_frames"] = len(values)
            manifest[name + "_records"] = len(rows)
        with (self.directory / "clocks.csv").open("w", newline="") as source:
            csv.writer(source).writerow(["trial", "stream"] + sorted(analysis.TRACE_FIELDS))
        for name in analysis.FILES:
            self.checksum(name, manifest)
        self.save_manifest(manifest)
        return manifest

    def test_known_delay_and_rate_difference_despite_latched_clock_fault(self):
        self.fixture(ppm=150)
        result = analysis.analyze(self.directory, search_seconds=0.5)
        self.assertEqual("already_faulted_onset_unknown", result["first_faults"]["mic"]["location"])
        good = [r for r in result["windows"] if r["status"] == "ok"]
        self.assertGreater(len(good), 30)
        # Clip start differs by 200 ms: sample-axis lag is -170 ms, not +30.
        self.assertAlmostEqual(-170, good[0]["sample_axis_lag_ms"], delta=0.3)
        self.assertAlmostEqual(30.24, good[0]["model_lag_ms"], delta=0.4)
        for trend in result["trends"]:
            self.assertEqual("ok", trend["status"])
            self.assertAlmostEqual(-150, trend["sample_axis_lag_slope_ppm"], delta=20)

    def test_repeated_peaks_silence_and_clipping_are_not_zero_lag(self):
        t = np.arange(40000)
        repeated = 3000 * np.sin(2 * np.pi * t / 1000)
        result = analysis.correlate_window(repeated[5000:15000], repeated)
        self.assertEqual("ambiguous_repeated_peaks", result["status"])
        self.assertNotIn("index", result)
        self.assertEqual("silence_or_low_energy", analysis.correlate_window(np.zeros(1000), np.zeros(2000))["status"])
        self.assertEqual("clipped", analysis.correlate_window(np.ones(1000) * 32767, repeated)["status"])

    def test_boundary_competitors_are_reported_as_ambiguous(self):
        rng = np.random.RandomState(110)
        size = int(0.75 * analysis.RATE)
        query = rng.normal(0, 2000, size)
        for boundary in (0, 3 * size):
            with self.subTest(boundary=boundary):
                search = rng.normal(0, 500, 4 * size)
                search[2 * size:3 * size] = query
                search[boundary:boundary + size] = query + rng.normal(0, 50, size)
                result = analysis.correlate_window(query, search)
                self.assertEqual("ambiguous_repeated_peaks", result["status"])
                self.assertGreater(result["second_peak_score"], 0.99)
                self.assertNotIn("index", result)

    def test_raw_adc_energy_floor_preserves_correlation_guards(self):
        rng = np.random.RandomState(111)
        reference = rng.normal(0, 500, 8000)
        query = 1024 + reference[2000:5000] / 20
        self.assertEqual("silence_or_low_energy", analysis.correlate_window(query, reference)["status"])
        result = analysis.correlate_window(query, reference, min_query_rms=12.5)
        self.assertEqual("ok", result["status"])
        self.assertEqual(2000, result["index"])
        self.assertGreater(result["score"], 0.99)
        unrelated = 1024 + rng.normal(0, 25, len(query))
        self.assertEqual("weak_correlation",
                         analysis.correlate_window(unrelated, reference, min_query_rms=12.5)["status"])
        self.assertEqual("silence_or_low_energy",
                         analysis.correlate_window(np.full(len(query), 1024.0), reference,
                                                   min_query_rms=12.5)["status"])
        repeated = np.concatenate((reference[2000:5000], reference[2000:5000], reference[2000:5000]))
        self.assertEqual("ambiguous_repeated_peaks",
                         analysis.correlate_window(query, repeated, min_query_rms=12.5)["status"])
        for floor in (0, 1001, float("nan"), float("inf")):
            with self.assertRaises(ValueError):
                analysis.correlate_window(query, reference, min_query_rms=floor)

    def test_analysis_records_explicit_energy_floor(self):
        self.fixture(seconds=2)
        result = analysis.analyze(self.directory, window_seconds=0.25, min_mic_rms_pcm=12.5)
        self.assertEqual(12.5, result["analysis_parameters"]["min_mic_rms_pcm"])
        self.assertEqual(0.25, result["analysis_parameters"]["window_seconds"])

    def test_truncation_and_incomplete_manifest_are_rejected(self):
        manifest = self.fixture(seconds=2)
        path = self.directory / "reference.wav"
        original = path.read_bytes()
        path.write_bytes(original[:-2])
        with self.assertRaisesRegex(ValueError, "checksum/length"):
            analysis.load_trial(self.directory)
        path.write_bytes(original + b"\0\0")
        self.checksum("reference.wav", manifest)
        self.save_manifest(manifest)
        with self.assertRaisesRegex(ValueError, "container length"):
            analysis.load_trial(self.directory)
        (self.directory / "manifest.json").unlink()
        with self.assertRaises(FileNotFoundError):
            analysis.load_trial(self.directory)

    def test_counter_gap_with_recomputed_checksum_is_not_bridged(self):
        manifest = self.fixture(seconds=2)
        path = self.directory / "mic.csv"
        with path.open() as source:
            rows = list(csv.DictReader(source))
        rows[2]["source_first"] = str(int(rows[2]["source_first"]) + 2)
        rows[2]["source_last"] = str(int(rows[2]["source_last"]) + 2)
        with path.open("w", newline="") as out:
            writer = csv.DictWriter(out, fieldnames=rows[0].keys())
            writer.writeheader()
            writer.writerows(rows)
        self.checksum("mic.csv", manifest)
        self.save_manifest(manifest)
        with self.assertRaisesRegex(ValueError, "Source frame gap"):
            analysis.load_trial(self.directory)
        rows[2]["source_first"] = str(int(rows[2]["source_first"]) - 2)
        rows[2]["source_last"] = str(int(rows[2]["source_last"]) - 2)
        rows[2]["index"] = str(int(rows[2]["index"]) + 1)
        with path.open("w", newline="") as out:
            writer = csv.DictWriter(out, fieldnames=rows[0].keys())
            writer.writeheader()
            writer.writerows(rows)
        self.checksum("mic.csv", manifest)
        self.save_manifest(manifest)
        with self.assertRaisesRegex(ValueError, "Sample gap"):
            analysis.load_trial(self.directory)

    def test_schema_sample_length_and_recorder_errors_rejected(self):
        manifest = self.fixture(seconds=2)
        for field, value in (("schema", 2), ("errors", 1), ("mic_frames", 1), ("cancel_enabled", True)):
            changed = dict(manifest)
            changed[field] = value
            self.save_manifest(changed)
            with self.assertRaises(ValueError):
                analysis.load_trial(self.directory)

    def test_real_cpp_writer_fixture_preserves_actual_pcm_and_trial(self):
        fixtures = sorted(Path(os.environ["TMPDIR"]).glob("diag-test-complete-*/trial_*/manifest.json"),
                          key=lambda path: path.stat().st_mtime_ns)
        self.assertTrue(fixtures, "Run existing native validation first to create the C++ writer fixture")
        manifest, streams, _, _ = analysis.load_trial(fixtures[-1].parent)
        self.assertTrue(np.all(streams["reference"][0] == 2345))
        self.assertTrue(np.all(streams["mic"][0][0] == [100, 101, 102, 103]))
        self.assertEqual(9000, streams["reference"][1][0]["index"])
        self.assertEqual(5000, streams["mic"][1][0]["index"])
        self.assertEqual("reference", manifest["mode"])

    def test_inconclusive_cli_does_not_exit_success(self):
        with mock.patch("sys.argv", ["analysis", "unused"]), \
             mock.patch.object(analysis, "analyze", return_value={"result_status": "inconclusive"}), \
             mock.patch("sys.stdout", new=io.StringIO()), mock.patch("sys.stderr", new=io.StringIO()):
            with self.assertRaises(SystemExit) as raised:
                analysis.main()
            self.assertEqual(3, raised.exception.code)

    def test_negative_search_edge_never_wraps_to_end_of_reference(self):
        manifest = self.fixture(seconds=2)
        path = self.directory / "mic.csv"
        with path.open() as source:
            rows = list(csv.DictReader(source))
        for row in rows:
            row["observed_ns"] = str(int(row["observed_ns"]) - 1500000000)
        with path.open("w", newline="") as out:
            writer = csv.DictWriter(out, fieldnames=rows[0].keys())
            writer.writeheader()
            writer.writerows(rows)
        self.checksum("mic.csv", manifest)
        self.save_manifest(manifest)
        result = analysis.analyze(self.directory, search_seconds=0.5)
        self.assertEqual("inconclusive", result["result_status"])
        self.assertTrue(all(row["status"] == "insufficient_overlap" for row in result["windows"]))
        self.assertTrue(all(row["search_ref_end"] == 0 for row in result["windows"]))


class DiagnosticOperatorTest(unittest.TestCase):
    def response(self, **changes):
        value = {"schema": 1, "revision": 110, "mode": 1, "os_version": "3.0.1.111d",
                 "trial": "test", "state": "saved", "errors": 0, "path": "/robot/trial", "write_complete": True}
        value.update(changes)
        network = mock.MagicMock()
        network.__enter__.return_value.read.return_value = ("AEC_DIAGNOSTIC " + json.dumps(value)).encode()
        return network

    def test_off_on_old_ota_and_absent_capability_are_rejected(self):
        for changes in ({"mode": 0}, {"mode": 2}, {"os_version": "3.0.1.109d"},
                        {"os_version": "3.0.1.110d"}, {"revision": 109}):
            with mock.patch.object(operator.urllib.request, "urlopen", return_value=self.response(**changes)):
                with self.assertRaisesRegex(RuntimeError, "REFERENCE"):
                    operator.diagnostic_request("192.0.2.1", "AecDiagnosticStatus")

    def test_scheduled_is_not_saved_and_trial_must_match(self):
        for changes in ({"state": "writing"}, {"trial": "different"}, {"errors": 4},
                        {"state": "failed"}, {"state": "invalid"}, {"write_complete": False}):
            with mock.patch.object(operator.urllib.request, "urlopen", return_value=self.response(**changes)), \
                 mock.patch.object(operator.time, "sleep"), mock.patch("sys.stdout", new=io.StringIO()):
                with self.assertRaises(RuntimeError):
                    operator.wait_for_diagnostic("192.0.2.1", "test")
        with mock.patch.object(operator.urllib.request, "urlopen", return_value=self.response()), \
             mock.patch("sys.stdout", new=io.StringIO()) as output:
            operator.wait_for_diagnostic("192.0.2.1", "test")
            self.assertIn("NOT an AEC comparison", output.getvalue())


if __name__ == "__main__":
    unittest.main()
