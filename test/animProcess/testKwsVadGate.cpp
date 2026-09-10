#include "audioUtil/kwsVadGate.h"
#include "gtest/gtest.h"
#include <vector>

namespace {
struct Sink {
  std::vector<size_t> starts;
  std::vector<float> samples;
  std::vector<size_t> available;
  int ends = 0;
  void Start(size_t offset) { starts.push_back(offset); }
  void Audio(const float* data, size_t count, size_t time) {
    samples.insert(samples.end(), data, data + count);
    available.push_back(time);
  }
  void End() { ++ends; }
};

void Push(Anki::AudioUtil::KwsVadGate& gate, Sink& sink, int value, bool speech)
{
  std::array<float, 160> block;
  block.fill(static_cast<float>(value));
  gate.Push(block.data(), block.size(), speech, sink);
}
}

TEST(KwsVadGate, SilenceDoesNotStartInference)
{
  Anki::AudioUtil::KwsVadGate gate(2, 2);
  Sink sink;
  for (int i = 0; i < 1000; ++i) { Push(gate, sink, i, false); }
  gate.Finish(sink);
  EXPECT_TRUE(sink.samples.empty());
  EXPECT_TRUE(sink.starts.empty());
  EXPECT_EQ(0, sink.ends);
}

TEST(KwsVadGate, BoundedPreRollAndContinuousHangover)
{
  Anki::AudioUtil::KwsVadGate gate(2, 2);
  Sink sink;
  for (int i = 0; i < 6; ++i) { Push(gate, sink, i, i == 3); }
  ASSERT_EQ(1u, sink.starts.size());
  EXPECT_EQ(160u, sink.starts[0]);
  ASSERT_EQ(800u, sink.samples.size());
  for (int i = 0; i < 5; ++i) { EXPECT_EQ(i + 1, sink.samples[i * 160]); }
  EXPECT_EQ(640u, sink.available[0]);
  EXPECT_EQ(640u, sink.available[1]);
  EXPECT_EQ(1, sink.ends);
}

TEST(KwsVadGate, ShortPauseDoesNotSplitSpeech)
{
  Anki::AudioUtil::KwsVadGate gate(0, 2);
  Sink sink;
  for (int i = 0; i < 5; ++i) { Push(gate, sink, i, i == 0 || i == 2); }
  EXPECT_EQ(1u, sink.starts.size());
  EXPECT_EQ(800u, sink.samples.size());
  EXPECT_EQ(1, sink.ends);
}

TEST(KwsVadGate, NewSessionDoesNotReuseOldAudioOrLoseClock)
{
  Anki::AudioUtil::KwsVadGate gate(1, 1);
  Sink sink;
  for (int i = 0; i < 6; ++i) { Push(gate, sink, i, i == 0 || i == 4); }
  ASSERT_EQ(2u, sink.starts.size());
  EXPECT_EQ(0u, sink.starts[0]);
  EXPECT_EQ(480u, sink.starts[1]);
  ASSERT_EQ(800u, sink.samples.size());
  EXPECT_EQ(3, sink.samples[320]);
  EXPECT_EQ(2, sink.ends);
}

TEST(KwsVadGate, PartialBlocksAndFinish)
{
  Anki::AudioUtil::KwsVadGate gate(2, 2);
  Sink sink;
  float samples[] = {0.1f, 0.2f, 0.3f};
  gate.Push(samples, 2, false, sink);
  gate.Push(samples, 3, true, sink);
  gate.Finish(sink);
  gate.Finish(sink);
  ASSERT_EQ(5u, sink.samples.size());
  EXPECT_EQ(0u, sink.starts[0]);
  EXPECT_EQ(5u, sink.available[0]);
  EXPECT_EQ(1, sink.ends);
}

TEST(KwsVadGate, InvalidInputIsRejected)
{
  EXPECT_THROW(Anki::AudioUtil::KwsVadGate(1, 0), std::invalid_argument);
  Anki::AudioUtil::KwsVadGate gate(1, 1);
  Sink sink;
  float sample = 0;
  EXPECT_THROW(gate.Push(nullptr, 1, false, sink), std::invalid_argument);
  EXPECT_THROW(gate.Push(&sample, 0, false, sink), std::invalid_argument);
  EXPECT_THROW(gate.Push(&sample, 161, false, sink), std::invalid_argument);
}
