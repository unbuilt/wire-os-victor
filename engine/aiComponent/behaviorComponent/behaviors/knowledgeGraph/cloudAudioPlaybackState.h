#ifndef __Vector_CloudAudioPlaybackState_H__
#define __Vector_CloudAudioPlaybackState_H__

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace Anki {
namespace Vector {

// Includes audio in transit to anim, not just audio it has acknowledged.
class CloudAudioPlaybackState
{
public:
  void Sent(size_t bytes) { _bytesSent += bytes; }

  bool UpdateProgress(uint32_t receivedFrames, uint32_t playedFrames)
  {
    if (playedFrames > receivedFrames || receivedFrames > _bytesSent / 2 ||
        playedFrames < _framesPlayed) {
      return false;
    }
    _framesPlayed = playedFrames;
    return true;
  }

  size_t BytesSent() const { return _bytesSent; }
  uint32_t FramesPlayed() const { return _framesPlayed; }
  size_t PendingBytes() const { return _bytesSent - static_cast<size_t>(_framesPlayed) * 2; }

  size_t SendBudget(size_t maxPendingBytes) const
  {
    return maxPendingBytes - std::min(maxPendingBytes, PendingBytes());
  }

private:
  size_t _bytesSent = 0;
  uint32_t _framesPlayed = 0;
};

} // namespace Vector
} // namespace Anki

#endif
