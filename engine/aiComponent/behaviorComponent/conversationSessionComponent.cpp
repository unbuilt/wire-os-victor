#include "engine/aiComponent/behaviorComponent/conversationSessionComponent.h"
#include "engine/aiComponent/behaviorComponent/userIntentComponent.h"
#include "engine/aiComponent/behaviorComponent/sleepTracker.h"
#include "engine/aiComponent/aiComponent.h"
#include "engine/aiComponent/alexaComponent.h"
#include "engine/components/battery/batteryComponent.h"
#include "clad/externalInterface/messageEngineToGame.h"
#include "engine/components/sdkComponent.h"
#include "engine/aiComponent/behaviorComponent/behaviorExternalInterface/beiRobotInfo.h"
#include "clad/types/offTreadsStates.h"
#include "engine/robot.h"
#include "coretech/common/engine/utils/timer.h"
#include "json/json.h"
#include "util/console/consoleInterface.h"
#include "util/logging/logging.h"
#include "util/logging/DAS.h"

namespace Anki {
namespace Vector {
namespace {
CONSOLE_VAR(bool, kAutomaticFollowUpEnabled, "MultiTurnVoice", true);
double Now() { return BaseStationTimer::getInstance()->GetCurrentTimeInSecondsDouble(); }
}

ConversationSessionComponent::ConversationSessionComponent()
: IDependencyManagedComponent(this, BCComponentID::ConversationSessionComponent) {}

void ConversationSessionComponent::GetInitDependencies(BCCompIDSet& deps) const
{
  deps.insert(BCComponentID::UserIntentComponent);
  deps.insert(BCComponentID::SleepTracker);
  deps.insert(BCComponentID::RobotInfo);
}
void ConversationSessionComponent::GetUpdateDependencies(BCCompIDSet& deps) const
{
  GetInitDependencies(deps);
}
void ConversationSessionComponent::InitDependent(Robot* robot, const BCCompMap& comps)
{
  _robot = robot;
  _comps = &comps;
  _uic = &comps.GetComponent<UserIntentComponent>();
  _uic->AttachConversation(this);
}

void ConversationSessionComponent::Configure(const Json::Value& value)
{
  ConversationSessionState::Config config;
  bool valid = value.isNull() || value.isObject();
  if (value.isObject()) {
    if (value.isMember("enabled")) {
      valid &= value["enabled"].isBool();
      if (value["enabled"].isBool()) { config.enabled = value["enabled"].asBool(); }
    }
    if (value.isMember("maxTurns")) {
      valid &= value["maxTurns"].isUInt();
      if (value["maxTurns"].isUInt()) { config.maxTurns = value["maxTurns"].asUInt(); }
    }
    for (const auto* key : {"sessionTimeout_sec", "followUpSettleTime_ms"}) {
      if (!value.isMember(key)) { continue; }
      valid &= value[key].isNumeric();
      if (value[key].isNumeric()) {
        const double number = value[key].asDouble();
        if (std::string(key) == "sessionTimeout_sec") { config.sessionTimeout_sec = number; }
        else { config.followUpSettleTime_ms = number; }
      }
    }
    for (const auto& key : value.getMemberNames()) {
      valid &= key == "enabled" || key == "maxTurns" || key == "sessionTimeout_sec" ||
               key == "followUpSettleTime_ms";
    }
  }
  valid &= config.IsValid();
  if (!valid) {
    PRINT_NAMED_ERROR("ConversationSession.InvalidConfig", "Invalid multiTurnVoice configuration; disabled");
    config.enabled = false;
  }
  _policy.config = config;
}

bool ConversationSessionComponent::IsSafe() const
{
  if (!_robot || !_uic || !IsEnabled()) { return false; }
  ShutdownReason reason;
  const auto& battery = _robot->GetComponent<BatteryComponent>();
  return !_robot->ToldToShutdown(reason) && !battery.IsBatteryLow() &&
         !battery.IsBatteryOverheated() && !battery.IsChargingStalledBecauseTooHot() &&
         !_uic->IsMicMuted() && _robot->GetOffTreadsState() == OffTreadsState::OnTreads &&
         _comps->GetComponent<BEIRobotInfo>().GetCpuTemperature_degC() < 90 &&
         !_comps->GetComponent<SleepTracker>().IsSleeping() &&
         !_robot->GetComponent<SDKComponent>().SDKWantsControl() &&
         _robot->GetComponent<AIComponent>().GetComponent<AlexaComponent>().IsIdle() &&
         _uic->GetEngineShouldRespondToTriggerWord() && !_uic->IsTriggerWordPending();
}

bool ConversationSessionComponent::IsEnabled() const
{
  return _policy.config.enabled && kAutomaticFollowUpEnabled;
}

bool ConversationSessionComponent::WantsFollowUp() const
{
  return _policy.Ready(Now()) && IsSafe() &&
         !_uic->IsAnyUserIntentPending() && !_uic->IsAnyUserIntentActive() &&
         _uic->IsCaptureQuiescent(_uic->GetCurrentStreamId());
}

uint32_t ConversationSessionComponent::OpenFollowUp()
{
  if (!WantsFollowUp() || !_policy.Open(Now())) { return 0; }
  _ownedStream = _uic->AllocateStreamId();
  _stopRequested = false;
  _uic->StartFollowUpStreaming(_ownedStream);
  DASMSG(followup_open, "voice.followup.open", "Settle complete; requesting follow-up earcon and fresh capture");
  DASMSG_SET(i1, _ownedStream, "Stream identity");
  DASMSG_SET(i2, _policy.Turn(), "Conversation turn");
  DASMSG_SEND();
  PRINT_NAMED_INFO("ConversationSession.Open", "turn=%u stream=%u token=%llu",
                   _policy.Turn(), _ownedStream, static_cast<unsigned long long>(_policy.Token()));
  return _ownedStream;
}

void ConversationSessionComponent::CancelFollowUp(uint32_t streamId)
{
  if (streamId == 0 || streamId != _ownedStream) { return; }
  const auto state = _policy.GetState();
  if (state == ConversationSessionState::State::Opening ||
      state == ConversationSessionState::State::Listening) {
    _policy.End("listening_cancelled");
  }
}

void ConversationSessionComponent::UpdateDependent(const BCCompMap&)
{
  using State = ConversationSessionState::State;
  const double now = Now();
  if (!kAutomaticFollowUpEnabled) { _policy.End("disabled"); }
  _policy.Update(now, IsSafe());
  if ((_policy.GetState() == State::Settling || _policy.GetState() == State::Quiescing) &&
      (_uic->IsAnyUserIntentPending() || _uic->IsAnyUserIntentActive())) {
    _policy.End("competing_intent");
  }
  if (_policy.Active()) { _hadSession = true; }
  if (_policy.GetState() == State::Quiescing) {
    if (!_stopRequested) {
      _stopRequested = true;
      // The answer and response behavior are done. This also cancels any
      // residual cloud worker, without relying on engine's stream-open flag.
      _uic->StopConversationStream(_uic->GetCurrentStreamId());
    }
    if (_uic->IsCaptureQuiescent(_uic->GetCurrentStreamId())) {
      _policy.Quiesced(now);
      DASMSG(followup_settle, "voice.followup.settle", "Response deactivated and previous capture quiescent");
      DASMSG_SET(i1, _uic->GetCurrentStreamId(), "Previous stream identity");
      DASMSG_SEND();
    }
  } else if (_policy.GetState() == State::Responding) {
    _stopRequested = false;
    _ownedStream = _uic->GetCurrentStreamId();
  } else if (_policy.GetState() == State::Opening) {
    if (_uic->IsCaptureOpen(_ownedStream)) {
      _policy.CaptureOpened(now);
      DASMSG(followup_listening, "voice.followup.listening", "Engine received capture-ready acknowledgement");
      DASMSG_SET(i1, _ownedStream, "Stream identity");
      DASMSG_SEND();
    } else if (_uic->IsCaptureQuiescent(_ownedStream)) {
      _policy.End("capture_rejected");
    }
  }
  if (!_policy.Active()) {
    if (_hadSession) {
      DASMSG(followup_end, "voice.followup.end", "Conversation ended");
      DASMSG_SET(s1, _policy.EndReason(), "Termination reason");
      DASMSG_SET(i1, _ownedStream, "Stream identity");
      DASMSG_SET(i2, _policy.Turn(), "Conversation turn");
      DASMSG_SEND();
      PRINT_NAMED_INFO("ConversationSession.End", "turn=%u reason=%s",
                       _policy.Turn(), _policy.EndReason());
      _hadSession = false;
    }
    if (_ownedStream != 0) {
      // A valid non-KG command may already be pending. Never drop it, nor
      // cancel a newer stream. Disable during an answer does not stop audio.
      const bool finishingDisabledAnswer = std::string(_policy.EndReason()) == "disabled" &&
        (_uic->IsUserIntentActive(USER_INTENT(knowledge_question)) ||
         _uic->IsUserIntentActive(USER_INTENT(knowledge_response_bypass)) ||
         _uic->IsUserIntentPending(USER_INTENT(knowledge_question)) ||
         _uic->IsUserIntentPending(USER_INTENT(knowledge_response_bypass)));
      if (_ownedStream != _uic->GetCurrentStreamId() || !finishingDisabledAnswer) {
        _uic->StopConversationStream(_ownedStream);
        _ownedStream = 0;
      }
    }
    _stopRequested = false;
  }
}
}
}
