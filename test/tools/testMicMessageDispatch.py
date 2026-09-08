"""Run the production mic queue bodies without constructing hardware services.

Uses generated CPPLite messages and existing GTest. Only the engine transport
and stream-display context are replaced by recording test doubles.
"""
import os
from pathlib import Path
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "animProcess/src/cozmoAnim/micData/micDataSystem.cpp"


def function_body(source, signature):
    start = source.index("{", source.index(signature))
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class MicMessageDispatchTest(unittest.TestCase):
    def run_harness(self, cheats, remove_forwarding=False):
        output = Path(os.environ.get(
            "MIC_DISPATCH_OUTPUT", ROOT / "_build/mic-message-dispatch")).resolve()
        output = output / ("missing-forwarding" if remove_forwarding else f"cheats-{cheats}")
        output.mkdir(parents=True, exist_ok=True)
        source = SOURCE.read_text()
        update = function_body(source, "void MicDataSystem::Update(")
        dispatch = update[update.index("  // Send out any messages we have to the engine"):
                          update.index("  const auto& rawBufferFullness")]
        if remove_forwarding:
            branch = """    else if (msg->tag == RobotInterface::RobotToEngine::Tag_micStreamState)
    {
      RobotInterface::SendAnimToEngine(msg->micStreamState);
    }
"""
            self.assertEqual(1, dispatch.count(branch))
            dispatch = dispatch.replace(branch, "")
        bodies = []
        for signature in (
                "void MicDataSystem::SendMicStreamState(uint32_t streamId, bool open)",
                "void MicDataSystem::SendMessageToEngine(std::unique_ptr<RobotInterface::RobotToEngine> msgPtr)"):
            bodies.append(signature + "\n" + function_body(source, signature))
        bodies.append("void MicDataSystem::Dispatch()\n{\n" + dispatch + "\n}")
        (output / "micMessageDispatchProduction.inc").write_text("\n".join(bodies))
        binary = output / "test-mic-message-dispatch"
        command = [
            "g++", "-std=c++14", "-pthread", "-g", "-O1", "-pipe",
            f"-DANKI_DEV_CHEATS={cheats}", "-I.", "-Igenerated/clad/robot",
            "-Igenerated/clad/util", "-Igenerated/coretech/vision",
            "-Ilib/das-client/testing/gtest/include", "-Ilib/das-client/testing/gtest",
            "-I" + str(output), "test/tools/fixtures/testMicMessageDispatch.cpp",
            "lib/das-client/testing/gtest/src/gtest-all.cc",
            "lib/das-client/testing/gtest/src/gtest_main.cc", "-o", str(binary)]
        environment = dict(os.environ, TMPDIR=str(output))
        build = subprocess.run(command, cwd=ROOT, env=environment,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        (output / "build.log").write_text(build.stdout)
        self.assertEqual(0, build.returncode, build.stdout)
        result = subprocess.run([str(binary)], cwd=ROOT, env=environment,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        (output / "results.log").write_text(result.stdout)
        return result

    def test_queued_dispatch_and_follow_up(self):
        for cheats in (0, 1):
            with self.subTest(cheats=cheats):
                result = self.run_harness(cheats)
                self.assertEqual(0, result.returncode, result.stdout)

    def test_regression_detects_missing_forwarding(self):
        result = self.run_harness(0, remove_forwarding=True)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("[  FAILED  ] MicMessageDispatch.SuccessfulAnswerQuiescesAndOpensFollowUp",
                      result.stdout)


if __name__ == "__main__":
    unittest.main()
