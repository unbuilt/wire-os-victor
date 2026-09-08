#include "engine/aiComponent/behaviorComponent/behaviors/robotDrivenDialog/behaviorConversationFollowUp.h"
#include "engine/aiComponent/behaviorComponent/conversationSessionComponent.h"
#include "engine/aiComponent/behaviorComponent/userIntentComponent.h"
#include "engine/actions/animActions.h"
#include "clad/types/animationTrigger.h"
#include "audioEngine/multiplexer/audioCladMessageHelper.h"

namespace Anki {
namespace Vector {
BehaviorConversationFollowUp::BehaviorConversationFollowUp(const Json::Value& config)
: ICozmoBehavior(config) {}

bool BehaviorConversationFollowUp::WantsToBeActivatedBehavior() const
{
  return GetBehaviorComp<ConversationSessionComponent>().WantsFollowUp();
}

void BehaviorConversationFollowUp::GetBehaviorOperationModifiers(BehaviorOperationModifiers& modifiers) const
{
  modifiers.wantsToBeActivatedWhenOnCharger = true;
  modifiers.behaviorAlwaysDelegates = false;
}

void BehaviorConversationFollowUp::OnBehaviorActivated()
{
  _listening = false;
  _captureClosed = false;
  _gettingOut = false;
  // Anim finishes the ordinary listening earcon before establishing the fresh
  // capture boundary. No repeated spoken prompt or motor get-in.
  const auto earcon = AudioEngine::Multiplexer::CladMessageHelper::CreatePostAudioEvent(
    AudioMetaData::GameEvent::GenericEvent::Play__Robot_Vic_Sfx__Wake_Word_On,
    AudioMetaData::GameObjectType::Behavior, 0);
  SmartPushResponseToTriggerWord(AnimationTrigger::Count, earcon, StreamAndLightEffect::StreamingEnabled);
  _streamId = GetBehaviorComp<ConversationSessionComponent>().OpenFollowUp();
  if (_streamId == 0) { CancelSelf(); }
}

void BehaviorConversationFollowUp::OnBehaviorDeactivated()
{
  GetBehaviorComp<ConversationSessionComponent>().CancelFollowUp(_streamId);
  _streamId = 0;
}

void BehaviorConversationFollowUp::BehaviorUpdate()
{
  if (!IsActivated() || _gettingOut) { return; }
  auto& session = GetBehaviorComp<ConversationSessionComponent>();
  auto& uic = GetBehaviorComp<UserIntentComponent>();
  // Preserve the normal pending-intent expiry and dispatcher routing.
  if (uic.IsAnyUserIntentPending() || uic.IsAnyUserIntentActive()) {
    CancelSelf();
    return;
  }
  using State = ConversationSessionState::State;
  const auto state = session.Policy().GetState();
  if (state != State::Opening && state != State::Listening) {
    if (_captureClosed) {
      CancelSelf();
      return;
    }
    _gettingOut = true;
    CancelDelegates(false);
    DelegateIfInControl(new TriggerAnimationAction(AnimationTrigger::VC_ListeningGetOut),
                        [this]() { CancelSelf(); });
  } else if (_listening && uic.IsCaptureQuiescent(_streamId)) {
    // Capture can finish before cloud recognition. Stop advertising listening,
    // but retain ownership and the result guard until normal routing can run.
    _listening = false;
    _captureClosed = true;
    CancelDelegates(false);
    DelegateIfInControl(new TriggerAnimationAction(AnimationTrigger::VC_ListeningGetOut));
  } else if (!_listening && !_captureClosed && uic.IsCaptureOpen(_streamId)) {
    _listening = true;
    DelegateIfInControl(new ReselectingLoopAnimationAction(AnimationTrigger::VC_ListeningLoop));
  }
}
}
}
