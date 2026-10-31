#pragma once

#include <algorithm>
#include <chrono>
#include <mutex>

// position = anchor.pts + (now - anchor.time). The audio sink re-anchors on every callback, so
// the position is continuous rather than a staircase of buffers. Without audio, the renderer
// anchors to its first frame; once audio has anchored, video can no longer move it.
class AVClock {
public:
  using Clock = std::chrono::steady_clock;

  // Forgets every anchor; the clock holds `pts` until something anchors it.
  void reset(double pts) {
    std::lock_guard lock(mutex_);
    anchor_ = {pts, Clock::now()};
    started_ = audio_driven_ = paused_ = false;
  }

  // Audio callback: `pts` is leaving the speaker right now.
  void syncToAudio(double pts) {
    std::lock_guard lock(mutex_);
    if (paused_) return;
    anchor_ = {pts, Clock::now()};
    started_ = audio_driven_ = true;
  }

  // Video: start (or, when the decoder cannot keep up, restart) the wall clock
  // from this frame. Ignored once audio is the master.
  void syncToVideo(double pts) {
    std::lock_guard lock(mutex_);
    if (paused_ || audio_driven_) return;
    anchor_ = {pts, Clock::now()};
    started_ = true;
  }

  void pause() {
    std::lock_guard lock(mutex_);
    anchor_.pts = positionLocked();
    paused_ = true;
  }

  auto now() const -> double {
    std::lock_guard lock(mutex_);
    return positionLocked();
  }

  auto started() const -> bool {
    std::lock_guard lock(mutex_);
    return started_;
  }

private:
  // How far past the last audio callback the clock may run before it assumes
  // the audio has stalled and waits for it.
  static constexpr double kMaxAudioExtrapolation = 0.25;

  struct Anchor {
    double pts = 0.0;
    Clock::time_point time = Clock::now();
  };

  auto positionLocked() const -> double {
    if (!started_ || paused_) return anchor_.pts;
    const double elapsed = std::chrono::duration<double>(Clock::now() - anchor_.time).count();
    return anchor_.pts + (audio_driven_ ? std::min(elapsed, kMaxAudioExtrapolation) : elapsed);
  }

  mutable std::mutex mutex_;
  Anchor anchor_;
  bool started_ = false;
  bool audio_driven_ = false;
  bool paused_ = false;
};
