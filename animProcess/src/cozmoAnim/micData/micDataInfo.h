/**
* File: micDataInfo.h
*
* Author: Lee Crippen
* Created: 10/25/2017
*
* Description: Holds onto info related to recording and processing mic data.
*
* Copyright: Anki, Inc. 2017
*
*/

#ifndef __AnimProcess_CozmoAnim_MicDataInfo_H_
#define __AnimProcess_CozmoAnim_MicDataInfo_H_

#include "micDataTypes.h"
#include "audioUtil/audioDataTypes.h"
#include "clad/cloud/mic.h"
#include "util/bitFlags/bitFlags.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace Anki {
namespace Vector {
namespace MicData {

class MicDataInfo
{
public:
  static constexpr uint32_t kDefaultFilesToCapture = 15;
  static constexpr uint32_t kMinAudioSizeToSave_ms = kTriggerOverlapSize_ms + 100;
  static constexpr uint32_t kDefaultAudioFadeIn_ms = 5;

  bool                          _doFFTProcess     = false;
  bool                          _repeating        = false;
  uint32_t                      _numMaxFiles      = kDefaultFilesToCapture;
  CloudMic::StreamType          _type             = CloudMic::StreamType::Normal;
  uint32_t                     _streamId         = 0;
  uint64_t                     _minimumCaptureSequence = 0;
  int64_t                      _minimumCaptureTime_ns = 0;
  std::string                   _writeLocationDir;
  std::string                   _writeNameBase;

  static constexpr uint32_t kMaxRecordTime_ms = std::numeric_limits<uint32_t>::max();
  
  // Note this will be called from a separate processing thread
  std::function<void(std::vector<uint32_t>&&)> _rawAudioFFTCallback;
  
  // Callback for when audio is written to disk
  std::function<void(const std::string&)> _audioSaveCallback;

  void SetTimeToRecord(uint32_t timeToRecord);
  void StopCollecting();
  
  // Add a linear fade to the begining of the stream
  // Note: Must set the fade in duration before CollectProcessedAudio() is called, default duration is 0 (no fade)
  void SetAudioFadeInTime(uint32_t fadeInTime_ms);
  
  void CollectRawAudio(const AudioUtil::AudioSample* audioChunk, size_t size,
                       uint64_t sequence = 0, int64_t captureTime_ns = 0);
  void CollectProcessedAudio(const AudioUtil::AudioSample* audioChunk, size_t size,
                             uint64_t sequence = 0, int64_t captureTime_ns = 0);

  AudioUtil::AudioChunkList GetProcessedAudio(size_t beginIndex);
  bool IsFreshCapture() const { return _minimumCaptureSequence != 0; }
  bool HasCapturedAudio() const;
  void UpdateForNextChunk();
  bool CheckDone() const;
  uint32_t GetTimeToRecord_ms() const;
  uint32_t GetTimeRecorded_ms() const;

  void EnableDataCollect(MicDataType type, bool saveToFile);
  void DisableDataCollect(MicDataType type);
  
private:
  Util::BitFlags8<MicDataType>  _typesToCollect;
  Util::BitFlags8<MicDataType>  _typesToSave;
  // These members are accessed via multiple threads when the job is running, so they use a mutex
  uint32_t _timeRecorded_ms  = 0;
  uint32_t _timeToRecord_ms  = 0;
  // Perform audio fade in at the begining of the processed audio data stream
  int32_t _fadeInSamples  = 0;
  float   _fadeInScalar   = 0.0f;
  float   _fadeInStepSize = 0.0f;
  
  AudioUtil::AudioChunkList _rawAudioData{};
  AudioUtil::AudioChunkList _processedAudioData{};
  mutable std::mutex _dataMutex;

  void SaveCollectedAudio(const std::string& dataDirectory, const std::string& nameToUse, const std::string& nameToRemove);
  std::string ChooseNextFileNameBase(std::string& out_dirToDelete);
  
  static std::vector<uint32_t> GetFFTResultFromRaw(const AudioUtil::AudioChunkList& data, float length_s);
};

} // namespace MicData
} // namespace Vector
} // namespace Anki

#endif // __AnimProcess_CozmoAnim_MicDataInfo_H_
