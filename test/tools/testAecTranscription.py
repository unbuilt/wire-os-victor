import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import wave

import numpy as np


path = Path(__file__).resolve().parents[2] / "tools/audio/transcribe_aec_recordings.py"
spec = importlib.util.spec_from_file_location("aec_transcription", path)
transcription = importlib.util.module_from_spec(spec)
spec.loader.exec_module(transcription)


class AecTranscriptionTest(unittest.TestCase):
    def recording(self, channels=1, rate=16000, samples=None):
        if samples is None:
            samples = np.array([-32768, 0, 32767], dtype="<i2")
        buffer = io.BytesIO()
        with wave.open(buffer, "wb") as output:
            output.setnchannels(channels)
            output.setsampwidth(2)
            output.setframerate(rate)
            output.writeframes(samples.tobytes())
        path = mock.Mock()
        path.read_bytes.return_value = buffer.getvalue()
        return path

    def test_pcm_scaling_and_metadata(self):
        samples, metadata = transcription.load_recording(self.recording())
        np.testing.assert_array_equal(samples, [-1, 0, 32767 / 32768])
        self.assertEqual(3, metadata["frames"])
        self.assertEqual(64, len(metadata["sha256"]))

    def test_wrong_format_and_empty_or_long_audio_are_rejected(self):
        for path in (self.recording(channels=3), self.recording(rate=15625),
                     self.recording(samples=np.zeros(0, dtype="<i2")),
                     self.recording(samples=np.zeros(480001, dtype="<i2"))):
            with self.assertRaises(ValueError):
                transcription.load_recording(path)

    def test_truncated_payload_is_rejected(self):
        path = self.recording()
        path.read_bytes.return_value = path.read_bytes.return_value[:-1]
        with self.assertRaisesRegex(ValueError, "truncated"):
            transcription.load_recording(path)

    def test_declared_partial_frame_is_not_silently_discarded(self):
        path = self.recording()
        data = bytearray(path.read_bytes.return_value)
        data.extend(b"\x00")
        data[4:8] = (len(data) - 8).to_bytes(4, "little")
        data[40:44] = (7).to_bytes(4, "little")
        path.read_bytes.return_value = bytes(data)
        with self.assertRaisesRegex(ValueError, "partial-frame"):
            transcription.load_recording(path)

    def test_empty_batch_and_invalid_threads_fail_before_loading_models(self):
        with mock.patch.object(transcription, "model_hashes") as hashes:
            for threads in (0, 9):
                with self.assertRaisesRegex(ValueError, "threads"):
                    transcription.transcribe(Path("model"), [], threads=threads)
            with self.assertRaisesRegex(ValueError, "At least one"):
                transcription.transcribe(Path("model"), [])
            hashes.assert_not_called()

    def test_local_safe_weights_are_required_and_hashed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            with self.assertRaisesRegex(ValueError, "safetensors"):
                transcription.model_hashes(path)
            (path / "model.safetensors").write_bytes(b"test-only")
            self.assertEqual(64, len(transcription.model_hashes(path)["model.safetensors"]))

    def test_offline_greedy_unprompted_decode_and_truncation(self):
        torch, transformers = mock.MagicMock(), mock.MagicMock()
        torch.__version__, transformers.__version__ = "test", "test"
        model = transformers.WhisperForConditionalGeneration.from_pretrained.return_value
        processor = transformers.WhisperProcessor.from_pretrained.return_value
        model.config.eos_token_id = 50256
        model.generate.return_value.__getitem__.return_value.item.return_value = 50256
        processor.batch_decode.return_value = [" Recognizer hypothesis. "]
        metadata = {"path": "test.wav"}
        with mock.patch.dict(sys.modules, {"torch": torch, "transformers": transformers}), \
                mock.patch.dict(transcription.os.environ, {}), \
                mock.patch.object(transcription, "model_hashes", return_value={"weights": "hash"}), \
                mock.patch.object(transcription, "load_recording",
                                  return_value=(np.zeros(160), metadata)):
            result = transcription.transcribe(Path("local-model"), [Path("test.wav")])
            self.assertEqual("1", transcription.os.environ["HF_HUB_OFFLINE"])
            self.assertEqual("Recognizer hypothesis.", result["recordings"][0]["transcript"])
            self.assertFalse(result["audio_uploaded"])
            self.assertIsNone(result["expected_phrase_prompt"])
            transformers.WhisperProcessor.from_pretrained.assert_called_once_with(
                "local-model", local_files_only=True)
            transformers.WhisperForConditionalGeneration.from_pretrained.assert_called_once_with(
                "local-model", local_files_only=True, use_safetensors=True,
                torch_dtype=torch.float32)
            self.assertEqual({"do_sample": False, "num_beams": 1, "max_new_tokens": 256},
                             model.generate.call_args.kwargs)
            model.generate.return_value.__getitem__.return_value.item.return_value = 123
            with self.assertRaisesRegex(RuntimeError, "token limit"):
                transcription.transcribe(Path("local-model"), [Path("test.wav")])
            transformers.utils.is_safetensors_available.return_value = False
            calls = transformers.WhisperForConditionalGeneration.from_pretrained.call_count
            with self.assertRaisesRegex(RuntimeError, "pickle-loader fallback"):
                transcription.transcribe(Path("local-model"), [Path("test.wav")])
            self.assertEqual(calls,
                             transformers.WhisperForConditionalGeneration.from_pretrained.call_count)

    def test_existing_report_is_not_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "report.json"
            output.write_text("original")
            with mock.patch.object(sys, "argv", ["transcribe", "--model-dir", directory,
                                                "--output", str(output), "test.wav"]), \
                    mock.patch.object(transcription, "transcribe") as run, \
                    mock.patch("sys.stderr", io.StringIO()):
                with self.assertRaises(SystemExit):
                    transcription.main()
            run.assert_not_called()
            self.assertEqual("original", output.read_text())


if __name__ == "__main__":
    unittest.main()
