#include "gtest/gtest.h"
#include "clad/robotInterface/messageRobotToEngine.h"
#include "engine/aiComponent/behaviorComponent/conversationSessionState.h"
#include <memory>
#include <mutex>
#include <vector>

namespace Anki {
namespace Vector {
namespace RobotInterface {
std::vector<RobotToEngine> delivered;
bool SendAnimToEngine(const RobotToEngine& message)
{
  delivered.push_back(message);
  return true;
}
}

namespace MicData {
struct ShowAudioStreamStateManager {
  bool ShouldStreamAfterTriggerWordResponse() const { return true; }
};
struct Context {
  ShowAudioStreamStateManager state;
  ShowAudioStreamStateManager* GetShowAudioStreamStateManager() { return &state; }
};
class MicDataSystem {
public:
  void SendMicStreamState(uint32_t streamId, bool open);
  void SendMessageToEngine(std::unique_ptr<RobotInterface::RobotToEngine> msgPtr);
  void Dispatch();
  bool IsMicMuted() const { return muted; }
  void SetWillStream(bool value) { willStream = value; }
  bool muted = false;
  bool willStream = false;
  RobotInterface::MicDirection _latestMicDirectionMsg{};
private:
  Context context;
  Context* _context = &context;
  std::vector<std::unique_ptr<RobotInterface::RobotToEngine>> _msgsToEngine;
  std::mutex _msgsMutex;
};

#define DEV_ASSERT_MSG(condition, ...) EXPECT_TRUE(condition)
#include "micMessageDispatchProduction.inc"
#undef DEV_ASSERT_MSG
}
}
}

using namespace Anki::Vector;
using Message = RobotInterface::RobotToEngine;
using State = ConversationSessionState::State;

class MicMessageDispatch : public ::testing::Test {
protected:
  void SetUp() override { RobotInterface::delivered.clear(); }
  MicData::MicDataSystem mic;
};

TEST_F(MicMessageDispatch, OpenCloseMuteStayOrderedAndDrainExactlyOnce)
{
  mic.SendMicStreamState(0x80000002u, true);
  mic.SendMicStreamState(0x80000002u, false);
  mic.muted = true;
  mic.SendMicStreamState(0, false);
  EXPECT_TRUE(RobotInterface::delivered.empty());
  mic.Dispatch();
  ASSERT_EQ(3u, RobotInterface::delivered.size());
  for (const auto& message : RobotInterface::delivered) {
    ASSERT_EQ(Message::Tag_micStreamState, message.tag);
  }
  EXPECT_EQ(0x80000002u, RobotInterface::delivered[0].micStreamState.streamId);
  EXPECT_TRUE(RobotInterface::delivered[0].micStreamState.open);
  EXPECT_FALSE(RobotInterface::delivered[0].micStreamState.muted);
  EXPECT_EQ(0x80000002u, RobotInterface::delivered[1].micStreamState.streamId);
  EXPECT_FALSE(RobotInterface::delivered[1].micStreamState.open);
  EXPECT_FALSE(RobotInterface::delivered[1].micStreamState.muted);
  EXPECT_EQ(0u, RobotInterface::delivered[2].micStreamState.streamId);
  EXPECT_FALSE(RobotInterface::delivered[2].micStreamState.open);
  EXPECT_TRUE(RobotInterface::delivered[2].micStreamState.muted);
  mic.Dispatch();
  EXPECT_EQ(3u, RobotInterface::delivered.size());
}

TEST_F(MicMessageDispatch, ExistingProducerTypesStillForwardWithSideEffects)
{
  RobotInterface::TriggerWordDetected trigger{};
  trigger.streamId = 0x80000001u;
  RobotInterface::MicDirection direction{};
  direction.direction = 7;
  direction.confidence = 123;
  RobotInterface::BeatDetectorState beat{};
  mic.SendMessageToEngine(std::make_unique<Message>(trigger));
  mic.SendMicStreamState(trigger.streamId, true);
  mic.SendMessageToEngine(std::make_unique<Message>(direction));
  mic.SendMessageToEngine(std::make_unique<Message>(beat));
  mic.Dispatch();
  ASSERT_EQ(4u, RobotInterface::delivered.size());
  EXPECT_EQ(Message::Tag_triggerWordDetected, RobotInterface::delivered[0].tag);
  EXPECT_EQ(trigger.streamId, RobotInterface::delivered[0].triggerWordDetected.streamId);
  EXPECT_EQ(Message::Tag_micStreamState, RobotInterface::delivered[1].tag);
  EXPECT_EQ(Message::Tag_micDirection, RobotInterface::delivered[2].tag);
  EXPECT_EQ(7u, RobotInterface::delivered[2].micDirection.direction);
  EXPECT_EQ(7u, mic._latestMicDirectionMsg.direction);
  EXPECT_EQ(123, mic._latestMicDirectionMsg.confidence);
  EXPECT_EQ(Message::Tag_beatDetectorState, RobotInterface::delivered[3].tag);
  EXPECT_TRUE(mic.willStream);
}

TEST_F(MicMessageDispatch, SuccessfulAnswerQuiescesAndOpensFollowUp)
{
  ConversationSessionState session;
  session.Claimed(1, true, true, 0);
  session.ResponseFinished(session.Token(), ConversationSessionState::Outcome::Succeeded);
  session.Deactivated(1, 25);
  ASSERT_EQ(State::Quiescing, session.GetState());
  mic.SendMicStreamState(42, false);
  EXPECT_FALSE(session.Ready(25.5));
  EXPECT_TRUE(RobotInterface::delivered.empty());
  mic.Dispatch();
  ASSERT_EQ(1u, RobotInterface::delivered.size());
  const auto& closed = RobotInterface::delivered.back();
  ASSERT_EQ(Message::Tag_micStreamState, closed.tag);
  ASSERT_EQ(42u, closed.micStreamState.streamId);
  ASSERT_FALSE(closed.micStreamState.open);
  ASSERT_FALSE(closed.micStreamState.muted);
  session.Quiesced(25.5);
  EXPECT_FALSE(session.Open(25.749));
  ASSERT_TRUE(session.Open(25.75));
  EXPECT_FALSE(session.Open(25.75));
  mic.SendMicStreamState(43, true);
  ASSERT_EQ(State::Opening, session.GetState());
  mic.Dispatch();
  ASSERT_EQ(2u, RobotInterface::delivered.size());
  const auto& opened = RobotInterface::delivered.back();
  ASSERT_EQ(Message::Tag_micStreamState, opened.tag);
  ASSERT_EQ(43u, opened.micStreamState.streamId);
  ASSERT_TRUE(opened.micStreamState.open);
  session.CaptureOpened(26);
  EXPECT_EQ(State::Listening, session.GetState());
  EXPECT_EQ(2u, session.Turn());
}
