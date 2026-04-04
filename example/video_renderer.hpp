#pragma once

#include "av_clock.hpp"
#include "frame_queue.hpp"

#include <SDL3/SDL.h>
#include <mutex>

// Helper: check if a format is high bit depth (10/12/16)
static inline auto isHighBitDepth(uint8_t bits) -> bool {
    return bits > 8;
}

// Helper: determine SDL pixel format based on bit depth
// SDL3 doesn't have native 10/12/16-bit YUV formats, so we use IYUV for 8-bit
// and fall back to RGBA8888 for higher bit depths with manual conversion
static inline auto getSdlPixelFormat(uint8_t bits) -> SDL_PixelFormat {
    if (bits <= 8) return SDL_PIXELFORMAT_IYUV;
    return SDL_PIXELFORMAT_RGBA8888; // We'll convert to RGBA manually
}

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

    const SDL_PixelFormat sdl_fmt = getSdlPixelFormat(vf.bits_per_component);
    const bool need_recreate = !texture_ || tex_w_ != vf.width || 
                               tex_h_ != vf.height || tex_fmt_ != sdl_fmt;

    // Recreate texture when dimensions or format change.
    if (need_recreate) {
      destroyTextureUnsafe();
      texture_ = SDL_CreateTexture(
          renderer_,
          sdl_fmt,
          SDL_TEXTUREACCESS_STREAMING,
          static_cast<int>(vf.width),
          static_cast<int>(vf.height));
      tex_w_ = vf.width;
      tex_h_ = vf.height;
      tex_fmt_ = sdl_fmt;
    }

    if (!texture_) return;

    if (vf.bits_per_component <= 8) {
      // Standard 8-bit YUV420P - use SDL's native YUV upload
      SDL_UpdateYUVTexture(
          texture_, nullptr,
          vf.y_plane.data(), vf.y_stride,
          vf.u_plane.data(), vf.u_stride,
          vf.v_plane.data(), vf.v_stride);
    } else {
      // High bit depth (10/12/16) - convert to RGBA8888 manually
      uploadHighBitDepthFrame(vf);
    }
  }

  // Convert high bit depth YUV420P to RGBA8888
  void uploadHighBitDepthFrame(const VideoFrame& vf) {
    const uint32_t w = vf.width;
    const uint32_t h = vf.height;
    std::vector<uint32_t> rgba(w * h);

    // Helper: clamp value to 8-bit range with proper scaling
    auto yuvToRgba = [&](uint16_t y, uint16_t u, uint16_t v) -> uint32_t {
      // Normalize to 8-bit range based on actual bit depth
      const float scale = 255.0f / static_cast<float>((1 << vf.bits_per_component) - 1);
      const int yi = static_cast<int>(y * scale);
      const int ui = static_cast<int>(u * scale) - 128;
      const int vi = static_cast<int>(v * scale) - 128;

      // BT.709 YUV to RGB conversion
      const int r = std::clamp(yi + static_cast<int>(1.5748f * vi), 0, 255);
      const int g = std::clamp(yi - static_cast<int>(0.1873f * ui) - static_cast<int>(0.4681f * vi), 0, 255);
      const int b = std::clamp(yi + static_cast<int>(1.8556f * ui), 0, 255);

      return (0xFFu << 24) | (uint32_t(b) << 16) | (uint32_t(g) << 8) | uint32_t(r);
    };

    // Read 16-bit values (data is stored as uint16_t internally)
    const auto* y_data = reinterpret_cast<const uint16_t*>(vf.y_plane.data());
    const auto* u_data = reinterpret_cast<const uint16_t*>(vf.u_plane.data());
    const auto* v_data = reinterpret_cast<const uint16_t*>(vf.v_plane.data());

    const int uv_h = (h + 1) / 2;

    for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
        // Y plane: row y, stride in uint16_t units
        const uint16_t y_val = y_data[y * (vf.y_stride / 2) + x];
        
        // UV planes: subsampled 2x1, so use (y/2, x/2)
        const uint32_t uv_y = y / 2;
        const uint32_t uv_x = x / 2;
        const uint16_t u_val = u_data[uv_y * (vf.u_stride / 2) + uv_x];
        const uint16_t v_val = v_data[uv_y * (vf.v_stride / 2) + uv_x];

        rgba[y * w + x] = yuvToRgba(y_val, u_val, v_val);
      }
    }

    SDL_UpdateTexture(texture_, nullptr, rgba.data(), static_cast<int>(w * sizeof(uint32_t)));
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
    tex_fmt_ = 0;
  }

  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* texture_ = nullptr;
  mutable std::mutex mutex_;

  uint32_t tex_w_ = 0;
  uint32_t tex_h_ = 0;
  uint32_t tex_fmt_ = 0;
  uint64_t dropped_count_ = 0;
  double last_pts_sec_ = 0.0;
};
