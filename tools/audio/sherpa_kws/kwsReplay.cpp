#include "audioUtil/kwsVadGate.h"
#include "sherpa-onnx/c-api/c-api.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <vector>

namespace {
constexpr int SampleRate = 16000;
constexpr int BlockSamples = 160;
using Clock = std::chrono::steady_clock;
using Stream = std::unique_ptr<const SherpaOnnxOnlineStream,
                              decltype(&SherpaOnnxDestroyOnlineStream)>;

double Seconds(Clock::duration duration)
{
  return std::chrono::duration<double>(duration).count();
}

double CpuSeconds()
{
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    throw std::runtime_error("getrusage failed");
  }
  return usage.ru_utime.tv_sec + usage.ru_stime.tv_sec +
    (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000000.0;
}

long PeakRssKiB()
{
  // ru_maxrss can retain the launching process's pre-exec high-water mark.
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    long value = 0;
    if (std::sscanf(line.c_str(), "VmHWM: %ld kB", &value) == 1 && value > 0) {
      return value;
    }
  }
  throw std::runtime_error("Could not read process VmHWM from /proc/self/status");
}

void RequireFile(const std::string& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input || input.peek() == std::ifstream::traits_type::eof()) {
    throw std::runtime_error("Missing or empty file: " + path);
  }
}

struct Options {
  std::string model, keywords, wav, activity;
  std::string precision = "int8", execution = "unspecified";
  int threads = 1;
  float score = 3.0f, threshold = 0.1f;
  int preRollMs = 500, hangoverMs = 1000;
  int expectedDetections = -1;
};

double Number(const std::string& text)
{
  size_t consumed = 0;
  const double value = std::stod(text, &consumed);
  if (consumed != text.size() || !std::isfinite(value)) {
    throw std::invalid_argument("Invalid number: " + text);
  }
  return value;
}

Options Parse(int argc, char** argv)
{
  Options options;
  for (int i = 1; i < argc; i += 2) {
    if (i + 1 == argc) { throw std::invalid_argument("Missing option value"); }
    const std::string key = argv[i], value = argv[i + 1];
    if (key == "--model-dir") { options.model = value; }
    else if (key == "--keywords") { options.keywords = value; }
    else if (key == "--wav") { options.wav = value; }
    else if (key == "--activity") { options.activity = value; }
    else if (key == "--model-precision") {
      if (value != "int8" && value != "fp32") {
        throw std::invalid_argument("Model precision must be int8 or fp32");
      }
      options.precision = value;
    }
    else if (key == "--execution") {
      if (value != "host" && value != "qemu" && value != "robot") {
        throw std::invalid_argument("Execution must be host, qemu or robot");
      }
      options.execution = value;
    }
    else if (key == "--threads") {
      const double number = Number(value);
      if (number < 1 || number > 4 || std::floor(number) != number) {
        throw std::invalid_argument("Threads must be an integer in 1..4");
      }
      options.threads = static_cast<int>(number);
    }
    else if (key == "--score") {
      const double number = Number(value);
      if (number <= 0 || number > 10) { throw std::invalid_argument("Score must be >0 and <=10"); }
      options.score = static_cast<float>(number);
    }
    else if (key == "--threshold") {
      const double number = Number(value);
      if (number <= 0 || number > 1) { throw std::invalid_argument("Threshold must be >0 and <=1"); }
      options.threshold = static_cast<float>(number);
    }
    else if (key == "--expect-detections") {
      const double number = Number(value);
      if (number < 0 || number > 1000 || std::floor(number) != number) {
        throw std::invalid_argument("Expected detections must be an integer in 0..1000");
      }
      options.expectedDetections = static_cast<int>(number);
    }
    else if (key == "--pre-roll-ms" || key == "--hangover-ms") {
      const double number = Number(value);
      if (number < 0 || number > 5000 || std::fmod(number, 10) != 0 ||
          (key == "--hangover-ms" && number == 0)) {
        throw std::invalid_argument("Gate durations must be multiples of 10 ms, at most 5000 ms; hangover must be positive");
      }
      if (key == "--pre-roll-ms") { options.preRollMs = static_cast<int>(number); }
      else { options.hangoverMs = static_cast<int>(number); }
    }
    else { throw std::invalid_argument("Unknown option: " + key); }
  }
  if (options.model.empty() || options.keywords.empty() || options.wav.empty()) {
    throw std::invalid_argument("--model-dir, --keywords and --wav are required");
  }
  return options;
}

std::vector<bool> ReadActivity(const std::string& path, size_t blocks)
{
  if (path.empty()) { return {}; }
  RequireFile(path);
  std::ifstream input(path);
  std::vector<bool> activity;
  std::string line;
  while (std::getline(input, line)) {
    if (line != "0" && line != "1") {
      throw std::runtime_error("Activity file must contain one 0 or 1 per 10 ms block");
    }
    activity.push_back(line == "1");
    if (activity.size() > blocks) {
      throw std::runtime_error("Activity file is longer than the recording");
    }
  }
  if (input.bad() || activity.size() != blocks) {
    throw std::runtime_error("Activity file does not cover the recording exactly");
  }
  return activity;
}

struct Sink {
  explicit Sink(const SherpaOnnxKeywordSpotter* spotter) : spotter(spotter) {}
  const SherpaOnnxKeywordSpotter* spotter;
  Stream stream{nullptr, SherpaOnnxDestroyOnlineStream};
  size_t origin = 0, sessions = 0, fed = 0, detections = 0;

  void Start(size_t offset) {
    if (stream) { throw std::logic_error("Overlapping KWS sessions"); }
    stream.reset(SherpaOnnxCreateKeywordStream(spotter));
    if (!stream) { throw std::runtime_error("Could not create KWS stream"); }
    origin = offset;
    ++sessions;
  }

  void Audio(const float* samples, size_t count, size_t availableAt) {
    if (!stream) { throw std::logic_error("Audio outside KWS session"); }
    SherpaOnnxOnlineStreamAcceptWaveform(stream.get(), SampleRate, samples,
                                        static_cast<int32_t>(count));
    fed += count;
    while (SherpaOnnxIsKeywordStreamReady(spotter, stream.get())) {
      SherpaOnnxDecodeKeywordStream(spotter, stream.get());
      std::unique_ptr<const SherpaOnnxKeywordResult, decltype(&SherpaOnnxDestroyKeywordResult)>
        result(SherpaOnnxGetKeywordResult(spotter, stream.get()), SherpaOnnxDestroyKeywordResult);
      if (!result || !result->keyword) { throw std::runtime_error("Missing KWS result"); }
      if (result->keyword[0] != '\0') {
        if (!result->json) { throw std::runtime_error("Missing KWS result JSON"); }
        ++detections;
        std::printf("{\"event\":\"keyword\",\"available_at_s\":%.6f,"
                    "\"session_start_s\":%.6f,\"result\":%s}\n",
                    availableAt / double(SampleRate), origin / double(SampleRate), result->json);
        SherpaOnnxResetKeywordStream(spotter, stream.get());
      }
    }
  }

  void End() { stream.reset(); }
};

int Run(const Options& options)
{
  const std::string prefix = options.model + "/";
  const std::string suffix = options.precision == "int8" ? ".int8.onnx" : ".onnx";
  const std::string encoder = prefix + "encoder-epoch-12-avg-2-chunk-16-left-64" + suffix;
  const std::string decoder = prefix + "decoder-epoch-12-avg-2-chunk-16-left-64" + suffix;
  const std::string joiner = prefix + "joiner-epoch-12-avg-2-chunk-16-left-64" + suffix;
  const std::string tokens = prefix + "tokens.txt";
  for (const auto& path : {encoder, decoder, joiner, tokens, options.keywords, options.wav}) {
    RequireFile(path);
  }
  std::unique_ptr<const SherpaOnnxWave, decltype(&SherpaOnnxFreeWave)>
    wave(SherpaOnnxReadWave(options.wav.c_str()), SherpaOnnxFreeWave);
  if (!wave || wave->sample_rate != SampleRate || wave->num_samples <= 0) {
    throw std::runtime_error("Expected a nonempty mono PCM16 WAV at 16000 Hz");
  }
  const size_t count = static_cast<size_t>(wave->num_samples);
  const auto activity = ReadActivity(options.activity, (count + BlockSamples - 1) / BlockSamples);
  const bool gated = !options.activity.empty();
  const long preModelPeak = PeakRssKiB();
  SherpaOnnxKeywordSpotterConfig config{};
  config.feat_config.sample_rate = SampleRate;
  config.feat_config.feature_dim = 80;
  config.model_config.transducer.encoder = encoder.c_str();
  config.model_config.transducer.decoder = decoder.c_str();
  config.model_config.transducer.joiner = joiner.c_str();
  config.model_config.tokens = tokens.c_str();
  config.model_config.num_threads = options.threads;
  config.model_config.provider = "cpu";
  config.max_active_paths = 4;
  config.num_trailing_blanks = 1;
  config.keywords_score = options.score;
  config.keywords_threshold = options.threshold;
  config.keywords_file = options.keywords.c_str();
  const auto loadStart = Clock::now();
  std::unique_ptr<const SherpaOnnxKeywordSpotter, decltype(&SherpaOnnxDestroyKeywordSpotter)>
    spotter(SherpaOnnxCreateKeywordSpotter(&config), SherpaOnnxDestroyKeywordSpotter);
  if (!spotter) { throw std::runtime_error("Could not load keyword spotter"); }
  const double loadSeconds = Seconds(Clock::now() - loadStart);
  Sink sink(spotter.get());
  Anki::AudioUtil::KwsVadGate gate(options.preRollMs / 10, options.hangoverMs / 10);
  const auto start = Clock::now();
  const double cpuStart = CpuSeconds();
  double maxBlockSeconds = 0;
  if (!gated) { sink.Start(0); }
  for (size_t offset = 0; offset < count; offset += BlockSamples) {
    const size_t n = std::min<size_t>(BlockSamples, count - offset);
    const auto blockStart = Clock::now();
    if (gated) { gate.Push(wave->samples + offset, n, activity[offset / BlockSamples], sink); }
    else { sink.Audio(wave->samples + offset, n, offset + n); }
    maxBlockSeconds = std::max(maxBlockSeconds, Seconds(Clock::now() - blockStart));
  }
  // Finite-file flush only. A live implementation receives real trailing audio.
  const std::array<float, BlockSamples> silence{};
  for (int i = 0; i < 100; ++i) {
    const auto blockStart = Clock::now();
    if (gated) { gate.Push(silence.data(), silence.size(), false, sink); }
    else { sink.Audio(silence.data(), silence.size(), count + (i + 1) * BlockSamples); }
    maxBlockSeconds = std::max(maxBlockSeconds, Seconds(Clock::now() - blockStart));
  }
  gate.Finish(sink);
  sink.End();
  const double cpu = CpuSeconds() - cpuStart;
  const double wall = Seconds(Clock::now() - start);
  const double duration = count / double(SampleRate);
  std::printf("{\"event\":\"summary\",\"mode\":\"%s\",\"threads\":%d,"
              "\"audio_seconds\":%.6f,\"eof_padding_seconds\":1,"
              "\"fed_audio_seconds\":%.6f,\"sessions\":%zu,\"detections\":%zu,"
              "\"model_load_seconds\":%.6f,\"replay_wall_seconds\":%.6f,"
              "\"replay_cpu_seconds\":%.6f,\"wall_rtf\":%.6f,\"cpu_rtf\":%.6f,"
              "\"max_push_ms\":%.6f,\"pre_model_peak_rss_kib\":%ld,\"peak_rss_kib\":%ld,"
              "\"score\":%.3f,\"threshold\":%.3f,\"pre_roll_ms\":%d,\"hangover_ms\":%d,"
              "\"includes_live_vad_cost\":false,\"model_precision\":\"%s\","
              "\"execution\":\"%s\",\"live_robot\":%s}\n",
              gated ? "vad-replay" : "continuous", options.threads,
              duration, sink.fed / double(SampleRate),
              sink.sessions, sink.detections, loadSeconds, wall, cpu, wall / duration,
              cpu / duration, maxBlockSeconds * 1000, preModelPeak, PeakRssKiB(),
              options.score, options.threshold, options.preRollMs, options.hangoverMs,
              options.precision.c_str(), options.execution.c_str(),
              options.execution == "robot" ? "true" : "false");
  if (std::fflush(stdout) != 0 || std::ferror(stdout)) {
    throw std::runtime_error("Could not write replay results");
  }
  if (options.expectedDetections >= 0 &&
      sink.detections != static_cast<size_t>(options.expectedDetections)) {
    throw std::runtime_error("Expected " + std::to_string(options.expectedDetections) +
                            " detections, got " + std::to_string(sink.detections));
  }
  return 0;
}
}

int main(int argc, char** argv)
{
  if (argc == 2 && std::string(argv[1]) == "--help") {
    std::puts("kws-replay --model-dir DIR --keywords FILE --wav FILE [--activity FILE]\n"
              "  [--score 3] [--threshold 0.1] [--pre-roll-ms 500] [--hangover-ms 1000]\n"
              "  [--model-precision int8|fp32] [--execution host|qemu|robot]\n"
              "  [--threads 1] (integer in 1..4)\n"
              "  [--expect-detections COUNT] (fails if the observed count differs)\n"
              "Activity: one 0/1 per 10ms block, including the final partial block.\n"
              "Outputs JSONL; offline replay only, never triggers or contacts a robot.");
    return 0;
  }
  try {
    return Run(Parse(argc, argv));
  } catch (const std::exception& error) {
    std::fprintf(stderr, "KWS trial failed: %s\n", error.what());
    return 1;
  }
}
