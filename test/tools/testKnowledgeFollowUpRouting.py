"""Exercise production UIC request/result/audio paths with recording services."""
import json
import os
from pathlib import Path
import subprocess
import unittest

from testMicMessageDispatch import ROOT, function_body


class KnowledgeFollowUpRoutingTest(unittest.TestCase):
    def test_production_routing(self):
        output = Path(os.environ.get(
            "KG_FOLLOWUP_OUTPUT", ROOT / "_build/kg-followup")).resolve()
        output.mkdir(parents=True, exist_ok=True)
        source = (ROOT / "engine/aiComponent/behaviorComponent/userIntentComponent.cpp").read_text()
        bodies = []
        for signature in (
                "void UserIntentComponent::StartFollowUpStreaming(",
                "void UserIntentComponent::StartWakeWordlessStreaming(",
                "void UserIntentComponent::StartWakeWordBargeInStreaming(",
                "void UserIntentComponent::SetTriggerWordPending(",
                "void UserIntentComponent::PushWakeWordBargeInResponse(",
                "void UserIntentComponent::PushResponseToTriggerWordInternal(",
                "void UserIntentComponent::PopResponseToTriggerWord(",
                "bool UserIntentComponent::CanArmWakeWordBargeIn(",
                "bool UserIntentComponent::HasAnimResponseToTriggerWord(",
                "void UserIntentComponent::StopConversationStream(",
                "void UserIntentComponent::OnCloudData(",
                "void UserIntentComponent::HandleCloudResponseAudio(",
                "bool UserIntentComponent::IsCloudAudioReady(",
                "bool UserIntentComponent::HasCloudAudioError(",
                "bool UserIntentComponent::HasCloudAudioInterruption(",
                "bool UserIntentComponent::HasCloudAudioStarted(",
                "bool UserIntentComponent::IsCloudAudioComplete(",
                "size_t UserIntentComponent::GetCloudAudioPendingBytes(",
                "std::vector<uint8_t> UserIntentComponent::ConsumeCloudAudioPcm(",
                "void UserIntentComponent::ClearCloudAudio(",
                "void UserIntentComponent::SetExpectedCloudAudioResponse("):
            start = source.index(signature)
            body = source.index("{", start)
            bodies.append(source[start:body] + function_body(source, signature))
        bodies.append("void UserIntentComponent::OnTriggerWord(const RobotToEngineEvent& event)\n" +
                      function_body(source, "auto triggerWordCallback ="))
        header = (ROOT / "engine/aiComponent/behaviorComponent/userIntentComponent.h").read_text()
        for result, signature in (
                ("bool", "IsCaptureQuiescent(uint32_t streamId) const"),
                ("bool", "IsCaptureOpen(uint32_t streamId) const"),
                ("uint32_t", "AllocateStreamId()")):
            bodies.append(result + " UserIntentComponent::" + signature + "\n" +
                          function_body(header, result + " " + signature))
        update = function_body(source, "void UserIntentComponent::UpdateDependent(")
        bodies.append("void UserIntentComponent::Update()\n" +
                      update[:update.index("  const size_t currTick")] + "\n}")
        session = (ROOT / "engine/aiComponent/behaviorComponent/"
                   "conversationSessionComponent.cpp").read_text()
        for signature in (
                "void ConversationSessionComponent::CancelFollowUp(",
                "void ConversationSessionComponent::UpdateDependent("):
            start = session.index(signature)
            body = session.index("{", start)
            bodies.append(session[start:body] + function_body(session, signature))
        followup = (ROOT / "engine/aiComponent/behaviorComponent/behaviors/"
                    "robotDrivenDialog/behaviorConversationFollowUp.cpp").read_text()
        for signature in (
                "void BehaviorConversationFollowUp::BehaviorUpdate(",
                "void BehaviorConversationFollowUp::OnBehaviorDeactivated("):
            start = followup.index(signature)
            body = followup.index("{", start)
            bodies.append(followup[start:body] + function_body(followup, signature))
        kg = (ROOT / "engine/aiComponent/behaviorComponent/behaviors/knowledgeGraph/"
              "behaviorKnowledgeGraphQuestion.cpp").read_text()
        kg_header = (ROOT / "engine/aiComponent/behaviorComponent/behaviors/knowledgeGraph/"
                     "behaviorKnowledgeGraphQuestion.h").read_text()
        self.assertRegex(kg_header, r"bool\s+wakeWordBargeInEnabled\s*=\s*true\s*;")
        config = json.loads((ROOT / "resources/config/engine/behaviorComponent/behaviors/"
                             "victorBehaviorTree/highLevelDelegates/knowledgeGraph/"
                             "knowledgeGraphQuestion.json").read_text())
        self.assertIs(config["wakeWordBargeInEnabled"], True)
        bodies.extend(line for line in kg.splitlines()
                      if "constexpr" in line and "kCloudAudio" in line)
        for signature in (
                "void BehaviorKnowledgeGraphQuestion::BehaviorUpdate(",
                "void BehaviorKnowledgeGraphQuestion::OnStreamingComplete(",
                "bool BehaviorKnowledgeGraphQuestion::ShouldUseCloudAudio(",
                "void BehaviorKnowledgeGraphQuestion::ConsumeIntentGraphResponse(",
                "void BehaviorKnowledgeGraphQuestion::BeginResponseCloudAudio(",
                "void BehaviorKnowledgeGraphQuestion::TransitionToSearchingLoop(",
                "void BehaviorKnowledgeGraphQuestion::TransitionToBeginResponse(",
                "void BehaviorKnowledgeGraphQuestion::FailCloudAudioResponse(",
                "void BehaviorKnowledgeGraphQuestion::CancelCloudAudioPlayback(",
                "void BehaviorKnowledgeGraphQuestion::FinishCloudAudioResponse(",
                "void BehaviorKnowledgeGraphQuestion::PlaySuccessfulResponseGetOut(",
                "void BehaviorKnowledgeGraphQuestion::WaitOutCloudAudioPlayback(",
                "void BehaviorKnowledgeGraphQuestion::OnBehaviorDeactivated(",
                "void BehaviorKnowledgeGraphQuestion::DisarmWakeWordBargeIn(",
                "void BehaviorKnowledgeGraphQuestion::BeginWakeWordBargeIn(",
                "void BehaviorKnowledgeGraphQuestion::UpdateWakeWordBargeIn(",
                "void BehaviorKnowledgeGraphQuestion::HandleWhileActivated(const RobotToEngineEvent",
                "void BehaviorKnowledgeGraphQuestion::UpdateCloudAudioStreaming("):
            start = kg.index(signature)
            body = kg.index("{", start)
            bodies.append(kg[start:body] + function_body(kg, signature))
        (output / "knowledgeFollowUpProduction.inc").write_text("\n".join(bodies))
        binary = output / "test-kg-followup"
        command = [
            "g++", "-std=c++14", "-pthread", "-pipe", "-g", "-O1",
            "-fsanitize=address,undefined", "-D__has_warning(x)=0",
            "-I.", "-Igenerated/clad/engine",
            "-Igenerated/clad/util", "-Ilib/util/source/3rd/jsoncpp",
            "-Ivictor-clad/tools/message-buffers/support/cpp/include",
            "-Ilib/das-client/testing/gtest/include", "-Ilib/das-client/testing/gtest",
            "-Ilib/util/source/anki", "-I" + str(output),
            "test/tools/fixtures/testKnowledgeFollowUpRouting.cpp",
            "test/engine/testConversationSessionState.cpp",
            "test/engine/testCloudAudioPlaybackState.cpp",
            "generated/clad/engine/clad/cloud/mic.cpp",
            "generated/clad/engine/clad/cloud/common.cpp",
            "victor-clad/tools/message-buffers/support/cpp/source/SafeMessageBuffer.cpp",
            "lib/util/source/3rd/jsoncpp/jsoncpp.cpp",
            "lib/das-client/testing/gtest/src/gtest-all.cc",
            "lib/das-client/testing/gtest/src/gtest_main.cc", "-o", str(binary)]
        env = dict(os.environ, TMPDIR=str(output), UBSAN_OPTIONS="halt_on_error=1")
        for name, cmd in (("build", command), ("results", [str(binary)])):
            result = subprocess.run(cmd, cwd=ROOT, env=env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            (output / (name + ".log")).write_text(result.stdout)
            self.assertEqual(0, result.returncode, result.stdout)


if __name__ == "__main__":
    unittest.main()
