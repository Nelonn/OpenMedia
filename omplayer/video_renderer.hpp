#pragma once

#include "av_clock.hpp"
#include "frame_queue.hpp"

#include <SDL3/SDL.h>
#include <mutex>

// VideoRenderer
//
// Consumes VideoFrames from a FrameQueue, compares each frame's pts_sec
// against the master AVClock, and uploads/displays when the frame is due.
//
// Frame pacing policy
// -------------------
//   diff = frame_pts_sec - clock_sec
//
//   diff >  FUTURE_THRESH  → frame is in the future; skip this tick
//   diff < -DROP_THRESH    → frame is very late; drop it, try next
//   otherwise              → display (the "just right" window)
//
// Only ONE frame is displayed per tick so the render loop controls pacing.
class VideoRenderer {
public:
  // Seconds: a frame more than this far ahead is held back.
  static constexpr double kFutureThresh = 0.010; // 10 ms
  // Seconds: a frame more than this far behind is dropped.
  static constexpr double kDropThresh = 0.100; // 100 ms

  ~VideoRenderer() { destroyTexture(); }

  void setRenderer(SDL_Renderer* r) { renderer_ = r; }

  // Called once per render-loop iteration from the main thread.
  // `clock` – the master clock this player uses.
  // Returns true if a new frame was uploaded (texture is dirty).
  auto tick(FrameQueue& queue, const AVClock& clock) -> bool {
    if (!renderer_) return false;

    const double master = clock.masterSeconds();
    bool uploaded = false;

    // Process frames until we either display one or run out of due frames.
    while (true) {
      // peekPop is a single lock acquisition: peek + conditional pop.
      // No TOCTOU gap between inspecting and consuming the front frame.
      auto opt = queue.peekPop([&](double pts_sec) {
        // Consume only if the frame is not too far in the future.
        return (pts_sec - master) <= kFutureThresh;
      });

      if (!opt) break; // queue empty or front frame is too early

      const double diff = opt->pts_sec - master;

      if (diff < -kDropThresh) {
        // Frame is very late – drop and try next.
        dropped_count_++;
        continue;
      }

      // Frame is on time (within [-kDropThresh, +kFutureThresh]).
      uploadFrame(*opt);
      last_pts_sec_ = opt->pts_sec;
      uploaded = true;
      break; // one frame per render tick
    }

    return uploaded;
  }

  auto texture() -> SDL_Texture* {
    std::lock_guard lock(mutex_);
    return texture_;
  }

  auto textureWidth() const -> uint32_t { return tex_w_; }
  auto textureHeight() const -> uint32_t { return tex_h_; }
  auto droppedCount() const -> uint64_t { return dropped_count_; }
  auto lastPtsSec() const -> double { return last_pts_sec_; }

  void reset() {
    std::lock_guard lock(mutex_);
    destroyTextureUnsafe();
    last_pts_sec_ = 0.0;
    dropped_count_ = 0;
  }

private:
  void uploadFrame(const VideoFrame& vf) {
    std::lock_guard lock(mutex_);

    // Recreate texture only when dimensions change.
    if (!texture_ || tex_w_ != vf.width || tex_h_ != vf.height) {
      destroyTextureUnsafe();
      texture_ = SDL_CreateTexture(
          renderer_,
          SDL_PIXELFORMAT_IYUV,
          SDL_TEXTUREACCESS_STREAMING,
          static_cast<int>(vf.width),
          static_cast<int>(vf.height));
      tex_w_ = vf.width;
      tex_h_ = vf.height;
    }

    if (texture_) {
      SDL_UpdateYUVTexture(
          texture_, nullptr,
          vf.y_plane.data(), vf.y_stride,
          vf.u_plane.data(), vf.u_stride,
          vf.v_plane.data(), vf.v_stride);
    }
  }

  void destroyTexture() {
    std::lock_guard lock(mutex_);
    destroyTextureUnsafe();
  }

  void destroyTextureUnsafe() {
    if (texture_) {
      SDL_DestroyTexture(texture_);
      texture_ = nullptr;
    }
    tex_w_ = tex_h_ = 0;
  }

  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* texture_ = nullptr;
  mutable std::mutex mutex_;

  uint32_t tex_w_ = 0;
  uint32_t tex_h_ = 0;
  uint64_t dropped_count_ = 0;
  double last_pts_sec_ = 0.0;
};
