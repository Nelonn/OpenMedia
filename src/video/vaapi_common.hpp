#pragma once

#include <hw_vaapi_priv.hpp>
#include <openmedia/hw_vaapi.h>
#include <openmedia/log.hpp>
#include <openmedia/video.hpp>
#include <va/va.h>
#include <video/vaapi_loader.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace openmedia::vaapi {

inline auto describe(VAStatus status) -> const char* {
  auto& libva = LibVA::getInstance();
  return libva.vaErrorStr ? libva.vaErrorStr(status) : "unknown VA error";
}

template<typename T>
constexpr auto alignUp(T value, T alignment) -> T {
  return ((value + alignment - 1) / alignment) * alignment;
}

// A VADisplay the codec opened itself, when the caller did not hand one in.
// Shared with every surface pool created on it, so that pictures still held by
// the caller keep the display alive after the codec is gone.
using OwnedDisplay = std::shared_ptr<OMVAAPIContext>;

inline auto openDisplay() -> OwnedDisplay {
  OMVAAPIInit init = {};
  OMVAAPIContext* context = HWVAAPIContext_create(init);
  if (!context) return nullptr;
  return OwnedDisplay(context, [](OMVAAPIContext* c) { HWVAAPIContext_delete(c); });
}

inline auto queryProfiles(VADisplay display) -> std::vector<VAProfile> {
  auto& libva = LibVA::getInstance();
  int count = libva.vaMaxNumConfigProfiles(display);
  if (count <= 0) return {};
  std::vector<VAProfile> profiles(static_cast<size_t>(count));
  if (libva.vaQueryConfigProfiles(display, profiles.data(), &count) != VA_STATUS_SUCCESS) return {};
  profiles.resize(static_cast<size_t>(std::max(count, 0)));
  return profiles;
}

inline auto supportsEntrypoint(VADisplay display, VAProfile profile, VAEntrypoint entrypoint) -> bool {
  auto& libva = LibVA::getInstance();
  int count = libva.vaMaxNumConfigEntrypoints(display);
  if (count <= 0) return false;
  std::vector<VAEntrypoint> entrypoints(static_cast<size_t>(count));
  if (libva.vaQueryConfigEntrypoints(display, profile, entrypoints.data(), &count) != VA_STATUS_SUCCESS) return false;
  entrypoints.resize(static_cast<size_t>(std::max(count, 0)));
  return std::find(entrypoints.begin(), entrypoints.end(), entrypoint) != entrypoints.end();
}

inline auto fourccForRtFormat(uint32_t rt_format) -> uint32_t {
  return rt_format == VA_RT_FORMAT_YUV420_10 ? VA_FOURCC_P010 : VA_FOURCC_NV12;
}

inline auto pixelFormatForFourcc(uint32_t fourcc) -> OMPixelFormat {
  return fourcc == VA_FOURCC_P010 ? OM_FORMAT_P010 : OM_FORMAT_NV12;
}

// The surfaces a codec decodes into or encodes from.
//
// A surface is busy while the codec still refers to it -- which the codec
// answers for itself -- or while a picture wrapping it is alive somewhere,
// which is what the counts here are for. Pictures reach the caller's render
// thread, so the counts are guarded.
class SurfacePool : public std::enable_shared_from_this<SurfacePool> {
public:
  static auto create(OwnedDisplay owner, VADisplay display, uint32_t rt_format, uint32_t width, uint32_t height,
                     size_t count) -> std::shared_ptr<SurfacePool> {
    auto pool = std::shared_ptr<SurfacePool>(new SurfacePool(std::move(owner), display, rt_format, width, height));
    if (!pool->grow(count)) return nullptr;
    return pool;
  }

  ~SurfacePool() {
    auto& libva = LibVA::getInstance();
    if (image_.image_id != VA_INVALID_ID) libva.vaDestroyImage(display_, image_.image_id);
    if (!surfaces_.empty()) libva.vaDestroySurfaces(display_, surfaces_.data(), static_cast<int>(surfaces_.size()));
  }

  SurfacePool(const SurfacePool&) = delete;
  SurfacePool& operator=(const SurfacePool&) = delete;

  auto display() const -> VADisplay { return display_; }
  auto width() const -> uint32_t { return width_; }
  auto height() const -> uint32_t { return height_; }
  auto rtFormat() const -> uint32_t { return rt_format_; }
  auto fourcc() const -> uint32_t { return fourcc_; }

  auto size() const -> size_t {
    std::lock_guard lock(mutex_);
    return surfaces_.size();
  }

  auto surface(int index) const -> VASurfaceID {
    std::lock_guard lock(mutex_);
    if (index < 0 || static_cast<size_t>(index) >= surfaces_.size()) return VA_INVALID_SURFACE;
    return surfaces_[static_cast<size_t>(index)];
  }

  // A copy of the surface list, for vaCreateContext.
  auto surfaces() const -> std::vector<VASurfaceID> {
    std::lock_guard lock(mutex_);
    return surfaces_;
  }

  // Adds surfaces until there are `count`. Surfaces created after the context
  // are not in its render target list; iHD, i965 and Mesa all accept them.
  auto grow(size_t count) -> bool {
    std::lock_guard lock(mutex_);
    if (surfaces_.size() >= count) return true;
    const size_t add = count - surfaces_.size();
    std::vector<VASurfaceID> created(add, VA_INVALID_SURFACE);

    VASurfaceAttrib attrib = {};
    attrib.type = VASurfaceAttribPixelFormat;
    attrib.flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrib.value.type = VAGenericValueTypeInteger;
    attrib.value.value.i = static_cast<int32_t>(fourcc_);

    auto& libva = LibVA::getInstance();
    const VAStatus status = libva.vaCreateSurfaces(display_, rt_format_, width_, height_, created.data(),
                                                   static_cast<unsigned int>(add), &attrib, 1);
    if (status != VA_STATUS_SUCCESS) {
      log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "[VAAPI] vaCreateSurfaces({}x{}, {} surfaces) failed: {}", width_,
          height_, add, describe(status));
      return false;
    }
    surfaces_.insert(surfaces_.end(), created.begin(), created.end());
    outstanding_.resize(surfaces_.size(), 0);
    return true;
  }

  // The index of a surface nothing holds. `held` answers for the codec's own
  // references; `exclude` is a surface already taken for the current picture.
  // Runs out only when the caller keeps more pictures than the pool was sized
  // for, in which case the pool grows rather than failing the picture.
  template<typename Held>
  auto acquire(Held&& held, int exclude = -1) -> int {
    for (int attempt = 0; attempt < 2; ++attempt) {
      size_t count = 0;
      {
        std::lock_guard lock(mutex_);
        count = surfaces_.size();
        for (size_t i = 0; i < count; ++i) {
          const size_t index = (next_ + i) % count;
          if (outstanding_[index] != 0 || static_cast<int>(index) == exclude) continue;
          if (held(static_cast<int>(index))) continue;
          next_ = (index + 1) % count;
          return static_cast<int>(index);
        }
      }
      if (count >= kMaxSurfaces || !grow(std::min(count + kGrowBy, kMaxSurfaces))) break;
      if (!warned_growth_) {
        warned_growth_ = true;
        log(OM_CATEGORY_HARDWARE, OM_LEVEL_WARNING,
            "[VAAPI] all {} surfaces are in use; growing the pool (pictures are being held for long)", count);
      }
    }
    return -1;
  }

  // Wraps a surface as a picture for the outside world. The surface stays
  // reserved for as long as the returned object lives.
  auto wrap(int index) -> std::shared_ptr<HardwarePicture>;

  void release(int index) {
    std::lock_guard lock(mutex_);
    if (index >= 0 && static_cast<size_t>(index) < outstanding_.size() && outstanding_[static_cast<size_t>(index)] > 0)
      --outstanding_[static_cast<size_t>(index)];
  }

  // Copies the visible part of a surface into system memory. Uses an image of
  // the pool's own size, created once: vaGetImage into a linear image reads far
  // faster than mapping the (tiled, write-combined) surface itself on Intel,
  // and vaDeriveImage is only the fallback.
  auto download(int index, uint32_t width, uint32_t height, Picture& out) -> bool;

private:
  static constexpr size_t kMaxSurfaces = 96;
  static constexpr size_t kGrowBy = 4;

  SurfacePool(OwnedDisplay owner, VADisplay display, uint32_t rt_format, uint32_t width, uint32_t height)
      : owner_(std::move(owner)), display_(display), rt_format_(rt_format), fourcc_(fourccForRtFormat(rt_format)),
        width_(width), height_(height) {
    image_.image_id = VA_INVALID_ID;
  }

  OwnedDisplay owner_;
  VADisplay display_ = nullptr;
  uint32_t rt_format_ = VA_RT_FORMAT_YUV420;
  uint32_t fourcc_ = VA_FOURCC_NV12;
  uint32_t width_ = 0;
  uint32_t height_ = 0;

  mutable std::mutex mutex_;
  std::vector<VASurfaceID> surfaces_;
  std::vector<uint32_t> outstanding_;
  size_t next_ = 0;
  bool warned_growth_ = false;

  // Only touched from the codec's own thread.
  VAImage image_ = {};
  bool get_image_works_ = true;
};

// What the caller gets for a decoded surface. Returning the surface to the pool
// is the only thing the destructor has to do; the pool keeps the display and
// the surface itself alive.
class PooledPicture final : public VAAPIHardwarePicture {
public:
  PooledPicture(std::shared_ptr<SurfacePool> pool, int index)
      : VAAPIHardwarePicture(pool->display(), pool->surface(index)), pool_(std::move(pool)), index_(index) {}
  ~PooledPicture() override { pool_->release(index_); }

  auto pool() const -> const std::shared_ptr<SurfacePool>& { return pool_; }
  auto index() const -> int { return index_; }

private:
  std::shared_ptr<SurfacePool> pool_;
  int index_ = -1;
};

inline auto SurfacePool::wrap(int index) -> std::shared_ptr<HardwarePicture> {
  {
    std::lock_guard lock(mutex_);
    if (index < 0 || static_cast<size_t>(index) >= surfaces_.size()) return nullptr;
    ++outstanding_[static_cast<size_t>(index)];
  }
  return std::make_shared<PooledPicture>(shared_from_this(), index);
}

inline auto SurfacePool::download(int index, uint32_t width, uint32_t height, Picture& out) -> bool {
  auto& libva = LibVA::getInstance();
  const VASurfaceID surface = this->surface(index);
  if (surface == VA_INVALID_SURFACE) return false;
  width = std::min(width, width_);
  height = std::min(height, height_);
  if (width == 0 || height == 0) return false;

  VAStatus status = libva.vaSyncSurface(display_, surface);
  if (status != VA_STATUS_SUCCESS) {
    log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "[VAAPI] vaSyncSurface failed: {}", describe(status));
    return false;
  }

  VAImage derived = {};
  derived.image_id = VA_INVALID_ID;
  const VAImage* image = nullptr;

  if (get_image_works_) {
    if (image_.image_id == VA_INVALID_ID) {
      VAImageFormat format = {};
      format.fourcc = fourcc_;
      format.byte_order = VA_LSB_FIRST;
      format.bits_per_pixel = fourcc_ == VA_FOURCC_P010 ? 24 : 12;
      status = libva.vaCreateImage(display_, &format, static_cast<int>(width_), static_cast<int>(height_), &image_);
      if (status != VA_STATUS_SUCCESS) {
        image_.image_id = VA_INVALID_ID;
        get_image_works_ = false;
      }
    }
    if (get_image_works_) {
      status = libva.vaGetImage(display_, surface, 0, 0, width_, height_, image_.image_id);
      if (status == VA_STATUS_SUCCESS) {
        image = &image_;
      } else {
        log(OM_CATEGORY_HARDWARE, OM_LEVEL_WARNING, "[VAAPI] vaGetImage failed ({}), mapping surfaces directly",
            describe(status));
        get_image_works_ = false;
      }
    }
  }
  if (!image) {
    status = libva.vaDeriveImage(display_, surface, &derived);
    if (status != VA_STATUS_SUCCESS) {
      log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "[VAAPI] vaDeriveImage failed: {}", describe(status));
      return false;
    }
    if (derived.format.fourcc != fourcc_) {
      libva.vaDestroyImage(display_, derived.image_id);
      return false;
    }
    image = &derived;
  }

  void* mapped = nullptr;
  status = libva.vaMapBuffer(display_, image->buf, &mapped);
  if (status != VA_STATUS_SUCCESS || !mapped) {
    if (image == &derived) libva.vaDestroyImage(display_, derived.image_id);
    return false;
  }

  const OMPixelFormat format = pixelFormatForFourcc(fourcc_);
  Picture pic(format, width, height);
  const auto* base = static_cast<const uint8_t*>(mapped);
  for (uint32_t plane = 0; plane < 2; ++plane) {
    const auto dims = pic.getPlaneDimensions(plane);
    // The chroma plane interleaves Cb and Cr, so an odd width still ends on a
    // whole pair; the destination stride is 16-byte aligned and has room.
    const uint32_t samples = plane == 0 ? dims.first : alignUp(dims.first, 2u);
    const size_t row_bytes = static_cast<size_t>(samples) * getBytesPerPixel(format, plane);
    const uint8_t* src = base + image->offsets[plane];
    uint8_t* dst = pic.planes.getData(plane);
    const size_t dst_stride = pic.planes.getLinesize(plane);
    for (uint32_t row = 0; row < dims.second; ++row) {
      std::memcpy(dst + row * dst_stride, src + static_cast<size_t>(row) * image->pitches[plane], row_bytes);
    }
  }

  libva.vaUnmapBuffer(display_, image->buf);
  if (image == &derived) libva.vaDestroyImage(display_, derived.image_id);

  // Everything but the storage is the caller's to fill in.
  out.buffer = std::move(pic.buffer);
  out.planes = pic.planes;
  out.format = format;
  out.width = width;
  out.height = height;
  return true;
}

// The buffers of one picture, destroyed together once the picture has been
// submitted -- or abandoned. Parameter buffers go to the driver first and the
// slice parameter/data pairs after them, the order ffmpeg and Chromium both use.
class Submission {
public:
  Submission(VADisplay display, VAContextID context) : display_(display), context_(context) {}
  ~Submission() { destroy(); }

  Submission(const Submission&) = delete;
  Submission& operator=(const Submission&) = delete;

  auto addParam(VABufferType type, const void* data, size_t size) -> bool {
    return create(type, data, size, 1, params_);
  }

  // `count` slice parameter structures of `param_size` bytes each, all of them
  // describing the one data buffer that follows.
  auto addSlice(const void* params, size_t param_size, uint32_t count, const void* data, size_t data_size) -> bool {
    if (!create(VASliceParameterBufferType, params, param_size, count, slices_)) return false;
    return create(VASliceDataBufferType, data, data_size, 1, slices_);
  }

  auto failed() const -> bool { return failed_; }

  // vaBeginPicture/vaRenderPicture/vaEndPicture on `target`. A picture that
  // began is always ended, even when rendering failed: a context left inside a
  // picture refuses the next vaBeginPicture.
  auto execute(VASurfaceID target) -> VAStatus {
    if (failed_) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    auto& libva = LibVA::getInstance();
    VAStatus status = libva.vaBeginPicture(display_, context_, target);
    if (status != VA_STATUS_SUCCESS) return status;
    VAStatus render = VA_STATUS_SUCCESS;
    if (!params_.empty()) {
      render = libva.vaRenderPicture(display_, context_, params_.data(), static_cast<int>(params_.size()));
    }
    if (render == VA_STATUS_SUCCESS && !slices_.empty()) {
      render = libva.vaRenderPicture(display_, context_, slices_.data(), static_cast<int>(slices_.size()));
    }
    status = libva.vaEndPicture(display_, context_);
    destroy();
    return render != VA_STATUS_SUCCESS ? render : status;
  }

private:
  auto create(VABufferType type, const void* data, size_t size, uint32_t count, std::vector<VABufferID>& into)
      -> bool {
    if (failed_) return false;
    VABufferID id = VA_INVALID_ID;
    const VAStatus status = LibVA::getInstance().vaCreateBuffer(display_, context_, type, static_cast<unsigned int>(size),
                                                                count, const_cast<void*>(data), &id);
    if (status != VA_STATUS_SUCCESS) {
      log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "[VAAPI] vaCreateBuffer(type {}, {} bytes) failed: {}",
          static_cast<int>(type), size * count, describe(status));
      failed_ = true;
      return false;
    }
    into.push_back(id);
    return true;
  }

  void destroy() {
    auto& libva = LibVA::getInstance();
    for (VABufferID id : params_) libva.vaDestroyBuffer(display_, id);
    for (VABufferID id : slices_) libva.vaDestroyBuffer(display_, id);
    params_.clear();
    slices_.clear();
  }

  VADisplay display_ = nullptr;
  VAContextID context_ = VA_INVALID_ID;
  std::vector<VABufferID> params_;
  std::vector<VABufferID> slices_;
  bool failed_ = false;
};

} // namespace openmedia::vaapi
