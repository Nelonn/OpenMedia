#pragma once

#include <SDL3/SDL.h>

#include "av_clock.hpp"
#include "blocking_queue.hpp"

#include <array>
#include <libyuv.h>
#include <openmedia/video.hpp>
#include <optional>
#include <vector>

// A decoded picture in host memory, ready for upload.
struct VideoFrame {
  struct Plane {
    std::vector<uint8_t> data;
    int stride = 0;
  };

  std::array<Plane, 3> planes;
  uint32_t width = 0;
  uint32_t height = 0;
  OMPixelFormat format = OM_FORMAT_UNKNOWN;
  OMColorSpace color_space = OM_COLOR_SPACE_UNKNOWN;
  OMColorRange color_range = OM_COLOR_RANGE_UNSPECIFIED;
  double pts = 0.0; // seconds
};

using FrameQueue = BlockingQueue<VideoFrame>;

inline auto bitDepth(OMPixelFormat format) -> int {
  switch (format) {
    case OM_FORMAT_YUV420P10: case OM_FORMAT_YUV422P10: case OM_FORMAT_YUV444P10:
    case OM_FORMAT_P010:
      return 10;
    case OM_FORMAT_YUV420P12: case OM_FORMAT_YUV422P12: case OM_FORMAT_YUV444P12:
    case OM_FORMAT_P012:
      return 12;
    case OM_FORMAT_YUV420P16: case OM_FORMAT_YUV422P16: case OM_FORMAT_YUV444P16:
    case OM_FORMAT_P016: case OM_FORMAT_GRAY16: case OM_FORMAT_RGBA64:
      return 16;
    default:
      return 8;
  }
}

namespace upload {

// SDL samples 8-bit 4:2:0 YUV and packed RGBA directly on the GPU; everything
// else goes through libyuv to ARGB8888.
enum class Path { Planar, SemiPlanar, Packed, Convert };

struct Plan {
  SDL_PixelFormat format;
  Path path;
};

inline auto planFor(OMPixelFormat format) -> Plan {
  switch (format) {
    case OM_FORMAT_NV12: return {SDL_PIXELFORMAT_NV12, Path::SemiPlanar};
    case OM_FORMAT_NV21: return {SDL_PIXELFORMAT_NV21, Path::SemiPlanar};
    case OM_FORMAT_YUV420P:
    case OM_FORMAT_YUVJ420P: return {SDL_PIXELFORMAT_IYUV, Path::Planar};
    // SDL names packed formats by their 32-bit word, OpenMedia by byte order.
    case OM_FORMAT_R8G8B8A8: return {SDL_PIXELFORMAT_ABGR8888, Path::Packed};
    case OM_FORMAT_B8G8R8A8: return {SDL_PIXELFORMAT_ARGB8888, Path::Packed};
    default: return {SDL_PIXELFORMAT_ARGB8888, Path::Convert};
  }
}

// J-formats carry full range in the pixel format rather than the range field.
inline auto isFullRange(const VideoFrame& vf) -> bool {
  switch (vf.format) {
    case OM_FORMAT_YUVJ420P: case OM_FORMAT_YUVJ422P: case OM_FORMAT_YUVJ444P:
      return true;
    default:
      return vf.color_range == OM_COLOR_RANGE_FULL;
  }
}

enum class Matrix { BT601, BT709, BT2020 };

// Unspecified colour space: SD is BT.601, HD and up is BT.709.
inline auto matrixOf(const VideoFrame& vf) -> Matrix {
  switch (vf.color_space) {
    case OM_COLOR_SPACE_BT2020: case OM_COLOR_SPACE_BT2020_CL:
      return Matrix::BT2020;
    case OM_COLOR_SPACE_BT601: case OM_COLOR_SPACE_SMPTE240M: case OM_COLOR_SPACE_FCC:
      return Matrix::BT601;
    case OM_COLOR_SPACE_UNKNOWN:
      return vf.height > 576 ? Matrix::BT709 : Matrix::BT601;
    default:
      return Matrix::BT709;
  }
}

// SDL defaults YUV textures to BT.601 full range, which washes out ordinary
// BT.709 limited-range video, so the colorspace is always set explicitly.
inline auto sdlColorspace(const VideoFrame& vf) -> SDL_Colorspace {
  const bool full = isFullRange(vf);
  switch (matrixOf(vf)) {
    case Matrix::BT2020: return full ? SDL_COLORSPACE_BT2020_FULL : SDL_COLORSPACE_BT2020_LIMITED;
    case Matrix::BT601: return full ? SDL_COLORSPACE_BT601_FULL : SDL_COLORSPACE_BT601_LIMITED;
    default: return full ? SDL_COLORSPACE_BT709_FULL : SDL_COLORSPACE_BT709_LIMITED;
  }
}

inline auto yuvConstants(const VideoFrame& vf) -> const libyuv::YuvConstants* {
  const bool full = isFullRange(vf);
  switch (matrixOf(vf)) {
    case Matrix::BT2020: return full ? &libyuv::kYuvV2020Constants : &libyuv::kYuv2020Constants;
    case Matrix::BT601: return full ? &libyuv::kYuvJPEGConstants : &libyuv::kYuvI601Constants;
    default: return full ? &libyuv::kYuvF709Constants : &libyuv::kYuvH709Constants;
  }
}

// CPU conversion of anything SDL cannot sample directly.
class Converter {
public:
  auto toArgb(const VideoFrame& vf) -> const uint8_t* {
    argb_.resize(size_t(vf.width) * vf.height * 4);
    return convert(vf, argb_.data(), int(vf.width) * 4) ? argb_.data() : nullptr;
  }

private:
  auto convert(const VideoFrame& vf, uint8_t* dst, int dst_stride) -> bool {
    const int w = int(vf.width), h = int(vf.height);
    const auto* yuv = yuvConstants(vf);
    const auto& [y, u, v] = vf.planes;
    // libyuv takes 16-bit strides in samples, not bytes.
    const auto p16 = [](const VideoFrame::Plane& p) { return reinterpret_cast<const uint16_t*>(p.data.data()); };

    switch (vf.format) {
      case OM_FORMAT_YUV422P:
      case OM_FORMAT_YUVJ422P:
        return !libyuv::I422ToARGBMatrix(y.data.data(), y.stride, u.data.data(), u.stride,
                                         v.data.data(), v.stride, dst, dst_stride, yuv, w, h);
      case OM_FORMAT_YUV444P:
      case OM_FORMAT_YUVJ444P:
        return !libyuv::I444ToARGBMatrix(y.data.data(), y.stride, u.data.data(), u.stride,
                                         v.data.data(), v.stride, dst, dst_stride, yuv, w, h);
      case OM_FORMAT_GRAY8:
        return !libyuv::I400ToARGBMatrix(y.data.data(), y.stride, dst, dst_stride, yuv, w, h);

      // libyuv has no NV16/NV24 entry point: split the chroma, reuse the planar kernels.
      case OM_FORMAT_NV16:
      case OM_FORMAT_NV24: {
        const bool full_width = vf.format == OM_FORMAT_NV24;
        const int cw = full_width ? w : (w + 1) / 2;
        splitChroma(u, cw, h);
        const auto kernel = full_width ? libyuv::I444ToARGBMatrix : libyuv::I422ToARGBMatrix;
        return !kernel(y.data.data(), y.stride, split_u_.data(), cw, split_v_.data(), cw,
                       dst, dst_stride, yuv, w, h);
      }

      case OM_FORMAT_P010:
      case OM_FORMAT_P012:
      case OM_FORMAT_P016: // valid bits in the MSBs
        return !libyuv::P010ToARGBMatrix(p16(y), y.stride / 2, p16(u), u.stride / 2,
                                         dst, dst_stride, yuv, w, h);
      case OM_FORMAT_YUV420P10:
        return !libyuv::I010ToARGBMatrix(p16(y), y.stride / 2, p16(u), u.stride / 2, p16(v), v.stride / 2,
                                         dst, dst_stride, yuv, w, h);
      case OM_FORMAT_YUV420P12:
        return !libyuv::I012ToARGBMatrix(p16(y), y.stride / 2, p16(u), u.stride / 2, p16(v), v.stride / 2,
                                         dst, dst_stride, yuv, w, h);
      case OM_FORMAT_YUV422P10:
        return !libyuv::I210ToARGBMatrix(p16(y), y.stride / 2, p16(u), u.stride / 2, p16(v), v.stride / 2,
                                         dst, dst_stride, yuv, w, h);
      case OM_FORMAT_YUV444P10:
        return !libyuv::I410ToARGBMatrix(p16(y), y.stride / 2, p16(u), u.stride / 2, p16(v), v.stride / 2,
                                         dst, dst_stride, yuv, w, h);

      case OM_FORMAT_YUV420P16:
      case OM_FORMAT_YUV422P12:
      case OM_FORMAT_YUV422P16:
      case OM_FORMAT_YUV444P12:
      case OM_FORMAT_YUV444P16:
        return convertNarrowed(vf, dst, dst_stride, yuv);

      default:
        if (warned_format_ != vf.format) {
          warned_format_ = vf.format;
          SDL_Log("[Renderer] Unsupported pixel format %d", int(vf.format));
        }
        return false;
    }
  }

  // libyuv only has 10-bit kernels for most layouts, so shift down to 10 bits first.
  auto convertNarrowed(const VideoFrame& vf, uint8_t* dst, int dst_stride,
                       const libyuv::YuvConstants* yuv) -> bool {
    const int w = int(vf.width), h = int(vf.height);
    const int shift = bitDepth(vf.format) - 10;
    const auto [cw, ch] = openmedia::getPlaneDimensions(vf.format, 1, vf.width, vf.height);
    if (shift < 0 || cw == 0 || ch == 0) return false;

    narrowPlane(vf.planes[0], narrow_y_, w, h, shift);
    narrowPlane(vf.planes[1], narrow_u_, int(cw), int(ch), shift);
    narrowPlane(vf.planes[2], narrow_v_, int(cw), int(ch), shift);

    const auto kernel = ch < vf.height  ? libyuv::I010ToARGBMatrix
                        : cw < vf.width ? libyuv::I210ToARGBMatrix
                                        : libyuv::I410ToARGBMatrix;
    return !kernel(narrow_y_.data(), w, narrow_u_.data(), int(cw), narrow_v_.data(), int(cw),
                   dst, dst_stride, yuv, w, h);
  }

  void splitChroma(const VideoFrame::Plane& uv, int cw, int ch) {
    split_u_.assign(size_t(cw) * ch, 0);
    split_v_.assign(size_t(cw) * ch, 0);
    if (uv.data.empty()) return;
    for (int row = 0; row < ch; ++row) {
      const uint8_t* s = uv.data.data() + size_t(row) * uv.stride;
      for (int col = 0; col < cw; ++col) {
        split_u_[size_t(row) * cw + col] = s[col * 2];
        split_v_[size_t(row) * cw + col] = s[col * 2 + 1];
      }
    }
  }

  static void narrowPlane(const VideoFrame::Plane& src, std::vector<uint16_t>& dst, int w, int h, int shift) {
    dst.assign(size_t(w) * h, 0);
    if (src.data.empty()) return;
    for (int row = 0; row < h; ++row) {
      const auto* s = reinterpret_cast<const uint16_t*>(src.data.data() + size_t(row) * src.stride);
      for (int col = 0; col < w; ++col) dst[size_t(row) * w + col] = uint16_t(s[col] >> shift);
    }
  }

  std::vector<uint8_t> argb_;
  std::vector<uint16_t> narrow_y_, narrow_u_, narrow_v_;
  std::vector<uint8_t> split_u_, split_v_;
  OMPixelFormat warned_format_ = OM_FORMAT_UNKNOWN;
};

} // namespace upload

// Owns the video texture and decides, once per vsync, which frame it shows.
class VideoRenderer {
public:
  explicit VideoRenderer(SDL_Renderer* renderer) : renderer_(renderer) {}
  ~VideoRenderer() { reset(); }

  // Shows the newest frame that is due at the next vsync; older due frames
  // are dropped. Main thread only.
  void present(FrameQueue& frames, AVClock& clock) {
    // First frame after start or seek: show it at once and start the clock
    // from it, instead of judging it against a clock that is not running yet.
    if (!clock.started()) {
      if (auto first = frames.popIf([](const VideoFrame&) { return true; })) {
        clock.syncToVideo(first->pts);
        show(*first);
      }
      return;
    }

    // A frame uploaded now is seen at the next vsync, one refresh away. It is
    // due when that vsync is the one nearest to its timestamp.
    const double refresh = refreshInterval();
    const double deadline = clock.now() + refresh * 1.5;

    std::optional<VideoFrame> due;
    while (auto next = frames.popIf([&](const VideoFrame& f) { return f.pts <= deadline; }))
      due = std::move(next);
    if (!due) return;

    // Without audio a decoder that cannot keep up would otherwise have every
    // frame dropped; follow it instead. (No effect once audio is the master.)
    if (clock.now() - due->pts > kMaxLag) clock.syncToVideo(due->pts);
    show(*due);
  }

  void show(const VideoFrame& vf) {
    if (vf.width == 0 || vf.height == 0 || vf.planes[0].data.empty()) return;

    const auto plan = upload::planFor(vf.format);
    const bool yuv = plan.path == upload::Path::Planar || plan.path == upload::Path::SemiPlanar;
    if (!ensureTexture(plan.format, yuv ? upload::sdlColorspace(vf) : SDL_COLORSPACE_SRGB, vf.width, vf.height))
      return;

    const auto& [y, u, v] = vf.planes;
    switch (plan.path) {
      case upload::Path::SemiPlanar:
        SDL_UpdateNVTexture(texture_, nullptr, y.data.data(), y.stride, u.data.data(), u.stride);
        break;
      case upload::Path::Planar:
        SDL_UpdateYUVTexture(texture_, nullptr, y.data.data(), y.stride, u.data.data(), u.stride,
                             v.data.data(), v.stride);
        break;
      case upload::Path::Packed:
        SDL_UpdateTexture(texture_, nullptr, y.data.data(), y.stride);
        break;
      case upload::Path::Convert:
        if (const uint8_t* argb = converter_.toArgb(vf))
          SDL_UpdateTexture(texture_, nullptr, argb, int(vf.width) * 4);
        break;
    }
  }

  void reset() {
    if (texture_) SDL_DestroyTexture(texture_);
    texture_ = nullptr;
    key_ = {};
  }

  auto texture() const -> SDL_Texture* { return texture_; }
  auto size() const -> std::pair<uint32_t, uint32_t> { return {key_.width, key_.height}; }

private:
  // Past this lag a wall-clocked video re-bases the clock instead of dropping.
  static constexpr double kMaxLag = 0.5;
  static constexpr double kFallbackRefresh = 1.0 / 60.0;
  static constexpr int kRefreshPollTicks = 120;

  struct TextureKey {
    SDL_PixelFormat format = SDL_PIXELFORMAT_UNKNOWN;
    SDL_Colorspace colorspace = SDL_COLORSPACE_UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
    bool operator==(const TextureKey&) const = default;
  };

  auto ensureTexture(SDL_PixelFormat format, SDL_Colorspace colorspace, uint32_t w, uint32_t h) -> bool {
    const TextureKey key {format, colorspace, w, h};
    if (texture_ && key == key_) return true;
    reset();

    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, format);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STREAMING);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, w);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, h);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, colorspace);
    texture_ = SDL_CreateTextureWithProperties(renderer_, props);
    SDL_DestroyProperties(props);

    if (!texture_) {
      SDL_Log("[Renderer] Cannot create texture: %s", SDL_GetError());
      return false;
    }
    SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_LINEAR);
    key_ = key;
    return true;
  }

  // Re-read now and then: the window may move to a display with another rate.
  auto refreshInterval() -> double {
    if (refresh_ticks_-- > 0) return refresh_interval_;
    refresh_ticks_ = kRefreshPollTicks;
    refresh_interval_ = kFallbackRefresh;
    if (SDL_Window* window = SDL_GetRenderWindow(renderer_)) {
      const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
      if (mode && mode->refresh_rate > 0.0f) refresh_interval_ = 1.0 / mode->refresh_rate;
    }
    return refresh_interval_;
  }

  SDL_Renderer* renderer_;
  SDL_Texture* texture_ = nullptr;
  TextureKey key_;
  upload::Converter converter_;
  double refresh_interval_ = kFallbackRefresh;
  int refresh_ticks_ = 0;
};
