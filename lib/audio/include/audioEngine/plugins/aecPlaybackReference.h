#pragma once

#include "audioEngine/audioExport.h"
#include "audioEngine/plugins/aecExperimentTiming.h"
#include "audioEngine/plugins/aecDiagnosticCapture.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <algorithm>

namespace Anki {
namespace AudioEngine {
namespace PlugIns {

// One ALSA writer, one microphone reader. No allocation, locks, or logging in Push().
// Timestamps describe estimated DAC presentation, not Wwise submission.
class AecPlaybackReference
{
public:
  static_assert(ATOMIC_INT_LOCK_FREE == 2 && ATOMIC_BOOL_LOCK_FREE == 2, "AEC reference requires lock-free atomics");
  static int64_t NowNs()
  {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  void Enable() { _enabled.store(true, std::memory_order_release); }
  bool Enabled() const { return _enabled.load(std::memory_order_acquire); }
  AecDiagnosticCapture& Diagnostic() { return _diagnostic; }
  uint32_t Dropped() const { return _dropped.load(); }
  uint32_t Errors() const { return _errors.load(); }
  uint32_t Discontinuities() const { return _discontinuities.load(); }
  bool ClockReady() const { return _clockReady.load(std::memory_order_acquire); }
  bool ClockFault() const { return _clockFault.load(std::memory_order_acquire); }
  int32_t ClockResidualUs() const { return _clockResidualUs.load(); }
  int32_t ClockDriftUs() const { return _clockDriftUs.load(); }
  int32_t ClockRatePpb() const { return _clockRatePpb.load(); }
  int32_t ClockFitErrorUs() const { return _clockFitErrorUs.load(); }
  int32_t ClockRawResidualUs() const { return _clockRawResidualUs.load(); }
  int32_t ClockWindowMinUs() const { return _clockWindowMinUs.load(); }
  int32_t ClockWindowMaxUs() const { return _clockWindowMaxUs.load(); }
  uint32_t ClockWindows() const { return _clockWindows.load(); }
  uint32_t ClockUpdates() const { return _clockUpdates.load(); }
  uint32_t ClockFaultReason() const { return _clockFaultReason.load(std::memory_order_acquire); }
  uint32_t ClockFaultElapsedMs() const { return _clockFaultElapsedMs.load(); }
  int32_t ClockFaultOffsetUs() const { return _clockFaultOffsetUs.load(); }
  int32_t ClockFaultWindowMinUs() const { return _clockFaultWindowMinUs.load(); }
  // Producer thread only, including ALSA recovery. Never join PCM across an error.
  void ReportError()
  {
    _diagnostic.NoteError(AecDiagnosticCapture::SinkError);
    _errors.fetch_add(1, std::memory_order_relaxed);
    _nextInputNs = 0;
    _history.fill(0);
    _phase = 0;
    ++_generation;
  }

  void Push(const int16_t* pcm, size_t frames, unsigned rate, unsigned channels, int64_t startNs,
            int64_t timestampToleranceNs = 2000000, double sampleNs = 31250.0)
  {
    if (!Enabled()) { return; }
    // The real Victor sink is mono 32 kHz. Reject format changes, never misinterpret PCM.
    if (rate != 32000 || channels != 1 || pcm == nullptr) { ReportError(); return; }
    if (_nextInputNs != 0) {
      const auto difference = startNs - _nextInputNs;
      if (difference >= -timestampToleranceNs && difference <= timestampToleranceNs) {
        startNs = _nextInputNs; // accepted PCM is contiguous, unlike the coarse driver pointer
      } else {
        _history.fill(0);
        _phase = 0;
        ++_generation;
        _discontinuities.fetch_add(1, std::memory_order_relaxed);
      }
    }
    _nextInputNs = startNs + static_cast<int64_t>(std::llround(frames * sampleNs));
    for (size_t i = 0; i < frames; ++i) {
      _history[_head] = pcm[i];
      _head = (_head + 1) % _history.size();
      if (++_phase != 2) { continue; }
      _phase = 0;
      // 15-tap Hamming-windowed half-band FIR, unity DC gain; group delay = 7 / 32000 s.
      static constexpr float taps[15] = {
        -0.00365145f, 0.f, 0.01617925f, 0.f, -0.06841178f, 0.f, 0.30494752f,
        0.50187292f, 0.30494752f, 0.f, -0.06841178f, 0.f, 0.01617925f, 0.f, -0.00365145f
      };
      float value = 0;
      for (size_t t = 0; t < _history.size(); ++t) {
        value += taps[t] * _history[(_head + t) % _history.size()];
      }
      const auto write = _write.load(std::memory_order_relaxed);
      const auto next = (write + 1) % kCapacity;
      const auto sequence = _outputSequence++;
      if (next == _read.load(std::memory_order_acquire)) {
        _dropped.fetch_add(1, std::memory_order_relaxed);
        _diagnostic.NoteError(AecDiagnosticCapture::ReferenceRingDrop);
        continue;
      }
      _samples[write] = {startNs + static_cast<int64_t>(std::llround((static_cast<int64_t>(i) - 7) * sampleNs)),
                        static_cast<int16_t>(std::max(-32768.f, std::min(32767.f, value))),
                        _generation, sequence};
      _write.store(next, std::memory_order_release);
    }
  }

  // Called with the unsmoothed DAC estimate, before Push, by the ALSA writer.
  void ObserveClock(int64_t observedNs, size_t frames, int64_t uncertaintyNs)
  {
    _clock.Observe(observedNs, static_cast<int64_t>(frames) * 31250, uncertaintyNs);
    _clockResidualUs.store(static_cast<int32_t>(_clock.ResidualNs() / 1000));
    _clockDriftUs.store(static_cast<int32_t>(_clock.DriftNs() / 1000));
    _clockFault.store(_clock.Fault(), std::memory_order_release);
    _clockReady.store(_clock.Ready(), std::memory_order_release);
  }

  void PushClocked(const int16_t* pcm, size_t frames, unsigned rate, unsigned channels,
                   int64_t observedNs, int64_t uncertaintyNs)
  {
    if (!Enabled()) { return; }
    if (rate != 32000 || channels != 1 || !pcm || frames == 0 || frames > 32000) {
      CaptureUnclockedAccepted(pcm, frames, rate, channels);
      ReportError();
      return;
    }
    const bool locked = _calibratedClock.Locked();
    const bool fault = _calibratedClock.Fault();
    const auto predicted = _calibratedClock.Observe(observedNs, frames * 31250LL, uncertaintyNs);
    _diagnostic.Reference(pcm, frames, _acceptedSamples, NowNs(), _calibratedClock, Errors(), Dropped());
    _acceptedSamples += frames;
    if (!fault && _calibratedClock.Fault()) {
      _history.fill(0);
      _phase = 0;
      ++_generation;
      _discontinuities.fetch_add(1, std::memory_order_relaxed);
    }

    if (!locked && _calibratedClock.Locked()) {
      // The one-time affine mapping cannot rewrite samples already queued.
      // A new segment plus the independent validation window fences the switch.
      _nextInputNs = 0;
      _history.fill(0);
      _phase = 0;
      ++_generation;
    }
    _clockResidualUs.store(static_cast<int32_t>(_calibratedClock.ResidualNs() / 1000));
    _clockDriftUs.store(static_cast<int32_t>(_calibratedClock.DriftNs() / 1000));
    _clockRatePpb.store(_calibratedClock.RatePpb());
    _clockFitErrorUs.store(static_cast<int32_t>(_calibratedClock.FitErrorNs() / 1000));
    _clockRawResidualUs.store(static_cast<int32_t>(_calibratedClock.RawResidualNs() / 1000));
    _clockWindowMinUs.store(static_cast<int32_t>(_calibratedClock.WindowMinNs() / 1000));
    _clockWindowMaxUs.store(static_cast<int32_t>(_calibratedClock.WindowMaxNs() / 1000));
    _clockWindows.store(_calibratedClock.Windows());
    _clockUpdates.store(_calibratedClock.Updates());
    if (!fault && _calibratedClock.Fault()) {
      _clockFaultElapsedMs.store(static_cast<uint32_t>(_calibratedClock.FaultElapsedNs() / 1000000));
      _clockFaultOffsetUs.store(_calibratedClock.FaultOffsetUs());
      _clockFaultWindowMinUs.store(static_cast<int32_t>(_calibratedClock.FaultWindowMinNs() / 1000));
      _clockFaultReason.store(_calibratedClock.Reason(), std::memory_order_release);
    }
    _clockFault.store(_calibratedClock.Fault(), std::memory_order_release);
    _clockReady.store(_calibratedClock.Ready(), std::memory_order_release);
    // The continuous piecewise-affine mapping never retimestamps queued PCM.
    _nextInputNs = 0;
    Push(pcm, frames, rate, channels, predicted, uncertaintyNs, 31250.0 * _calibratedClock.Scale());
  }

  // A short/invalid-timestamp write still accepted these exact PCM samples.
  // Record them with an explicit integrity error, never a synthetic clock observation.
  void CaptureUnclockedAccepted(const int16_t* pcm, size_t frames, unsigned rate, unsigned channels)
  {
    if (!Enabled()) { return; }
    _diagnostic.NoteError(AecDiagnosticCapture::SinkError);
    if (rate == 32000 && channels == 1 && pcm && frames > 0 && frames <= 32000) {
      _diagnostic.Reference(pcm, frames, _acceptedSamples, NowNs(), _calibratedClock, Errors(), Dropped(), false);
    } else {
      _diagnostic.NoteError(AecDiagnosticCapture::InvalidFormat);
    }
    _acceptedSamples += frames;
  }

  // Missing samples are explicitly counted. Never replay old reference through a gap.
  size_t Read(int16_t* out, size_t count, int64_t startNs)
  {
    size_t missing = 0;
    auto read = _read.load(std::memory_order_relaxed);
    for (size_t i = 0; i < count; ++i) {
      const auto target = startNs + static_cast<int64_t>(i) * 62500;
      auto write = _write.load(std::memory_order_acquire);
      while (read != write && _samples[read].timeNs < target - 31250) {
        read = (read + 1) % kCapacity;
      }
      if (read != write && _samples[read].timeNs <= target + 31250) {
        out[i] = _samples[read].value;
        read = (read + 1) % kCapacity;
      } else {
        out[i] = 0;
        ++missing;
      }
    }
    _read.store(read, std::memory_order_release);
    return missing;
  }

  // Resample the DAC reference onto the calibrated ADC grid (nominal 15,625 Hz).
  // Keep the left bracket for the next read; never interpolate through a gap,
  // an ALSA recovery, or a dropped sample. Work/storage remain bounded.
  size_t ReadMicrophone(int16_t* out, size_t count, int64_t startNs,
                        double sampleNs = kAecMicSampleNs)
  {
    size_t missing = 0;
    auto read = _read.load(std::memory_order_relaxed);
    const auto write = _write.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i) {
      const auto target = startNs + static_cast<int64_t>(std::llround(i * sampleNs));
      auto next = (read + 1) % kCapacity;
      while (read != write && next != write && _samples[next].timeNs <= target) {
        read = next;
        next = (read + 1) % kCapacity;
      }
      out[i] = 0;
      if (read != write && _samples[read].timeNs == target) {
        out[i] = _samples[read].value;
      } else if (read != write && next != write &&
                 _samples[read].timeNs < target && target < _samples[next].timeNs &&
                 _samples[next].timeNs > _samples[read].timeNs &&
                 _samples[next].sequence - _samples[read].sequence == 1u &&
                 _samples[read].generation == _samples[next].generation) {
        const auto& left = _samples[read];
        const auto& right = _samples[next];
        out[i] = left.value + (static_cast<int64_t>(right.value) - left.value) *
          (target - left.timeNs) / (right.timeNs - left.timeNs);
      } else {
        ++missing;
      }
    }
    _read.store(read, std::memory_order_release);
    return missing;
  }

private:
  static constexpr uint32_t kCapacity = 16384; // just over one second at 16 kHz
  struct Sample { int64_t timeNs; int16_t value; uint32_t generation, sequence; };
  std::array<Sample, kCapacity> _samples{};
  std::array<int16_t, 15> _history{};
  size_t _head = 0;
  unsigned _phase = 0;
  int64_t _nextInputNs = 0;
  uint32_t _generation = 0;
  uint32_t _outputSequence = 0;
  uint64_t _acceptedSamples = 0;
  AecDiagnosticCapture _diagnostic;
  AecCadenceMonitor _clock;
  AecCalibratedClock _calibratedClock{true};
  std::atomic<bool> _clockReady{false}, _clockFault{false};
  std::atomic<int32_t> _clockResidualUs{0}, _clockDriftUs{0};
  std::atomic<int32_t> _clockRatePpb{0}, _clockFitErrorUs{0}, _clockRawResidualUs{0};
  std::atomic<int32_t> _clockWindowMinUs{0}, _clockWindowMaxUs{0};
  std::atomic<uint32_t> _clockWindows{0};
  std::atomic<uint32_t> _clockUpdates{0}, _clockFaultReason{0}, _clockFaultElapsedMs{0};
  std::atomic<int32_t> _clockFaultOffsetUs{0}, _clockFaultWindowMinUs{0};
  std::atomic<uint32_t> _discontinuities{0};
  std::atomic<bool> _enabled{false};
  std::atomic<uint32_t> _read{0}, _write{0}, _dropped{0}, _errors{0};
};

AUDIOENGINE_EXPORT AecPlaybackReference& GetAecPlaybackReference();

} // namespace PlugIns
} // namespace AudioEngine
} // namespace Anki
