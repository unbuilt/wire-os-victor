"""Compile production capture start/stop and listening behavior bodies with GTest."""
import os
from pathlib import Path
import subprocess
import unittest

from testMicMessageDispatch import ROOT, SOURCE, function_body


class FollowUpTimingTest(unittest.TestCase):
    def test_capture_boundary_and_readiness(self):
        output = Path(os.environ.get(
            "FOLLOWUP_TIMING_OUTPUT", ROOT / "_build/followup-timing")).resolve() / "capture"
        output.mkdir(parents=True, exist_ok=True)
        source = (ROOT / "animProcess/src/cozmoAnim/micData/micDataInfo.cpp").read_text()
        bodies = ['#include <cstddef>', '#include "cozmoAnim/micData/micDataInfo.h"',
                  "namespace Anki { namespace Vector { namespace MicData {"]
        # Disk/FFT services are not part of this test. Compile unchanged bodies
        # of the actual collector and readiness methods against its real header.
        for signature in (
                "void MicDataInfo::CollectRawAudio(",
                "void MicDataInfo::CollectProcessedAudio(",
                "bool MicDataInfo::HasCapturedAudio(",
                "AudioUtil::AudioChunkList MicDataInfo::GetProcessedAudio(",
                "void MicDataInfo::StopCollecting(",
                "bool MicDataInfo::CheckDone(",
                "void MicDataInfo::EnableDataCollect("):
            start = source.index(signature)
            body_start = source.index("{", start)
            bodies.append(source[start:body_start] + function_body(source, signature))
        bodies.append("}}}")
        production = output / "captureProduction.cpp"
        production.write_text("\n".join(bodies))
        binary = output / "test-capture"
        command = [
            "g++", "-std=c++14", "-pthread", "-pipe", "-g", "-O1",
            "-fsanitize=address,undefined", "-I.", "-IanimProcess/src",
            "-Ilib/micData", "-Ilib/util/source/anki", "-Ilib/util/source/3rd/jsoncpp",
            "-Igenerated/clad/engine", "-Igenerated/clad/util",
            "-Ivictor-clad/tools/message-buffers/support/cpp/include",
            "-Ilib/das-client/testing/gtest/include", "-Ilib/das-client/testing/gtest",
            "test/animProcess/testMicCaptureBoundary.cpp", str(production),
            "lib/das-client/testing/gtest/src/gtest-all.cc",
            "lib/das-client/testing/gtest/src/gtest_main.cc", "-o", str(binary)]
        env = dict(os.environ, TMPDIR=str(output))
        for name, cmd in (("build", command), ("results", [str(binary)])):
            result = subprocess.run(cmd, cwd=ROOT, env=env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            (output / (name + ".log")).write_text(result.stdout)
            self.assertEqual(0, result.returncode, result.stdout)

    def test_production_timing(self):
        output = Path(os.environ.get(
            "FOLLOWUP_TIMING_OUTPUT", ROOT / "_build/followup-timing")).resolve()
        output.mkdir(parents=True, exist_ok=True)
        source = SOURCE.read_text()
        bodies = []
        for signature in (
                "void MicDataSystem::StartWakeWordlessStreaming(",
                "void MicDataSystem::StopWakeWordlessStreaming(",
                "void MicDataSystem::ClearCurrentStreamingJob(",
                "void MicDataSystem::AddMicDataJob(",
                "void MicDataSystem::NotifyCaptureStarted("):
            start = source.index(signature)
            body_start = source.index("{", start)
            bodies.append(source[start:body_start] + function_body(source, signature))
        update = function_body(source, "void MicDataSystem::Update(")
        start = update.index("    if (!_currentlyStreaming && HasStreamingJob()")
        end = update.index("    // ... this block is where we actually do the streaming")
        bodies.append("void MicDataSystem::StartReadyStream(uint64_t currTime_nanosec) {\n" +
                      update[start:end] + "\n}")
        behavior = (ROOT / "engine/aiComponent/behaviorComponent/behaviors/"
                    "robotDrivenDialog/behaviorConversationFollowUp.cpp").read_text()
        bodies.append("void BehaviorConversationFollowUp::BehaviorUpdate()\n" +
                      function_body(behavior, "void BehaviorConversationFollowUp::BehaviorUpdate("))
        response = (ROOT / "engine/aiComponent/behaviorComponent/behaviors/"
                    "knowledgeGraph/behaviorKnowledgeGraphQuestion.cpp").read_text()
        bodies.append("void BehaviorKnowledgeGraphQuestion::PlaySuccessfulResponseGetOut()\n" +
                      function_body(response, "void BehaviorKnowledgeGraphQuestion::PlaySuccessfulResponseGetOut("))
        (output / "followUpTimingProduction.inc").write_text("\n".join(bodies))
        binary = output / "test-followup-timing"
        command = [
            "g++", "-std=c++14", "-pthread", "-pipe", "-g", "-O1",
            "-DANKI_DEV_CHEATS=0", "-I.", "-Ilib/das-client/testing/gtest/include",
            "-Ilib/das-client/testing/gtest", "-I" + str(output),
            "test/tools/fixtures/testFollowUpTiming.cpp",
            "lib/das-client/testing/gtest/src/gtest-all.cc",
            "lib/das-client/testing/gtest/src/gtest_main.cc", "-o", str(binary)]
        env = dict(os.environ, TMPDIR=str(output))
        for name, cmd in (("build", command), ("results", [str(binary)])):
            result = subprocess.run(cmd, cwd=ROOT, env=env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            (output / (name + ".log")).write_text(result.stdout)
            self.assertEqual(0, result.returncode, result.stdout)


if __name__ == "__main__":
    unittest.main()
