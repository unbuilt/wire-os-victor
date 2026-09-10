#include "gtest/gtest.h"
#include "clad/robotInterface/messageEngineToRobot.h"
#include "clad/robotInterface/messageRobotToEngine.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define PRINT_NAMED_WARNING(...)
#define PRINT_NAMED_ERROR(...)
#define LOG_INFO(...)
#define LOG_WARNING(...)

namespace Anki {
namespace AudioEngine {
using AudioPlayingId = uint32_t;
constexpr AudioPlayingId kInvalidAudioPlayingId = 0;
enum class AudioCallbackFlag { Complete };
struct AudioCallbackInfo {};
struct AudioCallbackContext {
  void SetCallbackFlags(AudioCallbackFlag) {}
  void SetExecuteAsync(bool) {}
  void SetEventCallbackFunc(std::function<void(const AudioCallbackContext*, const AudioCallbackInfo&)> f)
  { callback = std::move(f); }
  std::function<void(const AudioCallbackContext*, const AudioCallbackInfo&)> callback;
};
template<class T> int ToAudioEventId(T event) { return static_cast<int>(event); }
template<class T> int ToAudioGameObject(T object) { return static_cast<int>(object); }
}
namespace AudioUtil {
struct SpeechRecognizerCallbackInfo { float score = 91; };
}
namespace Vector {
using RobotTimeStamp_t = uint32_t;
using TimeStamp_t = uint32_t;
enum class AlexaUXState : uint8_t;
namespace Audio {
struct CozmoAudioController {
  int events = 0;
  uint32_t PostAudioEvent(int event, int, AudioEngine::AudioCallbackContext* callback) {
    EXPECT_NE(static_cast<int>(AudioMetaData::GameEvent::GenericEvent::Invalid), event);
    ++events;
    if (callback) {
      callback->callback(callback, {});
      delete callback;
    }
    return 1;
  }
};
}
class ShowAudioStreamStateManager;
namespace Anim {
struct AnimationStreamer {
  int animations = 0;
  void SetStreamingAnimation(const std::string&, uint8_t) { ++animations; }
};
struct RobotDataLoader {
  int animation = 0;
  int* GetCannedAnimation(const std::string&) { return &animation; }
};
struct AnimContext {
  ShowAudioStreamStateManager* manager = nullptr;
  mutable RobotDataLoader loader;
  mutable Audio::CozmoAudioController audio;
  ShowAudioStreamStateManager* GetShowAudioStreamStateManager() const { return manager; }
  RobotDataLoader* GetDataLoader() const { return &loader; }
  Audio::CozmoAudioController* GetAudioController() const { return &audio; }
};
}
#include "responseDeclaration.inc"

namespace MicData {
struct MicDataSystem {
  std::atomic<bool> muted{false};
  std::atomic<bool> _wakeWordlessPending{false};
  bool streaming = false;
  bool simulate = false;
  int jobs = 0;
  std::vector<RobotInterface::RobotToEngine> messages;
  bool IsMicMuted() const { return muted; }
  bool HasStreamingJob() const { return streaming; }
  #include "pendingCaptureAccessor.inc"
  bool ShouldSimulateStreaming() const { return simulate; }
  void SetWillStream(bool) {}
  void SendMessageToEngine(std::unique_ptr<RobotInterface::RobotToEngine> message) {
    messages.push_back(*message);
  }
};
struct Direction {
  uint16_t GetDominantDirection() const { return 6; }
};
enum class TriggerWordDetectSource { Voice, Button, ButtonFromMute };
struct MicDataProcessor {
  Anim::AnimContext* _context;
  MicDataSystem* _micDataSystem;
  Direction direction;
  Direction* _micImmediateDirection = &direction;
  uint32_t _wakeStreamCounter = 0;
  void TriggerWordDetectCallback(TriggerWordDetectSource, const AudioUtil::SpeechRecognizerCallbackInfo&);
  RobotTimeStamp_t CreateTriggerWordDetectedJobs(bool shouldStream, uint32_t) {
    if (shouldStream) { ++_micDataSystem->jobs; }
    return 0;
  }
};
}
constexpr int32_t kUseDefaultStreamingDuration = -1;
#include "responseProduction.inc"
}
}

using namespace Anki::Vector;
using Source = MicData::TriggerWordDetectSource;
using Disposition = ShowAudioStreamStateManager::BargeInDisposition;

class WakeWordBargeIn : public ::testing::Test {
protected:
  WakeWordBargeIn() : state(&context), processor{&context, &mic} {
    context.manager = &state;
    state.SetAnimationStreamer(&streamer);
  }
  void SetResponse(uint32_t id, bool ordinaryAudio = false) {
    RobotInterface::SetTriggerWordResponse response;
    response.bargeInPlaybackId = id;
    response.postAudioEvent.audioEvent = ordinaryAudio
      ? static_cast<Anki::AudioMetaData::GameEvent::GenericEvent>(1)
      : Anki::AudioMetaData::GameEvent::GenericEvent::Invalid;
    response.shouldTriggerWordStartStream = true;
    response.shouldTriggerWordSimulateStream = true;
    state.SetTriggerWordResponse(response);
  }
  void Detect(Source source = Source::Voice) { processor.TriggerWordDetectCallback(source, {}); }
  void ExpectNoCaptureOrResponse() {
    state.Update();
    EXPECT_EQ(0, mic.jobs);
    EXPECT_EQ(0, context.audio.events);
    EXPECT_EQ(0, streamer.animations);
    EXPECT_EQ(0u, processor._wakeStreamCounter);
  }
  Anim::AnimContext context;
  ShowAudioStreamStateManager state;
  Anim::AnimationStreamer streamer;
  MicData::MicDataSystem mic;
  MicData::MicDataProcessor processor;
};

TEST_F(WakeWordBargeIn, InvalidEarconStillAllowsExactlyOneNotification)
{
  SetResponse(42);
  EXPECT_TRUE(state.HasValidTriggerResponse());
  EXPECT_FALSE(state.ShouldStreamAfterTriggerWordResponse());
  EXPECT_FALSE(state.ShouldSimulateStreamAfterTriggerWord());
  Detect();
  Detect();
  SetResponse(42);
  Detect();
  ASSERT_EQ(1u, mic.messages.size());
  const auto& message = mic.messages.front().triggerWordDetected;
  EXPECT_EQ(42u, message.bargeInPlaybackId);
  EXPECT_EQ(0u, message.streamId);
  EXPECT_EQ(6u, message.direction);
  EXPECT_EQ(91u, message.triggerScore);
  EXPECT_FALSE(message.willOpenStream);
  EXPECT_FALSE(message.isButtonPress);
  EXPECT_FALSE(message.fromMute);
  ExpectNoCaptureOrResponse();
}

TEST_F(WakeWordBargeIn, ButtonsSuppressWithoutConsumingVoiceNotification)
{
  SetResponse(42, true);
  Detect(Source::Button);
  Detect(Source::ButtonFromMute);
  EXPECT_TRUE(mic.messages.empty());
  Detect();
  Detect(Source::Button);
  ASSERT_EQ(1u, mic.messages.size());
  ExpectNoCaptureOrResponse();
}

TEST_F(WakeWordBargeIn, MutedAndActiveCaptureDoNotConsumeOrNotify)
{
  SetResponse(42);
  mic.muted = true;
  Detect();
  EXPECT_TRUE(mic.messages.empty());
  mic.muted = false;
  mic.streaming = true;
  Detect();
  EXPECT_TRUE(mic.messages.empty());
  mic.streaming = false;
  Detect();
  ASSERT_EQ(1u, mic.messages.size());
  ExpectNoCaptureOrResponse();
}

TEST_F(WakeWordBargeIn, IdChangesAndDisableRearmWhileSameIdStaysConsumed)
{
  SetResponse(42);
  Detect();
  SetResponse(43);
  Detect();
  SetResponse(0);
  Detect();
  SetResponse(43);
  Detect();
  ASSERT_EQ(3u, mic.messages.size());
  EXPECT_EQ(42u, mic.messages[0].triggerWordDetected.bargeInPlaybackId);
  EXPECT_EQ(43u, mic.messages[1].triggerWordDetected.bargeInPlaybackId);
  EXPECT_EQ(43u, mic.messages[2].triggerWordDetected.bargeInPlaybackId);
  ExpectNoCaptureOrResponse();
}

TEST_F(WakeWordBargeIn, PendingFreshCaptureBlocksRepeatedVoiceAndButtonTriggers)
{
  SetResponse(0, true);
  mic._wakeWordlessPending = true;
  Detect();
  Detect(Source::Button);
  Detect(Source::ButtonFromMute);
  EXPECT_TRUE(mic.messages.empty());
  ExpectNoCaptureOrResponse();
  mic._wakeWordlessPending = false;
  Detect();
  state.Update();
  EXPECT_EQ(1u, mic.messages.size());
  EXPECT_EQ(1, mic.jobs);
}

TEST_F(WakeWordBargeIn, OrdinaryZeroRetainsEarconGetInAndStream)
{
  for (auto source : {Source::Voice, Source::Button, Source::ButtonFromMute}) {
    SetResponse(0, true);
    Detect(source);
    state.Update();
    const auto& message = mic.messages.back().triggerWordDetected;
    EXPECT_EQ(0u, message.bargeInPlaybackId);
    EXPECT_NE(0u, message.streamId);
    EXPECT_TRUE(message.willOpenStream);
    EXPECT_EQ(source != Source::Voice, message.isButtonPress);
    EXPECT_EQ(source == Source::ButtonFromMute, message.fromMute);
  }
  EXPECT_EQ(3, mic.jobs);
  EXPECT_EQ(3, context.audio.events);
  EXPECT_EQ(2, streamer.animations);
}

TEST_F(WakeWordBargeIn, PendingOrdinaryResponseCannotPlayNotificationOnlyEarcon)
{
  SetResponse(0, true);
  Detect();
  SetResponse(42);
  state.Update();
  EXPECT_EQ(0, mic.jobs);
  EXPECT_EQ(0, context.audio.events);
  EXPECT_EQ(0, streamer.animations);
  bool completed = false;
  state.SetPendingTriggerResponseWithoutGetIn([&](bool success) {
    completed = true;
    EXPECT_FALSE(success);
  });
  state.Update();
  EXPECT_TRUE(completed);
  EXPECT_EQ(0, context.audio.events);
}

TEST_F(WakeWordBargeIn, ConcurrentConsumeAndSameIdUpdatesNeverFallThrough)
{
  SetResponse(42);
  std::atomic<int> notifications{0};
  std::atomic<int> fallthroughs{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 16; ++i) {
    threads.emplace_back([&] {
      for (int n = 0; n < 100; ++n) {
        SetResponse(42);
        uint32_t id = 999;
        const auto result = state.ConsumeBargeInTrigger(true, id);
        if (result == Disposition::Notify) { ++notifications; EXPECT_EQ(42u, id); }
        if (result == Disposition::NotArmed) { ++fallthroughs; }
        if (result == Disposition::Suppress) { EXPECT_EQ(0u, id); }
      }
    });
  }
  for (auto& thread : threads) { thread.join(); }
  EXPECT_EQ(1, notifications);
  EXPECT_EQ(0, fallthroughs);
}
