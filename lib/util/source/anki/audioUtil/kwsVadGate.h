#ifndef ANKI_AUDIO_UTIL_KWS_VAD_GATE_H
#define ANKI_AUDIO_UTIL_KWS_VAD_GATE_H

#include <array>
#include <cstddef>
#include <deque>
#include <stdexcept>

namespace Anki {
namespace AudioUtil {

// Speech decisions are supplied by the existing VAD, not inferred by this gate.
// Sink callbacks run synchronously: Start(offset), Audio(data, size, availableAt),
// and End(). Each Start must create a fresh stream, including feature state.
class KwsVadGate
{
public:
  static constexpr size_t BlockSamples = 160;

  KwsVadGate(size_t preRollBlocks, size_t hangoverBlocks)
    : _preRollBlocks(preRollBlocks), _hangoverBlocks(hangoverBlocks)
  {
    if (hangoverBlocks == 0) {
      throw std::invalid_argument("VAD hangover must be positive");
    }
  }

  template<class Sink>
  void Push(const float* samples, size_t count, bool speech, Sink& sink)
  {
    if (!samples || count == 0 || count > BlockSamples) {
      throw std::invalid_argument("Expected 1..160 audio samples");
    }
    Block block{};
    block.offset = _received;
    block.count = count;
    for (size_t i = 0; i < count; ++i) {
      block.samples[i] = samples[i];
    }
    _received += count;
    if (!_active && !speech) {
      _history.push_back(block);
      if (_history.size() > _preRollBlocks) {
        _history.pop_front();
      }
      return;
    }
    if (!_active) {
      sink.Start(_history.empty() ? block.offset : _history.front().offset);
      _active = true;
      for (const auto& previous : _history) {
        sink.Audio(previous.samples.data(), previous.count, _received);
      }
      _history.clear();
    }
    sink.Audio(block.samples.data(), block.count, _received);
    _quietBlocks = speech ? 0 : _quietBlocks + 1;
    if (_quietBlocks >= _hangoverBlocks) {
      Finish(sink);
    }
  }

  template<class Sink>
  void Finish(Sink& sink)
  {
    if (_active) {
      sink.End();
      _active = false;
    }
    _quietBlocks = 0;
    _history.clear();
  }

private:
  struct Block {
    std::array<float, BlockSamples> samples;
    size_t count;
    size_t offset;
  };
  size_t _preRollBlocks;
  size_t _hangoverBlocks;
  size_t _received = 0;
  size_t _quietBlocks = 0;
  bool _active = false;
  std::deque<Block> _history;
};

} // namespace AudioUtil
} // namespace Anki
#endif
