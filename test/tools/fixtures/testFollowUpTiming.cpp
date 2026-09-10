#include "gtest/gtest.h"
#include "engine/aiComponent/behaviorComponent/conversationSessionState.h"
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using Anki::Vector::ConversationSessionState;
using BaseStationTime_t = uint64_t;
constexpr int kTimePerChunk_ms = 10;
constexpr int kTriggerLessOverlapSize_ms = 0;
#define LOG_INFO(...)
#define LOG_WARNING(...)
#define DASMSG(...)
#define DASMSG_SET(...)
#define DASMSG_SEND(...)

namespace CloudMic {
enum class StreamType { Normal };
struct Hotword {
  StreamType mode;
  std::string locale, timeZone;
  bool noLogging;
  uint32_t streamId;
};
struct StreamIdentifier { uint32_t streamId; };
struct Message {
  static int Createhotword(Hotword) { return 1; }
  static int CreatecancelStream(StreamIdentifier) { return 2; }
};
}
struct MicDataInfo {
  uint32_t _streamId = 0;
  CloudMic::StreamType _type = CloudMic::StreamType::Normal;
  bool fresh = false, captured = false, stopped = false;
  bool IsFreshCapture() const { return fresh; }
  bool HasCapturedAudio() const { return captured; }
  void StopCollecting() { stopped = true; }
};
struct ShowAudioStreamStateManager {
  bool valid = true;
  std::function<void(bool)> callback;
  bool HasValidTriggerResponse() const { return valid; }
  void SetPendingTriggerResponseWithGetIn(std::function<void(bool)> cb) { callback = cb; }
  void SetPendingTriggerResponseWithoutGetIn(std::function<void(bool)> cb) { callback = cb; }
};
struct Context {
  ShowAudioStreamStateManager response;
  ShowAudioStreamStateManager* GetShowAudioStreamStateManager() { return &response; }
};
struct Udp {
  bool connected = true;
  bool HasClient() const { return connected; }
};
class MicDataSystem;
struct Processor {
  MicDataSystem* system;
  void CreateStreamJob(CloudMic::StreamType, int, uint32_t, bool);
  std::function<void()> beforeCreateStreamJob;
};
class MicDataSystem {
public:
  MicDataSystem() {
    _streamUpdatedCallbacks.push_back([this](bool open) { lights = open; });
  }
  void StartWakeWordlessStreaming(CloudMic::StreamType, bool, uint32_t, bool);
  void StopWakeWordlessStreaming(uint32_t);
  void ClearCurrentStreamingJob();
  void AddMicDataJob(std::shared_ptr<MicDataInfo>, bool);
  void NotifyCaptureStarted();
  void StartReadyStream(uint64_t);
  bool HasStreamingJob() const { return _currentStreamingJob != nullptr; }
  bool IsMicMuted() const { return muted; }
  bool ShouldSimulateStreaming() const { return false; }
  void SetWillStream(bool value) { lights = value; }
  void ResetMicListenDirection() {}
  void SendMicStreamState(uint32_t id, bool open) { states.emplace_back(id, open); }
  void SendUdpMessage(int message) { messages.push_back(message); }
  void EarconDone() { context.response.callback(true); }
  bool lights = false, muted = false;
  std::atomic<bool> _wakeWordlessPending{false};
  bool _currentlyStreaming = false, _streamingComplete = false;
  uint32_t _pendingStreamId = 0, _wakeWordlessGeneration = 0;
  size_t _streamingAudioIndex = 0;
  uint64_t _streamBeginTime_ns = 0;
  bool _enableDataCollection = false;
  std::string _timeZone;
  struct Locale { std::string ToString() const { return "en-US"; } } _locale;
  Context context;
  Context* _context = &context;
  Udp udp;
  Udp* _udpServer = &udp;
  Processor processor{this};
  Processor* _micDataProcessor = &processor;
  std::recursive_mutex _dataRecordJobMutex;
  std::shared_ptr<MicDataInfo> _currentStreamingJob;
  std::deque<std::shared_ptr<MicDataInfo>> _micProcessingJobs;
  std::vector<std::function<void(bool)>> _streamUpdatedCallbacks;
  std::vector<std::pair<uint32_t, bool>> states;
  std::vector<int> messages;
};
void Processor::CreateStreamJob(CloudMic::StreamType, int, uint32_t id, bool fresh) {
  if (beforeCreateStreamJob) { beforeCreateStreamJob(); }
  auto job = std::make_shared<MicDataInfo>();
  job->_streamId = id;
  job->fresh = fresh;
  system->AddMicDataJob(job, true);
}

enum class AnimationTrigger { VC_ListeningGetOut, VC_ListeningLoop, KnowledgeGraphSuccessReaction };
struct TriggerAnimationAction {
  explicit TriggerAnimationAction(AnimationTrigger trigger) : trigger(trigger) {}
  AnimationTrigger trigger;
};
using ReselectingLoopAnimationAction = TriggerAnimationAction;
using TriggerLiftSafeAnimationAction = TriggerAnimationAction;
struct UserIntentComponent {
  bool pending = false, active = false, open = false, closed = false;
  bool IsAnyUserIntentPending() const { return pending; }
  bool IsAnyUserIntentActive() const { return active; }
  bool IsCaptureOpen(uint32_t) const { return open; }
  bool IsCaptureQuiescent(uint32_t) const { return closed; }
};
struct ConversationSessionComponent {
  ConversationSessionState state;
  ConversationSessionState& Policy() { return state; }
  bool safe = true;
  bool IsSafe() const { return safe && state.config.enabled; }
};
class BehaviorConversationFollowUp {
public:
  void BehaviorUpdate();
  template<class T> T& GetBehaviorComp();
  bool IsActivated() const { return !cancelled; }
  void CancelSelf() { cancelled = true; }
  void CancelDelegates(bool) {}
  void DelegateIfInControl(TriggerAnimationAction* action, std::function<void()> cb = {}) {
    animations.push_back(action->trigger);
    delete action;
    completion = cb;
  }
  bool cancelled = false, _gettingOut = false, _listening = false, _captureClosed = false;
  uint32_t _streamId = 1;
  UserIntentComponent uic;
  ConversationSessionComponent session;
  std::vector<AnimationTrigger> animations;
  std::function<void()> completion;
};
template<> UserIntentComponent& BehaviorConversationFollowUp::GetBehaviorComp() { return uic; }
template<> ConversationSessionComponent& BehaviorConversationFollowUp::GetBehaviorComp() { return session; }

struct BaseStationTimer {
  static BaseStationTimer* getInstance() { static BaseStationTimer timer; return &timer; }
  double GetCurrentTimeInSecondsDouble() const { return 25; }
};
enum class EState { Responding, Interrupted };
enum class ActionResult { SUCCESS, FAILED };
class BehaviorKnowledgeGraphQuestion {
public:
  void PlaySuccessfulResponseGetOut();
  template<class T> T& GetBehaviorComp() { return session; }
  void DelegateIfInControl(TriggerLiftSafeAnimationAction* action,
                           std::function<void(ActionResult)> cb) {
    celebrated = true;
    completion = cb;
    delete action;
  }
  struct {
    uint64_t conversationToken = 0;
    EState state = EState::Responding;
    uint32_t playbackId = 0;
  } _dVars;
  ConversationSessionComponent session;
  bool celebrated = false;
  std::function<void(ActionResult)> completion;
};

#include "followUpTimingProduction.inc"

TEST(FollowUpTiming, SuccessfulContinuingAnswerSkipsCelebrationButStillWaitsForSettle) {
  BehaviorKnowledgeGraphQuestion response;
  auto& policy = response.session.state;
  policy.Claimed(1, true, true, 0);
  response._dVars.conversationToken = policy.Token();
  response.PlaySuccessfulResponseGetOut();
  EXPECT_FALSE(response.celebrated);
  EXPECT_EQ(ConversationSessionState::State::Responding, policy.GetState());
  policy.Deactivated(1, 25);
  EXPECT_EQ(ConversationSessionState::State::Quiescing, policy.GetState());
  policy.Quiesced(25);
  EXPECT_FALSE(policy.Ready(25.249));
  EXPECT_TRUE(policy.Ready(25.25));
}

TEST(FollowUpTiming, DisabledUnsafeAndFinalAnswersRetainSuccessAnimation) {
  for (int scenario = 0; scenario < 3; ++scenario) {
    BehaviorKnowledgeGraphQuestion response;
    auto& policy = response.session.state;
    policy.Claimed(1, true, true, 0);
    response._dVars.conversationToken = policy.Token();
    if (scenario == 0) policy.config.enabled = false;
    if (scenario == 1) response.session.safe = false;
    if (scenario == 2) policy.config.maxTurns = 1;
    response.PlaySuccessfulResponseGetOut();
    EXPECT_TRUE(response.celebrated);
    policy.Deactivated(1, 25);
    EXPECT_FALSE(policy.Active());
  }
}

TEST(FollowUpTiming, EarconAndQueuedAudioDoNotAdvertiseCaptureOrStartRequest) {
  MicDataSystem mic;
  mic.StartWakeWordlessStreaming(CloudMic::StreamType::Normal, false, 1, true);
  EXPECT_FALSE(mic.lights);
  EXPECT_TRUE(mic.states.empty());
  mic.EarconDone();
  ASSERT_TRUE(mic.HasStreamingJob());
  mic.StartReadyStream(100);
  EXPECT_FALSE(mic.lights);
  EXPECT_TRUE(mic.messages.empty());
  EXPECT_TRUE(mic.states.empty());
  mic.StartWakeWordlessStreaming(CloudMic::StreamType::Normal, false, 1, true);
  EXPECT_TRUE(mic.states.empty());
  mic._currentStreamingJob->captured = true;
  mic.StartReadyStream(200);
  ASSERT_EQ(1u, mic.states.size());
  EXPECT_TRUE(mic.states[0].second);
  EXPECT_TRUE(mic.lights);
  ASSERT_EQ(1u, mic.messages.size());
  EXPECT_EQ(1, mic.messages[0]);
  mic.StartReadyStream(300);
  EXPECT_EQ(1u, mic.states.size());
  mic.StopWakeWordlessStreaming(1);
  EXPECT_FALSE(mic.lights);
  EXPECT_FALSE(mic.states.back().second);
}

TEST(FollowUpTiming, PendingEarconBlocksWakesUntilFreshCaptureIsInstalled) {
  MicDataSystem mic;
  mic.StartWakeWordlessStreaming(CloudMic::StreamType::Normal, false, 1, true);
  EXPECT_TRUE(mic._wakeWordlessPending);
  EXPECT_FALSE(mic.HasStreamingJob());
  mic.StartReadyStream(100);
  EXPECT_TRUE(mic.messages.empty());
  EXPECT_FALSE(mic._currentlyStreaming);
  bool created = false;
  mic.processor.beforeCreateStreamJob = [&] {
    created = true;
    EXPECT_TRUE(mic._wakeWordlessPending);
    EXPECT_FALSE(mic.HasStreamingJob());
  };
  mic.EarconDone();
  EXPECT_TRUE(created);
  EXPECT_FALSE(mic._wakeWordlessPending);
  ASSERT_TRUE(mic.HasStreamingJob());
  EXPECT_TRUE(mic._currentStreamingJob->IsFreshCapture());
}

TEST(FollowUpTiming, CancelledEarconCannotReopenAndOfflineCaptureNeverLights) {
  MicDataSystem mic;
  mic.StartWakeWordlessStreaming(CloudMic::StreamType::Normal, false, 1, true);
  mic.StopWakeWordlessStreaming(1);
  mic.EarconDone();
  EXPECT_FALSE(mic.HasStreamingJob());
  mic.StartWakeWordlessStreaming(CloudMic::StreamType::Normal, false, 2, true);
  mic.EarconDone();
  mic._currentStreamingJob->captured = true;
  mic.udp.connected = false;
  mic.StartReadyStream(100);
  EXPECT_FALSE(mic.HasStreamingJob());
  EXPECT_FALSE(mic.lights);
  for (auto state : mic.states) EXPECT_FALSE(state.second);
}

TEST(FollowUpTiming, LegacyTriggerLightsRemainImmediate) {
  MicDataSystem mic;
  mic.StartWakeWordlessStreaming(CloudMic::StreamType::Normal, false, 1, false);
  EXPECT_TRUE(mic.lights);
  mic.EarconDone();
  ASSERT_EQ(1u, mic.states.size());
  EXPECT_TRUE(mic.states[0].second);
  mic.StartReadyStream(100);
  EXPECT_EQ(1u, mic.messages.size());
}

TEST(FollowUpTiming, FaceStopsWithCaptureButLateCloudResultStillRoutes) {
  BehaviorConversationFollowUp behavior;
  auto& state = behavior.session.state;
  state.Claimed(1, true, true, 0);
  state.ResponseFinished(state.Token(), ConversationSessionState::Outcome::Succeeded);
  state.Deactivated(1, 1);
  state.Quiesced(1);
  ASSERT_TRUE(state.Open(1.25));
  state.CaptureOpened(2);
  behavior.uic.open = true;
  behavior.BehaviorUpdate();
  ASSERT_EQ(1u, behavior.animations.size());
  EXPECT_EQ(AnimationTrigger::VC_ListeningLoop, behavior.animations.back());
  behavior.uic.open = false;
  behavior.uic.closed = true;
  behavior.BehaviorUpdate();
  EXPECT_EQ(AnimationTrigger::VC_ListeningGetOut, behavior.animations.back());
  EXPECT_FALSE(behavior.cancelled);
  behavior.BehaviorUpdate();
  EXPECT_EQ(2u, behavior.animations.size());
  EXPECT_TRUE(state.CanReceiveResult(8));
  behavior.uic.pending = true;
  behavior.BehaviorUpdate();
  EXPECT_TRUE(behavior.cancelled);
  EXPECT_TRUE(behavior.uic.pending);
}
