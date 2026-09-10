#if defined(ANKI_SHERPA_KWS) && ANKI_SHERPA_KWS
#include "speechRecognizerSherpaOnnx.h"

#include "audioUtil/kwsVadGate.h"
#include "sherpa-onnx/c-api/c-api.h"
#include "util/logging/logging.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <fstream>
#include <mutex>
#include <time.h>

namespace Anki {
namespace Vector {
namespace {
#define LOG_CHANNEL "SpeechRecognizer"
constexpr unsigned int kBlockSamples = 160;
constexpr unsigned int kSampleRate = 16000;
constexpr unsigned int kStatsSamples = 10 * kSampleRate;
using Clock = std::chrono::steady_clock;

bool ReadableFile(const std::string& path)
{
  std::ifstream file(path, std::ios::binary);
  if (!file || file.peek() == std::ifstream::traits_type::eof()) {
    LOG_ERROR("SpeechRecognizerSherpaOnnx.ModelFile", "Missing or empty file: %s", path.c_str());
    return false;
  }
  return true;
}
}

struct SpeechRecognizerSherpaOnnx::Impl
{
  explicit Impl(SpeechRecognizerSherpaOnnx& owner) : owner(owner) {}
  SpeechRecognizerSherpaOnnx& owner;
  std::mutex mutex;
  using Spotter = std::unique_ptr<const SherpaOnnxKeywordSpotter,
                                 decltype(&SherpaOnnxDestroyKeywordSpotter)>;
  using Stream = std::unique_ptr<const SherpaOnnxOnlineStream,
                                decltype(&SherpaOnnxDestroyOnlineStream)>;
  Spotter spotter{nullptr, SherpaOnnxDestroyKeywordSpotter};
  Stream stream{nullptr, SherpaOnnxDestroyOnlineStream};
  // The incoming flag already includes SVad's hangover and the mic processor's
  // quiet cooldown. Only the offline raw-SVad replay adds another 1000 ms.
  AudioUtil::KwsVadGate gate{50, 1};
  bool disabled = true;
  bool failed = false;
  bool cpuClockAvailable = true;
  uint64_t samples = 0;
  uint64_t fedSamples = 0;
  uint64_t sessions = 0;
  uint64_t detections = 0;
  uint64_t wallUs = 0;
  uint64_t cpuUs = 0;
  uint64_t maxUpdateUs = 0;

  int64_t CpuNs()
  {
    if (!cpuClockAvailable) { return -1; }
    timespec time{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0) {
      LOG_ERROR("SpeechRecognizerSherpaOnnx.CpuClock", "Thread CPU clock unavailable");
      cpuClockAvailable = false;
      return -1;
    }
    return static_cast<int64_t>(time.tv_sec) * 1000000000 + time.tv_nsec;
  }

  void Fail(const char* reason)
  {
    LOG_ERROR("SpeechRecognizerSherpaOnnx.Failed", "%s", reason);
    failed = true;
    disabled = true;
    stream.reset();
  }

  void Start(size_t)
  {
    stream.reset(SherpaOnnxCreateKeywordStream(spotter.get()));
    if (!stream) {
      Fail("Could not create keyword stream");
      return;
    }
    ++sessions;
  }

  void Audio(const float* audio, size_t count, size_t)
  {
    if (failed) { return; }
    SherpaOnnxOnlineStreamAcceptWaveform(stream.get(), kSampleRate, audio,
                                        static_cast<int32_t>(count));
    fedSamples += count;
    while (SherpaOnnxIsKeywordStreamReady(spotter.get(), stream.get())) {
      SherpaOnnxDecodeKeywordStream(spotter.get(), stream.get());
      std::unique_ptr<const SherpaOnnxKeywordResult, decltype(&SherpaOnnxDestroyKeywordResult)>
        result(SherpaOnnxGetKeywordResult(spotter.get(), stream.get()), SherpaOnnxDestroyKeywordResult);
      if (!result || !result->keyword) {
        Fail("Could not obtain keyword result");
        return;
      }
      if (result->keyword[0] != '\0') {
        AudioUtil::SpeechRecognizerCallbackInfo info{};
        info.result = result->keyword;
        // Like Picovoice, leave timing/score unspecified. The KWS API does not
        // expose a calibrated confidence or an exact keyword end boundary.
        ++detections;
        LOG_INFO("SpeechRecognizerSherpaOnnx.Detected", "keyword=%s", info.result.c_str());
        SherpaOnnxResetKeywordStream(spotter.get(), stream.get());
        owner.DoCallback(info);
      }
    }
  }

  void End() { stream.reset(); }

  void LogStats()
  {
    if (samples < kStatsSamples) { return; }
    LOG_INFO("SpeechRecognizerSherpaOnnx.Stats",
             "audio_ms=%" PRIu64 " fed_ms=%" PRIu64 " sessions=%" PRIu64
             " hits=%" PRIu64 " wall_us=%" PRIu64 " cpu_us=%" PRId64 " max_update_us=%" PRIu64,
             samples / 16, fedSamples / 16, sessions, detections, wallUs,
             cpuClockAvailable ? static_cast<int64_t>(cpuUs) : int64_t{-1}, maxUpdateUs);
    samples = fedSamples = sessions = detections = wallUs = cpuUs = maxUpdateUs = 0;
  }
};

SpeechRecognizerSherpaOnnx::SpeechRecognizerSherpaOnnx() : _impl(new Impl(*this)) {}
SpeechRecognizerSherpaOnnx::~SpeechRecognizerSherpaOnnx() = default;

bool SpeechRecognizerSherpaOnnx::Init(const std::string& modelDirectory)
{
  std::lock_guard<std::mutex> lock(_impl->mutex);
  _impl->gate.Finish(*_impl);
  _impl->spotter.reset();
  _impl->disabled = true;
  _impl->failed = false;
  const std::string prefix = modelDirectory + "/";
  const std::string encoder = prefix + "encoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx";
  const std::string decoder = prefix + "decoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx";
  const std::string joiner = prefix + "joiner-epoch-12-avg-2-chunk-16-left-64.int8.onnx";
  const std::string tokens = prefix + "tokens.txt";
  const std::string keywords = prefix + "keywords.txt";
  for (const auto& path : {encoder, decoder, joiner, tokens, keywords}) {
    if (!ReadableFile(path)) { return false; }
  }
  SherpaOnnxKeywordSpotterConfig config{};
  config.feat_config.sample_rate = kSampleRate;
  config.feat_config.feature_dim = 80;
  config.model_config.transducer.encoder = encoder.c_str();
  config.model_config.transducer.decoder = decoder.c_str();
  config.model_config.transducer.joiner = joiner.c_str();
  config.model_config.tokens = tokens.c_str();
  config.model_config.num_threads = 1;
  config.model_config.provider = "cpu";
  config.max_active_paths = 4;
  config.num_trailing_blanks = 1;
  config.keywords_score = 3.0f;
  config.keywords_threshold = 0.1f;
  config.keywords_file = keywords.c_str();
  _impl->spotter.reset(SherpaOnnxCreateKeywordSpotter(&config));
  if (!_impl->spotter) {
    _impl->Fail("Could not load keyword spotter");
    return false;
  }
  _impl->disabled = false;
  LOG_INFO("SpeechRecognizerSherpaOnnx.Init",
           "backend=sherpa_onnx model_dir=%s threads=1 score=3 threshold=0.1 pre_roll_ms=500",
           modelDirectory.c_str());
  return true;
}

void SpeechRecognizerSherpaOnnx::Update(const AudioUtil::AudioSample* audio, unsigned int count)
{
  UpdateWithVad(audio, count, true);
}

void SpeechRecognizerSherpaOnnx::UpdateWithVad(const AudioUtil::AudioSample* audio,
                                             unsigned int count, bool vadActive)
{
  std::lock_guard<std::mutex> lock(_impl->mutex);
  if (_impl->disabled) { return; }
  if (!audio || count == 0 || count % kBlockSamples != 0) {
    _impl->Fail("Expected whole 10 ms PCM16 blocks");
    return;
  }
  const auto start = Clock::now();
  const int64_t cpuStart = _impl->CpuNs();
  std::array<float, kBlockSamples> samples{};
  for (unsigned int offset = 0; offset < count; offset += kBlockSamples) {
    for (unsigned int i = 0; i < kBlockSamples; ++i) {
      samples[i] = audio[offset + i] / 32768.0f;
    }
    _impl->gate.Push(samples.data(), samples.size(), vadActive, *_impl);
    if (_impl->failed) { break; }
  }
  const int64_t cpuEnd = _impl->CpuNs();
  if (cpuStart >= 0 && cpuEnd >= cpuStart) {
    _impl->cpuUs += static_cast<uint64_t>((cpuEnd - cpuStart) / 1000);
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
  _impl->wallUs += elapsed;
  _impl->maxUpdateUs = std::max(_impl->maxUpdateUs, static_cast<uint64_t>(elapsed));
  _impl->samples += count;
  _impl->LogStats();
}

void SpeechRecognizerSherpaOnnx::Reset()
{
  std::lock_guard<std::mutex> lock(_impl->mutex);
  _impl->gate.Finish(*_impl);
}

bool SpeechRecognizerSherpaOnnx::HasFailed() const
{
  std::lock_guard<std::mutex> lock(_impl->mutex);
  return _impl->failed;
}

void SpeechRecognizerSherpaOnnx::StartInternal()
{
  std::lock_guard<std::mutex> lock(_impl->mutex);
  _impl->disabled = !_impl->spotter || _impl->failed;
}

void SpeechRecognizerSherpaOnnx::StopInternal()
{
  std::lock_guard<std::mutex> lock(_impl->mutex);
  _impl->disabled = true;
  _impl->gate.Finish(*_impl);
}

} // namespace Vector
} // namespace Anki
#endif
