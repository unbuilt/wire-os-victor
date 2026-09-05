#include "util/helpers/includeGTest.h"
#include "engine/aiComponent/behaviorComponent/behaviors/knowledgeGraph/cloudAudioPlaybackState.h"

using Anki::Vector::CloudAudioPlaybackState;

TEST(CloudAudioPlaybackState, StarvationDoesNotCreateSendBudget)
{
  CloudAudioPlaybackState playback;
  playback.Sent(32000);
  ASSERT_TRUE(playback.UpdateProgress(16000, 16000));
  // Regardless of the gap's wall time, a recovery burst stays within 3s.
  EXPECT_EQ(96000u, playback.SendBudget(96000));
  playback.Sent(96000);
  EXPECT_EQ(0u, playback.SendBudget(96000));
  EXPECT_EQ(96000u, playback.PendingBytes());
}

TEST(CloudAudioPlaybackState, CountsInFlightAudioAgainstWindow)
{
  CloudAudioPlaybackState playback;
  playback.Sent(96000);
  EXPECT_EQ(0u, playback.SendBudget(96000));
  ASSERT_TRUE(playback.UpdateProgress(24000, 8000));
  EXPECT_EQ(16000u, playback.SendBudget(96000));
  playback.Sent(16000);
  EXPECT_EQ(0u, playback.SendBudget(96000));
}

TEST(CloudAudioPlaybackState, ProgressResumesAFullWindow)
{
  CloudAudioPlaybackState playback;
  playback.Sent(96000);
  ASSERT_TRUE(playback.UpdateProgress(48000, 48000));
  EXPECT_EQ(0u, playback.PendingBytes());
  EXPECT_EQ(96000u, playback.SendBudget(96000));
  playback.Sent(64000);
  EXPECT_EQ(64000u, playback.PendingBytes());
}

TEST(CloudAudioPlaybackState, RejectsImpossibleOrStaleCounters)
{
  CloudAudioPlaybackState playback;
  playback.Sent(32000);
  ASSERT_TRUE(playback.UpdateProgress(16000, 8000));
  EXPECT_FALSE(playback.UpdateProgress(16001, 8001));
  EXPECT_FALSE(playback.UpdateProgress(16000, 16001));
  EXPECT_FALSE(playback.UpdateProgress(16000, 7999));
  EXPECT_EQ(16000u, playback.PendingBytes());
}
