#include "gtest/gtest.h"
#include "clad/cloud/mic.h"
#include "engine/aiComponent/behaviorComponent/knowledgeFollowUpRouting.h"
#include "engine/aiComponent/behaviorComponent/conversationSessionState.h"
#include "engine/aiComponent/behaviorComponent/behaviors/knowledgeGraph/cloudAudioPlaybackState.h"
#include <algorithm>
#include <cmath>
#include <list>
#include <memory>
#include <mutex>
#include <vector>

#define LOG_DEBUG(...)
#define LOG_INFO(...)
#define LOG_WARNING(...)
#define PRINT_INFO(...)
#define PRINT_DEBUG(...)
#define PRINT_NAMED_WARNING(...)
#define PRINT_NAMED_INFO(...)
#define DASMSG(...)
#define DASMSG_SET(...)
#define DASMSG_SEND(...)
#define USER_INTENT(x) UserIntentTag::x
#define FLT_NEAR(a, b) (std::abs((a) - (b)) < .0001)

namespace Anki { namespace Vector {
namespace RobotInterface {
struct StartWakeWordlessStreaming {
  uint8_t streamType = 0;
  bool playGetInFromAnimProcess = false, freshCapture = false;
  uint32_t streamId = 0;
};
struct StopWakeWordlessStreaming { uint32_t streamId = 0; };
struct EngineToRobot {
  StartWakeWordlessStreaming start;
  StopWakeWordlessStreaming stop;
  bool stopping;
  EngineToRobot(StartWakeWordlessStreaming&& s) : start(s), stopping(false) {}
  EngineToRobot(StopWakeWordlessStreaming&& s) : stop(s), stopping(true) {}
};
}
struct Robot {
  std::vector<RobotInterface::EngineToRobot> messages;
  void SendMessage(RobotInterface::EngineToRobot&& msg) { messages.push_back(std::move(msg)); }
};
struct BaseStationTimer {
  double now = 0;
  static BaseStationTimer* getInstance() { static BaseStationTimer timer; return &timer; }
  double GetCurrentTimeInSecondsDouble() const { return now; }
  float GetCurrentTimeInSeconds() const { return now; }
};
struct BCCompMap {};
class UserIntentComponent;
bool kAutomaticFollowUpEnabled = true;
double Now() { return BaseStationTimer::getInstance()->GetCurrentTimeInSecondsDouble(); }
class ConversationSessionComponent {
public:
  ConversationSessionState _policy;
  ConversationSessionState& state = _policy;
  UserIntentComponent* _uic = nullptr;
  uint32_t _ownedStream = 42;
  bool _stopRequested = false, _hadSession = true;
  ConversationSessionState& Policy() { return state; }
  bool IsSafe() const { return true; }
  void CancelFollowUp(uint32_t);
  void UpdateDependent(const BCCompMap&);
};
enum class UserIntentTag { knowledge_response_bypass, knowledge_question, silence, unmatched_intent };
struct UserIntent_KnowledgeResponse {
  std::string answer, query_text, response_id;
  bool cloud_audio_available = false;
};
struct UserIntent {
  UserIntent_KnowledgeResponse response;
  const UserIntent_KnowledgeResponse& Get_knowledge_response() const { return response; }
};
struct UserIntentData { UserIntent intent; };
using UserIntentPtr = std::shared_ptr<UserIntentData>;

class UserIntentComponent {
public:
  UserIntentComponent() { conversation._uic = this; }
  std::mutex _mutex;
  Robot robot;
  Robot* _robot = &robot;
  ConversationSessionComponent conversation;
  ConversationSessionComponent* _conversation = &conversation;
  CloudMic::Message _pendingCloudIntent;
  std::list<CloudMic::Message> _cloudEvents;
  uint32_t _expectedStreamId = 0, next = 0;
  bool _automaticListening = false, _acceptStreamResults = true, _streamResultReceived = false;
  bool _captureStateKnown = false, _isStreamOpen = false, _wasIntentError = false;
  bool _wasIntentUnclaimed = false, _pendingTriggerWillStream = false;
  bool _waitingForTriggerWordGetInToFinish = false;
  float _waitingForTriggerWordGetInToFinish_setTime_s = 0;
  struct Buffer {
    std::string responseId;
    bool haveStart = false, complete = false, hasError = false, interrupted = false;
    uint32_t sampleRateHz = 0, nextSequence = 0;
    uint8_t channels = 0;
    size_t totalBytes = 0;
    std::vector<uint8_t> pcm;
    void Reset() { *this = Buffer{}; }
  } _cloudAudio;
  std::string _cloudAudioExpectedId;
  Json::Value pending;
  unsigned delivered = 0;
  UserIntentPtr active;
  uint32_t AllocateStreamId() { return ++next; }
  bool HasAnimResponseToTriggerWord() const { return false; }
  bool SetIntentPendingFromCloudJSONValue(Json::Value json) {
    pending = std::move(json);
    ++delivered;
    return true;
  }
  bool IsUserIntentPending(UserIntentTag tag) const {
    const auto name = pending["intent"].asString();
    switch (tag) {
      case UserIntentTag::knowledge_response_bypass: return name == "intent_knowledge_response_extend_bypass";
      case UserIntentTag::knowledge_question: return name == "intent_knowledge_question";
      case UserIntentTag::silence: return name == "intent_system_noaudio";
      default: return name == "unmatched_intent";
    }
  }
  void DropAnyUserIntent() { pending = {}; }
  bool IsCloudStreamOpen() const { return _isStreamOpen; }
  uint32_t GetCurrentStreamId() const { return _expectedStreamId; }
  bool IsCaptureQuiescent(uint32_t) const { return !_isStreamOpen; }
  bool IsCaptureOpen(uint32_t) const { return _isStreamOpen; }
  bool IsAnyUserIntentPending() const { return !pending.isNull(); }
  bool IsAnyUserIntentActive() const { return active != nullptr; }
  bool IsUserIntentActive(UserIntentTag tag) const {
    return active && tag == UserIntentTag::knowledge_response_bypass;
  }
  bool WasUserIntentError() const { return _wasIntentError; }
  UserIntentPtr GetUserIntentIfActive(UserIntentTag tag) { return tag == UserIntentTag::knowledge_response_bypass ? active : nullptr; }
  void StartFollowUpStreaming(uint32_t);
  void StartWakeWordlessStreaming(CloudMic::StreamType, bool = false);
  void StopConversationStream(uint32_t, bool = true);
  void OnCloudData(CloudMic::Message&&);
  void HandleCloudResponseAudio(const CloudMic::Message&);
  std::vector<uint8_t> ConsumeCloudAudioPcm(const std::string&);
  void SetExpectedCloudAudioResponse(const std::string&);
  void ClearCloudAudio(const std::string& = "");
  bool IsCloudAudioReady(const std::string&, uint32_t&, uint8_t&, size_t = 0);
  bool HasCloudAudioError(const std::string&);
  bool HasCloudAudioInterruption(const std::string&);
  bool HasCloudAudioStarted(const std::string&);
  bool IsCloudAudioComplete(const std::string&);
  size_t GetCloudAudioPendingBytes(const std::string&);
  void Update();
};
enum class AnimationTrigger { VC_ListeningGetOut, VC_ListeningLoop, KnowledgeGraphSearchingFailGetOut,
                              KnowledgeGraphSearching, KnowledgeGraphSearchingFail,
                              KnowledgeGraphSearchingGetOutSuccess };
struct TriggerAnimationAction { explicit TriggerAnimationAction(AnimationTrigger) {} };
using TriggerLiftSafeAnimationAction = TriggerAnimationAction;
struct CompoundActionSequential {
  template<class T> void AddAction(T* action, bool) { delete action; }
};
struct ReselectingLoopAnimationAction { explicit ReselectingLoopAnimationAction(AnimationTrigger) {} };
class BehaviorConversationFollowUp {
public:
  UserIntentComponent& uic;
  bool _listening = true, _captureClosed = false, _gettingOut = false, cancelled = false;
  uint32_t _streamId = 42;
  explicit BehaviorConversationFollowUp(UserIntentComponent& u) : uic(u) {}
  template<class T> T& GetBehaviorComp();
  bool IsActivated() const { return !cancelled; }
  void CancelSelf() { cancelled = true; OnBehaviorDeactivated(); }
  void CancelDelegates(bool) {}
  template<class T> void DelegateIfInControl(T* action) { delete action; }
  template<class T, class F> void DelegateIfInControl(T* action, F callback) {
    delete action; callback();
  }
  void BehaviorUpdate();
  void OnBehaviorDeactivated();
};
template<> UserIntentComponent& BehaviorConversationFollowUp::GetBehaviorComp() { return uic; }
template<> ConversationSessionComponent& BehaviorConversationFollowUp::GetBehaviorComp() {
  return uic.conversation;
}
static const char* kAltParamsKey = "parameters";
static const char* kParamsKey = "params";
constexpr uint32_t kCloudAudioMaxBytes = 16000 * 2 * 60;
uint32_t sPlaybackId = 0;

struct SDKComponent {
  unsigned prepares = 0, completions = 0, cancellations = 0;
  uint32_t playbackId = 0;
  std::vector<uint8_t> sent;
  void PrepareStreamingAudio(uint16_t rate, uint16_t, uint32_t id) {
    EXPECT_EQ(16000u, rate);
    ++prepares;
    playbackId = id;
  }
  void SendStreamingAudioChunk(const uint8_t* data, uint16_t size, uint32_t id) {
    EXPECT_EQ(playbackId, id);
    EXPECT_LE(size, 1024u);
    sent.insert(sent.end(), data, data + size);
  }
  void CompleteStreamingAudio(uint32_t id) {
    EXPECT_EQ(playbackId, id);
    ++completions;
  }
  void CancelStreamingAudio(uint32_t) { ++cancellations; }
};
class BehaviorKnowledgeGraphQuestion {
public:
  UserIntentComponent& uic;
  explicit BehaviorKnowledgeGraphQuestion(UserIntentComponent& u) : uic(u) {}
  enum class EState { WaitingToStream, Listening, Responding, Interrupted };
  enum class EResponseSource { CloudAudio, LocalTts };
  enum class EGenerationStatus { None, Success, Fail };
  struct {
    EState state = EState::WaitingToStream;
    EResponseSource responseSource = EResponseSource::LocalTts;
    double streamingBeginTime = 0, streamingRequestTime = 0;
    bool wasPickedUp = false, cloudAudioExpected = false;
    std::string responseString, responseId;
    std::vector<uint8_t> cloudAudioPcm;
    uint8_t cloudAudioChannels = 0;
    uint32_t cloudAudioSampleRate = 0, playbackId = 0;
    uint64_t conversationToken = 0;
    CloudAudioPlaybackState cloudAudioPlayback;
    bool cloudAudioCompleteSent = false, cloudAudioResponseFinished = false;
    bool cloudAudioFailed = false;
    double cloudAudioStreamStartTime = 0, cloudAudioLastDataTime = 0;
    double cloudAudioLastPlaybackProgressTime = 0;
    double cloudAudioCompletionDeadline = 0;
    double cloudAudioReadyDeadline = 0;
    EGenerationStatus ttsGenerationStatus = EGenerationStatus::None;
  } _dVars;
  struct {
    bool cloudAudioEnabled = true;
    bool cloudAudioFallbackToLocalTts = true;
    unsigned cloudAudioVolume = 100;
    double streamingDuration = 10, cloudAudioRequestTimeout = 65;
  } _iVars;
  struct {
    bool IsFinished() const { return true; }
    bool IsValid() const { return true; }
  } _readyTTSWrapper;
  struct Info : SDKComponent {
    bool IsPickedUp() const { return false; }
    Info& GetRobotInfo() { return *this; }
    Info& GetSDKComponent() { return *this; }
  } info;
  unsigned opened = 0, responses = 0, failures = 0;
  bool cancelled = false;
  template<class T> T& GetBehaviorComp() { return uic; }
  bool IsActivated() const { return true; }
  Info& GetBEI() { return info; }
  void CancelDelegates(bool) {}
  bool IsControlDelegated() const { return true; }
  void CancelSelf() { cancelled = true; }
  template<class T> void DelegateIfInControl(T* action) { ++failures; delete action; }
  template<class T, class F> void DelegateIfInControl(T* action, F) { delete action; }
  void BeginStreamingQuestion() { ++opened; }
  void OnStreamingComplete(bool bypass) {
    EXPECT_TRUE(bypass);
    ConsumeIntentGraphResponse();
    ++responses;
    _dVars.state = EState::Responding;
  }
  bool IsResponsePending() const { return false; }
  void TransitionToNoResponse() { ++failures; }
  void UpdateCloudAudioStreaming();
  void FailCloudAudioResponse(bool quiet = false);
  void CancelCloudAudioPlayback();
  void OnResponseInterrupted() {}
  void BeginResponseTTS() { ++failures; }
  void WaitOutCloudAudioPlayback() {}
  void BeginResponseCloudAudio();
  void TransitionToSearchingLoop();
  void TransitionToBeginResponse() { ++responses; }
  void BehaviorUpdate();
  void ConsumeIntentGraphResponse();
};

template<> ConversationSessionComponent& BehaviorKnowledgeGraphQuestion::GetBehaviorComp() {
  return uic.conversation;
}

#include "knowledgeFollowUpProduction.inc"

using State = ConversationSessionState::State;
using Outcome = ConversationSessionState::Outcome;
void Open(UserIntentComponent& uic)
{
  auto& session = uic.conversation.state;
  session.Claimed(1, true, true, 0);
  session.ResponseFinished(session.Token(), Outcome::Succeeded);
  session.Deactivated(1, 25);
  session.Quiesced(25);
  ASSERT_TRUE(session.Open(25.25));
  uic.StartFollowUpStreaming(42);
  session.CaptureOpened(25.5);
  BaseStationTimer::getInstance()->now = 26;
}
Json::Value Params(std::string query = "What is its size?", std::string answer = "It is large.")
{
  Json::Value params;
  params["query_text"] = query;
  params["answer"] = answer;
  params["response_id"] = "answer-42";
  params["audio_id"] = "published-42";
  params["cloud_audio_available"] = true;
  return params;
}
void Result(UserIntentComponent& uic, Json::Value params = Params(),
            std::string intent = "intent_knowledge_response_extend", uint32_t id = 42)
{
  CloudMic::IntentResult result;
  result.intent = intent;
  result.parameters = Json::FastWriter().write(params);
  result.streamId = id;
  uic.OnCloudData(CloudMic::Message(std::move(result)));
}
void AudioStart(UserIntentComponent& uic, uint32_t id, const std::string& response)
{
  CloudMic::ResponseAudioStart start{};
  start.streamId = id;
  start.responseId = response;
  start.sampleRateHz = 16000;
  start.channels = 1;
  uic.OnCloudData(CloudMic::Message(std::move(start)));
}

void Prebuffer(UserIntentComponent& uic, uint32_t id = 42,
               const std::string& response = "answer-42")
{
  uic.SetExpectedCloudAudioResponse(response);
  AudioStart(uic, id, response);
  for (uint32_t sequence = 0; sequence < 60; ++sequence) {
    CloudMic::ResponseAudioChunk chunk;
    chunk.streamId = id;
    chunk.responseId = response;
    chunk.sequenceNumber = sequence;
    chunk.data.assign(32000, 0x5a);
    uic.OnCloudData(CloudMic::Message(std::move(chunk)));
  }
  ASSERT_EQ(kCloudAudioMaxBytes, uic._cloudAudio.pcm.size());
}

void ExpectRejectedAudioCleared(UserIntentComponent& uic)
{
  EXPECT_FALSE(uic._acceptStreamResults);
  EXPECT_FALSE(uic._cloudAudio.haveStart);
  EXPECT_TRUE(uic._cloudAudio.responseId.empty());
  EXPECT_TRUE(uic._cloudAudioExpectedId.empty());
  EXPECT_TRUE(uic._cloudAudio.pcm.empty());
  EXPECT_EQ(0u, uic._cloudAudio.pcm.capacity());
  EXPECT_EQ(0u, uic._cloudAudio.totalBytes);
  EXPECT_EQ(0u, uic._cloudAudio.nextSequence);
  AudioStart(uic, uic._expectedStreamId, "late-answer");
  EXPECT_FALSE(uic._cloudAudio.haveStart);
}

TEST(KnowledgeFollowUpRouting, ActualRequestUsesKGOnlyForAutomaticTurn)
{
  UserIntentComponent uic;
  uic.StartWakeWordlessStreaming(CloudMic::StreamType::Normal);
  ASSERT_EQ(1u, uic.robot.messages.size());
  EXPECT_EQ(static_cast<uint8_t>(CloudMic::StreamType::Normal), uic.robot.messages.back().start.streamType);
  EXPECT_FALSE(uic.robot.messages.back().start.freshCapture);
  Open(uic);
  const auto& request = uic.robot.messages.back().start;
  EXPECT_EQ(static_cast<uint8_t>(CloudMic::StreamType::KnowledgeGraph), request.streamType);
  EXPECT_EQ(42u, request.streamId);
  EXPECT_TRUE(request.freshCapture);
  EXPECT_FALSE(request.playGetInFromAnimProcess);
}

TEST(KnowledgeFollowUpRouting, ActualResultDispatchPreservesAudioAndNeverReopensQuestion)
{
  UserIntentComponent uic;
  Open(uic);
  Prebuffer(uic);
  uic.StopConversationStream(42, false);
  EXPECT_EQ(kCloudAudioMaxBytes, uic._cloudAudio.pcm.size());
  EXPECT_EQ("answer-42", uic._cloudAudioExpectedId);
  EXPECT_TRUE(uic._acceptStreamResults);
  Result(uic);
  CloudMic::StreamIdentifier closed;
  closed.streamId = 42;
  uic.OnCloudData(CloudMic::Message::CreatestreamClosed(std::move(closed)));
  uic.Update();
  EXPECT_EQ(State::AwaitingClaim, uic.conversation.state.GetState());
  EXPECT_EQ("intent_knowledge_response_extend_bypass", uic.pending["intent"].asString());
  EXPECT_EQ(Params(), uic.pending["params"]);
  uic.active = std::make_shared<UserIntentData>();
  auto& response = uic.active->intent.response;
  response.answer = uic.pending["params"]["answer"].asString();
  response.response_id = uic.pending["params"]["response_id"].asString();
  response.cloud_audio_available = true;
  uic.conversation.state.Claimed(2, true, true, 27);
  BehaviorKnowledgeGraphQuestion kg(uic);
  kg.BehaviorUpdate();
  EXPECT_EQ(0u, kg.opened);
  EXPECT_EQ(1u, kg.responses);
  EXPECT_EQ(response.answer, kg._dVars.responseString);
  EXPECT_EQ("answer-42", uic._cloudAudioExpectedId);
  EXPECT_TRUE(uic._cloudAudio.haveStart);
  EXPECT_EQ(kCloudAudioMaxBytes, uic._cloudAudio.pcm.size());
  EXPECT_TRUE(uic.ConsumeCloudAudioPcm("stale-answer").empty());
  const auto pcm = uic.ConsumeCloudAudioPcm("answer-42");
  EXPECT_EQ(kCloudAudioMaxBytes, pcm.size());
  EXPECT_TRUE(std::all_of(pcm.begin(), pcm.end(), [](uint8_t byte) { return byte == 0x5a; }));
  EXPECT_TRUE(uic._cloudAudio.pcm.empty());
  EXPECT_TRUE(kg._dVars.cloudAudioExpected);
  kg.BehaviorUpdate();
  EXPECT_EQ(1u, kg.responses);
  auto& session = uic.conversation.state;
  session.ResponseFinished(session.Token(), Outcome::Succeeded);
  session.Deactivated(2, 40);
  session.Quiesced(40);
  EXPECT_TRUE(session.Open(40.25));
  EXPECT_EQ(3u, session.Turn());
}

TEST(KnowledgeFollowUpRouting, OrdinaryKGListenerResultIsNotPromoted)
{
  UserIntentComponent uic;
  uic.StartWakeWordlessStreaming(CloudMic::StreamType::KnowledgeGraph);
  Result(uic, Params(), "intent_knowledge_response_extend", uic._expectedStreamId);
  uic.Update();
  EXPECT_EQ("intent_knowledge_response_extend", uic.pending["intent"].asString());
}

TEST(KnowledgeFollowUpRouting, TerminalResponsesDoNotDispatchOrReprompt)
{
  struct Case { std::string query, answer, intent, reason; };
  for (const auto& c : std::vector<Case>{
    {"", "", "intent_knowledge_response_extend", "empty_answer"},
    {" \t\r\n", " \n", "intent_knowledge_response_extend", "empty_answer"},
    {"question", " \n", "intent_knowledge_response_extend", "empty_answer"},
    {" Stop! ", "Here is more information", "intent_knowledge_response_extend", "spoken_stop"},
    {"cancel", "Here is more information", "intent_knowledge_response_extend", "spoken_stop"},
    {"stop listening", "Here is more information", "intent_knowledge_response_extend", "spoken_stop"},
    {"end conversation", "Here is more information", "intent_knowledge_response_extend", "spoken_stop"},
    {"", "", "intent_system_noaudio", "silence"},
    {"question", "answer", "intent_knowledge_question", "non_eligible_result"},
    {"question", "answer", "unmatched_intent", "non_eligible_result"},
    {"question", "answer", "intent_knowledge_unknown", "non_eligible_result"}}) {
    UserIntentComponent uic;
    Open(uic);
    Prebuffer(uic);
    Result(uic, Params(c.query, c.answer), c.intent);
    uic.Update();
    EXPECT_EQ(0u, uic.delivered) << c.reason;
    EXPECT_STREQ(c.reason.c_str(), uic.conversation.state.EndReason());
    EXPECT_FALSE(uic.conversation.state.Active());
    EXPECT_FALSE(uic.conversation.state.Open(27));
    ExpectRejectedAudioCleared(uic);
  }
}

TEST(KnowledgeFollowUpRouting, NoAnswerTextStopHeuristicOrSubstringQueryMatch)
{
  UserIntentComponent uic;
  Open(uic);
  Prebuffer(uic);
  Result(uic, Params("What does stop listening mean?", "Stop!"));
  uic.Update();
  EXPECT_EQ(1u, uic.delivered);
}

TEST(KnowledgeFollowUpRouting, MissingOrMalformedFieldsFailClosed)
{
  for (const auto* field : {"query_text", "answer"}) {
    UserIntentComponent uic;
    Open(uic);
    auto params = Params();
    params.removeMember(field);
    Result(uic, params);
    uic.Update();
    EXPECT_STREQ("malformed_result", uic.conversation.state.EndReason());
    EXPECT_EQ(0u, uic.delivered);
    ExpectRejectedAudioCleared(uic);
  }
  UserIntentComponent uic;
  Open(uic);
  Prebuffer(uic);
  CloudMic::IntentResult result;
  result.streamId = 42;
  result.intent = "intent_knowledge_response_extend";
  result.parameters = "{bad-json";
  uic.OnCloudData(CloudMic::Message(std::move(result)));
  uic.Update();
  EXPECT_STREQ("malformed_result", uic.conversation.state.EndReason());
  EXPECT_EQ(0u, uic.delivered);
  ExpectRejectedAudioCleared(uic);
}

TEST(KnowledgeFollowUpRouting, KGDeadlineBoundaryAndLateResultRejection)
{
  for (double now : {35.5, 85.5, 90.499, 90.5}) {
    UserIntentComponent uic;
    Open(uic);
    BaseStationTimer::getInstance()->now = now;
    Result(uic);
    uic.Update();
    EXPECT_EQ(now < 90.5 ? 1u : 0u, uic.delivered);
    uic.conversation.state.Update(now, true);
    EXPECT_EQ(now < 90.5, uic.conversation.state.Active());
  }
}

TEST(KnowledgeFollowUpRouting, ErrorsAndCloseWithoutResultEndOnce)
{
  for (bool error : {true, false}) {
    UserIntentComponent uic;
    Open(uic);
    if (error) {
      CloudMic::IntentError err;
      err.streamId = 42;
      err.error = CloudMic::ErrorType::Timeout;
      uic.OnCloudData(CloudMic::Message(std::move(err)));
    } else {
      CloudMic::StreamIdentifier closed;
      closed.streamId = 42;
      uic.OnCloudData(CloudMic::Message::CreatestreamClosed(std::move(closed)));
    }
    uic.Update();
    EXPECT_FALSE(uic.conversation.state.Active());
    Result(uic);
    uic.Update();
    EXPECT_EQ(0u, uic.delivered);
    EXPECT_EQ(error, uic._wasIntentError);
  }
}

TEST(KnowledgeFollowUpRouting, StaleDuplicateResultsAndAudioCannotReplaceOwner)
{
  UserIntentComponent uic;
  Open(uic);
  Result(uic, Params(), "intent_knowledge_response_extend", 41);
  uic.Update();
  EXPECT_EQ(0u, uic.delivered);
  Result(uic);
  Result(uic, Params("different", "duplicate"));
  uic.Update();
  EXPECT_EQ(1u, uic.delivered);
  uic.SetExpectedCloudAudioResponse("answer-42");
  AudioStart(uic, 42, "answer-42");
  AudioStart(uic, 41, "stale-stream");
  AudioStart(uic, 42, "stale-answer");
  EXPECT_EQ("answer-42", uic._cloudAudio.responseId);
  uic.StopConversationStream(41);
  EXPECT_TRUE(uic._acceptStreamResults);
  uic.StopConversationStream(42);
  ExpectRejectedAudioCleared(uic);
  uic.StartFollowUpStreaming(43);
  EXPECT_FALSE(uic._cloudAudio.haveStart);
  AudioStart(uic, 42, "retired-answer");
  EXPECT_FALSE(uic._cloudAudio.haveStart);
}

TEST(KnowledgeFollowUpRouting, OwnedTerminalStopClearsPrebufferButStaleStopPreservesNewOwner)
{
  UserIntentComponent uic;
  Open(uic);
  Prebuffer(uic);
  uic.StopConversationStream(42, true);
  ExpectRejectedAudioCleared(uic);
  uic.StartFollowUpStreaming(43);
  Prebuffer(uic, 43, "answer-43");
  uic.StopConversationStream(42, true);
  EXPECT_TRUE(uic._acceptStreamResults);
  EXPECT_TRUE(uic._cloudAudio.haveStart);
  EXPECT_EQ("answer-43", uic._cloudAudio.responseId);
  EXPECT_EQ("answer-43", uic._cloudAudioExpectedId);
  EXPECT_EQ(kCloudAudioMaxBytes, uic._cloudAudio.pcm.size());
  EXPECT_EQ(kCloudAudioMaxBytes, uic.ConsumeCloudAudioPcm("answer-43").size());
}

TEST(KnowledgeFollowUpRouting, AcceptedAnswerSurvivesActualBehaviorReleaseAndSessionCleanup)
{
  for (const auto& query : {"", " \t\r\n", "An independent question"}) {
    for (bool buffered : {true, false}) {
      UserIntentComponent uic;
      Open(uic);
      BehaviorConversationFollowUp followup(uic);
      if (buffered) { Prebuffer(uic); }
      const auto params = Params(query);
      Result(uic, params);
      CloudMic::StreamIdentifier closed;
      closed.streamId = 42;
      uic.OnCloudData(CloudMic::Message::CreatestreamClosed(std::move(closed)));
      uic.Update();
      ASSERT_EQ(State::AwaitingClaim, uic.conversation.state.GetState());
      ASSERT_EQ(params, uic.pending["params"]);
      uic.conversation.UpdateDependent({});
      followup.BehaviorUpdate();
      ASSERT_TRUE(followup.cancelled);
      ASSERT_EQ(0u, followup._streamId);
      uic.conversation.UpdateDependent({});
      EXPECT_EQ(State::AwaitingClaim, uic.conversation.state.GetState());
      ASSERT_EQ(1u, uic.robot.messages.size()); // no transport abort at handoff

      uic.active = std::make_shared<UserIntentData>();
      uic.active->intent.response.answer = params["answer"].asString();
      uic.active->intent.response.query_text = query;
      uic.active->intent.response.response_id = "answer-42";
      uic.active->intent.response.cloud_audio_available = true;
      uic.DropAnyUserIntent();
      uic.conversation.state.Claimed(2, true, true, Now());
      uic.conversation.UpdateDependent({});
      BehaviorKnowledgeGraphQuestion kg(uic);
      kg.BehaviorUpdate();
      ASSERT_EQ(0u, kg.opened);
      ASSERT_EQ("answer-42", uic._cloudAudioExpectedId);
      if (!buffered) { Prebuffer(uic); }

      // Retired cleanup cannot discard accepted PCM, even after ownership claim.
      uic.StopConversationStream(41, true);
      uic.ClearCloudAudio("retired-response");
      followup._streamId = 41;
      followup.OnBehaviorDeactivated();
      uic.conversation.UpdateDependent({});
      uint32_t rate = 0;
      uint8_t channels = 0;
      ASSERT_TRUE(uic.IsCloudAudioReady("answer-42", rate, channels, 3200));
      kg.BeginResponseCloudAudio();
      EXPECT_EQ(1u, kg.info.prepares);
      EXPECT_NE(0u, kg.info.playbackId);
      EXPECT_GT(kg.info.sent.size(), 0u);
      EXPECT_EQ(kCloudAudioMaxBytes, kg._dVars.cloudAudioPcm.size() + kg.info.sent.size());
      EXPECT_TRUE(std::all_of(kg.info.sent.begin(), kg.info.sent.end(),
                             [](uint8_t byte) { return byte == 0x5a; }));
      EXPECT_TRUE(std::all_of(kg._dVars.cloudAudioPcm.begin(), kg._dVars.cloudAudioPcm.end(),
                             [](uint8_t byte) { return byte == 0x5a; }));
      EXPECT_EQ(0u, kg.failures);

      // Successful renderer completion, deactivation and actual component cleanup.
      auto& policy = uic.conversation.state;
      policy.ResponseFinished(policy.Token(), Outcome::Succeeded);
      uic.active.reset();
      policy.Deactivated(2, Now());
      uic.conversation.UpdateDependent({});
      EXPECT_EQ(State::Settling, policy.GetState());
      ExpectRejectedAudioCleared(uic);
      EXPECT_TRUE(uic.robot.messages.back().stopping);
      EXPECT_EQ(42u, uic.robot.messages.back().stop.streamId);
    }
  }
}

TEST(KnowledgeFollowUpRouting, RejectedAnswerAndCancelledListenerRunActualOwnedCleanup)
{
  for (bool rejected : {true, false}) {
    UserIntentComponent uic;
    Open(uic);
    Prebuffer(uic);
    BehaviorConversationFollowUp followup(uic);
    if (rejected) {
      Result(uic, Params("stop", "Do not render"));
      uic.Update();
      followup.BehaviorUpdate();
    } else {
      followup.OnBehaviorDeactivated();
    }
    uic.conversation.UpdateDependent({});
    EXPECT_FALSE(uic.conversation.state.Active());
    EXPECT_EQ(0u, uic.delivered);
    ExpectRejectedAudioCleared(uic);
    EXPECT_TRUE(uic.robot.messages.back().stopping);
    EXPECT_EQ(42u, uic.robot.messages.back().stop.streamId);
  }
}

TEST(KnowledgeFollowUpRouting, RendererKeepsOwnedStreamForDelayedSentenceAndCompletesExactPCM)
{
  UserIntentComponent uic;
  Open(uic);
  Result(uic, Params(""));
  uic.Update();
  ASSERT_EQ(State::AwaitingClaim, uic.conversation.state.GetState());
  BehaviorConversationFollowUp followup(uic);
  followup.BehaviorUpdate();
  uic.active = std::make_shared<UserIntentData>();
  uic.active->intent.response.answer = "Synthetic test answer";
  uic.active->intent.response.response_id = "answer-42";
  uic.active->intent.response.cloud_audio_available = true;
  uic.DropAnyUserIntent();
  uic.conversation.state.Claimed(2, true, true, Now());
  uic.conversation.UpdateDependent({});
  BehaviorKnowledgeGraphQuestion kg(uic);
  kg.BehaviorUpdate();
  AudioStart(uic, 42, "answer-42");
  CloudMic::ResponseAudioChunk chunk{};
  chunk.streamId = 42;
  chunk.responseId = "answer-42";
  chunk.sequenceNumber = 0;
  chunk.data.assign(24000, 0x5a);
  uic.OnCloudData(CloudMic::Message(std::move(chunk)));
  kg.BeginResponseCloudAudio();
  ASSERT_EQ(24000u, kg.info.sent.size());
  EXPECT_EQ(0u, kg.info.completions);
  EXPECT_TRUE(uic._acceptStreamResults);
  uic.StopConversationStream(41, true);
  uic.ClearCloudAudio("stale-answer");
  uic.conversation.UpdateDependent({});
  BaseStationTimer::getInstance()->now += 1;
  CloudMic::ResponseAudioChunk delayed{};
  delayed.streamId = 42;
  delayed.responseId = "answer-42";
  delayed.sequenceNumber = 1;
  delayed.data.assign(1024, 0x6b);
  uic.OnCloudData(CloudMic::Message(std::move(delayed)));
  CloudMic::ResponseAudioEnd end{};
  end.streamId = 42;
  end.responseId = "answer-42";
  end.finalSequenceNumber = 2;
  uic.OnCloudData(CloudMic::Message(std::move(end)));
  kg.UpdateCloudAudioStreaming();
  ASSERT_EQ(25024u, kg.info.sent.size());
  EXPECT_TRUE(std::all_of(kg.info.sent.begin(), kg.info.sent.begin() + 24000,
                         [](uint8_t byte) { return byte == 0x5a; }));
  EXPECT_TRUE(std::all_of(kg.info.sent.begin() + 24000, kg.info.sent.end(),
                         [](uint8_t byte) { return byte == 0x6b; }));
  EXPECT_EQ(1u, kg.info.completions);
  EXPECT_EQ(0u, kg.failures);
  EXPECT_TRUE(kg._dVars.cloudAudioCompleteSent);
  EXPECT_TRUE(uic._cloudAudio.pcm.empty());
}
TEST(KnowledgeFollowUpRouting, InterruptedCloudAnswerStopsWithoutFallbackOrFollowUp)
{
  for (auto kind : {CloudMic::ResponseAudioErrorType::Cancelled,
                    CloudMic::ResponseAudioErrorType::Transport}) {
    for (int playback : {-1, 0, 1, 2}) {
      for (int policy : {0, 1, 2}) {
        UserIntentComponent uic;
        uic._expectedStreamId = 42;
        auto& session = uic.conversation.state;
        session.config.enabled = policy != 0;
        session.config.maxTurns = policy == 1 ? 1 : 5;
        session.Claimed(1, true, true, Now());
        BehaviorKnowledgeGraphQuestion kg(uic);
        kg._dVars.conversationToken = session.Token();
        kg._dVars.responseId = "interrupted";
        kg._dVars.responseString = "Do not repeat this partial answer.";
        kg._dVars.responseSource = BehaviorKnowledgeGraphQuestion::EResponseSource::CloudAudio;
        kg._dVars.state = BehaviorKnowledgeGraphQuestion::EState::Responding;
        uic.SetExpectedCloudAudioResponse("interrupted");
        AudioStart(uic, 42, "interrupted");
        CloudMic::ResponseAudioChunk chunk{};
        chunk.streamId = 42;
        chunk.responseId = "interrupted";
        chunk.data.assign(64000, 0x5a);
        uic.OnCloudData(CloudMic::Message(std::move(chunk)));
        if (playback > 0) {
          kg.BeginResponseCloudAudio();
          ASSERT_EQ(64000u, kg.info.sent.size());
          ASSERT_TRUE(kg._dVars.cloudAudioPlayback.UpdateProgress(32000, playback == 2 ? 32000 : 1));
        }
        CloudMic::ResponseAudioError error{};
        error.streamId = 42;
        error.responseId = "interrupted";
        error.error = kind;
        uic.OnCloudData(CloudMic::Message(std::move(error)));
        ASSERT_TRUE(uic.HasCloudAudioInterruption("interrupted"));
        EXPECT_FALSE(uic.IsCloudAudioComplete("interrupted"));
        EXPECT_TRUE(uic._cloudAudio.pcm.empty());
        if (playback > 0) { kg.UpdateCloudAudioStreaming(); }
        else if (playback == 0) { kg.TransitionToSearchingLoop(); }
        else { kg.BeginResponseCloudAudio(); }
        EXPECT_TRUE(kg.cancelled);
        EXPECT_TRUE(kg._dVars.cloudAudioFailed);
        EXPECT_TRUE(kg._dVars.cloudAudioResponseFinished);
        EXPECT_EQ(0u, kg.failures); // Neither local TTS nor failure animation.
        EXPECT_EQ(0u, kg.info.completions);
        EXPECT_EQ(playback > 0 ? 1u : 0u, kg.info.cancellations);
        EXPECT_TRUE(kg._dVars.cloudAudioPcm.empty());
        session.Deactivated(1, Now());
        EXPECT_FALSE(session.Active());
        EXPECT_FALSE(session.Open(Now() + 1));
      }
    }
  }
}

TEST(KnowledgeFollowUpRouting, OrdinaryProviderFailureRetainsLocalFallback)
{
  UserIntentComponent uic;
  BehaviorKnowledgeGraphQuestion kg(uic);
  kg._dVars.responseId = "provider";
  kg._dVars.responseString = "Complete answer available for local synthesis.";
  kg._dVars.state = BehaviorKnowledgeGraphQuestion::EState::Responding;
  kg.FailCloudAudioResponse();
  EXPECT_FALSE(kg.cancelled);
  EXPECT_EQ(BehaviorKnowledgeGraphQuestion::EResponseSource::LocalTts, kg._dVars.responseSource);
  EXPECT_EQ(1u, kg.failures);
}

TEST(KnowledgeFollowUpRouting, ReadinessTimeoutIsQuietOnlyForEstablishedAnswer)
{
  for (bool established : {false, true}) {
    UserIntentComponent uic;
    uic._expectedStreamId = 42;
    BehaviorKnowledgeGraphQuestion kg(uic);
    kg._dVars.responseId = "waiting";
    kg._dVars.responseString = "Answer";
    kg._dVars.responseSource = BehaviorKnowledgeGraphQuestion::EResponseSource::CloudAudio;
    if (established) { AudioStart(uic, 42, "waiting"); }
    kg.TransitionToSearchingLoop();
    EXPECT_EQ(established, kg.cancelled);
    EXPECT_EQ(0u, kg.info.completions);
    if (!established) {
      EXPECT_EQ(BehaviorKnowledgeGraphQuestion::EResponseSource::LocalTts, kg._dVars.responseSource);
    }
  }
}
}}
