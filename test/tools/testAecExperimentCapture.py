import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import unittest
from unittest import mock

_path = Path(__file__).resolve().parents[2] / "tools/audio/aec_experiment_capture.py"
_spec = importlib.util.spec_from_file_location("aec_capture", _path)
capture = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(capture)


class AecCaptureTest(unittest.TestCase):
    def run_capture(self, condition="speaker", response=b"AEC capture scheduled:", rate=16000,
                    frames=32000, elapsed=9, token=None, diagnostic=False, speech_prompt=None):
        source = mock.MagicMock()
        source.getnchannels.return_value = 1
        source.getsampwidth.return_value = 2
        source.getframerate.return_value = rate
        source.getcomptype.return_value = "NONE"
        source.getnframes.return_value = frames
        sdk = mock.MagicMock()
        robot = sdk.Robot.return_value.__enter__.return_value
        network = mock.MagicMock()
        network.__enter__.return_value.read.return_value = response
        if diagnostic:
            network.__enter__.return_value.read.side_effect = [
                ("AEC_DIAGNOSTIC " + json.dumps({"revision": 110, "schema": 1, "mode": 1,
                 "os_version": "3.0.1.111d", "trial": "same-trial", "state": state, "errors": 0,
                 "write_complete": state == "saved",
                 "path": "/diagnostic/same-trial"})).encode() for state in ("idle", "active", "saved")]
        argv = ["capture", "--robot-ip", "192.0.2.1", "--serial", "test", "--wav", "fixture.wav",
                "--condition", condition]
        if token is not None:
            argv.append("--token-stdin")
        if diagnostic:
            argv.append("--diagnostic")
        if speech_prompt is not None:
            argv.extend(["--speech-prompt", speech_prompt])
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(sys, "argv", argv))
            if token is not None:
                stack.enter_context(mock.patch.object(sys, "stdin", io.StringIO(token)))
            stack.enter_context(mock.patch.dict(sys.modules, {"anki_vector": sdk}))
            stack.enter_context(mock.patch.object(capture.wave, "open", return_value=source))
            source.__enter__.return_value = source
            stack.enter_context(mock.patch("builtins.open", mock.mock_open(read_data=b"fixture")))
            request = stack.enter_context(mock.patch.object(capture.urllib.request, "urlopen", return_value=network))
            stack.enter_context(mock.patch.object(capture.time, "monotonic", side_effect=[0, elapsed]))
            stack.enter_context(mock.patch.object(capture.time, "sleep"))
            stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
            stack.enter_context(contextlib.redirect_stderr(io.StringIO()))
            capture.main()
            sdk.Robot.assert_called_once_with(serial="test", ip="192.0.2.1",
                                             config={} if token is None else {"guid": token.strip()},
                                             cache_animation_lists=False)
        return robot, request

    def test_fixed_fixture_is_repeated_three_times(self):
        for condition in ("speaker", "double"):
            robot, request = self.run_capture(condition=condition)
            self.assertEqual([mock.call("fixture.wav", 50)] * 3,
                             robot.audio.stream_wav_file.call_args_list)
            self.assertIn(b"AecExperimentCapture", request.call_args[0][0].data)

    def test_near_control_does_not_play_robot_audio(self):
        robot, _ = self.run_capture(condition="near")
        robot.audio.stream_wav_file.assert_not_called()

    def test_custom_speech_prompt_is_a_human_cue_only(self):
        with mock.patch("builtins.print") as output:
            robot, _ = self.run_capture(condition="near", speech_prompt="What's the weather tomorrow?")
        robot.audio.stream_wav_file.assert_not_called()
        cues = [call.args[0] for call in output.call_args_list
                if call.args and "SAY:" in str(call.args[0])]
        self.assertEqual(3, len(cues))
        self.assertTrue(all("SAY: What's the weather tomorrow?" in cue for cue in cues))

    def test_diagnostic_explicitly_checks_capability_and_writer_commit(self):
        robot, request = self.run_capture(diagnostic=True, frames=160000)
        self.assertEqual(1, robot.audio.stream_wav_file.call_count)
        self.assertEqual(3, request.call_count)
        self.assertIn(b"AecDiagnosticStatus", request.call_args_list[0][0][0].data)
        self.assertIn(b"AecDiagnosticCapture", request.call_args_list[1][0][0].data)
        self.assertIn(b"AecDiagnosticStatus", request.call_args_list[2][0][0].data)

    def test_temporary_token_from_stdin(self):
        self.run_capture(token="test-only-token\n")

    def test_empty_temporary_token_is_rejected(self):
        with self.assertRaises(SystemExit):
            self.run_capture(token="\n")

    def test_capture_rejection_is_not_success(self):
        with self.assertRaisesRegex(RuntimeError, "did not accept"):
            self.run_capture(response=b"ERROR: capture already running")

    def test_overrun_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "overran"):
            self.run_capture(elapsed=15)

    def test_invalid_fixture_is_rejected(self):
        with self.assertRaises(SystemExit):
            self.run_capture(rate=48000)
        with self.assertRaises(SystemExit):
            self.run_capture(frames=64000)


if __name__ == "__main__":
    unittest.main()
