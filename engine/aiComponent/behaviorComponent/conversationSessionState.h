#pragma once

#include <cstddef>
#include <cstdint>
#include <cmath>

namespace Anki {
namespace Vector {

// Pure monotonic-time policy. Transport cleanup is performed by the component,
// after invalidation, never from an asynchronous callback.
class ConversationSessionState
{
public:
  enum class State { Inactive, Responding, Quiescing, Settling, Opening, Listening, AwaitingClaim };
  enum class Outcome { Succeeded, Failed, Cancelled };
  struct Config {
    bool enabled = true;
    unsigned maxTurns = 5;
    double sessionTimeout_sec = 120;
    double followUpSettleTime_ms = 250;
    bool IsValid() const {
      return maxTurns >= 1 && maxTurns <= 20 &&
             std::isfinite(sessionTimeout_sec) && sessionTimeout_sec > 0 && sessionTimeout_sec <= 600 &&
             std::isfinite(followUpSettleTime_ms) && followUpSettleTime_ms >= 0 && followUpSettleTime_ms <= 2000;
    }
  };

  Config config;
  State GetState() const { return _state; }
  uint64_t Token() const { return _token; }
  unsigned Turn() const { return _turn; }
  const char* EndReason() const { return _reason; }
  bool Active() const { return _state != State::Inactive; }
  bool CanContinueResponse(uint64_t token, double now) const {
    return _state == State::Responding && token == _token && CanAdmit(now);
  }

  void End(const char* reason) {
    if (Active()) {
      ++_token;
      _reason = reason;
      _state = State::Inactive;
    }
  }

  void Claimed(size_t activation, bool eligible, bool voice, double now) {
    if (!eligible || !voice || !config.enabled) {
      End("non_eligible_intent");
      return;
    }
    if (_state != State::AwaitingClaim) {
      End("superseded");
      _turn = 1;
      _admissionDeadline = now + config.sessionTimeout_sec;
    }
    ++_token;
    _activation = activation;
    _outcome = Outcome::Cancelled;
    _state = State::Responding;
  }

  void ResponseFinished(uint64_t token, Outcome outcome) {
    if (_state == State::Responding && token == _token) {
      _outcome = outcome;
    }
  }

  void Deactivated(size_t activation, double now) {
    if (_state != State::Responding || activation != _activation) { return; }
    if (_outcome != Outcome::Succeeded) { End("response_not_successful"); return; }
    if (!CanAdmit(now)) { End("admission_limit"); return; }
    _state = State::Quiescing;
    _deadline = now + 2;
  }

  void Quiesced(double now) {
    if (_state != State::Quiescing) { return; }
    _state = State::Settling;
    _settledAt = now + config.followUpSettleTime_ms / 1000;
    _deadline = _settledAt + 2;
  }

  bool Ready(double now) const {
    return _state == State::Settling && now >= _settledAt &&
           now < _deadline && CanAdmit(now);
  }

  bool Open(double now) {
    if (!Ready(now)) { return false; }
    ++_turn;
    ++_token;
    _state = State::Opening;
    _deadline = now + 5;
    return true;
  }

  void CaptureOpened(double now) {
    if (_state == State::Opening) {
      _state = State::Listening;
      // KG cloud-audio requests allow 60s, including capture and answer wait.
      // Keep the 65s engine guard even when cloud audio is disabled (the
      // transport's shorter 9s error still terminates that request).
      _deadline = now + 65;
    }
  }

  void Result(bool eligible, double now) {
    if (_state != State::Opening && _state != State::Listening) { return; }
    if (now >= _deadline) { End("late_result"); return; }
    if (!eligible) { End("non_eligible_result"); return; }
    _state = State::AwaitingClaim;
    _deadline = now + 2;
  }
  bool CanReceiveResult(double now) const {
    return (_state == State::Opening || _state == State::Listening) && now < _deadline;
  }

  void Update(double now, bool safe) {
    if (!Active()) { return; }
    if (!config.enabled) { End("disabled"); return; }
    if (!safe) { End("safety_or_superseded"); return; }
    if ((_state == State::Quiescing || _state == State::Settling) && !CanAdmit(now)) {
      End("admission_limit");
    } else if (_state != State::Responding && now >= _deadline) {
      End(_state == State::Opening ? "opening_timeout" :
          _state == State::Listening ? "result_timeout" : "activation_timeout");
    }
  }

private:
  bool CanAdmit(double now) const {
    return config.enabled && _turn < config.maxTurns && now < _admissionDeadline;
  }
  State _state = State::Inactive;
  Outcome _outcome = Outcome::Cancelled;
  uint64_t _token = 0;
  size_t _activation = 0;
  unsigned _turn = 0;
  double _admissionDeadline = 0;
  double _deadline = 0;
  double _settledAt = 0;
  const char* _reason = "initial";
};

}
}
