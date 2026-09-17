#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <openmedia/format_api.hpp> // Rational

using namespace openmedia;
using SteadyClock = std::chrono::steady_clock;
using TimePoint = std::chrono::time_point<SteadyClock>;

// ---------------------------------------------------------------------------
// AVClock
//
// Unified master clock for A/V sync. Two modes:
//   AUDIO – audio callback advances the clock; video slaves to it.
//   WALL  – wall-clock drives playback when there is no audio (video-only).
//
// All PTSes are stored in the track's native timebase units. Conversion to
// real seconds always goes through pts_to_seconds() so a wrong time_base
// never silently produces a wrong sync decision.
// ---------------------------------------------------------------------------
class AVClock {
public:
  enum class Mode { AUDIO,
                    WALL };

  AVClock() = default;

  // -----------------------------------------------------------------------
  // Configuration
  // -----------------------------------------------------------------------

  void setMode(Mode m) noexcept {
    mode_ = m;
  }

  Mode mode() const noexcept { return mode_; }

  // -----------------------------------------------------------------------
  // Reset / seek
  // -----------------------------------------------------------------------

  void reset(double seconds = 0.0) noexcept {
    pts_sec_.store(seconds, std::memory_order_release);
    wall_ref_pts_sec_ = seconds;
    wall_ref_time_ = SteadyClock::now();
    audio_update_ns_.store(0, std::memory_order_release);
    paused_ = false;
  }

  // Re-base WALL playback onto `seconds` starting now.
  //
  // Used twice: once when the first frame after a start/seek finally comes out
  // of the decoder (so playback begins at that frame instead of at whatever the
  // wall clock drifted to while the decoder was warming up), and again whenever
  // the decoder falls so far behind that catching up by dropping is hopeless.
  void setWallAnchor(double seconds) noexcept {
    wall_ref_pts_sec_ = seconds;
    wall_ref_time_ = SteadyClock::now();
    pts_sec_.store(seconds, std::memory_order_release);
    audio_update_ns_.store(0, std::memory_order_release);
  }

  void pause() noexcept {
    if (paused_) return;
    pts_sec_.store(masterSeconds(), std::memory_order_release);
    audio_update_ns_.store(0, std::memory_order_release);
    paused_ = true;
  }

  void resume() noexcept {
    if (!paused_) return;
    wall_ref_pts_sec_ = pts_sec_.load(std::memory_order_acquire);
    wall_ref_time_ = SteadyClock::now();
    audio_update_ns_.store(0, std::memory_order_release);
    paused_ = false;
  }

  // -----------------------------------------------------------------------
  // AUDIO mode: the audio callback reports the position playback has reached.
  //
  // It reports an absolute position rather than an increment on purpose. An
  // increment per callback accumulates whatever the sink mis-estimates, while
  // an absolute reading corrects itself on every callback.
  // -----------------------------------------------------------------------

  void setAudioSeconds(double seconds) noexcept {
    pts_sec_.store(seconds, std::memory_order_release);
    audio_update_ns_.store(nowNanos(), std::memory_order_release);
  }

  // -----------------------------------------------------------------------
  // WALL mode: call once per render loop.
  // -----------------------------------------------------------------------

  void wallTick() noexcept {
    if (mode_ != Mode::WALL || paused_) return;
    const auto now = SteadyClock::now();
    const double elapsed = std::chrono::duration<double>(now - wall_ref_time_).count();
    pts_sec_.store(wall_ref_pts_sec_ + elapsed, std::memory_order_release);
  }

  // -----------------------------------------------------------------------
  // Queries
  // -----------------------------------------------------------------------

  // Current master position in seconds.
  double masterSeconds() const noexcept {
    if (paused_) return pts_sec_.load(std::memory_order_acquire);

    if (mode_ == Mode::WALL) {
        const auto now = SteadyClock::now();
        const double elapsed = std::chrono::duration<double>(now - wall_ref_time_).count();
        return wall_ref_pts_sec_ + elapsed;
    }

    // AUDIO mode: the callback advances the clock one buffer at a time, so the
    // stored value is a staircase whose steps are longer than a video frame.
    // Comparing frame timestamps against it made the renderer pick frames a
    // refresh early or late at random, which is what the judder was. Fill in
    // the gap between callbacks from the wall clock; the step and the elapsed
    // time cancel out, so the result is continuous across each callback.
    const double base = pts_sec_.load(std::memory_order_acquire);
    const int64_t updated_ns = audio_update_ns_.load(std::memory_order_acquire);
    if (updated_ns == 0) return base;

    const double since = static_cast<double>(nowNanos() - updated_ns) / 1e9;
    // A stalled or starved audio thread must not let the clock run away.
    if (since < 0.0 || since > kMaxAudioExtrapolation) return base;
    return base + since;
  }

  bool paused() const noexcept { return paused_; }

private:
  // Seconds: how far past the last audio callback the clock may be
  // extrapolated before it is assumed the audio thread is not running.
  static constexpr double kMaxAudioExtrapolation = 0.25;

  static auto nowNanos() noexcept -> int64_t {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               SteadyClock::now().time_since_epoch())
        .count();
  }

  std::atomic<double> pts_sec_ {0.0};
  Mode mode_ {Mode::WALL};

  // Steady-clock reading at the last audio update, or 0 when there has not
  // been one since the last reset/seek/pause.
  std::atomic<int64_t> audio_update_ns_ {0};

  double wall_ref_pts_sec_ = 0.0;
  TimePoint wall_ref_time_ = SteadyClock::now();
  bool paused_ = false;
};

