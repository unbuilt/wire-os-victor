#include "gtest/gtest.h"

#include <algorithm>
#include <cstring>
#include "audioEngine/audioTools/streamingWaveDataInstance.h"
#include <AK/SoundEngine/Common/AkCommonDefs.h>

using Anki::AudioEngine::StreamingWaveDataInstance;
using Anki::AudioEngine::PlugIns::AudioDataStream;

namespace {
class StreamingWaveDataTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    AkChannelConfig channels;
    channels.SetStandard(AK_SPEAKER_SETUP_MONO);
    buffer.AttachContiguousDeinterleavedData(samples, 8, 8, channels);
    std::fill_n(samples, 8, 1.0f);
    buffer.eState = AK_NoMoreData;
  }

  bool Append(uint32_t frames)
  {
    auto data = std::make_unique<float[]>(frames);
    std::fill_n(data.get(), frames, 0.5f);
    return stream.AppendAudioDataStream(AudioDataStream(16000, 1, 0.f, frames, std::move(data)));
  }

  StreamingWaveDataInstance stream;
  AkAudioBuffer buffer;
  float samples[8];
};
}

TEST_F(StreamingWaveDataTest, EmptyOpenProducerProducesSilence)
{
  EXPECT_FALSE(stream.WriteToPluginBuffer(&buffer));
  EXPECT_EQ(AK_DataReady, buffer.eState);
  EXPECT_EQ(8, buffer.uValidFrames);
  for (float sample : samples) {
    EXPECT_FLOAT_EQ(0.f, sample);
  }
  EXPECT_EQ(0u, stream.GetNumberOfFramesPlayed());
}

TEST_F(StreamingWaveDataTest, StarvationResumesWithoutCountingSilenceAsPlayed)
{
  ASSERT_TRUE(Append(3));
  EXPECT_FALSE(stream.WriteToPluginBuffer(&buffer));
  EXPECT_EQ(3u, stream.GetNumberOfFramesPlayed());

  std::fill_n(samples, 8, 1.f);
  EXPECT_FALSE(stream.WriteToPluginBuffer(&buffer));
  EXPECT_EQ(AK_DataReady, buffer.eState);
  for (float sample : samples) {
    EXPECT_FLOAT_EQ(0.f, sample);
  }
  EXPECT_EQ(3u, stream.GetNumberOfFramesPlayed());

  ASSERT_TRUE(Append(2));
  stream.DoneProducingData();
  EXPECT_TRUE(stream.WriteToPluginBuffer(&buffer));
  EXPECT_EQ(AK_NoMoreData, buffer.eState);
  EXPECT_EQ(5u, stream.GetNumberOfFramesReceived());
  EXPECT_EQ(5u, stream.GetNumberOfFramesPlayed());
  EXPECT_FLOAT_EQ(0.5f, samples[0]);
  EXPECT_FLOAT_EQ(0.5f, samples[1]);
  EXPECT_FLOAT_EQ(0.f, samples[2]);
}

TEST_F(StreamingWaveDataTest, EmptyCompletedProducerReturnsEndOfStream)
{
  stream.DoneProducingData();
  EXPECT_TRUE(stream.WriteToPluginBuffer(&buffer));
  EXPECT_EQ(AK_NoMoreData, buffer.eState);
  EXPECT_EQ(0u, stream.GetNumberOfFramesPlayed());
}

TEST_F(StreamingWaveDataTest, ShortCompletedStreamDrainsBufferedFrames)
{
  ASSERT_TRUE(Append(3));
  stream.DoneProducingData();
  EXPECT_TRUE(stream.WriteToPluginBuffer(&buffer));
  EXPECT_EQ(AK_NoMoreData, buffer.eState);
  EXPECT_EQ(3u, stream.GetNumberOfFramesPlayed());
  EXPECT_FLOAT_EQ(0.5f, samples[2]);
  EXPECT_FLOAT_EQ(0.f, samples[3]);
}
