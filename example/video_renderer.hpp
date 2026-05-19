#pragma once

#include "av_clock.hpp"
#include "frame_queue.hpp"

#include <SDL3/SDL.h>
#include <mutex>

#ifndef __APPLE__
#include <openmedia/hw_vulkan.h>
#endif

// Helper: check if a format is high bit depth (10/12/16)
static inline auto isHighBitDepth(uint8_t bits) -> bool {
    return bits > 8;
}

// Helper: determine SDL pixel format based on bit depth
// SDL3 doesn't have native 10/12/16-bit YUV formats, so we use IYUV for 8-bit
// and fall back to RGBA8888 for higher bit depths with manual conversion
static inline auto getSdlPixelFormat(uint32_t fmt, uint8_t bits) -> SDL_PixelFormat {
    if (bits > 8) return SDL_PIXELFORMAT_RGBA8888;
    const auto format = static_cast<OMPixelFormat>(fmt);
    if (format == OM_FORMAT_NV12) return SDL_PIXELFORMAT_NV12;
    return SDL_PIXELFORMAT_IYUV;
}

static inline auto isNativeSdlYuv(uint32_t fmt) -> bool {
    const auto format = static_cast<OMPixelFormat>(fmt);
    return format == OM_FORMAT_YUV420P ||
           format == OM_FORMAT_YUVJ420P ||
           format == OM_FORMAT_NV12;
}

// VideoRenderer
//
// Consumes VideoFrames from a FrameQueue, compares each frame's pts_sec
// against the master AVClock, and uploads/displays when the frame is due.
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
      auto opt = queue.peekPop([&](double pts_sec) {
        return (pts_sec - master) <= kFutureThresh;
      });

      if (!opt) break; 

      const double diff = opt->pts_sec - master;

      if (diff < -kDropThresh) {
        dropped_count_++;
        continue;
      }

      uploadFrame(*opt);
      last_pts_sec_ = opt->pts_sec;
      uploaded = true;
      break; 
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

    if (vf.hw_picture && vf.hw_picture->getType() == openmedia::HWDeviceType::VULKAN) {
      // Detection success! In a real player, we'd use a Vulkan-aware renderer.
      // For this example, if we have software data available, use it.
      if (vf.y_plane.empty()) return; 
    }

    const SDL_PixelFormat sdl_fmt = getSdlPixelFormat(vf.pixel_format, vf.bits_per_component);
    const bool need_recreate = !texture_ || tex_w_ != vf.width || 
                               tex_h_ != vf.height || tex_fmt_ != sdl_fmt;

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

    if (vf.bits_per_component <= 8 && isNativeSdlYuv(vf.pixel_format)) {
      if (static_cast<OMPixelFormat>(vf.pixel_format) == OM_FORMAT_NV12) {
        SDL_UpdateNVTexture(
            texture_, nullptr,
            vf.y_plane.data(), vf.y_stride,
            vf.u_plane.data(), vf.u_stride);
      } else {
        SDL_UpdateYUVTexture(
            texture_, nullptr,
            vf.y_plane.data(), vf.y_stride,
            vf.u_plane.data(), vf.u_stride,
            vf.v_plane.data(), vf.v_stride);
      }
    } else {
      uploadHighBitDepthFrame(vf);
    }
  }

  void uploadHighBitDepthFrame(const VideoFrame& vf) {
    const uint32_t w = vf.width;
    const uint32_t h = vf.height;
    std::vector<uint32_t> rgba(w * h);
    const auto format = static_cast<OMPixelFormat>(vf.pixel_format);
    const bool semi_planar = (getNumPlanes(format) == 2);

    auto yuvToRgba = [&](uint16_t y, uint16_t u, uint16_t v) -> uint32_t {
      // P010/P012/P016 store valid bits in the MSBs. Convert to 8-bit before
      // applying the simple BT.601-ish display matrix below.
      const int yi = y >> 8;
      const int ui = (u >> 8) - 128;
      const int vi = (v >> 8) - 128;

      const int r = std::clamp(yi + static_cast<int>(1.402f * vi), 0, 255);
      const int g = std::clamp(yi - static_cast<int>(0.344f * ui + 0.714f * vi), 0, 255);
      const int b = std::clamp(yi + static_cast<int>(1.772f * ui), 0, 255);
      return (0xFFu << 24) | (uint32_t(b) << 16) | (uint32_t(g) << 8) | uint32_t(r);
    };

    const auto* y_data = reinterpret_cast<const uint16_t*>(vf.y_plane.data());
    const auto* uv_data = semi_planar ? reinterpret_cast<const uint16_t*>(vf.u_plane.data()) : nullptr;
    const auto* u_data = !semi_planar ? reinterpret_cast<const uint16_t*>(vf.u_plane.data()) : nullptr;
    const auto* v_data = !semi_planar ? reinterpret_cast<const uint16_t*>(vf.v_plane.data()) : nullptr;

    for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
        const uint16_t y_val = y_data[y * (vf.y_stride / 2) + x];
        const uint32_t uv_x = x / 2;
        const uint32_t uv_y = y / 2;
        
        uint16_t u_val, v_val;
        if (semi_planar) {
            u_val = uv_data[uv_y * (vf.u_stride / 2) + uv_x * 2];
            v_val = uv_data[uv_y * (vf.u_stride / 2) + uv_x * 2 + 1];
        } else {
            u_val = u_data[uv_y * (vf.u_stride / 2) + uv_x];
            v_val = v_data[uv_y * (vf.v_stride / 2) + uv_x];
        }
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
