import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location(
    "aec_timing", Path(__file__).resolve().parents[2] / "tools/audio/analyze_aec_timing.py")
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)


class AecTimingAnalysisTest(unittest.TestCase):
    def test_source_cadence_and_aggregate_coverage(self):
        rows = analysis.journal_stats("""
        AEC_EXPERIMENT.Stats: (tc100) blocks=500 valid=436 missing_samples=3686
        AEC_EXPERIMENT.ReferenceInvalid: ignored
        AEC_EXPERIMENT.Stats: (tc255) blocks=500 valid=435 missing_samples=3823
        """)
        summary = analysis.summarize(rows)
        self.assertEqual(2, summary["windows"])
        self.assertEqual(15625, summary["nominal_divider_mic_rate_hz"])
        self.assertAlmostEqual(10.24, summary["nominal_divider_block_ms"])
        self.assertAlmostEqual(87.1, summary["complete_reference_percent"])
        self.assertAlmostEqual(10.23, summary["coarse_anim_tick_ms_per_block"])
        self.assertAlmostEqual(4.2666666667, summary["legacy_100ms_reset_period_seconds_unbatched"])

    def test_new_cpu_fields_and_negative_phase_survive(self):
        rows = analysis.journal_stats(
            "AEC_EXPERIMENT.Stats: blocks=500 valid=500 missing_samples=0 "
            "mic_residual_us=-12 max_se_cpu_us=1234 max_se_wall_us=22000 ref_mean_square=0.1")
        self.assertEqual(-12, rows[0]["mic_residual_us"])
        self.assertEqual(1234, rows[0]["max_se_cpu_us"])
        self.assertEqual(100, analysis.summarize(rows)["complete_reference_percent"])

    def test_recorded_107_coverage_is_not_alignment(self):
        rows = analysis.journal_stats("""
        AEC_EXPERIMENT.Stats: blocks=500 valid=500 missing_samples=0 timing_invalid=500 mic_clock_fault=1 ref_clock_fault=1 mic_drift_us=180652 ref_drift_us=6979
        AEC_EXPERIMENT.Stats: blocks=500 valid=500 missing_samples=0 timing_invalid=500 mic_clock_fault=1 ref_clock_fault=1 mic_drift_us=183558 ref_drift_us=7426
        """)
        result = analysis.summarize(rows)
        self.assertEqual(100, result["complete_reference_percent"])
        self.assertEqual(1000, result["timing_invalid_blocks"])
        self.assertEqual(2, result["clock_fault_windows"])
        self.assertAlmostEqual(2906 / 5.12,
                               result["mic_minimum_change_per_nominal_second_ppm"]["median"])
        self.assertIn("not an oscillator measurement", result["caveat"])

    def test_source_clock_diagnostics_preserve_nanoseconds(self):
        rows = analysis.journal_stats(
            "AEC_EXPERIMENT.Clock: mic_rate_ppb=700000 source_last=4294967295 "
            "source_received_ns=123456789012345 source_errors=1 mic_window_min_us=-500",
            "AEC_EXPERIMENT.Clock:")
        self.assertEqual(123456789012345, rows[0]["source_received_ns"])
        self.assertEqual(1, rows[0]["source_errors"])
        self.assertEqual(-500, rows[0]["mic_window_min_us"])

    def test_tracking_first_fault_is_not_replaced_by_latest_window(self):
        rows = analysis.journal_stats(
            "AEC_EXPERIMENT.Tracking: revision=109 mic_fault_reason=7 "
            "mic_fault_observed_ns=123456789012345 mic_fault_predicted_ns=123456748052345 "
            "mic_fault_offset_us=40960 source_expected=4294967295 source_fault_first=7 "
            "source_fault_last=8 ref_fault_min_us=-2046",
            "AEC_EXPERIMENT.Tracking:")
        self.assertEqual(123456789012345, rows[0]["mic_fault_observed_ns"])
        self.assertEqual(4294967295, rows[0]["source_expected"])
        self.assertEqual(7, rows[0]["source_fault_first"])
        self.assertEqual(-2046, rows[0]["ref_fault_min_us"])


if __name__ == "__main__":
    unittest.main()
