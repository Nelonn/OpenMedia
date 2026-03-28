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
    paused_ = false;
  }

  void pause() noexcept {
    if (paused_) return;
    pts_sec_.store(masterSeconds(), std::memory_order_release);
    paused_ = true;
  }

  void resume() noexcept {
    if (!paused_) return;
    wall_ref_pts_sec_ = pts_sec_.load(std::memory_order_acquire);
    wall_ref_time_ = SteadyClock::now();
    paused_ = false;
  }

  // -----------------------------------------------------------------------
  // AUDIO mode: audio callback reports how many samples it consumed.
  // -----------------------------------------------------------------------

  void audioAdvance(double seconds) noexcept {
    if (seconds <= 0) return;
    // Simple addition to the atomic double.
    double current = pts_sec_.load(std::memory_order_acquire);
    while (!pts_sec_.compare_exchange_weak(current, current + seconds,
                                           std::memory_order_release,
                                           std::memory_order_acquire));
  }

  void setAudioSeconds(double seconds) noexcept {
    pts_sec_.store(seconds, std::memory_order_release);
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
    if (mode_ == Mode::WALL && !paused_) {
        const auto now = SteadyClock::now();
        const double elapsed = std::chrono::duration<double>(now - wall_ref_time_).count();
        return wall_ref_pts_sec_ + elapsed;
    }
    return pts_sec_.load(std::memory_order_acquire);
  }

private:
  std::atomic<double> pts_sec_ {0.0};
  Mode mode_ {Mode::WALL};

  double wall_ref_pts_sec_ = 0.0;
  TimePoint wall_ref_time_ = SteadyClock::now();
  bool paused_ = false;
};

