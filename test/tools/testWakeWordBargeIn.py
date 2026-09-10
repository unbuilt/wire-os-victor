"""Compile production animator trigger bodies with recording service doubles."""
import os
import subprocess
import unittest

from testMicMessageDispatch import ROOT, function_body


class WakeWordBargeInTest(unittest.TestCase):
    def test_protocol_round_trips(self):
        output = ROOT / "_build/wake-word-barge-in/protocol"
        output.mkdir(parents=True, exist_ok=True)
        # Compile only the renderer event from this otherwise unrelated generated
        # unit; its legacy DebugScreenMode constructor is ambiguous on host GCC.
        source = (ROOT / "generated/clad/engine/clad/robotInterface/"
                  "messageFromAnimProcess.cpp").read_text()
        event = output / "audioStreamStatusEvent.cpp"
        event.write_text(
            '#include "clad/robotInterface/messageFromAnimProcess.h"\n'
            "namespace Anki { namespace Vector {\n" +
            source[source.index("// MESSAGE AudioStreamStatusEvent"):
                   source.index("} // namespace Vector")] + "\n}}")
        binary = output / "test-conversation-protocol"
        command = [
            "g++", "-std=c++14", "-pthread", "-O0", "-ffunction-sections",
            "-fdata-sections", "-D__has_warning(x)=0",
            "-I.", "-Igenerated/clad/engine", "-Igenerated/clad/util",
            "-Igenerated/coretech/vision", "-Igenerated/coretech/common",
            "-Ilib/util/source/anki", "-Ilib/util/source/3rd/jsoncpp",
            "-Ivictor-clad/tools/message-buffers/support/cpp/include",
            "-Ilib/das-client/testing/gtest/include", "-Ilib/das-client/testing/gtest",
            "test/engine/testConversationProtocol.cpp",
            "generated/clad/engine/clad/robotInterface/messageEngineToRobot.cpp",
            "generated/clad/engine/clad/robotInterface/messageRobotToEngine.cpp",
            str(event), "generated/clad/engine/clad/audio/audioMessage.cpp",
            "generated/clad/engine/clad/types/sdkAudioTypes.cpp",
            "generated/clad/engine/clad/cloud/mic.cpp",
            "generated/clad/engine/clad/cloud/common.cpp",
            "victor-clad/tools/message-buffers/support/cpp/source/SafeMessageBuffer.cpp",
            "lib/das-client/testing/gtest/src/gtest-all.cc",
            "lib/das-client/testing/gtest/src/gtest_main.cc",
            "-Wl,--gc-sections", "-o", str(binary)]
        env = dict(os.environ, TMPDIR=str(output))
        for name, cmd in (("build", command), ("results", [str(binary)])):
            result = subprocess.run(cmd, cwd=ROOT, env=env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            (output / (name + ".log")).write_text(result.stdout)
            self.assertEqual(0, result.returncode, result.stdout)

    def test_production_trigger_and_response(self):
        output = ROOT / "_build/wake-word-barge-in"
        output.mkdir(parents=True, exist_ok=True)
        manager = ROOT / "animProcess/src/cozmoAnim/showAudioStreamStateManager"
        header = manager.with_suffix(".h").read_text()
        declaration = header[header.index("class ShowAudioStreamStateManager{"):
                             header.index("} // namespace Vector")]
        (output / "responseDeclaration.inc").write_text(declaration)
        system_header = (ROOT / "animProcess/src/cozmoAnim/micData/micDataSystem.h").read_text()
        signature = "bool HasPendingWakeWordlessStreaming() const"
        (output / "pendingCaptureAccessor.inc").write_text(
            signature + function_body(system_header, signature))
        source = manager.with_suffix(".cpp").read_text()
        bodies = []
        for signature in (
                "ShowAudioStreamStateManager::ShowAudioStreamStateManager(",
                "ShowAudioStreamStateManager::~ShowAudioStreamStateManager(",
                "void ShowAudioStreamStateManager::Update(",
                "void ShowAudioStreamStateManager::SetTriggerWordResponse(",
                "void ShowAudioStreamStateManager::SetPendingTriggerResponseWithGetIn(",
                "void ShowAudioStreamStateManager::SetPendingTriggerResponseWithoutGetIn(",
                "void ShowAudioStreamStateManager::StartTriggerResponseWithGetIn(",
                "void ShowAudioStreamStateManager::StartTriggerResponseWithoutGetIn(",
                "bool ShowAudioStreamStateManager::HasValidTriggerResponse(",
                "ShowAudioStreamStateManager::BargeInDisposition\n"
                "ShowAudioStreamStateManager::ConsumeBargeInTrigger(",
                "bool ShowAudioStreamStateManager::ShouldStreamAfterTriggerWordResponse(",
                "bool ShowAudioStreamStateManager::ShouldSimulateStreamAfterTriggerWord("):
            start = source.index(signature)
            bodies.append(source[start:source.index("{", start)] +
                          function_body(source, signature))
        processor = (ROOT / "animProcess/src/cozmoAnim/micData/micDataProcessor.cpp").read_text()
        signature = "void MicDataProcessor::TriggerWordDetectCallback("
        start = processor.index(signature)
        bodies.append("namespace MicData {\n" +
                      processor[start:processor.index("{", start)] +
                      function_body(processor, signature) + "\n}")
        (output / "responseProduction.inc").write_text("\n".join(bodies))
        env = dict(os.environ, TMPDIR=str(output))
        for cheats in (0, 1):
            binary = output / f"test-wake-word-barge-in-{cheats}"
            command = [
                "g++", "-std=c++14", "-pthread", "-pipe", "-g", "-O1",
                f"-DANKI_DEV_CHEATS={cheats}", "-DANKI_USE_SHERPA_ONNX=0",
                "-Igenerated/clad/robot", "-Igenerated/clad/util",
                "-Igenerated/coretech/vision", "-Ilib/das-client/testing/gtest/include",
                "-Ilib/das-client/testing/gtest", "-I" + str(output),
                "test/tools/fixtures/testWakeWordBargeIn.cpp",
                "lib/das-client/testing/gtest/src/gtest-all.cc",
                "lib/das-client/testing/gtest/src/gtest_main.cc", "-o", str(binary)]
            for name, cmd in (("build", command), ("results", [str(binary)])):
                result = subprocess.run(cmd, cwd=ROOT, env=env, text=True,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                (output / f"{name}-{cheats}.log").write_text(result.stdout)
                self.assertEqual(0, result.returncode, result.stdout)


if __name__ == "__main__":
    unittest.main()
