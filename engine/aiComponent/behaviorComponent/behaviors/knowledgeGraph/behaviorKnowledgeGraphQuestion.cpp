/***********************************************************************************************************************
 *
 *  BehaviorLeaveAMessage
 *  Victor / Engine
 *
 *  Created by Jarrod Hatfield on 4/01/2018
 *
 *  Description
 *  + Parent behavior for allowing the user to record a message and save it to Victor's local storage.
 *
 **********************************************************************************************************************/

// #include "engine/aiComponent/behaviorComponent/behaviors/knowledgeGraph/behaviorKnowledgeGraphQuestion.h"
#include "behaviorKnowledgeGraphQuestion.h"
#include "clad/externalInterface/messageEngineToGame.h"
#include "engine/actions/animActions.h"
#include "engine/actions/compoundActions.h"
#include "engine/aiComponent/behaviorComponent/behaviors/animationWrappers/behaviorTextToSpeechLoop.h"
#include "engine/aiComponent/behaviorComponent/behaviorContainer.h"
#include "engine/aiComponent/behaviorComponent/behaviorExternalInterface/behaviorExternalInterface.h"
#include "engine/aiComponent/behaviorComponent/behaviorExternalInterface/beiRobotInfo.h"
#include "engine/aiComponent/behaviorComponent/userIntentComponent.h"
#include "engine/aiComponent/behaviorComponent/userIntentData.h"
#include "engine/aiComponent/behaviorComponent/userIntents.h"
#include "engine/audio/engineRobotAudioClient.h"
#include "engine/components/localeComponent.h"
#include "engine/components/sdkComponent.h"
#include "engine/actions/basicActions.h"
#include "engine/robot.h"

#include "clad/robotInterface/messageEngineToRobot.h"
#include "clad/robotInterface/messageRobotToEngine.h"
#include "clad/types/sdkAudioTypes.h"

#include "coretech/common/engine/jsonTools.h"
#include "coretech/common/engine/utils/timer.h"
#include "util/console/consoleInterface.h"
#include "util/global/globalDefinitions.h"
#include "util/logging/logging.h"
#include "clad/types/animationTrigger.h"

#include <algorithm>
#include <cstring>

#define PRINT_DEBUG(format, ...) \
  PRINT_CH_DEBUG("KnowledgeGraph", "BehaviorKnowledgeGraphQuestion", format, ##__VA_ARGS__)

#define PRINT_INFO(format, ...) \
  PRINT_CH_INFO("KnowledgeGraph", "BehaviorKnowledgeGraphQuestion", format, ##__VA_ARGS__)

namespace Anki
{
  namespace Vector
  {

    namespace
    {
      const char *kKey_Duration = "streamingTimeout";
      const char *kKey_ReadyStringID = "readyStringID";
      const char *kKey_EarConEnd = "earConAudioEventEnd";
      const char *kKey_CloudAudioEnabled = "cloudAudioEnabled";
      const char *kKey_CloudAudioRequestTimeout = "cloudAudioRequestTimeout";
      const char *kKey_CloudAudioReadyTimeout = "cloudAudioReadyTimeout";
      const char *kKey_CloudAudioFallback = "cloudAudioFallbackToLocalTts";
      const char *kKey_CloudAudioVolume = "cloudAudioVolume";

      const double kDefaultDuration = 10.0;
      const char *kDefaultReadyStringID = "BehaviorKnowledgeGraphQuestion.Ready";

      // Cloud audio pacing / playback tuning.
      constexpr double kCloudAudioBufferAheadSec = 3.0;   // keep at most this much audio buffered in anim
      constexpr double kCloudAudioCompletionGraceSec = 5.0;
      constexpr uint16_t kCloudAudioMaxChunkBytes = 1024; // matches the anim streaming player limit
      // The cloud synthesizes an answer sentence by sentence, so playback starts on a
      // prebuffer rather than on the whole answer. 0.75s at 16kHz mono s16le.
      constexpr size_t kCloudAudioPrebufferBytes = 24000;
      // How long the answer may stall mid-stream before we treat it as finished. The
      // anim player pads gaps with silence, so a stall is audible but not fatal.
      constexpr double kCloudAudioStallTimeoutSec = 20.0;
      // Control is held in slices while the answer streams, since its length is not
      // known up front.
      constexpr float kCloudAudioWaitSliceSec = 1.0f;
      // Absolute cap on a single answer, in case the stream never ends.
      constexpr double kCloudAudioMaxPlaybackSec = 90.0;
      // Below this much played audio a broken answer is re-spoken locally rather
      // than left truncated.
      constexpr double kCloudAudioSalvageSec = 1.5;
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    BehaviorKnowledgeGraphQuestion::InstanceConfig::InstanceConfig() : streamingDuration(kDefaultDuration),
                                                                       earConEnd(AudioMetaData::GameEvent::GenericEvent::Invalid)
    {
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    BehaviorKnowledgeGraphQuestion::DynamicVariables::DynamicVariables() : state(EState::GettingIn),
                                                                           streamingBeginTime(0.0),
                                                                           ttsGenerationStatus(EGenerationStatus::None),
                                                                           wasPickedUp(false)
    {
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    BehaviorKnowledgeGraphQuestion::BehaviorKnowledgeGraphQuestion(const Json::Value &config) : ICozmoBehavior(config),
                                                                                                _readyTTSWrapper(UtteranceTriggerType::Immediate)
    {
      JsonTools::GetValueOptional(config, kKey_Duration, _iVars.streamingDuration);
      _iVars.readyStringID = JsonTools::ParseString(config, kKey_ReadyStringID, kDefaultReadyStringID);

      std::string earConString;
      if (JsonTools::GetValueOptional(config, kKey_EarConEnd, earConString))
      {
        _iVars.earConEnd = AudioMetaData::GameEvent::GenericEventFromString(earConString);
      }

      JsonTools::GetValueOptional(config, kKey_CloudAudioEnabled, _iVars.cloudAudioEnabled);
      JsonTools::GetValueOptional(config, kKey_CloudAudioRequestTimeout, _iVars.cloudAudioRequestTimeout);
      JsonTools::GetValueOptional(config, kKey_CloudAudioReadyTimeout, _iVars.cloudAudioReadyTimeout);
      JsonTools::GetValueOptional(config, kKey_CloudAudioFallback, _iVars.cloudAudioFallbackToLocalTts);
      {
        int cloudAudioVolume = static_cast<int>(_iVars.cloudAudioVolume);
        if (JsonTools::GetValueOptional(config, kKey_CloudAudioVolume, cloudAudioVolume))
        {
          _iVars.cloudAudioVolume = static_cast<uint32_t>(cloudAudioVolume);
        }
      }

      SubscribeToTags({{
          ExternalInterface::MessageEngineToGameTag::TouchButtonEvent,
          ExternalInterface::MessageEngineToGameTag::RobotFallingEvent, // do we need this?
      }});

      // cloud-audio playback status is reported by the anim process
      SubscribeToTags({RobotInterface::RobotToEngineTag::audioStreamStatusEvent});
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    void BehaviorKnowledgeGraphQuestion::GetBehaviorJsonKeys(std::set<const char *> &expectedKeys) const
    {
      expectedKeys.insert(kKey_Duration);
      expectedKeys.insert(kKey_ReadyStringID);
      expectedKeys.insert(kKey_EarConEnd);
      expectedKeys.insert(kKey_CloudAudioEnabled);
      expectedKeys.insert(kKey_CloudAudioRequestTimeout);
      expectedKeys.insert(kKey_CloudAudioReadyTimeout);
      expectedKeys.insert(kKey_CloudAudioFallback);
      expectedKeys.insert(kKey_CloudAudioVolume);
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    void BehaviorKnowledgeGraphQuestion::InitBehavior()
    {
      const BehaviorContainer &container = GetBEI().GetBehaviorContainer();
      container.FindBehaviorByIDAndDowncast<BehaviorTextToSpeechLoop>(BEHAVIOR_ID(KnowledgeGraphTTS),
                                                                      BEHAVIOR_CLASS(TextToSpeechLoop),
                                                                      _iVars.ttsBehavior);

      _readyTTSWrapper.Initialize(GetBEI().GetTextToSpeechCoordinator());
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    void BehaviorKnowledgeGraphQuestion::GetAllDelegates(std::set<IBehavior *> &delegates) const
    {
      delegates.insert(_iVars.ttsBehavior.get());
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    void BehaviorKnowledgeGraphQuestion::GetBehaviorOperationModifiers(BehaviorOperationModifiers &modifiers) const
    {
      modifiers.wantsToBeActivatedWhenCarryingObject = true;
      modifiers.wantsToBeActivatedWhenOnCharger = true;
      modifiers.wantsToBeActivatedWhenOffTreads = true;
      modifiers.behaviorAlwaysDelegates = true;
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    bool BehaviorKnowledgeGraphQuestion::WantsToBeActivatedBehavior() const
    {
      return true;
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    void BehaviorKnowledgeGraphQuestion::OnBehaviorActivated()
    {
      _dVars = DynamicVariables();

      // Get ready text for current locale
      const auto &bei = GetBEI();
      const auto &robotInfo = bei.GetRobotInfo();
      const auto &localeComponent = robotInfo.GetLocaleComponent();
      std::string readyText = localeComponent.GetString(_iVars.readyStringID);

      // Remove ready for intent graph responses
      UserIntentComponent &uic = GetBehaviorComp<UserIntentComponent>();
      UserIntentPtr intentDataPtr = uic.GetUserIntentIfActive(USER_INTENT(knowledge_response_bypass));
      if (intentDataPtr != nullptr)
      {
        readyText = "";
      }

      const std::string &readyTextAddr = readyText;

      // start generating our ready text; if we fail, then we'll simply exit the behavior, and cry :(
      if (_readyTTSWrapper.SetUtteranceText(readyTextAddr, {}))
      {
        auto callback = [this]()
        {
          // after our getin animation, we can prompt the user to speak
          _dVars.state = EState::WaitingToStream;

          // Need to loop this forever and we'll just cancel it on our own after a timeout
          DelegateIfInControl(new ReselectingLoopAnimationAction(AnimationTrigger::KnowledgeGraphListening));
        };

        // open up streaming after we play our get-in to avoid motor noise
        DelegateIfInControl(new TriggerLiftSafeAnimationAction(AnimationTrigger::KnowledgeGraphGetIn), callback);
      }
      else
      {
        PRINT_NAMED_WARNING("BehaviorKnowledgeGraphQuestion", "Failed to generate Ready TTS (%s)", readyTextAddr.c_str());
      }
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    void BehaviorKnowledgeGraphQuestion::OnBehaviorDeactivated()
    {
      // make sure we cancel the ready utterance if we bail during it playing
      _readyTTSWrapper.CancelUtterance();

      // if we were streaming cloud audio and didn't finish cleanly, stop it and drop the buffer
      if (EResponseSource::CloudAudio == _dVars.responseSource && !_dVars.cloudAudioResponseFinished)
      {
        CancelCloudAudioPlayback();
      }
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    void BehaviorKnowledgeGraphQuestion::HandleWhileActivated(const EngineToGameEvent &event)
    {
      using namespace ExternalInterface;

      const MessageEngineToGameTag tag = event.GetData().GetTag();
      switch (tag)
      {
      case MessageEngineToGameTag::TouchButtonEvent:
      case MessageEngineToGameTag::RobotFallingEvent:
      {
        if (EState::Responding == _dVars.state)
        {
          OnResponseInterrupted();
        }
        break;
      }

      default:
        break;
      }
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    void BehaviorKnowledgeGraphQuestion::BehaviorUpdate()
    {
      if (IsActivated())
      {
        UserIntentComponent &uic = GetBehaviorComp<UserIntentComponent>();
        UserIntentPtr intentDataPtr = uic.GetUserIntentIfActive(USER_INTENT(knowledge_response_bypass));

        // Enter if the a knowledge response was returned and activated
        if (intentDataPtr != nullptr && EState::WaitingToStream == _dVars.state)
        {
          CancelDelegates(false);
          // This skips over the streaming because the results were alreday returned
          OnStreamingComplete(true);
        }
        // at this point our get in animation is complete, so as soon as the audio is finished playing we can transition in
        else if (EState::WaitingToStream == _dVars.state)
        {
          // once we've finished speaking our ready text, we can start streaming
          // hopefully the ready text is already finished by the time we even get into this state
          if (_readyTTSWrapper.IsFinished())
          {
            BeginStreamingQuestion();
          }
          else if (!_readyTTSWrapper.IsValid())
          {
            PRINT_NAMED_WARNING("BehaviorKnowledgeGraphQuestion", "Ready prompt TTS failed to play, not cool man");

            // we need to bail, luckily TransitionToNoResponse() handles this exact transition for us
            CancelDelegates(false);
            TransitionToNoResponse();
          }
        }
        // if we're recording the user's question, we need to be listening for a response
        else if (EState::Listening == _dVars.state)
        {
          // if we've gotten a response from knowledge graph, transition into the response state
          const double currentTime = BaseStationTimer::getInstance()->GetCurrentTimeInSecondsDouble();
          if (FLT_NEAR(_dVars.streamingBeginTime, 0.f) &&
              uic.IsCloudStreamOpen())
          {
            _dVars.streamingBeginTime = currentTime;
          }

          const double requestTimeout = _iVars.cloudAudioEnabled
            ? std::max(_iVars.streamingDuration, _iVars.cloudAudioRequestTimeout)
            : _iVars.streamingDuration;
          const bool timeIsUp = (!FLT_NEAR(_dVars.streamingBeginTime, 0.f)) &&
                                (currentTime >= (_dVars.streamingBeginTime + requestTimeout));
          if (IsResponsePending() || timeIsUp)
          {
            CancelDelegates(false);
            OnStreamingComplete(false);
          }
        }
        else if (EState::Responding == _dVars.state)
        {
          // pace any remaining cloud audio out to the anim process
          if (EResponseSource::CloudAudio == _dVars.responseSource)
          {
            UpdateCloudAudioStreaming();
          }

          const bool isPickedUp = GetBEI().GetRobotInfo().IsPickedUp();

          // if we're playing our response, allow victor to be interrupted
          // require victor to be on the ground prior to being picked up, else ignore it if he's already in the air
          if (!_dVars.wasPickedUp && isPickedUp)
          {
            OnResponseInterrupted();
          }

          _dVars.wasPickedUp = isPickedUp;
        }
      }
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    void BehaviorKnowledgeGraphQuestion::BeginStreamingQuestion()
    {
      PRINT_DEBUG("Knowledge Graph streaming begun ...");

      _dVars.state = EState::Listening;

      GetBehaviorComp<UserIntentComponent>().StartWakeWordlessStreaming(CloudMic::StreamType::KnowledgeGraph);
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    void BehaviorKnowledgeGraphQuestion::OnStreamingComplete(bool skipResponsePending)
    {
      // go into searching state until we can generate our response
      _dVars.state = EState::Searching;

      PlayEarconEnd();

      // see if we got a response from knowledge graph
      if (skipResponsePending)
      {
        ConsumeIntentGraphResponse();
      }
      else if (IsResponsePending())
      {
        // need to consume the response immediately or the system gets cranky
        ConsumeResponse();
      }

      // did we get a response from knowledge graph?
      if (!_dVars.responseString.empty())
      {
        PRINT_INFO("Streaming is complete ... valid response received");

        // decide whether to play cloud-synthesized audio or synthesize locally
        if (ShouldUseCloudAudio())
        {
          _dVars.responseSource = EResponseSource::CloudAudio;
          const double now = BaseStationTimer::getInstance()->GetCurrentTimeInSecondsDouble();
          _dVars.cloudAudioReadyDeadline = now + _iVars.cloudAudioReadyTimeout;
          PRINT_INFO("Cloud audio expected for response %s (waiting up to %.1fs)",
                     _dVars.responseId.c_str(), _iVars.cloudAudioReadyTimeout);
        }
        else
        {
          _dVars.responseSource = EResponseSource::LocalTts;
        }

        // start generating the response now so that we can minimize the wait time
        auto callback = [this](bool success)
        {
          if (success)
          {
            PRINT_INFO("TTS generation is complete");
            _dVars.ttsGenerationStatus = EGenerationStatus::Success;
          }
          else
          {
            PRINT_INFO("TTS generation FAILED");
            _dVars.ttsGenerationStatus = EGenerationStatus::Fail;
          }
        };

        _iVars.ttsBehavior->SetTextToSay(_dVars.responseString, callback);

        // let's transition into our "searching" loop
        // since we always want to loop at least once, play the get in + loop anim before we do any logic for the tts
        CompoundActionSequential *messageAnimation = new CompoundActionSequential();
        messageAnimation->AddAction(new TriggerLiftSafeAnimationAction(AnimationTrigger::KnowledgeGraphSearchingGetIn), true);
        messageAnimation->AddAction(new TriggerLiftSafeAnimationAction(AnimationTrigger::KnowledgeGraphSearching), true);

        DelegateIfInControl(messageAnimation,
                            &BehaviorKnowledgeGraphQuestion::TransitionToSearchingLoop);
      }
      else
      {
        PRINT_INFO("Streaming is complete ... NO response received");

        // if not, let the user know we failed ...
        TransitionToNoResponse();
      }
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    bool BehaviorKnowledgeGraphQuestion::IsResponsePending() const
    {
      const UserIntentComponent &uic = GetBehaviorComp<UserIntentComponent>();
      return uic.IsAnyUserIntentPending();
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    void BehaviorKnowledgeGraphQuestion::ConsumeResponse()
    {
      PRINT_DEBUG("Checking for response");

      // if we have a valid knowledge graph response intent, grab the response string from it
      // if it's not the knowledge graph response, we simply return the empty string which will be handled appropriately
      UserIntentComponent &uic = GetBehaviorComp<UserIntentComponent>();

      UserIntent intent;
      if (uic.IsUserIntentPending(USER_INTENT(knowledge_response), intent))
      {
        uic.DropUserIntent(intent.GetTag());

        // grab the response string
        const UserIntent_KnowledgeResponse &intentResponse = intent.Get_knowledge_response();
        _dVars.responseString = intentResponse.answer;
        _dVars.responseId = intentResponse.response_id;
        _dVars.cloudAudioExpected = intentResponse.cloud_audio_available;
        // Pin the audio buffer to this answer so a still-in-flight fetch for the
        // previous one cannot overwrite it.
        uic.SetExpectedCloudAudioResponse(_dVars.responseId);

        PRINT_DEBUG("Knowledge Graph Question: %s", Util::HidePersonallyIdentifiableInfo(intentResponse.query_text.c_str()));
        PRINT_DEBUG("Knowledge Graph Response: %s", Util::HidePersonallyIdentifiableInfo(intentResponse.answer.c_str()));
      }
      else if (uic.IsUserIntentPending(USER_INTENT(knowledge_unknown)))
      {
        // don't need to do anything other than clear the response
        uic.DropUserIntent(USER_INTENT(knowledge_unknown));
      }
      else if (uic.IsUserIntentPending(USER_INTENT(unmatched_intent)))
      {
        // this shouldn't really happen, but handle it safely regardless
        uic.DropUserIntent(USER_INTENT(unmatched_intent));
        PRINT_NAMED_WARNING("BehaviorKnowledgeGraphQuestion", "unmatched_intent returned as response from knowledge graph");
      }
      else
      {
#if ALLOW_DEBUG_LOGGING
        {
          // this really shouldn't happen, something went wrong
          // this will be handled fine, but we should let dev know about it
          const UserIntentData *intentData = uic.GetPendingUserIntent();
          const std::string intentString = UserIntentTagToString(intentData ? intentData->intent.GetTag() : UserIntentTag::INVALID);
          PRINT_NAMED_ERROR("BehaviorKnowledgeGraphQuestion", "Invalid intent returned from Knowledge Graph: %s", intentString.c_str());
        }
#endif
      }
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    void BehaviorKnowledgeGraphQuestion::ConsumeIntentGraphResponse()
    {

      // if we have a valid knowledge graph response intent, grab the response string from it
      // if it's not the knowledge graph response, we simply return the empty string which will be handled appropriately
      UserIntentComponent &uic = GetBehaviorComp<UserIntentComponent>();
      UserIntentPtr intentDataPtr = uic.GetUserIntentIfActive(USER_INTENT(knowledge_response_bypass));
      const UserIntent_KnowledgeResponse &intentResponse = intentDataPtr->intent.Get_knowledge_response();

      _dVars.responseString = intentResponse.answer;
      _dVars.responseId = intentResponse.response_id;
      _dVars.cloudAudioExpected = intentResponse.cloud_audio_available;
      uic.SetExpectedCloudAudioResponse(_dVars.responseId);

      PRINT_DEBUG("Intent Graph Question: %s", Util::HidePersonallyIdentifiableInfo(intentResponse.query_text.c_str()));
      PRINT_DEBUG("Intent Graph Response: %s", Util::HidePersonallyIdentifiableInfo(intentResponse.answer.c_str()));
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    void BehaviorKnowledgeGraphQuestion::BeginResponseTTS()
    {
      PRINT_DEBUG("Starting TTS behavior ...");

      // delegate to our tts behavior
      if (_iVars.ttsBehavior->WantsToBeActivated())
      {
        auto callback = [this]()
        {
          // don't play the success anim if we've been interrupted
          if (EState::Interrupted != _dVars.state)
          {
            // play our "woot woot" animation
            DelegateIfInControl(new TriggerLiftSafeAnimationAction(AnimationTrigger::KnowledgeGraphSuccessReaction));
          }
        };

        DelegateIfInControl(_iVars.ttsBehavior.get(), callback);
      }

      // ... annnnnd we're done
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    void BehaviorKnowledgeGraphQuestion::TransitionToSearchingLoop()
    {
      // If this response is using cloud audio, loop the searching anim until the PCM
      // stream is ready, then transition out. On error/timeout, fall back to local TTS.
      if (EResponseSource::CloudAudio == _dVars.responseSource)
      {
        UserIntentComponent &uic = GetBehaviorComp<UserIntentComponent>();
        uint32_t sampleRate = 0;
        uint8_t channels = 0;
        const double now = BaseStationTimer::getInstance()->GetCurrentTimeInSecondsDouble();

        if (uic.IsCloudAudioReady(_dVars.responseId, sampleRate, channels, kCloudAudioPrebufferBytes))
        {
          _dVars.cloudAudioSampleRate = sampleRate;
          _dVars.cloudAudioChannels = channels;
          PRINT_INFO("Cloud audio ready for %s (%u Hz, %u ch)", _dVars.responseId.c_str(), sampleRate, channels);
          DelegateIfInControl(new TriggerLiftSafeAnimationAction(AnimationTrigger::KnowledgeGraphSearchingGetOutSuccess),
                              &BehaviorKnowledgeGraphQuestion::TransitionToBeginResponse);
          return;
        }

        const bool hadError = uic.HasCloudAudioError(_dVars.responseId);
        const bool timedOut = (now >= _dVars.cloudAudioReadyDeadline);
        if (hadError || timedOut)
        {
          PRINT_INFO("Cloud audio unavailable for %s (%s); falling back",
                     _dVars.responseId.c_str(), hadError ? "error" : "timeout");
          uic.ClearCloudAudio();

          if (_iVars.cloudAudioFallbackToLocalTts)
          {
            // fall through to the local-tts wait below
            _dVars.responseSource = EResponseSource::LocalTts;
          }
          else
          {
            // give up: play the searching fail get-out
            CompoundActionSequential *failAnim = new CompoundActionSequential();
            failAnim->AddAction(new TriggerLiftSafeAnimationAction(AnimationTrigger::KnowledgeGraphSearchingFail), true);
            failAnim->AddAction(new TriggerLiftSafeAnimationAction(AnimationTrigger::KnowledgeGraphSearchingFailGetOut), true);
            DelegateIfInControl(failAnim);
            return;
          }
        }
        else
        {
          // still waiting on the PCM stream: keep looping the searching anim
          DelegateIfInControl(new TriggerLiftSafeAnimationAction(AnimationTrigger::KnowledgeGraphSearching),
                              &BehaviorKnowledgeGraphQuestion::TransitionToSearchingLoop);
          return;
        }
      }

      // keep looping back here until the tts audio has been generated ...
      if (EGenerationStatus::None != _dVars.ttsGenerationStatus)
      {
        if (EGenerationStatus::Success == _dVars.ttsGenerationStatus)
        {
          // TTS generation is done, so let's transition out of the searching animation and into the hearts all over the world!!!
          DelegateIfInControl(new TriggerLiftSafeAnimationAction(AnimationTrigger::KnowledgeGraphSearchingGetOutSuccess),
                              &BehaviorKnowledgeGraphQuestion::TransitionToBeginResponse);
        }
        else
        {
          // we failed to generate the tts, but since we've already looped our searching anim, we just need to get out now
          CompoundActionSequential *messageAnimation = new CompoundActionSequential();
          messageAnimation->AddAction(new TriggerLiftSafeAnimationAction(AnimationTrigger::KnowledgeGraphSearchingFail), true);
          messageAnimation->AddAction(new TriggerLiftSafeAnimationAction(AnimationTrigger::KnowledgeGraphSearchingFailGetOut), true);

          DelegateIfInControl(messageAnimation);

          // ... annnnnd we're done
        }
      }
      else
      {
        DelegateIfInControl(new TriggerLiftSafeAnimationAction(AnimationTrigger::KnowledgeGraphSearching),
                            &BehaviorKnowledgeGraphQuestion::TransitionToSearchingLoop);
      }
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    void BehaviorKnowledgeGraphQuestion::TransitionToBeginResponse()
    {
      _dVars.state = EState::Responding;

      DEV_ASSERT(!_dVars.responseString.empty(), "Responding to knowledge graph request but no response string exists");

      _dVars.wasPickedUp = GetBEI().GetRobotInfo().IsPickedUp();

      // speak the response, either as cloud-synthesized audio or via local TTS
      if (EResponseSource::CloudAudio == _dVars.responseSource)
      {
        BeginResponseCloudAudio();
      }
      else
      {
        BeginResponseTTS();
      }
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    void BehaviorKnowledgeGraphQuestion::TransitionToNoResponse()
    {
      _dVars.state = EState::NoResponse;

      // our get-out from listening is simply a series of animations
      CompoundActionSequential *messageAnimation = new CompoundActionSequential();
      messageAnimation->AddAction(new TriggerLiftSafeAnimationAction(AnimationTrigger::KnowledgeGraphSearchingGetIn), true);
      messageAnimation->AddAction(new TriggerLiftSafeAnimationAction(AnimationTrigger::KnowledgeGraphSearching), true);
      messageAnimation->AddAction(new TriggerLiftSafeAnimationAction(AnimationTrigger::KnowledgeGraphSearchingFail), true);
      messageAnimation->AddAction(new TriggerLiftSafeAnimationAction(AnimationTrigger::KnowledgeGraphSearchingFailGetOut), true);

      DelegateIfInControl(messageAnimation);

      // ... annnnnd we're done
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    void BehaviorKnowledgeGraphQuestion::OnResponseInterrupted()
    {
      DEV_ASSERT(EState::Responding == _dVars.state, "Should only allow interruptions during response state");
      PRINT_INFO("Interruption event received, cancelling TTS");

      _dVars.state = EState::Interrupted;

      if (EResponseSource::CloudAudio == _dVars.responseSource)
      {
        CancelCloudAudioPlayback();
      }
      else if (IsControlDelegated() && _iVars.ttsBehavior.get()->IsActivated())
      {
        _iVars.ttsBehavior.get()->Interrupt(false);
      }
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    bool BehaviorKnowledgeGraphQuestion::ShouldUseCloudAudio() const
    {
      return _iVars.cloudAudioEnabled && _dVars.cloudAudioExpected && !_dVars.responseId.empty();
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    void BehaviorKnowledgeGraphQuestion::BeginResponseCloudAudio()
    {
      UserIntentComponent &uic = GetBehaviorComp<UserIntentComponent>();
      _dVars.cloudAudioPcm = uic.ConsumeCloudAudioPcm(_dVars.responseId);

      if (_dVars.cloudAudioPcm.empty())
      {
        // shouldn't happen (we only get here once IsCloudAudioReady was true), but be safe
        PRINT_NAMED_WARNING("BehaviorKnowledgeGraphQuestion", "Cloud audio buffer empty at playback; falling back");
        if (_iVars.cloudAudioFallbackToLocalTts)
        {
          _dVars.responseSource = EResponseSource::LocalTts;
          BeginResponseTTS();
        }
        return;
      }

      const uint8_t channels = (_dVars.cloudAudioChannels > 0) ? _dVars.cloudAudioChannels : 1;
      const uint32_t sampleRate = (_dVars.cloudAudioSampleRate > 0) ? _dVars.cloudAudioSampleRate : 16000;
      _dVars.cloudAudioChannels = channels;
      _dVars.cloudAudioSampleRate = sampleRate;
      _dVars.cloudAudioPlayback = CloudAudioPlaybackState{};
      _dVars.cloudAudioCompleteSent = false;
      _dVars.cloudAudioResponseFinished = false;

      // prepare the streaming player with the PCM format
      GetBEI().GetRobotInfo().GetSDKComponent().PrepareStreamingAudio(
          static_cast<uint16_t>(sampleRate), static_cast<uint16_t>(_iVars.cloudAudioVolume));

      const double now = BaseStationTimer::getInstance()->GetCurrentTimeInSecondsDouble();
      _dVars.cloudAudioStreamStartTime = now;
      _dVars.cloudAudioLastDataTime = now;
      _dVars.cloudAudioLastPlaybackProgressTime = now;

      PRINT_INFO("Streaming cloud audio for %s starting with %zu buffered bytes",
                 _dVars.responseId.c_str(), _dVars.cloudAudioPcm.size());

      // The answer is still being synthesized, so its duration is unknown. Hold
      // control in slices and re-arm until the stream really ends.
      WaitOutCloudAudioPlayback();

      // send an initial batch of chunks right away
      UpdateCloudAudioStreaming();
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    void BehaviorKnowledgeGraphQuestion::WaitOutCloudAudioPlayback()
    {
      if (_dVars.cloudAudioResponseFinished || EResponseSource::CloudAudio != _dVars.responseSource)
      {
        return;
      }

      const double now = BaseStationTimer::getInstance()->GetCurrentTimeInSecondsDouble();
      // Success requires anim's Completed event. Time in a starved stream is
      // silence, not played answer audio, and cannot establish completion.
      if ((_dVars.cloudAudioCompleteSent && now >= _dVars.cloudAudioCompletionDeadline) ||
          now - _dVars.cloudAudioStreamStartTime >= kCloudAudioMaxPlaybackSec)
      {
        PRINT_NAMED_WARNING("BehaviorKnowledgeGraphQuestion",
                            "Cloud audio for %s timed out waiting for playback completion",
                            _dVars.responseId.c_str());
        FailCloudAudioResponse();
        return;
      }

      DelegateIfInControl(new WaitAction(kCloudAudioWaitSliceSec), [this]() {
        WaitOutCloudAudioPlayback();
      });
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    void BehaviorKnowledgeGraphQuestion::UpdateCloudAudioStreaming()
    {
      if (_dVars.cloudAudioResponseFinished)
      {
        return;
      }

      SDKComponent &sdk = GetBEI().GetRobotInfo().GetSDKComponent();
      UserIntentComponent &uic = GetBehaviorComp<UserIntentComponent>();

      const double now = BaseStationTimer::getInstance()->GetCurrentTimeInSecondsDouble();

      // Pick up whatever the cloud has produced since the last tick. The answer
      // arrives sentence by sentence, so this keeps running well after playback
      // has started.
      if (!_dVars.cloudAudioCompleteSent)
      {
        std::vector<uint8_t> more = uic.ConsumeCloudAudioPcm(_dVars.responseId);
        if (!more.empty())
        {
          _dVars.cloudAudioPcm.insert(_dVars.cloudAudioPcm.end(), more.begin(), more.end());
          _dVars.cloudAudioLastDataTime = now;
        }
      }

      const uint8_t channels = (_dVars.cloudAudioChannels > 0) ? _dVars.cloudAudioChannels : 1;
      const uint32_t sampleRate = (_dVars.cloudAudioSampleRate > 0) ? _dVars.cloudAudioSampleRate : 16000;
      const double bytesPerSecond = static_cast<double>(sampleRate) * 2.0 * channels;

      if (!_dVars.cloudAudioCompleteSent && !_dVars.cloudAudioFailed)
      {
        const bool failed = uic.HasCloudAudioError(_dVars.responseId);
        const bool stalled = !uic.IsCloudAudioComplete(_dVars.responseId) &&
          now - _dVars.cloudAudioLastDataTime >= kCloudAudioStallTimeoutSec;
        if (failed || stalled)
        {
          _dVars.cloudAudioFailed = true;
          PRINT_NAMED_WARNING("BehaviorKnowledgeGraphQuestion",
                              "Cloud audio for %s %s after %.2fs played",
                              _dVars.responseId.c_str(), failed ? "failed" : "stalled",
                              static_cast<double>(_dVars.cloudAudioPlayback.FramesPlayed()) / sampleRate);
          if (static_cast<double>(_dVars.cloudAudioPlayback.FramesPlayed()) / sampleRate < kCloudAudioSalvageSec)
          {
            FailCloudAudioResponse();
            return;
          }
        }
      }

      if (_dVars.cloudAudioPlayback.PendingBytes() > 0 &&
          now - _dVars.cloudAudioLastPlaybackProgressTime >= kCloudAudioStallTimeoutSec)
      {
        PRINT_NAMED_WARNING("BehaviorKnowledgeGraphQuestion", "Cloud audio playback made no progress");
        FailCloudAudioResponse();
        return;
      }

      // Pace sending so we keep at most kCloudAudioBufferAheadSec buffered in anim,
      // which has a hard buffer limit of its own.
      const size_t maxAheadBytes = static_cast<size_t>(kCloudAudioBufferAheadSec * bytesPerSecond);

      size_t offset = 0;
      while (offset < _dVars.cloudAudioPcm.size() &&
             _dVars.cloudAudioPlayback.SendBudget(maxAheadBytes) >= 2)
      {
        const size_t remaining = _dVars.cloudAudioPcm.size() - offset;
        const size_t budget = _dVars.cloudAudioPlayback.SendBudget(maxAheadBytes);
        const uint16_t n = static_cast<uint16_t>(
          std::min<size_t>(kCloudAudioMaxChunkBytes, std::min(remaining, budget)) & ~size_t{1});

        if (_dVars.cloudAudioPlayback.PendingBytes() == 0)
        {
          _dVars.cloudAudioLastPlaybackProgressTime = now;
        }
        sdk.SendStreamingAudioChunk(_dVars.cloudAudioPcm.data() + offset, n);
        offset += n;
        _dVars.cloudAudioPlayback.Sent(n);
      }

      if (offset > 0)
      {
        _dVars.cloudAudioPcm.erase(_dVars.cloudAudioPcm.begin(), _dVars.cloudAudioPcm.begin() + offset);
      }

      if (_dVars.cloudAudioCompleteSent || !_dVars.cloudAudioPcm.empty())
      {
        return;
      }

      // Nothing left to send. Only end the stream once the cloud says the answer is
      // finished — until then the anim player pads the gap with silence and keeps
      // the stream alive for the next sentence.
      const bool cloudDone = uic.IsCloudAudioComplete(_dVars.responseId) &&
                             (uic.GetCloudAudioPendingBytes(_dVars.responseId) == 0);

      if (!cloudDone && !_dVars.cloudAudioFailed)
      {
        return;
      }

      sdk.CompleteStreamingAudio();
      _dVars.cloudAudioCompleteSent = true;
      _dVars.cloudAudioCompletionDeadline = now +
        static_cast<double>(_dVars.cloudAudioPlayback.PendingBytes()) / bytesPerSecond +
        kCloudAudioCompletionGraceSec;
      uic.ClearCloudAudio();
      PRINT_INFO("Cloud audio stream for %s ended after %zu bytes",
                 _dVars.responseId.c_str(), _dVars.cloudAudioPlayback.BytesSent());
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    void BehaviorKnowledgeGraphQuestion::CancelCloudAudioPlayback()
    {
      GetBEI().GetRobotInfo().GetSDKComponent().CancelStreamingAudio();

      GetBehaviorComp<UserIntentComponent>().ClearCloudAudio();

      _dVars.cloudAudioPcm.clear();
      _dVars.cloudAudioPlayback = CloudAudioPlaybackState{};
      _dVars.cloudAudioResponseFinished = true;

      if (IsControlDelegated())
      {
        CancelDelegates(false);
      }
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    void BehaviorKnowledgeGraphQuestion::FailCloudAudioResponse()
    {
      if (_dVars.cloudAudioResponseFinished)
      {
        return;
      }

      SDKComponent &sdk = GetBEI().GetRobotInfo().GetSDKComponent();
      const double playedSec = static_cast<double>(_dVars.cloudAudioPlayback.FramesPlayed()) /
        std::max<uint32_t>(1, _dVars.cloudAudioSampleRate);
      sdk.CancelStreamingAudio();
      GetBehaviorComp<UserIntentComponent>().ClearCloudAudio();
      _dVars.cloudAudioPcm.clear();
      _dVars.cloudAudioPlayback = CloudAudioPlaybackState{};
      _dVars.cloudAudioCompleteSent = true;

      if (IsControlDelegated())
      {
        CancelDelegates(false);
      }

      if (_iVars.cloudAudioFallbackToLocalTts && EState::Interrupted != _dVars.state &&
          !_dVars.responseString.empty() && playedSec < kCloudAudioSalvageSec)
      {
        PRINT_INFO("Falling back to local TTS for %s", _dVars.responseId.c_str());
        _dVars.responseSource = EResponseSource::LocalTts;
        BeginResponseTTS();
        return;
      }

      _dVars.cloudAudioResponseFinished = true;
      if (EState::Interrupted != _dVars.state)
      {
        DelegateIfInControl(new TriggerLiftSafeAnimationAction(AnimationTrigger::KnowledgeGraphSearchingFailGetOut));
      }
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    void BehaviorKnowledgeGraphQuestion::FinishCloudAudioResponse()
    {
      if (_dVars.cloudAudioResponseFinished)
      {
        return;
      }
      _dVars.cloudAudioResponseFinished = true;

      // clear any leftover buffer state
      _dVars.cloudAudioPcm.clear();
      _dVars.cloudAudioPlayback = CloudAudioPlaybackState{};

      // don't play the success anim if we've been interrupted
      if (EState::Interrupted != _dVars.state)
      {
        const auto reaction = _dVars.cloudAudioFailed
          ? AnimationTrigger::KnowledgeGraphSearchingFailGetOut
          : AnimationTrigger::KnowledgeGraphSuccessReaction;
        DelegateIfInControl(new TriggerLiftSafeAnimationAction(reaction));
      }
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    void BehaviorKnowledgeGraphQuestion::HandleWhileActivated(const RobotToEngineEvent &event)
    {
      if (event.GetData().GetTag() != RobotInterface::RobotToEngineTag::audioStreamStatusEvent)
      {
        return;
      }
      if (EResponseSource::CloudAudio != _dVars.responseSource ||
          EState::Responding != _dVars.state ||
          _dVars.cloudAudioResponseFinished)
      {
        return;
      }

      const auto &statusEvent = event.GetData().Get_audioStreamStatusEvent();
      switch (statusEvent.streamResultID)
      {
      case SDKAudioStreamingState::ChunkAdded:
      {
        const auto previousPlayed = _dVars.cloudAudioPlayback.FramesPlayed();
        if (!_dVars.cloudAudioPlayback.UpdateProgress(statusEvent.audioReceived, statusEvent.audioPlayed))
        {
          PRINT_NAMED_WARNING("BehaviorKnowledgeGraphQuestion", "Invalid cloud audio playback progress");
        }
        else if (_dVars.cloudAudioPlayback.FramesPlayed() > previousPlayed)
        {
          _dVars.cloudAudioLastPlaybackProgressTime =
            BaseStationTimer::getInstance()->GetCurrentTimeInSecondsDouble();
        }
        break;
      }

      case SDKAudioStreamingState::Completed:
        if (!_dVars.cloudAudioCompleteSent)
        {
          PRINT_NAMED_WARNING("BehaviorKnowledgeGraphQuestion", "Cloud audio playback ended before stream completion");
          FailCloudAudioResponse();
          break;
        }
        PRINT_INFO("Cloud audio playback complete for %s", _dVars.responseId.c_str());
        CancelDelegates(false);
        FinishCloudAudioResponse();
        break;

      case SDKAudioStreamingState::PrepareFailed:
      case SDKAudioStreamingState::PostFailed:
      case SDKAudioStreamingState::AddAudioFailed:
      case SDKAudioStreamingState::BufferOverflow:
        PRINT_NAMED_WARNING("BehaviorKnowledgeGraphQuestion",
                            "Cloud audio playback failed (%s)",
                            EnumToString(statusEvent.streamResultID));
        FailCloudAudioResponse();
        break;

      default:
        break;
      }
    }

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    void BehaviorKnowledgeGraphQuestion::PlayEarconEnd()
    {
      using namespace AudioMetaData::GameEvent;
      BehaviorExternalInterface &bei = GetBEI();

      if (GenericEvent::Invalid != _iVars.earConEnd)
      {
        // Play earcon end audio
        bei.GetRobotAudioClient().PostEvent(_iVars.earConEnd, AudioMetaData::GameObjectType::Behavior);
      }
    }

  } // namespace Vector
} // namespace Anki
