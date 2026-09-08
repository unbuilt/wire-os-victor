#pragma once

#include "audioEngine/audioExport.h"
#include "audioEngine/plugins/aecExperimentTiming.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <sys/types.h>

namespace Anki { namespace AudioEngine { namespace PlugIns {

// Two single-writer lanes. Only Begin/worker/Shutdown allocate, wait or do I/O.
// The owning object must outlive both audio producers.
class AUDIOENGINE_EXPORT AecDiagnosticCapture
{
public:
  static_assert(ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_BOOL_LOCK_FREE == 2,
                "Diagnostic capture requires lock-free lifecycle atomics");
  enum Error : uint32_t { Overflow = 1, SourceGap = 2, SinkError = 4, MissingStream = 8,
                          Aborted = 16, InvalidFormat = 32, ReferenceRingDrop = 64, MicQueueOverflow = 128,
                          InvalidObservation = 256 };
  AecDiagnosticCapture() = default;
  ~AecDiagnosticCapture();
  bool Begin(const std::string& directory, unsigned seconds, int experimentMode,
             int micAgeMs = 10, int refDelayMs = 0);
  bool Busy() const;
  void Shutdown();
  std::string StatusJson(int experimentMode) const;
  void Reference(const int16_t* pcm, uint32_t count, uint64_t index, int64_t receivedNs,
                 const AecCalibratedClock& clock, uint32_t sinkErrors, uint32_t ringDrops,
                 bool observationValid = true);
  void Microphone(const int16_t* planar, uint64_t index, int64_t receivedNs,
                  uint32_t first, uint32_t last, bool valid, const AecCalibratedClock& clock);
  void NoteError(uint32_t error);

private:
  friend struct AecDiagnosticCaptureTestAccess;
  enum State : uint32_t { Idle, Active, Writing, Saved, Failed };
  struct Record {
    uint64_t index = 0, offset = 0;
    int64_t receivedNs = 0;
    uint32_t count = 0, first = 0, last = 0, valid = 0, sinkErrors = 0, ringDrops = 0;
    AecClockTrace clock;
  };
  struct Lane {
    std::unique_ptr<int16_t[]> pcm;
    std::unique_ptr<Record[]> records;
    size_t sampleCapacity = 0, recordCapacity = 0, samples = 0, count = 0;
    std::array<AecClockTrace, 64> history;
    size_t historyCount = 0;
    AecClockTrace firstFault;
  };
  struct Buffer { Lane mic, ref; };
  bool Enter(int64_t receivedNs);
  void Leave() { _writers.fetch_sub(1); }
  void Append(Lane& lane, const int16_t* pcm, uint32_t count, unsigned channels,
              uint64_t index, int64_t receivedNs, uint32_t first, uint32_t last, bool valid,
              const AecCalibratedClock& clock, uint32_t sinkErrors, uint32_t ringDrops);
  void Worker();
  bool WriteFiles();
  static int64_t NowNs();
  static void Allocate(Lane& lane, size_t samples, size_t records);
  static bool PrepareDirectory(const std::string& directory, const std::string& trial);
  using WriteFunction = ssize_t (*)(int, const void*, size_t);
  static bool WriteAll(int fd, const void* data, size_t bytes, WriteFunction writer = nullptr);
  static bool WriteWave(const std::string& path, const Lane& lane, unsigned channels, unsigned rate,
                        uint64_t* expectedHash = nullptr);
  static bool ValidateWave(const std::string& path, size_t samples, unsigned channels, unsigned rate);
  static bool WriteRecords(const std::string& path, const Lane& lane, const std::string& id);
  static bool WriteHistory(const std::string& path, const Buffer& buffer, const std::string& id);
  static bool FileMetadata(const std::string& path, std::string& metadata, uint64_t expectedBytes = 0,
                           const uint64_t* expectedHash = nullptr);
  static bool SyncFile(const std::string& path);
  static std::string Escape(const std::string& value);

  mutable std::mutex _control;
  std::thread _worker;
  std::unique_ptr<Buffer> _buffer;
  std::atomic<uint32_t> _state{Idle}, _writers{0}, _errors{0};
  std::atomic<bool> _cancel{false};
  bool _shutdown = false;
  int64_t _startNs = 0, _endNs = 0;
  unsigned _seconds = 0;
  int _micAgeMs = 10, _refDelayMs = 0;
  std::string _id, _path, _message;
};

}}}
