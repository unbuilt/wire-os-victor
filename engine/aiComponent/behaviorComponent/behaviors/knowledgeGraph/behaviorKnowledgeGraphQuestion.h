/***********************************************************************************************************************
 *
 *  BehaviorKnowledgeGraphQuestion
 *  Victor / Engine
 *
 *  Created by Jarrod Hatfield on 5/09/2018
 *
 *  Description
 *  + Parent behavior requesting a knowledge graph inquiry.
 *  + Streams a users voice (assumed to be a question for Victor) to the cloud which returns a response/answer string
 *
 **********************************************************************************************************************/

#ifndef __Cozmo_Basestation_Behaviors_BehaviorKnowledgeGraphQuestion_H__
#define __Cozmo_Basestation_Behaviors_BehaviorKnowledgeGraphQuestion_H__

#include "engine/aiComponent/behaviorComponent/behaviors/iCozmoBehavior.h"
#include "engine/components/mics/voiceMessageTypes.h"
#include "engine/components/backpackLights/engineBackpackLightComponentTypes.h"
#include "components/textToSpeech/textToSpeechWrapper.h"
#include "clad/audio/audioEventTypes.h"
#include "cloudAudioPlaybackState.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Anki
{
  namespace Vector
  {

    class BehaviorTextToSpeechLoop;

    // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    class BehaviorKnowledgeGraphQuestion : public ICozmoBehavior
    {
      friend class BehaviorFactory;
      BehaviorKnowledgeGraphQuestion(const Json::Value &config);

    public:
      // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
      // Overrides from ICozmoBehavior

      virtual bool WantsToBeActivatedBehavior() const override;
      virtual void GetBehaviorOperationModifiers(BehaviorOperationModifiers &modifiers) const override;
      virtual void GetBehaviorJsonKeys(std::set<const char *> &expectedKeys) const override;

    protected:
      // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
      // Overrides from ICozmoBehavior

      virtual void InitBehavior() override;
      virtual void GetAllDelegates(std::set<IBehavior *> &delegates) const override;

      virtual void OnBehaviorActivated() override;
      virtual void OnBehaviorDeactivated() override;
      virtual void BehaviorUpdate() override;

      virtual void HandleWhileActivated(const EngineToGameEvent &event) override;
      virtual void HandleWhileActivated(const RobotToEngineEvent &event) override;

      // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
      // State Transitions

      void TransitionToSearchingLoop();
      void TransitionToBeginResponse();
      void TransitionToNoResponse();

      // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
      // Helpers

      // start streaming mic data to knowledge graph
      void BeginStreamingQuestion();
      // speak back the response we got from knowledge graph
      void BeginResponseTTS();

      // stream cloud-synthesized speech (PCM) to the robot speaker instead of
      // synthesizing locally. Falls back to local TTS if the audio isn't usable.
      void BeginResponseCloudAudio();
      // paces buffered cloud PCM out to the anim process; called from BehaviorUpdate
      void UpdateCloudAudioStreaming();
      // holds control while cloud audio plays; re-arms itself until playback ends
      void WaitOutCloudAudioPlayback();
      // stop any in-progress cloud audio playback and discard the buffer
      void CancelCloudAudioPlayback();
      // play the success get-out exactly once when cloud audio finishes (or times out)
      // abandon a broken cloud stream: fall back to local TTS if configured, else fail out
      void FailCloudAudioResponse(bool quiet = false);
      void FinishCloudAudioResponse();
      void PlaySuccessfulResponseGetOut();
      // decide whether this response should use cloud audio or local TTS
      bool ShouldUseCloudAudio() const;

      // we have a response intent pending from knowledgeGraph
      bool IsResponsePending() const;
      // translate that response intent into and answer string and store it for later
      void ConsumeResponse();

      // translate that response intent into an answer string and store it for later
      // if it was a knowledge graph response
      void ConsumeIntentGraphResponse();

      // we're done listening to the question
      void OnStreamingComplete(bool);
      // we allow the response audio to be interrupted under certain conditions
      void OnResponseInterrupted();

      // streaming cues are backpack lights and audio cues to let the user know when to speak
      void PlayEarconEnd();

    private:
      // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

      enum class EState : uint8_t
      {
        GettingIn,
        WaitingToStream,
        Listening,
        Searching,
        Responding,
        NoResponse,
        NoConnection,
        Interrupted
      };

      enum class EGenerationStatus
      {
        None,
        Success,
        Fail
      };

      // where the spoken response is coming from for this activation
      enum class EResponseSource : uint8_t
      {
        LocalTts,   // synthesized on-robot via the TTS behavior (default / fallback)
        CloudAudio  // streamed PCM synthesized in the cloud
      };

      // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
      // Instance Vars are members that last the lifetime of the behavior and generally do not change (config vars)

      struct InstanceConfig
      {
        InstanceConfig();

        double streamingDuration;  // how long before we give up on streaming and bail
        std::string readyStringID; // audio tts to let the user know they can begin speaking
        std::shared_ptr<BehaviorTextToSpeechLoop> ttsBehavior;

        AudioMetaData::GameEvent::GenericEvent earConEnd;

        // Cloud audio (off by default). When enabled and the cloud advertises audio
        // for a response, the robot plays the cloud-synthesized PCM instead of local TTS.
        bool cloudAudioEnabled = false;
        double cloudAudioRequestTimeout = 65.0; // exceeds vic-cloud's 60s KG request budget
        double cloudAudioReadyTimeout = 5.0;      // seconds to wait for the PCM stream before falling back
        bool cloudAudioFallbackToLocalTts = true; // on error/timeout, speak with local TTS
        uint32_t cloudAudioVolume = 100;          // volume passed to the streaming player
        Json::Value multiTurnVoice;

      } _iVars;

      // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
      // Dynamic Vars are members that change over the lifetime of the behavior and are generally reset every activation

      struct DynamicVariables
      {
        DynamicVariables();

        EState state;
        double streamingBeginTime;             // the time we begun actually streaming the mic data
        double streamingRequestTime = 0;
        std::string responseString;            // the text we got back from knowledge graph
        EGenerationStatus ttsGenerationStatus; // track the status of the response tts
        BackpackLightDataLocator lightsHandle; // lights, camera, action!
        bool wasPickedUp;

        // Cloud audio state (only meaningful when cloud audio is enabled)
        EResponseSource responseSource = EResponseSource::LocalTts;
        std::string responseId;                  // correlates the ResponseAudio* stream with this answer
        bool cloudAudioExpected = false;         // cloud advertised audio for this response
        double cloudAudioReadyDeadline = 0.0;    // absolute time we stop waiting for the PCM stream
        uint32_t cloudAudioSampleRate = 0;
        uint8_t cloudAudioChannels = 0;
        std::vector<uint8_t> cloudAudioPcm;      // PCM drained from the cloud but not yet sent to anim
        CloudAudioPlaybackState cloudAudioPlayback;
        double cloudAudioStreamStartTime = 0.0;  // when we sent ExternalAudioPrepare
        double cloudAudioLastDataTime = 0.0;     // last time new PCM arrived, to detect a stalled answer
        double cloudAudioLastPlaybackProgressTime = 0.0;
        bool cloudAudioFailed = false;
        bool cloudAudioCompleteSent = false;     // ExternalAudioComplete has been sent
        double cloudAudioCompletionDeadline = 0.0;
        bool cloudAudioResponseFinished = false; // success get-out already handled
        uint64_t conversationToken = 0;
        uint32_t playbackId = 0;

      } _dVars;

      TextToSpeechWrapper _readyTTSWrapper;

    }; // class BehaviorKnowledgeGraphQuestion

  } // namespace Vector
} // namespace Anki

#endif // __Cozmo_Basestation_Behaviors_BehaviorKnowledgeGraphQuestion_H__
