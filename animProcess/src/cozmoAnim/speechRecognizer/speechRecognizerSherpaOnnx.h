#ifndef ANKI_VECTOR_SPEECH_RECOGNIZER_SHERPA_ONNX_H
#define ANKI_VECTOR_SPEECH_RECOGNIZER_SHERPA_ONNX_H

#include "audioUtil/speechRecognizer.h"
#include <memory>
#include <string>

namespace Anki {
namespace Vector {

class SpeechRecognizerSherpaOnnx : public AudioUtil::SpeechRecognizer
{
public:
  SpeechRecognizerSherpaOnnx();
  ~SpeechRecognizerSherpaOnnx() override;
  SpeechRecognizerSherpaOnnx(const SpeechRecognizerSherpaOnnx&) = delete;
  SpeechRecognizerSherpaOnnx& operator=(const SpeechRecognizerSherpaOnnx&) = delete;

  bool Init(const std::string& modelDirectory);
  void Update(const AudioUtil::AudioSample* audio, unsigned int count) override;
  void UpdateWithVad(const AudioUtil::AudioSample* audio, unsigned int count, bool vadActive);
  void Reset();
  bool HasFailed() const;

private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
  void StartInternal() override;
  void StopInternal() override;
};

} // namespace Vector
} // namespace Anki
#endif
