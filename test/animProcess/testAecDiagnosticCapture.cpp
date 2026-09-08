#include "audioEngine/plugins/aecDiagnosticCapture.h"
#include "audioEngine/plugins/aecPlaybackReference.h"
#include "gtest/gtest.h"
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace Anki::AudioEngine::PlugIns;

namespace Anki { namespace AudioEngine { namespace PlugIns {
struct AecDiagnosticCaptureTestAccess {
  static bool Enter(AecDiagnosticCapture& capture) { return capture.Enter(AecPlaybackReference::NowNs()); }
  static void Leave(AecDiagnosticCapture& capture) { capture.Leave(); }
  static size_t Bytes(const AecDiagnosticCapture& capture) {
    if (!capture._buffer) { return 0; }
    return sizeof(AecDiagnosticCapture::Buffer) +
      (capture._buffer->mic.sampleCapacity + capture._buffer->ref.sampleCapacity) * sizeof(int16_t) +
      (capture._buffer->mic.recordCapacity + capture._buffer->ref.recordCapacity) * sizeof(AecDiagnosticCapture::Record);
  }
  static bool WriterPattern(const std::string& path, unsigned channels, size_t frames,
                            std::vector<unsigned char>& expected, unsigned pattern = 0) {
    AecDiagnosticCapture::Lane lane;
    lane.samples = frames * channels;
    lane.count = channels == 4 ? frames / 160 : 0;
    lane.pcm.reset(new int16_t[lane.samples]);
    expected.assign(44 + lane.samples * 2, 0);
    auto u16 = [&expected](size_t offset, uint16_t value) {
      expected[offset] = value & 255; expected[offset + 1] = value >> 8;
    };
    auto u32 = [&u16](size_t offset, uint32_t value) { u16(offset, value); u16(offset + 2, value >> 16); };
    std::memcpy(expected.data(), "RIFF", 4); u32(4, 36 + lane.samples * 2);
    std::memcpy(expected.data() + 8, "WAVEfmt ", 8); u32(16, 16);
    u16(20, 1); u16(22, channels); u32(24, channels == 4 ? 15625 : 32000);
    u32(28, (channels == 4 ? 15625 : 32000) * channels * 2); u16(32, channels * 2); u16(34, 16);
    std::memcpy(expected.data() + 36, "data", 4); u32(40, lane.samples * 2);
    for (size_t offset = 44; offset < expected.size(); ++offset) {
      expected[offset] = offset < 300 ? offset - 44 : 1 + (offset * 13) % 127;
    }
    expected[4096] = 255;
    if (pattern) {
      for (size_t i = 0; i < lane.samples; ++i) {
        // Odd multiplier visits every PCM16 value; seed puts 0xff at each 4096-byte boundary.
        const auto value = static_cast<uint16_t>(pattern == 1 ? 65535 : i * 73 + 69);
        u16(44 + 2 * i, value);
      }
    }
    for (size_t i = 0; i < lane.samples; ++i) {
      const auto planar = channels == 1 ? i : (i / 640) * 640 + (i % 4) * 160 + (i % 640) / 4;
      lane.pcm[planar] = static_cast<int16_t>(expected[44 + 2 * i] | (expected[45 + 2 * i] << 8));
    }
    return AecDiagnosticCapture::WriteWave(path, lane, channels, channels == 4 ? 15625 : 32000);
  }
  static bool WriterIo(int fd, const void* data, size_t bytes,
                       ssize_t (*writer)(int, const void*, size_t) = nullptr) {
    return AecDiagnosticCapture::WriteAll(fd, data, bytes, writer);
  }
  static bool WaveValid(const std::string& path, size_t samples, unsigned channels, unsigned rate) {
    return AecDiagnosticCapture::ValidateWave(path, samples, channels, rate);
  }
  static bool Metadata(const std::string& path, uint64_t bytes, const uint64_t* hash) {
    std::string metadata;
    return AecDiagnosticCapture::FileMetadata(path, metadata, bytes, hash);
  }
};
}}}

namespace {
std::string Directory(const char* name)
{
  const auto* root = std::getenv("TMPDIR");
  EXPECT_NE(nullptr, root);
  return std::string(root ? root : ".") + "/diag-test-" + name + "-" + std::to_string(getpid());
}
std::string Field(const std::string& text, const std::string& field)
{
  const auto start = text.find("\"" + field + "\":\"") + field.size() + 4;
  return text.substr(start, text.find('"', start) - start);
}
std::string Read(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void Wait(AecDiagnosticCapture& capture)
{
  for (int i = 0; i < 500 && capture.Busy(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
  ASSERT_FALSE(capture.Busy());
}
void Feed(AecDiagnosticCapture& capture, bool gap = false)
{
  AecCalibratedClock mic(true), ref(true);
  int16_t raw[640], accepted[1024];
  for (unsigned i = 0; i < 640; ++i) { raw[i] = 100 + i / 160; }
  std::fill(std::begin(accepted), std::end(accepted), 2345); // actual post-gain PCM, not a fixture
  const auto now = AecPlaybackReference::NowNs();
  for (uint32_t i = 0; i < 32; ++i) {
    ref.Observe(now + i * 32000000LL, 32000000, 34000000);
    capture.Reference(accepted, 1024, 9000 + i * 1024, AecPlaybackReference::NowNs(), ref, 0, 0);
    mic.Observe(now - 1000000000 + i * 10240000LL, 10240000, 100000000);
    capture.Microphone(raw, 5000 + i * 160, AecPlaybackReference::NowNs(),
                       i * 2 + (gap && i >= 10 ? 2 : 0), i * 2 + 1 + (gap && i >= 10 ? 2 : 0), true, mic);
  }
}
std::vector<unsigned char> ioBytes;
unsigned ioCalls = 0;
ssize_t InterruptedShortWrite(int, const void* data, size_t bytes)
{
  ++ioCalls;
  if (ioCalls == 1 || ioCalls == 3) { errno = EINTR; return -1; }
  const auto count = std::min<size_t>(3, bytes);
  const auto* first = static_cast<const unsigned char*>(data);
  ioBytes.insert(ioBytes.end(), first, first + count);
  return count;
}
ssize_t ZeroWrite(int, const void*, size_t) { ++ioCalls; return 0; }
ssize_t FullDiskWrite(int, const void*, size_t) { ++ioCalls; errno = ENOSPC; return -1; }
}

TEST(AecDiagnostic, ReferenceOnlyBoundsAndAtomicArtifactCommit)
{
  AecDiagnosticCapture capture;
  const auto directory = Directory("complete");
  EXPECT_FALSE(capture.Begin(directory, 1, 0));
  EXPECT_FALSE(capture.Begin(directory, 1, 2));
  EXPECT_FALSE(capture.Begin(directory, 0, 1));
  EXPECT_FALSE(capture.Begin(directory, 16, 1));
  ASSERT_TRUE(capture.Begin(directory, 1, 1));
  EXPECT_FALSE(capture.Begin(directory, 1, 1));
  const auto path = Field(capture.StatusJson(1), "path");
  EXPECT_TRUE(Read(path + "/manifest.json").empty());
  Feed(capture);
  Wait(capture);
  EXPECT_EQ("saved", Field(capture.StatusJson(1), "state"));
  const auto manifest = Read(path + "/manifest.json");
  EXPECT_NE(std::string::npos, manifest.find("\"errors\":0"));
  const auto ref = Read(path + "/reference.wav");
  ASSERT_EQ(44u + 32u * 1024u * 2u, ref.size());
  EXPECT_EQ(2345, static_cast<unsigned char>(ref[44]) + 256 * static_cast<unsigned char>(ref[45]));
  const auto raw = Read(path + "/raw.wav");
  ASSERT_EQ(44u + 32u * 640u * 2u, raw.size());
  for (unsigned ch = 0; ch < 4; ++ch) { EXPECT_EQ(100 + ch, static_cast<unsigned char>(raw[44 + 2 * ch])); }
  EXPECT_NE(std::string::npos, Read(path + "/reference.csv").find(",9000,0,1024,"));
  EXPECT_NE(std::string::npos, Read(path + "/mic.csv").find(",5000,0,160,"));
  // Remain available as a real C++ writer fixture for offline-tool regression.
}

TEST(AecDiagnostic, GapsOverflowAndSinkErrorsAreExplicit)
{
  AecDiagnosticCapture capture;
  ASSERT_TRUE(capture.Begin(Directory("errors"), 1, 1));
  Feed(capture, true);
  AecCalibratedClock clock;
  clock.Observe(AecPlaybackReference::NowNs(), 32000000, 34000000);
  int16_t pcm[1024]{};
  for (uint32_t i = 0; i < 100; ++i) {
    capture.Reference(pcm, 1024, i * 1024, AecPlaybackReference::NowNs(), clock, 0, 0);
  }
  capture.NoteError(AecDiagnosticCapture::SinkError);
  Wait(capture);
  EXPECT_NE(std::string::npos, capture.StatusJson(1).find("\"errors\":7"));
}

TEST(AecDiagnostic, StopWaitsForInflightWriterAndRepeatedCaptureHasDistinctTrial)
{
  AecDiagnosticCapture capture;
  ASSERT_TRUE(capture.Begin(Directory("race"), 1, 1));
  const auto first = Field(capture.StatusJson(1), "trial");
  ASSERT_TRUE(AecDiagnosticCaptureTestAccess::Enter(capture));
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  EXPECT_TRUE(capture.Busy());
  EXPECT_EQ("writing", Field(capture.StatusJson(1), "state"));
  AecDiagnosticCaptureTestAccess::Leave(capture);
  Wait(capture);
  ASSERT_TRUE(capture.Begin(Directory("race"), 1, 1));
  EXPECT_NE(first, Field(capture.StatusJson(1), "trial"));
  std::atomic<bool> run{true};
  std::atomic<unsigned> started{0};
  std::thread producer([&] {
    AecCalibratedClock clock;
    int16_t pcm[32]{};
    uint64_t index = 0;
    while (run.load()) {
      capture.Reference(pcm, 32, index, AecPlaybackReference::NowNs(), clock, 0, 0);
      if (index == 0) { started.fetch_add(1); }
      index += 32;
    }
  });
  std::thread microphone([&] {
    AecCalibratedClock clock;
    int16_t pcm[640]{};
    uint32_t block = 0;
    while (run.load()) {
      capture.Microphone(pcm, uint64_t(block) * 160, AecPlaybackReference::NowNs(),
                         block * 2, block * 2 + 1, true, clock);
      if (block == 0) { started.fetch_add(1); }
      ++block;
    }
  });
  while (started.load() != 2) { std::this_thread::yield(); }
  capture.Shutdown();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  run.store(false);
  producer.join();
  microphone.join();
  EXPECT_FALSE(capture.Busy());
  EXPECT_FALSE(capture.Begin(Directory("race"), 1, 1));
}

TEST(AecDiagnostic, RecentHistoryRetainsExactWindowInputsAndFirstFault)
{
  AecCalibratedClock clock(true);
  for (int64_t i = 0; i < 9000; ++i) {
    clock.Observe(1000000000 + i * 10240000 + (i > 7000 ? 4000000 : 0), 10240000, 100000000);
  }
  ASSERT_TRUE(clock.Fault());
  AecClockTrace history[64];
  const auto count = clock.CopyHistory(history, 64);
  EXPECT_GE(count, 17u);
  EXPECT_EQ(5007360000LL, history[0].spanNs);
  EXPECT_EQ(0, history[0].minimumNs);
  EXPECT_EQ(0, history[0].minimumAtNs);
  EXPECT_EQ(4000000, clock.FirstFaultTrace().minimumNs);
  EXPECT_EQ(0, clock.FirstFaultTrace().previousMinimumNs);
  EXPECT_EQ(1u, clock.FirstFaultTrace().closed);
}

TEST(AecDiagnostic, FailedSecondArtifactNeverPublishesCompleteTrial)
{
  AecDiagnosticCapture capture;
  ASSERT_TRUE(capture.Begin(Directory("write-failure"), 1, 1));
  const auto path = Field(capture.StatusJson(1), "path");
  ASSERT_EQ(0, mkdir((path + "/reference.wav").c_str(), 0700));
  Feed(capture);
  Wait(capture);
  EXPECT_EQ("failed", Field(capture.StatusJson(1), "state"));
  EXPECT_FALSE(Read(path + "/raw.wav").empty());
  EXPECT_TRUE(Read(path + "/manifest.json").empty());
}

TEST(AecDiagnostic, ProductionReferenceTapCapturesAcceptedPostGainBeforeFir)
{
  AecPlaybackReference reference;
  reference.Enable();
  int16_t pcm[1024], raw[640]{};
  std::fill(std::begin(pcm), std::end(pcm), 9000);
  const auto start = AecPlaybackReference::NowNs();
  reference.PushClocked(pcm, 1024, 32000, 1, start, 34000000);
  auto& capture = reference.Diagnostic();
  ASSERT_TRUE(capture.Begin(Directory("production-tap"), 1, 1));
  std::fill(std::begin(pcm), std::end(pcm), 2345);
  reference.PushClocked(pcm, 1024, 32000, 1, start + 32000000, 34000000);
  AecCalibratedClock mic;
  mic.Observe(start, 10240000, 100000000);
  capture.Microphone(raw, 0, AecPlaybackReference::NowNs(), 10, 11, true, mic);
  Wait(capture);
  const auto path = Field(capture.StatusJson(1), "path");
  const auto wav = Read(path + "/reference.wav");
  ASSERT_EQ(44u + 1024u * 2u, wav.size());
  // A post-FIR tap would have a startup transient and only 512 samples.
  EXPECT_EQ(2345, static_cast<unsigned char>(wav[44]) + 256 * static_cast<unsigned char>(wav[45]));
  EXPECT_NE(std::string::npos, Read(path + "/reference.csv").find(",1024,0,1024,"));
}

TEST(AecDiagnostic, MaximumAllocationIsBoundedAndOffDoesNotAllocate)
{
  AecDiagnosticCapture capture;
  EXPECT_EQ(0u, AecDiagnosticCaptureTestAccess::Bytes(capture));
  EXPECT_FALSE(capture.Begin(Directory("memory"), 15, 0));
  EXPECT_EQ(0u, AecDiagnosticCaptureTestAccess::Bytes(capture));
  ASSERT_TRUE(capture.Begin(Directory("memory"), 15, 1));
  EXPECT_LT(AecDiagnosticCaptureTestAccess::Bytes(capture), 5u * 1024u * 1024u);
  capture.Shutdown();
  EXPECT_EQ(0u, AecDiagnosticCaptureTestAccess::Bytes(capture));
}

TEST(AecDiagnostic, RotationTouchesOnlyDedicatedKnownArtifacts)
{
  const auto directory = Directory("rotation");
  ASSERT_EQ(0, mkdir(directory.c_str(), 0700));
  for (unsigned i = 0; i < 100; ++i) {
    const auto path = directory + "/trial_1_" + std::to_string(1000 + i);
    ASSERT_EQ(0, mkdir(path.c_str(), 0700));
    std::ofstream(path + "/raw.wav") << "old capture";
  }
  AecDiagnosticCapture capture;
  ASSERT_TRUE(capture.Begin(directory, 1, 1));
  capture.Shutdown();
  unsigned count = 0;
  DIR* dir = opendir(directory.c_str());
  ASSERT_NE(nullptr, dir);
  while (const auto* entry = readdir(dir)) { count += std::string(entry->d_name).compare(0, 6, "trial_") == 0; }
  closedir(dir);
  EXPECT_EQ(100u, count);
}

TEST(AecDiagnostic, MissingObservationCannotBeReportedSaved)
{
  AecDiagnosticCapture capture;
  ASSERT_TRUE(capture.Begin(Directory("missing-observation"), 1, 1));
  Feed(capture);
  AecCalibratedClock unavailable;
  int16_t pcm[1024]{};
  capture.Reference(pcm, 1024, 9000 + 32 * 1024, AecPlaybackReference::NowNs(), unavailable, 0, 0);
  Wait(capture);
  EXPECT_EQ("invalid", Field(capture.StatusJson(1), "state"));
  EXPECT_NE(std::string::npos, capture.StatusJson(1).find("\"errors\":256"));
}

TEST(AecDiagnostic, WriterPreservesEveryByteAcrossTargetStreamBoundary)
{
  const auto directory = Directory("writer-byte-oracle");
  ASSERT_EQ(0, mkdir(directory.c_str(), 0700));
  for (unsigned channels : {1u, 4u}) {
    const auto path = directory + "/" + std::to_string(channels) + ".wav";
    std::vector<unsigned char> expected;
    ASSERT_TRUE(AecDiagnosticCaptureTestAccess::WriterPattern(
      path, channels, channels == 1 ? 480256 : 234560, expected));
    const int fd = open(path.c_str(), O_RDONLY);
    ASSERT_GE(fd, 0);
    std::vector<unsigned char> actual(expected.size());
    size_t count = 0;
    while (count < actual.size()) {
      const auto bytes = read(fd, actual.data() + count, actual.size() - count);
      if (bytes <= 0) { break; }
      count += bytes;
    }
    EXPECT_EQ(0, close(fd));
    EXPECT_EQ(expected.size(), count) << "channels=" << channels;
    if (count == expected.size()) {
      EXPECT_EQ(0, std::memcmp(expected.data(), actual.data(), count)) << "channels=" << channels;
    }
  }

}

TEST(AecDiagnostic, WriterPreservesAllFfAndEveryPcm16Value)
{
  const auto directory = Directory("writer-full-range");
  ASSERT_EQ(0, mkdir(directory.c_str(), 0700));
  for (unsigned channels : {1u, 4u}) {
    for (unsigned pattern : {1u, 2u}) {
      const auto path = directory + "/" + std::to_string(channels) + "-" + std::to_string(pattern) + ".wav";
      std::vector<unsigned char> expected;
      ASSERT_TRUE(AecDiagnosticCaptureTestAccess::WriterPattern(
        path, channels, channels == 1 ? 65536 : 65600, expected, pattern));
      std::vector<bool> values(65536);
      for (size_t i = 44; i < expected.size(); i += 2) {
        values[expected[i] | (expected[i + 1] << 8)] = true;
      }
      EXPECT_EQ(pattern == 1 ? 1 : 65536, std::count(values.begin(), values.end(), true));
      for (size_t i = 4096; i < expected.size(); i += 4096) { EXPECT_EQ(255, expected[i]); }
      const int fd = open(path.c_str(), O_RDONLY);
      ASSERT_GE(fd, 0);
      std::vector<unsigned char> actual(expected.size());
      size_t count = 0;
      while (count < actual.size()) {
        const auto bytes = read(fd, actual.data() + count, actual.size() - count);
        if (bytes <= 0) { break; }
        count += bytes;
      }
      EXPECT_EQ(0, close(fd));
      EXPECT_EQ(expected.size(), count) << "channels=" << channels << " pattern=" << pattern;
      if (count == expected.size()) {
        EXPECT_EQ(0, std::memcmp(expected.data(), actual.data(), count))
          << "channels=" << channels << " pattern=" << pattern;
      }
    }
  }
}

TEST(AecDiagnostic, WriterRetriesInterruptedShortWritesAndRejectsNoProgress)
{
  const unsigned char expected[] = {0, 255, 128, 127, 1, 254, 2, 253, 3, 252, 4, 251};
  ioCalls = 0;
  ioBytes.clear();
  ASSERT_TRUE(AecDiagnosticCaptureTestAccess::WriterIo(-1, expected, sizeof(expected), InterruptedShortWrite));
  EXPECT_EQ(std::vector<unsigned char>(std::begin(expected), std::end(expected)), ioBytes);
  EXPECT_EQ(6u, ioCalls);
  ioCalls = 0;
  EXPECT_FALSE(AecDiagnosticCaptureTestAccess::WriterIo(-1, expected, sizeof(expected), ZeroWrite));
  EXPECT_EQ(1u, ioCalls);
  ioCalls = 0;
  EXPECT_FALSE(AecDiagnosticCaptureTestAccess::WriterIo(-1, expected, sizeof(expected), FullDiskWrite));
  EXPECT_EQ(1u, ioCalls);
  ioCalls = 0;
  EXPECT_TRUE(AecDiagnosticCaptureTestAccess::WriterIo(-1, expected, 0, ZeroWrite));
  EXPECT_EQ(0u, ioCalls);
}

TEST(AecDiagnostic, WriterReadbackRejectsTruncationHeaderAndSameSizeContentDamage)
{
  const auto directory = Directory("writer-readback");
  ASSERT_EQ(0, mkdir(directory.c_str(), 0700));
  const auto path = directory + "/reference.wav";
  std::vector<unsigned char> expected;
  ASSERT_TRUE(AecDiagnosticCaptureTestAccess::WriterPattern(path, 1, 480256, expected));
  uint64_t hash = 14695981039346656037ULL;
  for (const auto byte : expected) { hash = (hash ^ byte) * 1099511628211ULL; }
  EXPECT_TRUE(AecDiagnosticCaptureTestAccess::WaveValid(path, 480256, 1, 32000));
  EXPECT_TRUE(AecDiagnosticCaptureTestAccess::Metadata(path, expected.size(), &hash));

  ASSERT_EQ(0, truncate(path.c_str(), expected.size() - 1));
  EXPECT_FALSE(AecDiagnosticCaptureTestAccess::WaveValid(path, 480256, 1, 32000));
  EXPECT_FALSE(AecDiagnosticCaptureTestAccess::Metadata(path, expected.size(), &hash));

  const auto restore = [&] {
    const int fd = open(path.c_str(), O_WRONLY | O_TRUNC);
    EXPECT_GE(fd, 0);
    const bool result = AecDiagnosticCaptureTestAccess::WriterIo(fd, expected.data(), expected.size());
    EXPECT_EQ(0, close(fd));
    return result;
  };
  ASSERT_TRUE(restore());
  int fd = open(path.c_str(), O_WRONLY);
  ASSERT_GE(fd, 0);
  const unsigned char badChannel = 3;
  ASSERT_EQ(1, pwrite(fd, &badChannel, 1, 22));
  EXPECT_EQ(0, close(fd));
  EXPECT_FALSE(AecDiagnosticCaptureTestAccess::WaveValid(path, 480256, 1, 32000));
  EXPECT_FALSE(AecDiagnosticCaptureTestAccess::Metadata(path, expected.size(), &hash));

  ASSERT_TRUE(restore());
  fd = open(path.c_str(), O_WRONLY);
  ASSERT_GE(fd, 0);
  const auto damaged = static_cast<unsigned char>(expected[5000] ^ 1);
  ASSERT_EQ(1, pwrite(fd, &damaged, 1, 5000));
  EXPECT_EQ(0, close(fd));
  EXPECT_TRUE(AecDiagnosticCaptureTestAccess::WaveValid(path, 480256, 1, 32000));
  EXPECT_FALSE(AecDiagnosticCaptureTestAccess::Metadata(path, expected.size(), &hash));
}
