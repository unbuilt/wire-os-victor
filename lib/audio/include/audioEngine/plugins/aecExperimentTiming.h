#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <limits>

namespace Anki {
namespace AudioEngine {
namespace PlugIns {

// Nominal divider cadence; the physical oscillator is not calibrated by this constant.
constexpr int64_t kAecMicSampleNs = 64000;
constexpr int64_t kAecMicBlockNs = 160 * kAecMicSampleNs;

// Sample count supplies cadence; delivery time only validates it. The lower
// envelope removes bounded batching jitter, without steering either audio clock.
// A fault latches until process restart: never silently chase sustained drift.
class AecCadenceMonitor
{
public:
  int64_t Observe(int64_t observedNs, int64_t durationNs, int64_t limitNs)
  {
    if (!_started) {
      _started = true;
      _nextNs = observedNs;
      _windowStartNs = observedNs;
    }
    const auto predicted = _nextNs;
    _nextNs += durationNs;
    _residualNs = observedNs - predicted;
    _minimumNs = std::min(_minimumNs, _residualNs);
    if (_residualNs < -limitNs || _residualNs > limitNs) { _fault = true; }
    if (predicted - _windowStartNs >= (_ready ? 5000000000LL : 1000000000LL)) {
      if (_ready) {
        _driftNs = _minimumNs - _baselineNs;
        if (_driftNs < -2000000 || _driftNs > 2000000) { _fault = true; }
      } else {
        _baselineNs = _minimumNs;
        _ready = true;
      }
      _minimumNs = std::numeric_limits<int64_t>::max();
      _windowStartNs = predicted;
    }
    return predicted;
  }
  bool Ready() const { return _ready && !_fault; }
  bool Fault() const { return _fault; }
  int64_t ResidualNs() const { return _residualNs; }
  int64_t DriftNs() const { return _driftNs; }

private:
  bool _started = false, _ready = false, _fault = false;
  int64_t _nextNs = 0, _windowStartNs = 0, _baselineNs = 0;
  int64_t _minimumNs = std::numeric_limits<int64_t>::max();
  int64_t _residualNs = 0, _driftNs = 0;
};

// Startup affine identification, optionally followed by bounded period tracking.
// Twelve five-second lower-envelope observations reject dispatch/pointer phase.
// Their fit must agree to 1 ms; an independent window must then agree to 2 ms.
// Tracking changes only future cadence, never the phase of queued samples.
// Delivery-envelope motion is not proof of physical oscillator motion.
struct AecClockTrace
{
  int64_t observedNs = 0, predictedNs = 0, elapsedNs = 0, durationNs = 0;
  int64_t predictedBeforeNs = 0;
  int64_t residualNs = 0, minimumNs = 0, minimumAtNs = 0, maximumNs = 0;
  int64_t windowStartNs = 0, spanNs = 0, previousMinimumNs = 0;
  int32_t rateBeforePpb = 0, rateAfterPpb = 0;
  uint32_t windows = 0, updates = 0, reason = 0, closed = 0, ready = 0;
};

class AecCalibratedClock
{
public:
  enum class FaultReason : uint32_t {
    None, InvalidObservation, ObservationBound, Fit, PhaseBound, EnvelopeStep,
    TrackingRate, SourceContinuity
  };
  explicit AecCalibratedClock(bool track = false) : _track(track) {}
  int64_t Observe(int64_t observedNs, int64_t durationNs, int64_t uncertaintyNs)
  {
    _trace = {};
    _trace.observedNs = observedNs;
    _trace.elapsedNs = _elapsedNs;
    _trace.durationNs = durationNs;
    _trace.rateBeforePpb = RatePpb();
    if (observedNs <= 0 || observedNs > std::numeric_limits<int64_t>::max() - 1209600000000000LL ||
        durationNs <= 0 || durationNs > 1000000000 ||
        uncertaintyNs <= 0 || uncertaintyNs > 1000000000 ||
        (_started && observedNs < _lastObservedNs) ||
        _elapsedNs > 604800000000000LL) {
      Fail(FaultReason::InvalidObservation, observedNs, CurrentPredictionOrLastNs());
      _trace.predictedBeforeNs = CurrentPredictionOrLastNs();
      FinishTrace(CurrentPredictionOrLastNs());
      return _lastPredictedNs;
    }
    if (!_started) { _started = true; _epochNs = observedNs; }
    _lastObservedNs = observedNs;
    const auto raw = observedNs - _epochNs - _elapsedNs;
    _rawResidualNs = raw;
    const auto correction = _locked ? Correction(_elapsedNs) : 0;
    _trace.predictedBeforeNs = _epochNs + _elapsedNs + correction;
    _residualNs = raw - correction;
    // During identification the admissible rate contributes at most 2,000 ppm.
    // After locking there is no expanding allowance.
    const auto rateAllowance = _locked ? 0 : _elapsedNs / 500;
    if (std::abs(_residualNs) > uncertaintyNs + rateAllowance) {
      Fail(FaultReason::ObservationBound, observedNs, _epochNs + _elapsedNs + correction);
    }
    if (_residualNs < _minimumNs) {
      _minimumNs = _residualNs;
      _minimumAtNs = _elapsedNs;
    }
    _maximumNs = std::max(_maximumNs, _residualNs);
    _trace.minimumNs = _minimumNs;
    _trace.minimumAtNs = _minimumAtNs;
    _trace.maximumNs = _maximumNs;
    _trace.windowStartNs = _windowStartNs;
    _trace.spanNs = _elapsedNs - _windowStartNs;
    _trace.previousMinimumNs = _previousMinimumNs;
    if (_elapsedNs - _windowStartNs >= 5000000000LL) {
      _trace.closed = 1;
      _windowMinNs = _minimumNs;
      _windowMaxNs = _maximumNs;
      ++_windows;
      if (_locked) {
        _driftNs = _minimumNs;
        if (std::abs(_driftNs) > 2000000) {
          Fail(FaultReason::PhaseBound, observedNs, _epochNs + _elapsedNs + correction);
        }
        // The first independent window is validation only. Never track a fault.
        if (_track && _ready && !_fault) { Track(observedNs); }
        _previousMinimumNs = _minimumNs;
        _ready = !_fault;
      } else if (!_fault) {
        _points[_pointCount++] = {double(_minimumAtNs), double(_minimumNs)};
        if (_pointCount == _points.size()) { Fit(); }
      }
      _minimumNs = std::numeric_limits<int64_t>::max();
      _maximumNs = std::numeric_limits<int64_t>::min();
      _windowStartNs = _elapsedNs;
    }
    _lastPredictedNs = _epochNs + _elapsedNs + (_locked ? Correction(_elapsedNs) : 0);
    FinishTrace(_lastPredictedNs);
    _elapsedNs += durationNs;
    return _lastPredictedNs;
  }

  void Invalidate(int64_t observedNs)
  {
    Fail(FaultReason::SourceContinuity, observedNs, CurrentPredictionOrLastNs());
  }
  bool Ready() const { return _ready && !_fault; }
  bool Fault() const { return _fault; }
  bool Locked() const { return _locked; }
  double Scale() const { return _locked ? 1.0 + _slope : 1.0; }
  int32_t RatePpb() const { return static_cast<int32_t>(_slope * 1e9); }
  int64_t ResidualNs() const { return _residualNs; }
  int64_t RawResidualNs() const { return _rawResidualNs; }
  int64_t DriftNs() const { return _driftNs; }
  int64_t FitErrorNs() const { return _fitErrorNs; }
  int64_t WindowMinNs() const { return _windowMinNs; }
  int64_t WindowMaxNs() const { return _windowMaxNs; }
  uint32_t Windows() const { return _windows; }
  uint32_t Updates() const { return _updates; }
  uint32_t Reason() const { return static_cast<uint32_t>(_reason); }
  int64_t FaultObservedNs() const { return _faultObservedNs; }
  int64_t FaultPredictedNs() const { return _faultPredictedNs; }
  int64_t FaultElapsedNs() const { return _faultElapsedNs; }
  int64_t FaultWindowMinNs() const { return _faultWindowMinNs; }
  int32_t FaultOffsetUs() const
  {
    const double value = (double(_faultObservedNs) - double(_faultPredictedNs)) / 1000;
    return static_cast<int32_t>(std::max(double(std::numeric_limits<int32_t>::min()),
      std::min(double(std::numeric_limits<int32_t>::max()), value)));
  }
  const AecClockTrace& Trace() const { return _trace; }
  const AecClockTrace& FirstFaultTrace() const { return _firstFaultTrace; }
  size_t CopyHistory(AecClockTrace* output, size_t capacity) const
  {
    const auto count = std::min(capacity, _historyCount);
    for (size_t i = 0; i < count; ++i) {
      output[i] = _traceHistory[(_historyHead + _traceHistory.size() - count + i) % _traceHistory.size()];
    }
    return count;
  }

private:
  void FinishTrace(int64_t predicted)
  {
    _trace.predictedNs = predicted;
    _trace.residualNs = _residualNs;
    _trace.rateAfterPpb = RatePpb();
    _trace.windows = _windows;
    _trace.updates = _updates;
    _trace.reason = Reason();
    _trace.ready = Ready();
    const bool firstFault = _fault && _firstFaultTrace.reason == 0;
    if (firstFault) { _firstFaultTrace = _trace; }
    if (_trace.closed || firstFault) {
      _traceHistory[_historyHead] = _trace;
      _historyHead = (_historyHead + 1) % _traceHistory.size();
      _historyCount = std::min(_historyCount + 1, _traceHistory.size());
    }
  }
  int64_t CurrentPredictionOrLastNs() const
  {
    // Invalid input must not participate in prediction arithmetic. Before an
    // epoch exists or beyond its validated horizon, retain the last safe value.
    if (!_started || _elapsedNs < 0 || _elapsedNs > 604800000000000LL ||
        _epochNs > std::numeric_limits<int64_t>::max() - _elapsedNs) {
      return _lastPredictedNs;
    }
    const double correctionValue = _locked ? _offsetNs + _slope * _elapsedNs : 0;
    if (!std::isfinite(correctionValue) ||
        correctionValue >= double(std::numeric_limits<int64_t>::max()) ||
        correctionValue <= double(std::numeric_limits<int64_t>::min())) {
      return _lastPredictedNs;
    }
    const auto correction = static_cast<int64_t>(std::llround(correctionValue));
    const auto nominal = _epochNs + _elapsedNs;
    if ((correction > 0 && nominal > std::numeric_limits<int64_t>::max() - correction) ||
        (correction < 0 && nominal < std::numeric_limits<int64_t>::min() - correction)) {
      return _lastPredictedNs;
    }
    return nominal + correction;
  }
  void Fail(FaultReason reason, int64_t observed, int64_t predicted)
  {
    if (!_fault) {
      _reason = reason;
      _faultObservedNs = observed;
      _faultPredictedNs = predicted;
      _faultElapsedNs = _elapsedNs;
      _faultWindowMinNs = _minimumNs == std::numeric_limits<int64_t>::max() ? 0 : _minimumNs;
    }
    _fault = true;
  }
  void Track(int64_t observedNs)
  {
    const double span = _elapsedNs - _windowStartNs;
    const auto change = _minimumNs - _previousMinimumNs;
    // An entire missing 80-sample source frame is 5.12 ms. A 1 ms
    // envelope innovation is already rejected; it is not a new phase anchor.
    if (std::abs(change) > 1000000) {
      Fail(FaultReason::EnvelopeStep, observedNs, _epochNs + _elapsedNs + Correction(_elapsedNs));
      return;
    }
    // Damped phase/frequency loop: 40 s phase time constant, half the
    // measured envelope derivative. Limits are design bounds, not jitter estimates.
    const double delta = 0.5 * change / span + _minimumNs / 40000000000.0;
    const double nextSlope = _slope + std::max(-0.0001, std::min(0.0001, delta));
    if (std::abs(delta) > 0.0002 || std::abs(nextSlope) > 0.002) {
      Fail(FaultReason::TrackingRate, observedNs, _epochNs + _elapsedNs + Correction(_elapsedNs));
      return;
    }
    const auto pivot = Correction(_elapsedNs);
    _slope = nextSlope;
    _offsetNs = pivot - _slope * _elapsedNs;
    ++_updates;
  }
  int64_t Correction(int64_t elapsed) const
  {
    return static_cast<int64_t>(std::llround(_offsetNs + _slope * elapsed));
  }
  void Fit()
  {
    double x = 0, y = 0, xx = 0, xy = 0;
    for (const auto& p : _points) { x += p.x; y += p.y; }
    x /= _points.size(); y /= _points.size();
    for (const auto& p : _points) {
      xx += (p.x - x) * (p.x - x);
      xy += (p.x - x) * (p.y - y);
    }
    _slope = xy / xx;
    _offsetNs = y - _slope * x;
    for (const auto& p : _points) {
      _fitErrorNs = std::max(_fitErrorNs,
        static_cast<int64_t>(std::ceil(std::abs(p.y - _offsetNs - _slope * p.x))));
    }
    if (!std::isfinite(_slope) || std::abs(_slope) > 0.002 || _fitErrorNs > 1000000) {
      Fail(FaultReason::Fit, _lastObservedNs, _epochNs + _elapsedNs);
    } else {
      _locked = true;
    }
  }
  struct Point { double x, y; };
  std::array<Point, 12> _points{};
  size_t _pointCount = 0;
  bool _started = false, _locked = false, _ready = false, _fault = false;
  const bool _track;
  FaultReason _reason = FaultReason::None;
  uint32_t _updates = 0;
  int64_t _previousMinimumNs = 0;
  int64_t _faultObservedNs = 0, _faultPredictedNs = 0, _faultElapsedNs = 0, _faultWindowMinNs = 0;
  int64_t _epochNs = 0, _elapsedNs = 0, _lastObservedNs = 0, _lastPredictedNs = 0;
  int64_t _windowStartNs = 0, _minimumAtNs = 0;
  int64_t _minimumNs = std::numeric_limits<int64_t>::max();
  int64_t _maximumNs = std::numeric_limits<int64_t>::min();
  int64_t _windowMinNs = 0, _windowMaxNs = 0, _fitErrorNs = 0;
  int64_t _residualNs = 0, _rawResidualNs = 0, _driftNs = 0;
  uint32_t _windows = 0;
  double _slope = 0, _offsetNs = 0;
  AecClockTrace _trace{}, _firstFaultTrace{};
  std::array<AecClockTrace, 64> _traceHistory{};
  size_t _historyHead = 0, _historyCount = 0;
};

// Counter is from syscon's transmitted frame, not the host callback count.
// Two adjacent 80-sample frames must also adjoin the preceding mic message.
class AecSourceContinuity
{
public:
  bool Observe(uint32_t first, uint32_t last, bool valid)
  {
    if (!valid || last - first != 1u || (_started && first - _last != 1u)) {
      if (_errors == 0) {
        _expected = _started ? _last + 1u : first;
        _first = first; _lastFault = last;
        _reason = !valid ? 1u : (last - first != 1u ? 2u : 3u);
      }
      if (_errors != std::numeric_limits<uint32_t>::max()) { ++_errors; }
    }
    _started = true;
    _last = last;
    return _errors == 0;
  }
  uint32_t Errors() const { return _errors; }
  uint32_t Expected() const { return _expected; }
  uint32_t First() const { return _first; }
  uint32_t Last() const { return _lastFault; }
  uint32_t Reason() const { return _reason; }
private:
  bool _started = false;
  uint32_t _last = 0, _errors = 0;
  uint32_t _expected = 0, _first = 0, _lastFault = 0, _reason = 0;
};

struct AecExecutionTiming
{
  static int64_t ThreadCpuNs()
  {
    timespec value{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) { return -1; }
    return static_cast<int64_t>(value.tv_sec) * 1000000000 + value.tv_nsec;
  }

  void Add(int64_t wallNs, int64_t cpuStartNs, int64_t cpuEndNs)
  {
    const auto wallUs = static_cast<uint32_t>(std::max<int64_t>(0, wallNs) / 1000);
    maxWallUs = std::max(maxWallUs, wallUs);
    totalWallUs += wallUs;
    wallOverBudget += (wallNs > kAecMicBlockNs);
    if (cpuStartNs < 0 || cpuEndNs < cpuStartNs) { ++errors; return; }
    const auto cpuNs = cpuEndNs - cpuStartNs;
    const auto cpuUs = static_cast<uint32_t>(cpuNs / 1000);
    maxCpuUs = std::max(maxCpuUs, cpuUs);
    totalCpuUs += cpuUs;
    cpuOverBudget += (cpuNs > kAecMicBlockNs);
    // Wall minus CPU includes preemption/blocking and clock-read overhead;
    // it is not a measurement of scheduler latency alone.
    maxNonCpuUs = std::max(maxNonCpuUs, wallUs > cpuUs ? wallUs - cpuUs : 0);
  }

  uint32_t maxWallUs = 0, maxCpuUs = 0, maxNonCpuUs = 0;
  uint32_t wallOverBudget = 0, cpuOverBudget = 0, errors = 0;
  uint64_t totalWallUs = 0, totalCpuUs = 0;
};

} // namespace PlugIns
} // namespace AudioEngine
} // namespace Anki
