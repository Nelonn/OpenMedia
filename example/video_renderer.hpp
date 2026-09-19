#pragma once

#include <SDL3/SDL.h>

#include "av_clock.hpp"
#include "frame_queue.hpp"
#include <algorithm>
#include <mutex>
#include <vector>
#include <libyuv.h>

// ---------------------------------------------------------------------------
// Upload planning
//
// SDL can sample 8-bit 4:2:0 YUV and packed 32-bit RGB directly on the GPU.
// Everything else — 4:2:2, 4:4:4, grayscale and every 10/12/16-bit format —
// goes through libyuv and is uploaded as ARGB8888.
// ---------------------------------------------------------------------------
struct UploadPlan {
    enum class Kind {
        Planar,     // three planes, SDL_UpdateYUVTexture
        SemiPlanar, // Y + interleaved UV, SDL_UpdateNVTexture
        Packed,     // single 32-bit plane, SDL_UpdateTexture
        Convert,    // libyuv → ARGB8888
    };

    SDL_PixelFormat format = SDL_PIXELFORMAT_ARGB8888;
    Kind kind = Kind::Convert;
};

static inline auto planUpload(uint32_t fmt, uint8_t bits) -> UploadPlan {
    if (bits <= 8) {
        switch (static_cast<OMPixelFormat>(fmt)) {
            case OM_FORMAT_NV12:
                return {SDL_PIXELFORMAT_NV12, UploadPlan::Kind::SemiPlanar};
            case OM_FORMAT_NV21:
                return {SDL_PIXELFORMAT_NV21, UploadPlan::Kind::SemiPlanar};
            case OM_FORMAT_YUV420P:
            case OM_FORMAT_YUVJ420P:
                return {SDL_PIXELFORMAT_IYUV, UploadPlan::Kind::Planar};
            // SDL names packed formats by their 32-bit word, OpenMedia by byte
            // order, so the two labels look swapped on little-endian hosts.
            case OM_FORMAT_R8G8B8A8:
                return {SDL_PIXELFORMAT_ABGR8888, UploadPlan::Kind::Packed};
            case OM_FORMAT_B8G8R8A8:
                return {SDL_PIXELFORMAT_ARGB8888, UploadPlan::Kind::Packed};
            default:
                break;
        }
    }
    return {SDL_PIXELFORMAT_ARGB8888, UploadPlan::Kind::Convert};
}

// The J-variants carry their full-range-ness in the pixel format rather than in
// the colour range field, so fold that in before picking a matrix.
static inline auto isFullRange(const VideoFrame& vf) -> bool {
    switch (static_cast<OMPixelFormat>(vf.pixel_format)) {
        case OM_FORMAT_YUVJ420P:
        case OM_FORMAT_YUVJ422P:
        case OM_FORMAT_YUVJ444P:
            return true;
        default:
            return vf.color_range == OM_COLOR_RANGE_FULL;
    }
}

static inline auto effectiveColorSpace(OMColorSpace space, uint32_t height) -> OMColorSpace {
    // Unspecified colour space: use the same heuristic every player uses —
    // SD content is BT.601, HD and above is BT.709.
    if (space == OM_COLOR_SPACE_UNKNOWN)
        return (height > 576) ? OM_COLOR_SPACE_BT709 : OM_COLOR_SPACE_BT601;
    return space;
}

// Map the decoded picture's colorimetry onto an SDL colorspace.
//
// This matters more than it looks: SDL_CreateTexture() defaults YUV textures to
// SDL_COLORSPACE_JPEG (BT.601, *full* range). Virtually all camera/streaming
// video is BT.709 limited range, so leaving the default in place renders every
// clip with wrong hue and crushed contrast.
static inline auto toSdlColorspace(OMColorSpace space, bool full, uint32_t height)
    -> SDL_Colorspace {
    switch (effectiveColorSpace(space, height)) {
        case OM_COLOR_SPACE_BT2020:
        case OM_COLOR_SPACE_BT2020_CL:
            return full ? SDL_COLORSPACE_BT2020_FULL : SDL_COLORSPACE_BT2020_LIMITED;
        case OM_COLOR_SPACE_BT601:
        case OM_COLOR_SPACE_SMPTE240M:
        case OM_COLOR_SPACE_FCC:
            return full ? SDL_COLORSPACE_BT601_FULL : SDL_COLORSPACE_BT601_LIMITED;
        default:
            return full ? SDL_COLORSPACE_BT709_FULL : SDL_COLORSPACE_BT709_LIMITED;
    }
}

// The libyuv equivalent of the above, for the CPU conversion path.
static inline auto toYuvConstants(OMColorSpace space, bool full, uint32_t height)
    -> const libyuv::YuvConstants* {
    switch (effectiveColorSpace(space, height)) {
        case OM_COLOR_SPACE_BT2020:
        case OM_COLOR_SPACE_BT2020_CL:
            return full ? &libyuv::kYuvV2020Constants : &libyuv::kYuv2020Constants;
        case OM_COLOR_SPACE_BT601:
        case OM_COLOR_SPACE_SMPTE240M:
        case OM_COLOR_SPACE_FCC:
            return full ? &libyuv::kYuvJPEGConstants : &libyuv::kYuvI601Constants;
        default:
            return full ? &libyuv::kYuvF709Constants : &libyuv::kYuvH709Constants;
    }
}

// VideoRenderer
//
// Consumes VideoFrames from a FrameQueue, compares each frame's pts_sec
// against the master AVClock, and uploads/displays when the frame is due.
class VideoRenderer {
public:
  // Fallback presentation interval when the display refresh rate is unknown.
  static constexpr double kFallbackRefresh = 1.0 / 60.0;
  // How many ticks between re-reads of the refresh rate. The window can be
  // dragged to a display with a different one, but not often enough to justify
  // asking SDL every frame.
  static constexpr int kRefreshPollTicks = 120;
  // Seconds: a frame more than this far behind is dropped to catch up.
  static constexpr double kDropThresh = 0.100; // 100 ms
  // Seconds: past this the decoder simply cannot keep up with real time, so
  // dropping would just blank the window. Re-base the wall clock instead and
  // play at whatever rate the decoder manages.
  static constexpr double kResyncThresh = 0.500; // 500 ms

  ~VideoRenderer() { destroyTexture(); }

  void setRenderer(SDL_Renderer* r) { renderer_ = r; }

  // Called once per render-loop iteration from the main thread.
  // `clock` – the master clock this player uses.
  // Returns true if a new frame was uploaded (texture is dirty).
  auto tick(FrameQueue& queue, AVClock& clock) -> bool {
    if (!renderer_) return false;

    // First frame of a playback or of a seek: show it as soon as it exists and
    // start the clock from its timestamp. Comparing it against a clock that has
    // been running since before the decoder was even opened would classify it
    // as hopelessly late and drop it — together with everything behind it.
    if (!primed_) {
      auto first = queue.tryPop();
      if (!first) return false;
      if (clock.mode() == AVClock::Mode::WALL)
        clock.setWallAnchor(first->pts_sec);
      uploadFrame(*first);
      last_pts_sec_ = first->pts_sec;
      primed_ = true;
      return true;
    }

    // A frame uploaded now is not seen until the next present, one refresh
    // away, so that is the moment its timestamp has to be judged against. It is
    // due when that present is the nearest refresh to it — hence a look-ahead
    // of half an interval.
    //
    // The look-ahead has to be measured in refreshes rather than in a fixed
    // number of milliseconds. A 10 ms window is two whole refreshes on a 200 Hz
    // display, so whether a frame went up early or late depended on where the
    // clock happened to sit, and 60 fps came out as an irregular 3/4/5/6-
    // refresh stutter instead of a steady 3-3-4.
    const double refresh = refreshInterval();

    // Process frames until we either display one or run out of due frames.
    while (true) {
      const double master = clock.masterSeconds();
      const double present_at = master + refresh;

      auto opt = queue.peekPop([&](double pts_sec) {
        return (pts_sec - present_at) <= refresh * 0.5;
      });

      if (!opt) break;

      const double diff = opt->pts_sec - master;

      if (diff < -kResyncThresh && clock.mode() == AVClock::Mode::WALL) {
        // Video-only playback that cannot run at real time: slow the clock down
        // to the decoder rather than discarding every frame it produces.
        clock.setWallAnchor(opt->pts_sec);
      } else if (diff < -kDropThresh) {
        dropped_count_++;
        continue;
      }

      uploadFrame(*opt);
      last_pts_sec_ = opt->pts_sec;
      return true;
    }

    return false;
  }

  auto texture() -> SDL_Texture* {
    std::lock_guard lock(mutex_);
    return texture_;
  }

  auto textureWidth() const -> uint32_t { return tex_w_; }
  auto textureHeight() const -> uint32_t { return tex_h_; }
  auto droppedCount() const -> uint64_t { return dropped_count_; }
  auto lastPtsSec() const -> double { return last_pts_sec_; }

  // Forget sync state but keep the current texture on screen. Used on seek so
  // the window does not flash black while the decoder refills.
  void resetSync() {
    primed_ = false;
    last_pts_sec_ = 0.0;
  }

  void reset() {
    std::lock_guard lock(mutex_);
    destroyTextureUnsafe();
    primed_ = false;
    last_pts_sec_ = 0.0;
    dropped_count_ = 0;
    warned_format_ = 0;
  }

private:
  // Presentation interval of the display the window currently sits on.
  auto refreshInterval() -> double {
    if (refresh_interval_ == 0.0 || ++refresh_poll_ >= kRefreshPollTicks) {
      refresh_poll_ = 0;
      refresh_interval_ = queryRefreshInterval();
    }
    return refresh_interval_;
  }

  auto queryRefreshInterval() const -> double {
    if (SDL_Window* window = SDL_GetRenderWindow(renderer_)) {
      const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
      const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
      if (mode && mode->refresh_rate > 0.0f) {
        return 1.0 / static_cast<double>(mode->refresh_rate);
      }
    }
    return kFallbackRefresh;
  }

  void uploadFrame(const VideoFrame& vf) {
    std::lock_guard lock(mutex_);

    if (vf.y_plane.empty() || vf.width == 0 || vf.height == 0) return;

    const UploadPlan plan = planUpload(vf.pixel_format, vf.bits_per_component);
    const SDL_Colorspace colorspace =
        (plan.kind == UploadPlan::Kind::Planar || plan.kind == UploadPlan::Kind::SemiPlanar)
            ? toSdlColorspace(vf.color_space, isFullRange(vf), vf.height)
            : SDL_COLORSPACE_SRGB; // packed RGB, or converted to RGB below

    const bool need_recreate = !texture_ || tex_w_ != vf.width ||
                               tex_h_ != vf.height || tex_fmt_ != plan.format ||
                               tex_colorspace_ != colorspace;

    if (need_recreate) {
      destroyTextureUnsafe();
      texture_ = createTexture(plan.format, colorspace, vf.width, vf.height);
      if (!texture_) {
        SDL_Log("[Renderer] SDL_CreateTexture failed: %s", SDL_GetError());
        return;
      }
      SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_LINEAR);
      tex_w_ = vf.width;
      tex_h_ = vf.height;
      tex_fmt_ = plan.format;
      tex_colorspace_ = colorspace;
    }

    switch (plan.kind) {
      case UploadPlan::Kind::SemiPlanar:
        SDL_UpdateNVTexture(
            texture_, nullptr,
            vf.y_plane.data(), vf.y_stride,
            vf.u_plane.data(), vf.u_stride);
        break;

      case UploadPlan::Kind::Planar:
        SDL_UpdateYUVTexture(
            texture_, nullptr,
            vf.y_plane.data(), vf.y_stride,
            vf.u_plane.data(), vf.u_stride,
            vf.v_plane.data(), vf.v_stride);
        break;

      case UploadPlan::Kind::Packed:
        SDL_UpdateTexture(texture_, nullptr, vf.y_plane.data(), vf.y_stride);
        break;

      case UploadPlan::Kind::Convert:
        uploadConvertedFrame(vf);
        break;
    }
  }

  auto createTexture(SDL_PixelFormat fmt, SDL_Colorspace colorspace,
                     uint32_t w, uint32_t h) -> SDL_Texture* {
    SDL_PropertiesID props = SDL_CreateProperties();
    if (!props)
      return SDL_CreateTexture(renderer_, fmt, SDL_TEXTUREACCESS_STREAMING,
                               static_cast<int>(w), static_cast<int>(h));

    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, fmt);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
                          SDL_TEXTUREACCESS_STREAMING);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, static_cast<int>(w));
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, static_cast<int>(h));
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, colorspace);

    SDL_Texture* tex = SDL_CreateTextureWithProperties(renderer_, props);
    SDL_DestroyProperties(props);
    return tex;
  }

  // CPU fallback for anything SDL cannot sample directly, via libyuv.
  void uploadConvertedFrame(const VideoFrame& vf) {
    const int w = static_cast<int>(vf.width);
    const int h = static_cast<int>(vf.height);
    const int dst_stride = w * 4;

    argb_.resize(static_cast<size_t>(dst_stride) * h);
    if (!convertToArgb(vf, argb_.data(), dst_stride)) return;

    SDL_UpdateTexture(texture_, nullptr, argb_.data(), dst_stride);
  }

  auto convertToArgb(const VideoFrame& vf, uint8_t* dst, int dst_stride) -> bool {
    const int w = static_cast<int>(vf.width);
    const int h = static_cast<int>(vf.height);
    const auto format = static_cast<OMPixelFormat>(vf.pixel_format);
    const libyuv::YuvConstants* yuv =
        toYuvConstants(vf.color_space, isFullRange(vf), vf.height);

    // libyuv takes 16-bit plane strides in samples, not bytes.
    const auto* y16 = reinterpret_cast<const uint16_t*>(vf.y_plane.data());
    const auto* u16 = reinterpret_cast<const uint16_t*>(vf.u_plane.data());
    const auto* v16 = reinterpret_cast<const uint16_t*>(vf.v_plane.data());
    const int y16_stride = vf.y_stride / 2;
    const int u16_stride = vf.u_stride / 2;
    const int v16_stride = vf.v_stride / 2;

    switch (format) {
      // ---- 8-bit ----
      case OM_FORMAT_YUV422P:
      case OM_FORMAT_YUVJ422P:
        return libyuv::I422ToARGBMatrix(
                   vf.y_plane.data(), vf.y_stride,
                   vf.u_plane.data(), vf.u_stride,
                   vf.v_plane.data(), vf.v_stride,
                   dst, dst_stride, yuv, w, h) == 0;

      case OM_FORMAT_YUV444P:
      case OM_FORMAT_YUVJ444P:
        return libyuv::I444ToARGBMatrix(
                   vf.y_plane.data(), vf.y_stride,
                   vf.u_plane.data(), vf.u_stride,
                   vf.v_plane.data(), vf.v_stride,
                   dst, dst_stride, yuv, w, h) == 0;

      case OM_FORMAT_GRAY8:
        return libyuv::I400ToARGBMatrix(
                   vf.y_plane.data(), vf.y_stride,
                   dst, dst_stride, yuv, w, h) == 0;

      // Semi-planar 4:2:2 and 4:4:4, which hardware decoders hand back for
      // those chroma formats. libyuv has no NV16/NV24 entry point, so split the
      // interleaved chroma and reuse the planar kernels.
      case OM_FORMAT_NV16:
      case OM_FORMAT_NV24: {
        const bool full_width = (format == OM_FORMAT_NV24);
        const int cw = full_width ? w : (w + 1) / 2;
        splitChroma(vf.u_plane, vf.u_stride, cw, h);
        return (full_width
                    ? libyuv::I444ToARGBMatrix(vf.y_plane.data(), vf.y_stride,
                                               split_u_.data(), cw, split_v_.data(), cw,
                                               dst, dst_stride, yuv, w, h)
                    : libyuv::I422ToARGBMatrix(vf.y_plane.data(), vf.y_stride,
                                               split_u_.data(), cw, split_v_.data(), cw,
                                               dst, dst_stride, yuv, w, h)) == 0;
      }

      // ---- 10/12/16-bit semi-planar (valid bits in the MSBs) ----
      case OM_FORMAT_P010:
      case OM_FORMAT_P012:
      case OM_FORMAT_P016:
        return libyuv::P010ToARGBMatrix(
                   y16, y16_stride, u16, u16_stride,
                   dst, dst_stride, yuv, w, h) == 0;

      // ---- 10-bit planar (valid bits in the LSBs) ----
      case OM_FORMAT_YUV420P10:
        return libyuv::I010ToARGBMatrix(
                   y16, y16_stride, u16, u16_stride, v16, v16_stride,
                   dst, dst_stride, yuv, w, h) == 0;

      case OM_FORMAT_YUV420P12:
        return libyuv::I012ToARGBMatrix(
                   y16, y16_stride, u16, u16_stride, v16, v16_stride,
                   dst, dst_stride, yuv, w, h) == 0;

      case OM_FORMAT_YUV422P10:
        return libyuv::I210ToARGBMatrix(
                   y16, y16_stride, u16, u16_stride, v16, v16_stride,
                   dst, dst_stride, yuv, w, h) == 0;

      case OM_FORMAT_YUV444P10:
        return libyuv::I410ToARGBMatrix(
                   y16, y16_stride, u16, u16_stride, v16, v16_stride,
                   dst, dst_stride, yuv, w, h) == 0;

      // ---- remaining high bit depths: narrow to 10-bit, then as above ----
      case OM_FORMAT_YUV420P16:
      case OM_FORMAT_YUV422P12:
      case OM_FORMAT_YUV422P16:
      case OM_FORMAT_YUV444P12:
      case OM_FORMAT_YUV444P16:
        return convertNarrowedToArgb(vf, dst, dst_stride, yuv);

      default:
        if (warned_format_ != vf.pixel_format) {
          warned_format_ = vf.pixel_format;
          SDL_Log("[Renderer] Unsupported pixel format 0x%08X, nothing to display",
                  vf.pixel_format);
        }
        return false;
    }
  }

  // libyuv only ships 10- and 12-bit entry points, and only for a subset of the
  // chroma layouts. Everything else is shifted down into a 10-bit scratch copy
  // first so it can reuse the I010/I210/I410 kernels.
  auto convertNarrowedToArgb(const VideoFrame& vf, uint8_t* dst, int dst_stride,
                             const libyuv::YuvConstants* yuv) -> bool {
    const auto format = static_cast<OMPixelFormat>(vf.pixel_format);
    const int w = static_cast<int>(vf.width);
    const int h = static_cast<int>(vf.height);
    const int shift = static_cast<int>(vf.bits_per_component) - 10;
    if (shift < 0) return false;

    const auto chroma = openmedia::getPlaneDimensions(format, 1, vf.width, vf.height);
    const int cw = static_cast<int>(chroma.first);
    const int ch = static_cast<int>(chroma.second);
    if (cw <= 0 || ch <= 0) return false;

    narrowPlane(vf.y_plane, vf.y_stride, narrow_y_, w, h, shift);
    narrowPlane(vf.u_plane, vf.u_stride, narrow_u_, cw, ch, shift);
    narrowPlane(vf.v_plane, vf.v_stride, narrow_v_, cw, ch, shift);

    if (ch == h) {
      // Full-height chroma: 4:4:4 when it is also full width, else 4:2:2.
      return (cw == w
                  ? libyuv::I410ToARGBMatrix(narrow_y_.data(), w, narrow_u_.data(), cw,
                                             narrow_v_.data(), cw, dst, dst_stride,
                                             yuv, w, h)
                  : libyuv::I210ToARGBMatrix(narrow_y_.data(), w, narrow_u_.data(), cw,
                                             narrow_v_.data(), cw, dst, dst_stride,
                                             yuv, w, h)) == 0;
    }
    // 4:2:0
    return libyuv::I010ToARGBMatrix(narrow_y_.data(), w, narrow_u_.data(), cw,
                                    narrow_v_.data(), cw, dst, dst_stride,
                                    yuv, w, h) == 0;
  }

  // Splits interleaved UV into two tightly packed planes.
  void splitChroma(const std::vector<uint8_t>& src, int src_stride, int cw, int ch) {
    split_u_.assign(static_cast<size_t>(cw) * ch, 0);
    split_v_.assign(static_cast<size_t>(cw) * ch, 0);
    if (src.empty() || src_stride <= 0) return;
    for (int row = 0; row < ch; ++row) {
      const uint8_t* s = src.data() + static_cast<size_t>(row) * src_stride;
      uint8_t* du = split_u_.data() + static_cast<size_t>(row) * cw;
      uint8_t* dv = split_v_.data() + static_cast<size_t>(row) * cw;
      for (int col = 0; col < cw; ++col) {
        du[col] = s[col * 2 + 0];
        dv[col] = s[col * 2 + 1];
      }
    }
  }

  static void narrowPlane(const std::vector<uint8_t>& src, int src_stride_bytes,
                          std::vector<uint16_t>& dst, int w, int h, int shift) {
    dst.resize(static_cast<size_t>(w) * h);
    if (src.empty() || src_stride_bytes <= 0) {
      std::fill(dst.begin(), dst.end(), uint16_t(0));
      return;
    }
    for (int row = 0; row < h; ++row) {
      const auto* s = reinterpret_cast<const uint16_t*>(
          src.data() + static_cast<size_t>(row) * src_stride_bytes);
      uint16_t* d = dst.data() + static_cast<size_t>(row) * w;
      for (int col = 0; col < w; ++col) d[col] = uint16_t(s[col] >> shift);
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
    tex_fmt_ = SDL_PIXELFORMAT_UNKNOWN;
    tex_colorspace_ = SDL_COLORSPACE_UNKNOWN;
  }

  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* texture_ = nullptr;
  mutable std::mutex mutex_;

  uint32_t tex_w_ = 0;
  uint32_t tex_h_ = 0;
  SDL_PixelFormat tex_fmt_ = SDL_PIXELFORMAT_UNKNOWN;
  SDL_Colorspace tex_colorspace_ = SDL_COLORSPACE_UNKNOWN;
  uint64_t dropped_count_ = 0;
  double last_pts_sec_ = 0.0;
  bool primed_ = false;
  uint32_t warned_format_ = 0;

  // Cached display refresh interval, 0 until first queried.
  double refresh_interval_ = 0.0;
  int refresh_poll_ = 0;

  // Scratch for the libyuv conversion path.
  std::vector<uint8_t> argb_;
  std::vector<uint16_t> narrow_y_, narrow_u_, narrow_v_;
  std::vector<uint8_t> split_u_, split_v_;
};
