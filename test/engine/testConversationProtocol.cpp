#include "util/helpers/includeGTest.h"
#include "clad/robotInterface/messageEngineToRobot.h"
#include "clad/robotInterface/messageRobotToEngine.h"
#include "clad/cloud/mic.h"
#include <vector>

using namespace Anki::Vector;

namespace {
template<class Message>
Message RoundTrip(const Message& message)
{
  std::vector<uint8_t> bytes(message.Size());
  EXPECT_EQ(bytes.size(), message.Pack(bytes.data(), bytes.size()));
  Message decoded;
  EXPECT_EQ(bytes.size(), decoded.Unpack(bytes.data(), bytes.size()));
  return decoded;
}
}

TEST(ConversationProtocol, FreshCaptureIsIndependentOfIdentity)
{
  RobotInterface::StartWakeWordlessStreaming ordinary;
  ordinary.streamId = 123;
  ordinary.streamType = static_cast<uint8_t>(CloudMic::StreamType::KnowledgeGraph);
  const auto copied = RoundTrip(ordinary);
  EXPECT_EQ(123u, copied.streamId);
  EXPECT_FALSE(copied.freshCapture);
  ordinary.freshCapture = true;
  EXPECT_TRUE(RoundTrip(ordinary).freshCapture);
}

TEST(ConversationProtocol, CaptureAcknowledgementCarriesIdentityAndMute)
{
  RobotInterface::MicStreamState state;
  state.streamId = 345;
  state.open = false;
  state.muted = true;
  const auto copied = RoundTrip(state);
  EXPECT_EQ(345u, copied.streamId);
  EXPECT_FALSE(copied.open);
  EXPECT_TRUE(copied.muted);
  RobotInterface::StopWakeWordlessStreaming stop;
  stop.streamId = state.streamId;
  EXPECT_EQ(345u, RoundTrip(stop).streamId);
}

TEST(ConversationProtocol, WakeStreamIdentityReachesEngine)
{
  RobotInterface::TriggerWordDetected trigger;
  trigger.streamId = 0x80000002u;
  trigger.willOpenStream = true;
  EXPECT_EQ(trigger.streamId, RoundTrip(trigger).streamId);
}

TEST(ConversationProtocol, BargeInResponseAndNotificationAreOptIn)
{
  RobotInterface::SetTriggerWordResponse response{};
  RobotInterface::TriggerWordDetected trigger{};
  EXPECT_EQ(0u, RoundTrip(response).bargeInPlaybackId);
  EXPECT_EQ(0u, RoundTrip(trigger).bargeInPlaybackId);
  response.bargeInPlaybackId = trigger.bargeInPlaybackId = 12345;
  trigger.willOpenStream = false;
  trigger.direction = 7;
  trigger.triggerScore = 93;
  const auto decodedResponse = RoundTrip(response);
  EXPECT_EQ(12345u, decodedResponse.bargeInPlaybackId);
  EXPECT_TRUE(decodedResponse.getInAnimationName.empty());
  EXPECT_FALSE(decodedResponse.shouldTriggerWordStartStream);
  const auto decodedTrigger = RoundTrip(trigger);
  EXPECT_EQ(12345u, decodedTrigger.bargeInPlaybackId);
  EXPECT_EQ(0u, decodedTrigger.streamId);
  EXPECT_EQ(7u, decodedTrigger.direction);
  EXPECT_EQ(93u, decodedTrigger.triggerScore);
  EXPECT_FALSE(decodedTrigger.willOpenStream);
}

TEST(ConversationProtocol, RendererCommandsAndCompletionRemainCorrelated)
{
  RobotInterface::ExternalAudioPrepare prepare;
  RobotInterface::ExternalAudioChunk chunk;
  RobotInterface::ExternalAudioComplete complete;
  RobotInterface::ExternalAudioCancel cancel;
  AudioStreamStatusEvent status;
  prepare.playbackId = chunk.playbackId = complete.playbackId = cancel.playbackId = status.playbackId = 12345;
  status.streamResultID = SDKAudioStreamingState::Completed;
  EXPECT_EQ(12345u, RoundTrip(prepare).playbackId);
  EXPECT_EQ(12345u, RoundTrip(chunk).playbackId);
  EXPECT_EQ(12345u, RoundTrip(complete).playbackId);
  EXPECT_EQ(12345u, RoundTrip(cancel).playbackId);
  const auto copied = RoundTrip(status);
  EXPECT_EQ(12345u, copied.playbackId);
  EXPECT_EQ(SDKAudioStreamingState::Completed, copied.streamResultID);
}

TEST(ConversationProtocol, CloudResultErrorAndAudioKeepCaptureIdentity)
{
  CloudMic::IntentResult result;
  CloudMic::IntentError error;
  CloudMic::ResponseAudioChunk audio;
  result.streamId = error.streamId = audio.streamId = 72;
  result.intent = "silence";
  error.error = CloudMic::ErrorType::Timeout;
  audio.responseId = "answer";
  audio.data = {1, 2};
  EXPECT_EQ(72u, RoundTrip(result).streamId);
  EXPECT_EQ(72u, RoundTrip(error).streamId);
  EXPECT_EQ(72u, RoundTrip(audio).streamId);
}
