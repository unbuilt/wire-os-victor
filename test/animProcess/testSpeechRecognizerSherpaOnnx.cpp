// The opt-in runner supplies the downloaded model and audio fixtures.
#if defined(ANKI_SHERPA_KWS_INTEGRATION_TESTS) && ANKI_SHERPA_KWS_INTEGRATION_TESTS
#include "cozmoAnim/speechRecognizer/speechRecognizerSherpaOnnx.h"
#include "sherpa-onnx/c-api/c-api.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {
using Recognizer = Anki::Vector::SpeechRecognizerSherpaOnnx;

class SpeechRecognizerSherpaTest : public testing::Test
{
protected:
  void SetUp() override
  {
    const char* modelPath = std::getenv("SHERPA_KWS_TEST_MODEL");
    const char* wavePath = std::getenv("SHERPA_KWS_TEST_WAV");
    const char* activityPath = std::getenv("SHERPA_KWS_TEST_ACTIVITY");
    ASSERT_NE(nullptr, modelPath);
    ASSERT_NE(nullptr, wavePath);
    ASSERT_NE(nullptr, activityPath);
    model = modelPath;
    std::unique_ptr<const SherpaOnnxWave, decltype(&SherpaOnnxFreeWave)>
      wave(SherpaOnnxReadWave(wavePath), SherpaOnnxFreeWave);
    ASSERT_NE(nullptr, wave.get());
    ASSERT_EQ(16000, wave->sample_rate);
    samples.resize((wave->num_samples + 159) / 160 * 160);
    for (int i = 0; i < wave->num_samples; ++i) {
      samples[i] = static_cast<int16_t>(std::max(-32768.0f,
        std::min(32767.0f, wave->samples[i] * 32768.0f)));
    }
    std::ifstream input(activityPath);
    std::string line;
    unsigned int cooldown = 0;
    while (std::getline(input, line)) {
      ASSERT_TRUE(line == "0" || line == "1");
      if (line == "1") { cooldown = 100; }
      else if (cooldown != 0) { --cooldown; }
      activity.push_back(line == "1" || cooldown != 0);
    }
    ASSERT_EQ(samples.size() / 160, activity.size());
  }

  void Feed(Recognizer& recognizer)
  {
    for (size_t i = 0; i < activity.size(); ++i) {
      recognizer.UpdateWithVad(samples.data() + i * 160, 160, activity[i]);
    }
  }

  std::string model;
  std::vector<int16_t> samples;
  std::vector<bool> activity;
};
}

TEST_F(SpeechRecognizerSherpaTest, LiveCallbackAndFreshSessions)
{
  Recognizer recognizer;
  ASSERT_TRUE(recognizer.Init(model));
  int detections = 0;
  recognizer.SetCallback([&](const Anki::AudioUtil::SpeechRecognizerCallbackInfo& info) {
    EXPECT_EQ("你好小维", info.result);
    EXPECT_EQ(0.0f, info.score);
    ++detections;
  });
  Feed(recognizer);
  EXPECT_EQ(1, detections);
  Feed(recognizer);
  EXPECT_EQ(2, detections);
  EXPECT_FALSE(recognizer.HasFailed());
}

TEST_F(SpeechRecognizerSherpaTest, StopAndRestartClearAudio)
{
  Recognizer recognizer;
  ASSERT_TRUE(recognizer.Init(model));
  int detections = 0;
  recognizer.SetCallback([&](const Anki::AudioUtil::SpeechRecognizerCallbackInfo&) { ++detections; });
  recognizer.Stop();
  Feed(recognizer);
  EXPECT_EQ(0, detections);
  recognizer.Start();
  Feed(recognizer);
  EXPECT_EQ(1, detections);
  recognizer.Reset();
  std::array<int16_t, 160> silence{};
  for (int i = 0; i < 300; ++i) {
    recognizer.UpdateWithVad(silence.data(), silence.size(), false);
  }
  EXPECT_EQ(1, detections);
  EXPECT_FALSE(recognizer.HasFailed());
}

TEST_F(SpeechRecognizerSherpaTest, MissingModelAndInvalidAudioFailExplicitly)
{
  Recognizer recognizer;
  EXPECT_FALSE(recognizer.Init(model + "/does-not-exist"));
  ASSERT_TRUE(recognizer.Init(model));
  recognizer.UpdateWithVad(nullptr, 160, true);
  EXPECT_TRUE(recognizer.HasFailed());
  recognizer.Start();
  int detections = 0;
  recognizer.SetCallback([&](const Anki::AudioUtil::SpeechRecognizerCallbackInfo&) { ++detections; });
  Feed(recognizer);
  EXPECT_EQ(0, detections);
  ASSERT_TRUE(recognizer.Init(model));
  recognizer.UpdateWithVad(samples.data(), 159, true);
  EXPECT_TRUE(recognizer.HasFailed());
}
#endif
