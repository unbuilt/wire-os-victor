/**
* File: micDataProcessor.h
*
* Author: Lee Crippen
* Created: 9/27/2017
*
* Description: Handles processing the mic samples from the robot process: combining the channels,
*              and extracting direction data.
*
* Copyright: Anki, Inc. 2017
*
*/

#ifndef __AnimProcess_CozmoAnim_MicDataProcessor_H_
#define __AnimProcess_CozmoAnim_MicDataProcessor_H_

#include "svad.h"

#include "micDataTypes.h"
#include "coretech/common/engine/robotTimeStamp.h"
#include "cozmoAnim/micData/micTriggerConfig.h"
#include "clad/cloud/mic.h"
#include "util/container/fixedCircularBuffer.h"
#include "util/global/globalDefinitions.h"
#include "audioEngine/plugins/aecExperimentTiming.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

// Declarations
namespace Anki {
  namespace AudioUtil {
    class SpeechRecognizer;
    struct SpeechRecognizerCallbackInfo;
  }
  namespace Vector {
    class BeatDetector;
    namespace MicData {
      class MicDataSystem;
      class MicImmediateDirection;
    }
    class RobotDataLoader;
    namespace RobotInterface {
      struct MicData;
    }
    class SpeechRecognizerSystem;
  }
  namespace Util {
    class Locale;
  }
}

namespace Anki {
namespace Vector {
namespace MicData {

class MicDataProcessor {
public:
  MicDataProcessor(const Anim::AnimContext* context, MicDataSystem* micDataSystem, const std::string& writeLocation);
  ~MicDataProcessor();
  MicDataProcessor(const MicDataProcessor& other) = delete;
  MicDataProcessor& operator=(const MicDataProcessor& other) = delete;

  void Init();

  void ProcessMicDataPayload(const RobotInterface::MicData& payload);
  int GetAecExperimentMode() const { return _aecExperimentMode; }
  int GetAecMicAgeMs() const { return _aecMicAgeMs; }
  int GetAecRefDelayMs() const { return _aecRefDelayMs; }
  void RecordRawAudio(uint32_t duration_ms, const std::string& path, bool runFFT);

  enum class ProcessingState {
    None = 0,               // Raw single mic data
    NoProcessingSingleMic,  // Cheap single mic processing
    SigEsBeamformingOff,    // Signal Essence fall back policy, clean & mix mics
    SigEsBeamformingOn      // Signal Essence beamforming processing
  };
  
  // Stop processing data from mics
  void MuteMics(bool mute);
  
  void ResetMicListenDirection();
  float GetIncomingMicDataPercentUsed();
  
  BeatDetector& GetBeatDetector() { assert(nullptr != _beatDetector); return *_beatDetector.get(); }

  // Create and start stream audio data job
  // Note: Overlap size is only as large as the audio buffer, see kTriggerAudioLengthShipping_ms
  RobotTimeStamp_t CreateStreamJob(CloudMic::StreamType streamType = CloudMic::StreamType::Normal,
                                  uint32_t overlapLength_ms = 0, uint32_t streamId = 0,
                                  bool freshCapture = false);

  void VoiceTriggerWordDetection(const AudioUtil::SpeechRecognizerCallbackInfo& info);

  void FakeTriggerWordDetection(bool fromMute = false);

  void GetLatestMicDirectionData(MicDirectionData& out_lastSample, DirectionIndex& out_dominantDirection) const;


private:
  const Anim::AnimContext* _context = nullptr;
  MicDataSystem* _micDataSystem = nullptr;
  SpeechRecognizerSystem* _speechRecognizerSystem = nullptr;
  
  std::string _writeLocationDir = "";
  // Members for caching off lookup indices for mic processing results
  int _bestSearchBeamIndex = 0;
  int _bestSearchBeamConfidence = 0;
  int _selectedSearchBeamIndex = 0;
  int _selectedSearchBeamConfidence = 0;
  int _searchConfidenceState = 0;
  int _policyFallbackFlag = 0;

  // Members for general purpose processing and state
  std::unique_ptr<SVadConfig_t> _sVadConfig;
  std::unique_ptr<SVadObject_t> _sVadObject;
  uint32_t _vadCountdown = 0;
  std::unique_ptr<MicImmediateDirection> _micImmediateDirection;

  static constexpr uint32_t kRawAudioBufferSize = kRawAudioPerBuffer_ms / kTimePerChunk_ms;
  float _rawAudioBufferFullness[2] = { 0.f, 0.f };
  // We have 2 fixed buffers for incoming raw audio that we alternate between, so that the processing thread can work
  // on one set of data while the main thread can copy new data into the other set.
  struct ReceivedMicData {
    RobotInterface::MicData payload;
    uint64_t captureSequence = 0;
    int64_t captureTime_ns = 0;
    int64_t receivedNs = 0;
    int64_t arrivalNs = 0;
    int32_t residualUs = 0;
    int32_t driftUs = 0;
    double sampleNs = AudioEngine::PlugIns::kAecMicSampleNs;
    int32_t ratePpb = 0, fitErrorUs = 0, rawResidualUs = 0;
    int32_t windowMinUs = 0, windowMaxUs = 0;
    uint32_t windows = 0, sourceErrors = 0, transportUs = 0;
    uint32_t updates = 0, faultReason = 0, faultElapsedMs = 0;
    int32_t faultOffsetUs = 0, faultWindowMinUs = 0;
    uint32_t sourceFaultReason = 0, sourceExpected = 0, sourceFaultFirst = 0, sourceFaultLast = 0;
    uint64_t faultObservedNs = 0, faultPredictedNs = 0;
    bool clockReady = false;
    bool clockFault = false;
  };
  Util::FixedCircularBuffer<ReceivedMicData, kRawAudioBufferSize> _rawAudioBuffers[2];
  int _aecExperimentMode = 0;
  int _aecMicAgeMs = 0;
  int _aecRefDelayMs = 0;
  int64_t _aecMicReceivedNs = 0;
  AudioEngine::PlugIns::AecCalibratedClock _aecCaptureClock{true};
  uint64_t _aecDiagnosticRawIndex = 0;
  AudioEngine::PlugIns::AecSourceContinuity _aecSourceContinuity;
  ReceivedMicData _aecTimingSnapshot;
  std::atomic<uint32_t> _aecInputOverflows{0};
  bool _aecMicClockReady = false;
  bool _aecMicClockFault = false;
  int32_t _aecMicResidualUs = 0, _aecMicDriftUs = 0;
  uint32_t _aecMaxQueueUs = 0;
  uint32_t _aecTimingInvalidBlocks = 0;
  uint32_t _aecBlocks = 0;
  uint32_t _aecMissing = 0;
  uint32_t _aecValidBlocks = 0;
  AudioEngine::PlugIns::AecExecutionTiming _aecExecutionTiming;
  uint64_t _aecReferenceEnergy = 0;
  // Index of the buffer that is currently being used by the processing thread
  uint32_t _rawAudioProcessingIndex = 0;
  std::thread _processThread;
  std::thread _processTriggerThread;
  std::mutex _rawMicDataMutex;
  uint64_t _captureSequence = 0;
  bool _muteMics = false;
  bool _processThreadStop = false;
  bool _robotWasMoving = false;
  bool _isSpeakerActive = false;
  bool _wasSpeakerActive = false;
  bool _isInLowPowerMode = false;
  uint32_t _speakerCooldownCnt = 0;

#if ANKI_DEV_CHEATS
  static constexpr uint32_t kTriggerAudioLength_ms = kTriggerAudioLengthDebug_ms;
#else
  static constexpr uint32_t kTriggerAudioLength_ms = kTriggerAudioLengthShipping_ms;
#endif

  // Internal buffer used to add to the streaming audio once a trigger is detected
  static constexpr uint32_t kImmediateBufferSize = kTriggerAudioLength_ms / kTimePerChunk_ms;
  struct TimedMicData {
    std::array<AudioUtil::AudioSample, kSamplesPerBlockPerChannel> audioBlock;
    RobotTimeStamp_t timestamp;
    uint64_t captureSequence = 0;
    int64_t captureTime_ns = 0;
  };
  Util::FixedCircularBuffer<TimedMicData, kImmediateBufferSize> _immediateAudioBuffer;

  using RawAudioChunk = std::array<AudioUtil::AudioSample, kIncomingAudioChunkSize>;
  static constexpr uint32_t kImmediateBufferRawSize = kTriggerAudioLength_ms / kTimePerChunk_ms;
  
  std::mutex _procAudioXferMutex;
  std::condition_variable _dataReadyCondition;
  std::condition_variable _xferAvailableCondition;
  size_t _procAudioRawComplete = 0;
  size_t _procAudioXferCount = 0;
  std::atomic<ProcessingState> _activeProcState{ProcessingState::None};

  // Mutex for different accessing signal essence software
  std::mutex _seInteractMutex;

  // Aubio beat detector
  std::unique_ptr<BeatDetector> _beatDetector;
  
  enum class TriggerWordDetectSource : uint8_t {
    Invalid=0,
    Voice,
    Button,
    ButtonFromMute,
  };
  

  void InitVAD();
  
  void TriggerWordDetectCallback(TriggerWordDetectSource source,
                                 const AudioUtil::SpeechRecognizerCallbackInfo& info);
  
  // Return 0 if the stream job can not be created
  RobotTimeStamp_t CreateTriggerWordDetectedJobs(bool shouldStream, uint32_t streamId);
  std::atomic<uint32_t> _wakeStreamCounter{0};
  
  void ProcessRawAudio(RobotTimeStamp_t timestamp,
                       const AudioUtil::AudioSample* audioChunk,
                       uint32_t robotStatus,
                       float robotAngle,
                       uint64_t captureSequence, int64_t captureTime_ns);

  MicDirectionData ProcessMicrophonesSE(const AudioUtil::AudioSample* audioChunk,
                                        AudioUtil::AudioSample* bufferOut,
                                        uint32_t robotStatus,
                                        float robotAngle);

  void ProcessRawLoop();
  void ProcessTriggerLoop();
  
  void UpdateBeatDetector(const AudioUtil::AudioSample* const samples, const uint32_t nSamples);
  
  void SetActiveMicDataProcessingState(ProcessingState state);
  const char* GetProcessingStateName(ProcessingState state) const;
  
  void SetupConsoleFuncs();
};

} // namespace MicData
} // namespace Vector
} // namespace Anki

#endif // __AnimProcess_CozmoAnim_MicDataProcessor_H_
