#include "gtest/gtest.h"
#include "audioEngine/plugins/aecPlaybackReference.h"
#include <vector>
#include <thread>

using Anki::AudioEngine::PlugIns::AecPlaybackReference;
using Anki::AudioEngine::PlugIns::AecCadenceMonitor;
using Anki::AudioEngine::PlugIns::AecExecutionTiming;
using Anki::AudioEngine::PlugIns::AecCalibratedClock;
using Anki::AudioEngine::PlugIns::AecSourceContinuity;
using Anki::AudioEngine::PlugIns::kAecMicBlockNs;
namespace {
constexpr int64_t kStart = 1000000000;
constexpr int64_t kFirstOutput = kStart - 6 * 31250;
}

TEST(AecPlaybackReference, DefaultOffDoesNotQueue)
{
  AecPlaybackReference reference;
  int16_t input[320]{};
  int16_t output[160];
  reference.Push(input, 320, 32000, 1, kStart);
  EXPECT_EQ(160u, reference.Read(output, 160, kFirstOutput));
  EXPECT_FALSE(reference.Enabled());
  EXPECT_EQ(0u, reference.Errors());
}

TEST(AecPlaybackReference, RejectsUnexpectedFormat)
{
  AecPlaybackReference reference;
  reference.Enable();
  int16_t input[320]{}, output[160];
  reference.Push(input, 320, 48000, 1, kStart);
  reference.Push(input, 160, 32000, 2, kStart);
  EXPECT_EQ(2u, reference.Errors());
  EXPECT_EQ(160u, reference.Read(output, 160, kFirstOutput));
}

TEST(AecPlaybackReference, FiltersAndResamplesActualOutput)
{
  AecPlaybackReference reference;
  reference.Enable();
  std::vector<int16_t> input(640, 1000);
  int16_t output[320];
  reference.Push(input.data(), input.size(), 32000, 1, kStart);
  EXPECT_EQ(0u, reference.Read(output, 320, kFirstOutput));
  for (size_t i = 8; i < 320; ++i) { EXPECT_NEAR(1000, output[i], 1); }
  EXPECT_EQ(320u, reference.Read(output, 320, kFirstOutput));
  for (auto value : output) { EXPECT_EQ(0, value); }
}

TEST(AecPlaybackReference, ChunkBoundariesAndTimestampJitterPreservePhase)
{
  AecPlaybackReference whole, split;
  whole.Enable(); split.Enable();
  std::vector<int16_t> input(640);
  for (size_t i = 0; i < input.size(); ++i) { input[i] = (i * 79) % 20000; }
  whole.Push(input.data(), 640, 32000, 1, kStart);
  split.Push(input.data(), 101, 32000, 1, kStart);
  split.Push(input.data() + 101, 539, 32000, 1, kStart + 101 * 31250 + 100000);
  int16_t expected[320], actual[320];
  EXPECT_EQ(0u, whole.Read(expected, 320, kFirstOutput));
  EXPECT_EQ(0u, split.Read(actual, 320, kFirstOutput));
  for (size_t i = 0; i < 320; ++i) { EXPECT_EQ(expected[i], actual[i]); }
}

TEST(AecPlaybackReference, DoesNotConsumeFuturePlaybackOrReplayStaleSamples)
{
  AecPlaybackReference reference;
  reference.Enable();
  std::vector<int16_t> input(320, 300);
  int16_t output[160];
  reference.Push(input.data(), 320, 32000, 1, kStart);
  EXPECT_EQ(160u, reference.Read(output, 160, kFirstOutput - 10000000));
  EXPECT_EQ(0u, reference.Read(output, 160, kFirstOutput));
  EXPECT_EQ(160u, reference.Read(output, 160, kFirstOutput + 10000000));
}

TEST(AecPlaybackReference, GapAndConfigurableAlignment)
{
  AecPlaybackReference reference;
  reference.Enable();
  std::vector<int16_t> input(320, 700);
  int16_t output[160];
  reference.Push(input.data(), 320, 32000, 1, kStart);
  reference.Push(input.data(), 320, 32000, 1, kStart + 30000000);
  EXPECT_EQ(0u, reference.Read(output, 160, kFirstOutput));
  EXPECT_EQ(160u, reference.Read(output, 160, kFirstOutput + 10000000));
  EXPECT_EQ(0u, reference.Read(output, 160, kFirstOutput + 50000000 - 20000000));
}

TEST(AecPlaybackReference, OverflowIsBoundedAndReported)
{
  AecPlaybackReference reference;
  reference.Enable();
  std::vector<int16_t> input(40000, 100);
  reference.Push(input.data(), input.size(), 32000, 1, kStart);
  EXPECT_EQ(20000u - 16383u, reference.Dropped());
  std::vector<int16_t> output(20000);
  EXPECT_EQ(20000u - 16383u, reference.Read(output.data(), output.size(), kFirstOutput));
}

TEST(AecPlaybackReference, ConcurrentProducerConsumerRemainBounded)
{
  AecPlaybackReference reference;
  reference.Enable();
  std::thread producer([&] {
    int16_t input[320]{};
    for (int i = 0; i < 2000; ++i) {
      reference.Push(input, 320, 32000, 1, kStart + i * 10000000LL);
    }
  });
  int16_t output[160];
  for (int i = 0; i < 2000; ++i) {
    EXPECT_LE(reference.ReadMicrophone(output, 160, kFirstOutput + i * kAecMicBlockNs), 160u);
  }
  producer.join();
  EXPECT_EQ(0u, reference.Errors());
}

TEST(AecCadence, PhysicalClockSurvivesOneHourOfBatchedDelivery)
{
  AecCadenceMonitor clock;
  int64_t oldClock = 0;
  unsigned oldResets = 0;
  for (int64_t i = 0; i < 351563; ++i) {
    const auto physical = kStart + i * kAecMicBlockNs;
    // Anim delivers messages on its 33 ms update, often several at once.
    const auto arrival = kStart + ((i * kAecMicBlockNs + 32999999) / 33000000) * 33000000;
    EXPECT_EQ(physical, clock.Observe(arrival, kAecMicBlockNs, 100000000));
    ASSERT_FALSE(clock.Fault());
    if (!oldClock || arrival - (oldClock + 10000000) > 100000000) {
      oldResets += oldClock != 0;
      oldClock = arrival;
    } else {
      oldClock += 10000000;
    }
  }
  EXPECT_TRUE(clock.Ready());
  EXPECT_GT(oldResets, 700u);
  EXPECT_LT(oldResets, 1100u);
}

TEST(AecCadence, DriftIsNotSilentlySmoothed)
{
  AecCadenceMonitor clock;
  for (int64_t i = 0; i < 10000; ++i) {
    // 100 ppm residual drift, below the immediate 100 ms interruption bound.
    clock.Observe(kStart + i * (kAecMicBlockNs + 1024), kAecMicBlockNs, 100000000);
  }
  EXPECT_TRUE(clock.Fault());
  EXPECT_FALSE(clock.Ready());
  EXPECT_GT(clock.DriftNs(), 2000000);
}

TEST(AecCadence, GapsLatchAndDoNotReanchor)
{
  AecCadenceMonitor clock;
  for (int64_t i = 0; i < 200; ++i) {
    clock.Observe(kStart + i * kAecMicBlockNs, kAecMicBlockNs, 100000000);
  }
  EXPECT_TRUE(clock.Ready());
  EXPECT_EQ(kStart + 200 * kAecMicBlockNs,
            clock.Observe(kStart + 200 * kAecMicBlockNs + 200000000, kAecMicBlockNs, 100000000));
  EXPECT_TRUE(clock.Fault());
  clock.Observe(kStart + 201 * kAecMicBlockNs, kAecMicBlockNs, 100000000);
  EXPECT_FALSE(clock.Ready());
}

TEST(AecPlaybackReference, PeriodQuantizedPointerDoesNotPunchHoles)
{
  AecPlaybackReference legacy, reference;
  legacy.Enable(); reference.Enable();
  int16_t input[512];
  std::fill(std::begin(input), std::end(input), 900);
  int16_t output[256];
  size_t legacyMissing = 0, missing = 0;
  for (int64_t i = 0; i < 2000; ++i) {
    const auto actual = kStart + i * 16000000;
    // The driver pointer remains fixed within each 16 ms period.
    const auto estimate = actual + (i % 8) * 2000000;
    legacy.Push(input, 512, 32000, 1, estimate);
    reference.ObserveClock(estimate, 512, 18000000);
    reference.Push(input, 512, 32000, 1, estimate, 18000000);
    legacyMissing += legacy.Read(output, 256, actual - 6 * 31250);
    missing += reference.Read(output, 256, actual - 6 * 31250);
  }
  EXPECT_GT(legacyMissing, 0u);
  EXPECT_EQ(0u, missing);
  EXPECT_EQ(0u, reference.Discontinuities());
  EXPECT_TRUE(reference.ClockReady());
  EXPECT_FALSE(reference.ClockFault());
}

TEST(AecPlaybackReference, PhysicalAdcGridInterpolatesWithoutBlockSlips)
{
  AecPlaybackReference reference;
  reference.Enable();
  int16_t input[512];
  std::fill(std::begin(input), std::end(input), 1000);
  int16_t output[160];
  int64_t producer = 0;
  for (int64_t block = 0; block < 6000; ++block) {
    const auto target = kFirstOutput + block * kAecMicBlockNs;
    while (producer * 16000000 <= block * kAecMicBlockNs + kAecMicBlockNs) {
      reference.Push(input, 512, 32000, 1, kStart + producer++ * 16000000);
    }
    ASSERT_EQ(0u, reference.ReadMicrophone(output, 160, target));
    if (block) {
      for (auto sample : output) { ASSERT_NEAR(1000, sample, 1); }
    }
  }
  EXPECT_EQ(0u, reference.Dropped());
}

TEST(AecPlaybackReference, AdcInterpolationUsesPhysicalSampleTimes)
{
  AecPlaybackReference reference;
  reference.Enable();
  int16_t input[1024], output[160];
  for (int i = 0; i < 1024; ++i) { input[i] = i * 10; }
  reference.Push(input, 1024, 32000, 1, kStart);
  ASSERT_EQ(0u, reference.ReadMicrophone(output, 160, kStart + 100 * 31250));
  for (int i = 0; i < 160; ++i) {
    EXPECT_NEAR(1000 + i * 20.48, output[i], 2);
  }
}

TEST(AecPlaybackReference, HardwarePeriodClockRetainsLongTermDriftChecks)
{
  AecPlaybackReference reference;
  reference.Enable();
  int16_t input[1024]{}, output[512];
  for (int64_t i = 0; i < 500; ++i) {
    const auto actual = kStart + i * 32000000;
    const auto estimate = actual + (i % 16) * 2000000;
    reference.ObserveClock(estimate, 1024, 34000000);
    reference.Push(input, 1024, 32000, 1, estimate, 34000000);
    EXPECT_EQ(0u, reference.Read(output, 512, actual - 6 * 31250));
  }
  EXPECT_TRUE(reference.ClockReady());
  EXPECT_EQ(0u, reference.Discontinuities());
  reference.ObserveClock(kStart + 500 * 32000000LL + 100000000, 1024, 34000000);
  EXPECT_TRUE(reference.ClockFault());
}

TEST(AecPlaybackReference, AdcInterpolationRejectsGapsAndRecovery)
{
  AecPlaybackReference reference;
  reference.Enable();
  int16_t input[320]{}, output[160];
  reference.Push(input, 320, 32000, 1, kStart);
  reference.ReportError();
  reference.Push(input, 320, 32000, 1, kStart + 30000000);
  EXPECT_EQ(160u, reference.ReadMicrophone(output, 160, kFirstOutput + 10000000));
  EXPECT_EQ(1u, reference.Errors());
  EXPECT_GT(reference.ReadMicrophone(output, 160, kFirstOutput + 30000000), 0u);
}

TEST(AecPlaybackReference, DacDriftLatchesEvenWithinPointerUncertainty)
{
  AecPlaybackReference reference;
  for (int64_t i = 0; i < 3000; ++i) {
    reference.ObserveClock(kStart + i * 16001600 + (i % 8) * 2000000,
                           512, 18000000);
  }
  EXPECT_TRUE(reference.ClockFault());
  EXPECT_FALSE(reference.ClockReady());
}

TEST(AecExecutionTiming, SeparatesCpuFromWallAndReportsErrors)
{
  AecExecutionTiming timing;
  timing.Add(22000000, 10000000, 13000000);
  EXPECT_EQ(22000u, timing.maxWallUs);
  EXPECT_EQ(3000u, timing.maxCpuUs);
  EXPECT_EQ(19000u, timing.maxNonCpuUs);
  EXPECT_EQ(1u, timing.wallOverBudget);
  EXPECT_EQ(0u, timing.cpuOverBudget);
  timing.Add(11000000, 0, 11000000);
  EXPECT_EQ(1u, timing.cpuOverBudget);
  timing.Add(1000000, -1, -1);
  EXPECT_EQ(1u, timing.errors);
}

TEST(AecExecutionTiming, ThreadClockDoesNotChargeSleepingTime)
{
  const auto cpuStart = AecExecutionTiming::ThreadCpuNs();
  ASSERT_GE(cpuStart, 0);
  const auto wallStart = AecPlaybackReference::NowNs();
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  const auto cpuEnd = AecExecutionTiming::ThreadCpuNs();
  const auto wall = AecPlaybackReference::NowNs() - wallStart;
  EXPECT_GE(wall, 30000000);
  EXPECT_LT(cpuEnd - cpuStart, wall / 2);
}

TEST(AecCalibration, RecordedScaleDriftCalibratesOnceAtSource)
{
  for (const double ppm : {700.0, -700.0, 80.0}) {
    AecCalibratedClock clock;
    int32_t lockedRate = 0;
    bool wasReady = false;
    for (int64_t block = 0; block < 60000; ++block) {
      const auto actual = static_cast<int64_t>(std::llround(block * kAecMicBlockNs * (1 + ppm / 1e6)));
      const auto arrival = kStart + actual + (block % 7) * 1000000;
      const auto predicted = clock.Observe(arrival, kAecMicBlockNs, 100000000);
      ASSERT_FALSE(clock.Fault()) << block << " ppm=" << ppm;
      if (clock.Ready()) {
        EXPECT_NEAR(kStart + actual, predicted, 1000000);
        if (!wasReady) { lockedRate = clock.RatePpb(); }
        EXPECT_EQ(lockedRate, clock.RatePpb()); // no chasing after the fit
        wasReady = true;
      } else {
        EXPECT_LT(block * kAecMicBlockNs, 66000000000LL);
      }

    }
    EXPECT_TRUE(wasReady);
    EXPECT_NEAR(ppm * 1000, clock.RatePpb(), 2000);
  }
}

TEST(AecCalibration, BiasedBatchedEnvelopeEventuallyFailsRatherThanSteers)
{
  AecCalibratedClock clock;
  int32_t rate = 0;
  for (int64_t block = 0; block < 60000 && !clock.Fault(); ++block) {
    const auto actual = static_cast<int64_t>(std::llround(block * kAecMicBlockNs * 1.0007));
    const auto batched = kStart + ((actual + 32999999) / 33000000) * 33000000;
    clock.Observe(batched, kAecMicBlockNs, 100000000);
    if (clock.Ready()) {
      if (!rate) { rate = clock.RatePpb(); }
      EXPECT_EQ(rate, clock.RatePpb());
    }
  }
  EXPECT_TRUE(clock.Fault());
  EXPECT_GT(std::abs(clock.DriftNs()), 2000000);
}

TEST(AecCalibration, RequiresIndependentValidationThenLatchesPhaseStep)
{
  AecCalibratedClock clock;
  for (int64_t block = 0; block < 8000; ++block) {
    const auto step = block >= 7000 ? 10240000 : 0;
    clock.Observe(kStart + block * kAecMicBlockNs + step, kAecMicBlockNs, 100000000);
    if (block == 6000) { EXPECT_TRUE(clock.Locked()); EXPECT_FALSE(clock.Ready()); }
    if (block == 6500) { EXPECT_TRUE(clock.Ready()); }
  }
  EXPECT_TRUE(clock.Fault());
  EXPECT_FALSE(clock.Ready());
  EXPECT_EQ(0, clock.RatePpb());
}

TEST(AecCalibration, PhaseChangeDuringFitCannotBecomeClockRate)
{
  AecCalibratedClock clock;
  for (int64_t block = 0; block < 7000; ++block) {
    clock.Observe(kStart + block * kAecMicBlockNs + (block >= 3000 ? 10240000 : 0),
                  kAecMicBlockNs, 100000000);
  }
  EXPECT_TRUE(clock.Fault());
  EXPECT_FALSE(clock.Ready());
  EXPECT_GT(clock.FitErrorNs(), 1000000);
}

TEST(AecCalibration, RateChangeAfterLockIsNotAbsorbed)
{
  AecCalibratedClock clock;
  for (int64_t block = 0; block < 12000; ++block) {
    clock.Observe(kStart + block * kAecMicBlockNs + std::max<int64_t>(0, block - 7000) * 1024,
                  kAecMicBlockNs, 100000000);
  }
  EXPECT_TRUE(clock.Fault());
  EXPECT_EQ(0, clock.RatePpb());
}

TEST(AecCalibration, RejectsImpossibleClocksAndExhaustion)
{
  AecCalibratedClock reversed, badDuration, badRate, exhausted, impossibleEpoch;
  impossibleEpoch.Observe(std::numeric_limits<int64_t>::max(), kAecMicBlockNs, 100000000);
  EXPECT_TRUE(impossibleEpoch.Fault());
  reversed.Observe(kStart, kAecMicBlockNs, 100000000);
  reversed.Observe(kStart - 1, kAecMicBlockNs, 100000000);
  EXPECT_TRUE(reversed.Fault());
  badDuration.Observe(kStart, 0, 100000000);
  EXPECT_TRUE(badDuration.Fault());
  for (int64_t block = 0; block < 7000; ++block) {
    badRate.Observe(kStart + block * (kAecMicBlockNs + 30000), kAecMicBlockNs, 100000000);
  }
  EXPECT_TRUE(badRate.Fault());
  for (int64_t second = 0; second < 604803; ++second) {
    exhausted.Observe(kStart + second * 1000000000, 1000000000, 100000000);
  }
  EXPECT_TRUE(exhausted.Fault());
}

TEST(AecCalibration, InvalidObservationLatchesCurrentNominalPrediction)
{
  for (const int64_t duration : {kAecMicBlockNs, int64_t(32000000)}) {
    AecCalibratedClock clock;
    EXPECT_EQ(kStart, clock.Observe(kStart, duration, 100000000));
    EXPECT_EQ(kStart, clock.Observe(kStart - 1, duration, 100000000));
    EXPECT_EQ(kStart - 1, clock.FaultObservedNs());
    EXPECT_EQ(kStart + duration, clock.FaultPredictedNs());
    EXPECT_EQ(-duration / 1000, clock.FaultOffsetUs());
    EXPECT_EQ(duration, clock.FaultElapsedNs());
    clock.Observe(kStart + 100 * duration, duration, 100000000);
    EXPECT_EQ(kStart + duration, clock.FaultPredictedNs());
  }
}

TEST(AecCalibration, InvalidObservationLatchesCurrentCalibratedTrackingPrediction)
{
  for (const int64_t duration : {kAecMicBlockNs, int64_t(32000000)}) {
    AecCalibratedClock clock(true), control(true);
    int64_t lastObserved = 0, lastPredicted = 0, block = 0;
    for (; block * duration < 90000000000; ++block) {
      const auto elapsed = block * duration;
      const auto observed = kStart + std::llround(elapsed * 1.0007) +
        std::max<int64_t>(0, elapsed - 70000000000) / 20000;
      lastPredicted = clock.Observe(observed, duration, 100000000);
      control.Observe(observed, duration, 100000000);
      lastObserved = observed;
    }
    ASSERT_TRUE(clock.Ready());
    ASSERT_GT(clock.Updates(), 0u);
    ASSERT_NE(0, clock.RatePpb());
    const auto elapsed = block * duration;
    const auto expected = control.Observe(
      kStart + std::llround(elapsed * 1.0007) + (elapsed - 70000000000) / 20000,
      duration, 100000000);
    ASSERT_TRUE(control.Ready());
    EXPECT_EQ(lastPredicted, clock.Observe(lastObserved - 1, duration, 100000000));
    EXPECT_EQ(lastObserved - 1, clock.FaultObservedNs());
    EXPECT_EQ(expected, clock.FaultPredictedNs());
    EXPECT_EQ((lastObserved - 1 - expected) / 1000, clock.FaultOffsetUs());
    EXPECT_EQ(elapsed, clock.FaultElapsedNs());
  }
}

TEST(AecCalibration, InvalidPredictionFallbackIsSafeAtStartupHorizonAndEpochLimit)
{
  for (const int64_t observed : {int64_t(0), std::numeric_limits<int64_t>::min(),
                                  std::numeric_limits<int64_t>::max()}) {
    AecCalibratedClock clock(true);
    EXPECT_EQ(0, clock.Observe(observed, kAecMicBlockNs, 100000000));
    EXPECT_EQ(observed, clock.FaultObservedNs());
    EXPECT_EQ(0, clock.FaultPredictedNs());
    EXPECT_EQ(0, clock.FaultElapsedNs());
  }
  const auto epoch = std::numeric_limits<int64_t>::max() - 1209600000000000LL;
  AecCalibratedClock highEpoch;
  highEpoch.Observe(epoch, kAecMicBlockNs, 100000000);
  highEpoch.Observe(std::numeric_limits<int64_t>::min(), kAecMicBlockNs, 100000000);
  EXPECT_EQ(std::numeric_limits<int64_t>::min(), highEpoch.FaultObservedNs());
  EXPECT_EQ(epoch + kAecMicBlockNs, highEpoch.FaultPredictedNs());
  EXPECT_EQ(std::numeric_limits<int32_t>::min(), highEpoch.FaultOffsetUs());

  AecCalibratedClock exhausted;
  int64_t last = 0;
  for (int64_t second = 0; second <= 604800; ++second) {
    last = exhausted.Observe(kStart + second * 1000000000, 1000000000, 100000000);
  }
  ASSERT_FALSE(exhausted.Fault());
  const auto invalid = kStart + 604801000000000LL;
  EXPECT_EQ(last, exhausted.Observe(invalid, 1000000000, 100000000));
  EXPECT_EQ(invalid, exhausted.FaultObservedNs());
  EXPECT_EQ(last, exhausted.FaultPredictedNs());
  EXPECT_EQ(604801000000000LL, exhausted.FaultElapsedNs());
}

TEST(AecSource, LossDuplicatesMalformedMetadataAndWrap)
{
  AecSourceContinuity source, gap, duplicate, withinBlock, unavailable;
  EXPECT_TRUE(source.Observe(0xffffffffu, 0, true));
  EXPECT_TRUE(source.Observe(1, 2, true));
  EXPECT_TRUE(gap.Observe(10, 11, true));
  EXPECT_FALSE(gap.Observe(14, 15, true));
  EXPECT_FALSE(gap.Observe(16, 17, true)); // never silently recover a lost history
  EXPECT_TRUE(duplicate.Observe(10, 11, true));
  EXPECT_FALSE(duplicate.Observe(10, 11, true));
  EXPECT_FALSE(withinBlock.Observe(10, 12, true));
  EXPECT_FALSE(unavailable.Observe(10, 11, false));
}

TEST(AecPlaybackReference, CalibratedClocksResampleContinuousSignalWithoutSlips)
{
  AecPlaybackReference reference;
  AecCalibratedClock micClock;
  reference.Enable();
  int16_t input[1024], output[160];
  const double dacScale = 1.00008, micScale = 1.0007;
  int64_t producer = 0;
  size_t checked = 0;
  for (int64_t block = 0; block < 30000; ++block) {
    const auto micActual = static_cast<int64_t>(std::llround(block * kAecMicBlockNs * micScale));
    const auto micTime = micClock.Observe(kStart + micActual + (block % 7) * 1000000,
                                          kAecMicBlockNs, 100000000);
    while (producer * 32000000 * dacScale <= micActual + 20000000) {
      for (int i = 0; i < 1024; ++i) {
        const double seconds = (producer * 1024 + i) * 31250.0 * dacScale / 1e9;
        input[i] = static_cast<int16_t>(10000 * std::sin(2 * 3.141592653589793 * 100 * seconds));
      }
      const auto dacActual = static_cast<int64_t>(std::llround(producer * 32000000 * dacScale));
      reference.PushClocked(input, 1024, 32000, 1, kStart + dacActual + (producer % 16) * 2000000,
                            34000000);
      ++producer;
    }
    const auto missing = reference.ReadMicrophone(output, 160, micTime, 64000 * micClock.Scale());
    ASSERT_FALSE(micClock.Fault());
    ASSERT_FALSE(reference.ClockFault());
    if (micClock.Ready() && reference.ClockReady()) {
      ASSERT_EQ(0u, missing);
      for (int i = 0; i < 160; ++i) {
        const double seconds = (block * 160 + i) * 64000.0 * micScale / 1e9;
        EXPECT_NEAR(10000 * std::sin(2 * 3.141592653589793 * 100 * seconds), output[i], 100);
      }
      ++checked;
    }
  }
  EXPECT_GT(checked, 23000u);
  EXPECT_EQ(0u, reference.Dropped());
  EXPECT_EQ(0u, reference.Discontinuities());
}

TEST(AecPlaybackReference, CalibratedDacGapFailsClosed)
{
  AecPlaybackReference reference;
  reference.Enable();
  int16_t input[1024]{}, output[512];
  for (int64_t period = 0; period < 2200; ++period) {
    reference.PushClocked(input, 1024, 32000, 1, kStart + period * 32000000, 34000000);
    reference.Read(output, 512, kFirstOutput + period * 32000000);
  }
  ASSERT_TRUE(reference.ClockReady());
  reference.PushClocked(input, 1024, 32000, 1, kStart + 2200 * 32000000LL + 100000000,
                        34000000);
  EXPECT_TRUE(reference.ClockFault());
  EXPECT_FALSE(reference.ClockReady());
  EXPECT_EQ(1u, reference.Discontinuities());
}

TEST(AecTracking, SyntheticReplayOf108AggregateTrajectoryIsNotRecordedAudio)
{
  AecCalibratedClock frozen, tracking(true);
  // Run 2097's published minima at windows 13..19, interpolated synthetic
  // delivery envelopes. Journals do not contain individual source observations.
  const double offsets[] = {0, 835000, 1062000, 1731000, 1976000, 2023000, 2443000, 2664000};
  int64_t previous = 0, previousDuration = 0;
  for (int64_t block = 0; block < 40000; ++block) {
    const double elapsed = block * kAecMicBlockNs;
    const double index = std::max(0.0, (elapsed - 60000000000.0) / 5000000000.0);
    const int part = std::min(6, static_cast<int>(index));
    const double offset = index >= 7 ? offsets[7] + (index - 7) * 221000 :
      offsets[part] + (offsets[part + 1] - offsets[part]) * (index - part);
    const auto actual = kStart + static_cast<int64_t>(std::llround(elapsed * 1.000918287 + offset));
    const auto observed = actual + (block % 7) * 1000000;
    frozen.Observe(observed, kAecMicBlockNs, 100000000);
    const auto predicted = tracking.Observe(observed, kAecMicBlockNs, 100000000);
    ASSERT_FALSE(tracking.Fault()) << block << " reason=" << tracking.Reason();
    if (tracking.Ready()) {
      EXPECT_NEAR(actual, predicted, 2000000);
      // Rate updates pivot continuously. The preceding block owns its duration.
      EXPECT_NEAR(previous + previousDuration, predicted, 2);
    }
    previous = predicted;
    previousDuration = std::llround(kAecMicBlockNs * tracking.Scale());
  }
  EXPECT_TRUE(frozen.Fault());
  EXPECT_TRUE(tracking.Ready());
  EXPECT_GT(tracking.Updates(), 50u);
}

TEST(AecTracking, Synthetic108LongRun1758EnvelopeTrajectory)
{
  // Selected actual aggregate minima from pre-restart-journal.txt. Piecewise
  // interpolation is a synthetic stress input, not recovered source timestamps.
  // Paired mic/DAC records share the mic-window index as a synthetic time axis;
  // the actual DAC window counter need not match that index.
  struct Envelope { int window, micUs, refUs; };
  const Envelope points[] = {
    {12,0,0}, {13,-362,-127}, {18,-429,-327}, {20,-490,-516}, {23,-327,-727},
    {28,194,-1042}, {30,452,-1107}, {33,1012,-1520}, {36,1349,-1734}, {38,737,-1959},
    {40,500,-2046}, {44,1380,-2509}, {46,2353,-2679}, {49,2953,-2867},
    {50,2583,-3024}, {54,1929,-3494}, {59,2539,-4029}, {60,2853,-4116},
    {64,3216,-4577}, {69,1752,-5200}, {70,1653,-5295},
    {74,984,-6027}, {77,2012,-6453}, {80,3371,-6753}, {83,5237,-7139},
    {87,5873,-7770}, {89,5455,-7976}, {92,4040,-8183}, {97,3659,-8813},
    {100,2977,-9459}, {105,3758,-10076}, {107,4250,-10332},
    {110,3626,-10907}, {113,2710,-11405}, {115,2534,-11702}
  };
  for (const bool mic : {false, true}) {
    AecCalibratedClock frozen, tracking(true);
    size_t part = 0;
    const int64_t duration = mic ? kAecMicBlockNs : 32000000;
    for (int64_t block = 0; block * duration < 575000000000; ++block) {
      const double elapsed = block * duration;
      const double window = elapsed / 5000000000.0;
      while (part + 2 < sizeof(points) / sizeof(points[0]) && window > points[part + 1].window) { ++part; }
      const auto& left = points[part];
      const auto& right = points[part + 1];
      const auto offset = window < 12 ? 0.0 :
        (mic ? left.micUs : left.refUs) + (window - left.window) / (right.window - left.window) *
        ((mic ? right.micUs : right.refUs) - (mic ? left.micUs : left.refUs));
      const auto actual = kStart + std::llround(elapsed * (1 + (mic ? 815084 : 59335) / 1e9) + offset * 1000);
      const auto observed = actual + (block % (mic ? 7 : 16)) * 1000000;
      frozen.Observe(observed, duration, mic ? 100000000 : 34000000);
      const auto predicted = tracking.Observe(observed, duration, mic ? 100000000 : 34000000);
      ASSERT_FALSE(tracking.Fault()) << "mic=" << mic << " block=" << block;
      if (tracking.Ready()) { EXPECT_NEAR(actual, predicted, 2000000); }
    }
    EXPECT_TRUE(frozen.Fault());
    EXPECT_TRUE(tracking.Ready());
  }
}

TEST(AecPlaybackReference, ClockFaultItselfFencesOneInterpolatedSampleWithoutOutputLoss)
{
  AecPlaybackReference reference;
  reference.Enable();
  int16_t input[1024], output[500];
  std::fill(std::begin(input), std::end(input), 10000);
  bool sawFault = false;
  for (int64_t period = 0; period < 3000; ++period) {
    const auto start = kStart + period * 32000000;
    const bool wasFaulted = reference.ClockFault();
    // PCM submissions remain uninterrupted. Only the observation changes.
    reference.PushClocked(input, 1024, 32000, 1, start + (period >= 2200 ? 3000000 : 0), 34000000);
    const auto missing = reference.ReadMicrophone(output, 500, start - 218750);
    if (!wasFaulted && reference.ClockFault()) {
      sawFault = true;
      EXPECT_EQ(1u, reference.Discontinuities());
      EXPECT_EQ(0u, reference.Dropped());
      EXPECT_EQ(0u, reference.Errors());
      EXPECT_EQ(1u, missing); // generation fence, not lost accepted PCM
      EXPECT_FALSE(reference.ClockReady());
      break;
    }
  }
  EXPECT_TRUE(sawFault);
}

TEST(AecTracking, PositiveAndNegativeBoundedRateVariation)
{
  for (const double direction : {-1.0, 1.0}) {
    AecCalibratedClock clock(true);
    double actual = kStart;
    int32_t previousRate = 0;
    bool ready = false;
    for (int64_t block = 0; block < 120000; ++block) {
      const double seconds = block * kAecMicBlockNs / 1e9;
      const double ppm = 700 + direction * (seconds < 70 ? 0 : 80 * std::sin((seconds - 70) / 120));
      const auto predicted = clock.Observe(std::llround(actual) + (block % 7) * 1000000,
                                           kAecMicBlockNs, 100000000);
      ASSERT_FALSE(clock.Fault()) << block;
      if (ready) { EXPECT_LE(std::abs(clock.RatePpb() - previousRate), 100001); }
      if (clock.Ready()) { EXPECT_NEAR(actual, predicted, 2000000); }
      previousRate = clock.RatePpb();
      ready = clock.Ready();
      actual += kAecMicBlockNs * (1 + ppm / 1e6);
    }
    EXPECT_TRUE(clock.Ready());
  }
}

TEST(AecTracking, TotalRateBoundAndExternalSourceFaultRemainLatched)
{
  AecCalibratedClock rate(true), source(true);
  for (int64_t block = 0; block < 16000; ++block) {
    const auto elapsed = block * kAecMicBlockNs;
    rate.Observe(kStart + std::llround(elapsed * 1.00199) +
                   std::max<int64_t>(0, block - 7000) * 1024, kAecMicBlockNs, 100000000);
    source.Observe(kStart + elapsed, kAecMicBlockNs, 100000000);
    if (block == 7000) { source.Invalidate(0); }
  }
  EXPECT_TRUE(rate.Fault());
  EXPECT_EQ(static_cast<uint32_t>(AecCalibratedClock::FaultReason::TrackingRate), rate.Reason());
  EXPECT_TRUE(source.Fault());
  EXPECT_FALSE(source.Ready());
  EXPECT_EQ(static_cast<uint32_t>(AecCalibratedClock::FaultReason::SourceContinuity), source.Reason());
  EXPECT_EQ(0, source.FaultObservedNs());
  EXPECT_LT(source.FaultOffsetUs(), 0);
  AecCalibratedClock impossible(true);
  impossible.Observe(std::numeric_limits<int64_t>::min(), kAecMicBlockNs, 100000000);
  EXPECT_EQ(std::numeric_limits<int32_t>::min(), impossible.FaultOffsetUs());
}

TEST(AecTracking, StepsLossAndExcessiveRateNeverRecoverOrUpdateAfterFault)
{
  for (const int mode : {0, 1, 2, 3}) {
    AecCalibratedClock clock(true);
    uint32_t updatesAtFault = 0, reason = 0;
    int64_t observedAtFault = 0, predictedAtFault = 0;
    for (int64_t block = 0; block < 18000; ++block) {
      const auto step = block < 7000 ? 0 :
        (mode == 0 ? 5120000 : mode == 1 ? -5120000 : mode == 2 ? 1500000 :
         (block - 7000) * 20480);
      clock.Observe(kStart + block * kAecMicBlockNs + step + (block % 7) * 1000000,
                    kAecMicBlockNs, 100000000);
      if (clock.Fault()) {
        if (!reason) {
          reason = clock.Reason(); updatesAtFault = clock.Updates();
          observedAtFault = clock.FaultObservedNs(); predictedAtFault = clock.FaultPredictedNs();
        }
        EXPECT_EQ(updatesAtFault, clock.Updates());
        EXPECT_EQ(reason, clock.Reason());
        EXPECT_EQ(observedAtFault, clock.FaultObservedNs());
        EXPECT_EQ(predictedAtFault, clock.FaultPredictedNs());
        EXPECT_FALSE(clock.Ready());
      }
    }
    EXPECT_TRUE(clock.Fault()) << mode;
  }
}

TEST(AecTracking, BoundedQuantizationCannotEstablishPhysicalClockCause)
{
  AecCalibratedClock clock(true);
  int64_t maximumError = 0;
  for (int64_t block = 0; block < 60000 && !clock.Fault(); ++block) {
    const auto actual = std::llround(block * kAecMicBlockNs * 1.0007);
    const auto predicted = clock.Observe(kStart + ((actual + 32999999) / 33000000) * 33000000,
                                        kAecMicBlockNs, 100000000);
    if (clock.Ready()) { maximumError = std::max<int64_t>(maximumError, std::abs(predicted - kStart - actual)); }
  }
  EXPECT_LT(maximumError, 2000000);
}

TEST(AecSource, FirstFaultRetainsExpectedAndReceivedTransportFrames)
{
  AecSourceContinuity source;
  ASSERT_TRUE(source.Observe(10, 11, true));
  ASSERT_FALSE(source.Observe(14, 15, true));
  EXPECT_EQ(12u, source.Expected());
  EXPECT_EQ(14u, source.First());
  EXPECT_EQ(15u, source.Last());
  EXPECT_EQ(3u, source.Reason());
  source.Observe(999, 999, false);
  EXPECT_EQ(12u, source.Expected());
  EXPECT_EQ(14u, source.First());
  EXPECT_EQ(15u, source.Last());
  EXPECT_EQ(3u, source.Reason());
}
