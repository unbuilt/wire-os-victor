#include "gtest/gtest.h"
#include "cozmoAnim/micData/micDataInfo.h"

#include <array>
#include <thread>

using namespace Anki::Vector::MicData;

TEST(MicCaptureBoundary, ExcludesQueuedAndBoundaryStraddlingAudio)
{
  MicDataInfo job;
  job._minimumCaptureSequence = 102;
  job.EnableDataCollect(MicDataType::Processed, false);
  std::array<Anki::AudioUtil::AudioSample, kSamplesPerBlockPerChannel> samples{};
  job.CollectProcessedAudio(samples.data(), samples.size(), 99);
  job.CollectProcessedAudio(samples.data(), samples.size(), 100);
  job.CollectProcessedAudio(samples.data(), samples.size(), 101);
  EXPECT_TRUE(job.GetProcessedAudio(0).empty());
  EXPECT_FALSE(job.HasCapturedAudio());
  job.CollectProcessedAudio(samples.data(), samples.size(), 102);
  EXPECT_TRUE(job.HasCapturedAudio());
  EXPECT_EQ(1u, job.GetProcessedAudio(0).size());
}

TEST(MicCaptureBoundary, StopQuiescesWorkerHoldingJobSnapshot)
{
  MicDataInfo job;
  job.EnableDataCollect(MicDataType::Processed, false);
  std::array<Anki::AudioUtil::AudioSample, kSamplesPerBlockPerChannel> samples{};
  job.CollectProcessedAudio(samples.data(), samples.size());
  job.StopCollecting();
  std::thread staleCollector([&] {
    job.CollectProcessedAudio(samples.data(), samples.size());
    job.CollectRawAudio(samples.data(), 0);
  });
  staleCollector.join();
  EXPECT_TRUE(job.CheckDone());
  EXPECT_EQ(1u, job.GetProcessedAudio(0).size());
}

TEST(MicCaptureBoundary, ExcludesOldSourceAudioArrivingAfterStart)
{
  MicDataInfo job;
  job._minimumCaptureSequence = 102;
  job._minimumCaptureTime_ns = 1000000000;
  job.EnableDataCollect(MicDataType::Processed, false);
  std::array<Anki::AudioUtil::AudioSample, kSamplesPerBlockPerChannel> samples{};
  job.CollectProcessedAudio(samples.data(), samples.size(), 103, 999999999);
  EXPECT_TRUE(job.GetProcessedAudio(0).empty());
  EXPECT_FALSE(job.HasCapturedAudio());
  job.CollectProcessedAudio(samples.data(), samples.size(), 104, 1000000000);
  EXPECT_EQ(1u, job.GetProcessedAudio(0).size());
  EXPECT_TRUE(job.HasCapturedAudio());
}

TEST(MicCaptureBoundary, LegacyCaptureKeepsOverlap)
{
  MicDataInfo job;
  job.EnableDataCollect(MicDataType::Processed, false);
  std::array<Anki::AudioUtil::AudioSample, kSamplesPerBlockPerChannel> samples{};
  job.CollectProcessedAudio(samples.data(), samples.size());
  EXPECT_EQ(1u, job.GetProcessedAudio(0).size());
}
