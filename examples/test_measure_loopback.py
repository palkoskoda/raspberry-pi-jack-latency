# SPDX-License-Identifier: MIT
import contextlib
import io
import json
from pathlib import Path
from struct import pack_into
import tempfile
import unittest
import wave

from measure_loopback import main, measure, nearest_rank


class LoopbackTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.path = Path(directory.name) / "capture.wav"

    def write_capture(self, left, right, frames=256, rate=1000):
        samples = bytearray(frames * 4)
        for channel, signal in enumerate((left, right)):
            for frame, amplitude in signal.items():
                pack_into("<h", samples, frame * 4 + channel * 2, amplitude)
        with wave.open(str(self.path), "wb") as recording:
            recording.setparams((2, 2, rate, frames, "NONE", ""))
            recording.writeframes(samples)

    def test_both_signs_and_nearest_rank(self):
        self.write_capture({10: 8000, 100: -8000}, {13: -32768, 107: 5000})
        result = measure(self.path, expected_triggers=2)
        self.assertTrue(result["complete"])
        self.assertEqual(
            result["summary_ms"], {"min": 3, "median": 3, "p95": 7, "p99": 7, "max": 7}
        )
        self.assertEqual([trial["delay_ms"] for trial in result["trials"]], [3, 7])
        self.assertEqual(nearest_rank(list(range(1, 21)), 95), 19)

    def test_rearm_requires_uninterrupted_quiet(self):
        # Threshold recrossings and short silences are part of one output attack.
        self.write_capture({10: 8000}, {13: 4000, 16: 800, 20: 4000, 24: -800})
        result = measure(self.path)
        self.assertTrue(result["complete"])
        self.assertEqual(result["matched_triggers"], 1)
        self.assertEqual(result["output_onsets_without_match"], 0)

    def test_reports_overlapping_trigger_windows(self):
        self.write_capture({10: 8000, 40: -8000}, {15: 4000, 45: -4000})
        result = measure(self.path, max_ms=35)
        self.assertFalse(result["complete"])
        self.assertEqual([trial["problem"] for trial in result["trials"]], [
            "overlapping_trigger_windows", "overlapping_trigger_windows"
        ])

    def test_reports_multiple_missing_busy_and_incomplete_separately(self):
        self.write_capture(
            {10: 8000, 75: 8000, 130: 8000, 241: 8000},
            {12: 4000, 30: -4000, 128: 4000, 132: -4000, 246: 4000},
        )
        result = measure(self.path, max_ms=20)
        self.assertFalse(result["complete"])
        self.assertEqual(result["trials"][0]["candidate_frames"], [12, 30])
        self.assertEqual([trial["problem"] for trial in result["trials"]], [
            "multiple_output_onsets", "missing_output", "output_already_active",
            "recording_ended_inside_window",
        ])
        self.assertIsNone(result["summary_ms"])

    def test_rounds_search_bounds_inward_and_includes_both_ends(self):
        self.write_capture(
            {100: 8000, 300: 8000}, {104: 4000, 309: 4000}, frames=400, rate=2000
        )
        result = measure(self.path, min_ms=1.9, max_ms=4.6)
        self.assertTrue(result["complete"])
        self.assertEqual(result["settings"]["search_offsets_frames"], [4, 9])
        self.assertEqual([trial["delay_ms"] for trial in result["trials"]], [2, 4.5])

    def test_handles_pairs_that_straddle_read_chunks(self):
        self.write_capture({8190: 8000}, {8194: -4000}, frames=9000, rate=48000)
        result = measure(self.path, max_ms=1)
        self.assertTrue(result["complete"])
        self.assertAlmostEqual(result["trials"][0]["delay_ms"], 1 / 12)

    def test_measures_signed_channel_skew_and_applies_correction_to_window(self):
        self.write_capture({10: 8000, 100: 8000}, {9: 4000, 99: 4000})
        calibration = measure(self.path, min_ms=-2, max_ms=2)
        self.assertTrue(calibration["complete"])
        self.assertEqual(calibration["summary_ms"]["median"], -1)
        result = measure(self.path, min_ms=0, max_ms=0, channel_skew_frames=-1)
        self.assertTrue(result["complete"])
        self.assertEqual(result["trials"][0]["delay_frames"], 0)
        self.assertEqual(result["settings"]["search_offsets_frames"], [-1, -1])

    def test_negative_window_requires_leading_recording(self):
        self.write_capture({0: 8000}, {1: 4000})
        result = measure(self.path, min_ms=-2, max_ms=2)
        self.assertFalse(result["complete"])
        self.assertEqual(result["trials"][0]["problem"], "recording_started_inside_window")

    def test_expected_count_mismatch_preserves_partial_statistics(self):
        self.write_capture({10: 8000}, {13: 4000})
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            exit_status = main([str(self.path), "--expected-triggers", "2"])
        result = json.loads(output.getvalue())
        self.assertEqual(exit_status, 1)
        self.assertEqual(result["matched_triggers"], 1)
        self.assertEqual(result["summary_ms"]["p95"], 3)

    def test_no_triggers_and_invalid_channels(self):
        self.write_capture({}, {})
        result = measure(self.path)
        self.assertFalse(result["complete"])
        self.assertEqual(result["detected_triggers"], 0)
        with wave.open(str(self.path), "wb") as recording:
            recording.setparams((1, 2, 44100, 0, "NONE", ""))
        with self.assertRaisesRegex(ValueError, "expected stereo"):
            measure(self.path)

    def test_rejects_truncated_wave(self):
        self.write_capture({10: 8000}, {13: 4000})
        self.path.write_bytes(self.path.read_bytes()[:-4])
        with self.assertRaisesRegex(ValueError, "declared frame count"):
            measure(self.path)


if __name__ == "__main__":
    unittest.main()
