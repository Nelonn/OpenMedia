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

  void setTimeBase(Rational tb) noexcept {
    time_base_ = tb;
  }

  void setMode(Mode m) noexcept {
    mode_ = m;
  }

  Mode mode() const noexcept { return mode_; }

  // -----------------------------------------------------------------------
  // Reset / seek
  // -----------------------------------------------------------------------

  void reset(int64_t pts = 0) noexcept {
    pts_.store(pts, std::memory_order_release);
    wall_ref_pts_ = pts;
    wall_ref_time_ = SteadyClock::now();
    paused_ = false;
  }

  void pause() noexcept {
    if (paused_) return;
    // Snapshot the current pts so we can resume from the right place.
    pts_.store(masterPts(), std::memory_order_release);
    paused_ = true;
  }

  void resume() noexcept {
    if (!paused_) return;
    wall_ref_pts_ = pts_.load(std::memory_order_acquire);
    wall_ref_time_ = SteadyClock::now();
    paused_ = false;
  }

  // -----------------------------------------------------------------------
  // AUDIO mode: audio callback reports how many samples it consumed.
  // -----------------------------------------------------------------------

  // Called from the SDL audio callback (lock-free path).
  // `samples_consumed` – number of PCM frames (not bytes) consumed.
  // `sample_rate`      – Hz.
  void audioAdvance(int64_t samples_consumed, int sample_rate) noexcept {
    if (sample_rate <= 0) return;
    // Convert sample count → seconds → PTS units
    const double secs = static_cast<double>(samples_consumed) / sample_rate;
    const int64_t delta = secondsToPts(secs);
    pts_.fetch_add(delta, std::memory_order_relaxed);
  }

  // Hard-set from the decoder side (e.g. after seek).
  void setAudioPts(int64_t pts) noexcept {
    pts_.store(pts, std::memory_order_release);
  }

  // -----------------------------------------------------------------------
  // WALL mode: call once per render loop.
  // -----------------------------------------------------------------------

  void wallTick() noexcept {
    if (mode_ != Mode::WALL || paused_) return;
    const auto now = SteadyClock::now();
    const double secs = std::chrono::duration<double>(now - wall_ref_time_).count();
    pts_.store(wall_ref_pts_ + secondsToPts(secs), std::memory_order_release);
  }

  // -----------------------------------------------------------------------
  // Queries
  // -----------------------------------------------------------------------

  // Current master PTS in stream timebase units.
  int64_t masterPts() const noexcept {
    return pts_.load(std::memory_order_acquire);
  }

  // Current master position in seconds.
  double masterSeconds() const noexcept {
    return ptsToSeconds(masterPts());
  }

  // Convert PTS (in this stream's timebase) to seconds.
  double ptsToSeconds(int64_t pts) const noexcept {
    if (time_base_.den == 0) return 0.0;
    return static_cast<double>(pts) *
           static_cast<double>(time_base_.num) /
           static_cast<double>(time_base_.den);
  }

  // Convert seconds to PTS units (rounded).
  int64_t secondsToPts(double secs) const noexcept {
    if (time_base_.num == 0) return 0;
    return static_cast<int64_t>(
        secs * static_cast<double>(time_base_.den) /
        static_cast<double>(time_base_.num));
  }

  Rational timeBase() const noexcept { return time_base_; }

private:
  std::atomic<int64_t> pts_ {0};
  Rational time_base_ {1, 90000}; // sane default: 90 kHz
  Mode mode_ {Mode::WALL};

  // Wall-clock anchor (used in WALL mode and to serve paused snapshots)
  int64_t wall_ref_pts_ = 0;
  TimePoint wall_ref_time_ = SteadyClock::now();
  bool paused_ = false;
};
