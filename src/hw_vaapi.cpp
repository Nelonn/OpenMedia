#include "hw_vaapi_priv.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

using namespace openmedia;

OMVAAPIContext* HWVAAPIContext_create(OMVAAPIInit init) {
  auto* context = static_cast<OMVAAPIContext*>(std::malloc(sizeof(OMVAAPIContext)));
  if (!context) return nullptr;

  new (context) OMVAAPIContext();

  if (!context->initialize(init)) {
    context->~OMVAAPIContext();
    std::free(context);
    return nullptr;
  }

  return context;
}

void HWVAAPIContext_delete(OMVAAPIContext* context) {
  if (!context) return;
  context->~OMVAAPIContext();
  std::free(context);
}

VADisplay HWVAAPIContext_getDisplay(OMVAAPIContext* context) {
  if (!context) return nullptr;
  return context->display;
}

int HWVAAPIContext_copyToHost(OMVAAPIContext* context,
                              uint32_t surface,
                              uint8_t* y_plane,
                              uint32_t y_stride,
                              uint8_t* uv_plane,
                              uint32_t uv_stride,
                              uint32_t width,
                              uint32_t height) {
  if (!context || !context->display || !y_plane || !uv_plane || width == 0 || height == 0) return 0;

  auto& libva = openmedia::LibVA::getInstance();
  if (!libva.isLoaded() && !libva.load()) return 0;
  VADisplay display = context->display;

  if (libva.vaSyncSurface(display, surface) != VA_STATUS_SUCCESS) return 0;

  // Deriving tells what the surface holds (NV12 or P010) without copying.
  // Reading through the derived mapping is slow on Intel, where it goes
  // through write-combined, tiled memory, so it is only the fallback; a
  // linear image filled by vaGetImage is what gets read.
  VAImage derived = {};
  derived.image_id = VA_INVALID_ID;
  uint32_t fourcc = VA_FOURCC_NV12;
  if (libva.vaDeriveImage(display, surface, &derived) == VA_STATUS_SUCCESS) {
    fourcc = derived.format.fourcc;
  }
  if (fourcc != VA_FOURCC_NV12 && fourcc != VA_FOURCC_P010 && fourcc != VA_FOURCC_P016 && fourcc != VA_FOURCC_P012) {
    if (derived.image_id != VA_INVALID_ID) libva.vaDestroyImage(display, derived.image_id);
    return 0;
  }

  VAImage image = {};
  image.image_id = VA_INVALID_ID;
  bool use_derived = true;
  {
    VAImageFormat format = {};
    format.fourcc = fourcc;
    format.byte_order = VA_LSB_FIRST;
    format.bits_per_pixel = fourcc == VA_FOURCC_NV12 ? 12 : 24;
    if (libva.vaCreateImage(display, &format, static_cast<int>(width), static_cast<int>(height), &image) == VA_STATUS_SUCCESS) {
      if (libva.vaGetImage(display, surface, 0, 0, width, height, image.image_id) == VA_STATUS_SUCCESS) {
        use_derived = false;
      } else {
        libva.vaDestroyImage(display, image.image_id);
        image.image_id = VA_INVALID_ID;
      }
    }
  }
  if (use_derived) {
    if (derived.image_id == VA_INVALID_ID) return 0;
    image = derived;
    derived.image_id = VA_INVALID_ID;
  } else if (derived.image_id != VA_INVALID_ID) {
    libva.vaDestroyImage(display, derived.image_id);
  }

  void* mapped = nullptr;
  if (libva.vaMapBuffer(display, image.buf, &mapped) != VA_STATUS_SUCCESS || !mapped) {
    libva.vaDestroyImage(display, image.image_id);
    return 0;
  }

  const uint32_t bpp = fourcc == VA_FOURCC_NV12 ? 1 : 2;
  // Chroma rows interleave Cb and Cr, so an odd width still ends on a pair.
  const uint32_t y_copy = std::min(width * bpp, y_stride);
  const uint32_t uv_copy = std::min(((width + 1) & ~1u) * bpp, uv_stride);
  const uint32_t uv_height = (height + 1) / 2;

  const auto* src = static_cast<const uint8_t*>(mapped);
  const uint8_t* src_y = src + image.offsets[0];
  const uint8_t* src_uv = src + image.offsets[1];
  for (uint32_t row = 0; row < height; ++row) {
    std::memcpy(y_plane + static_cast<size_t>(row) * y_stride, src_y + static_cast<size_t>(row) * image.pitches[0], y_copy);
  }
  for (uint32_t row = 0; row < uv_height; ++row) {
    std::memcpy(uv_plane + static_cast<size_t>(row) * uv_stride, src_uv + static_cast<size_t>(row) * image.pitches[1], uv_copy);
  }

  libva.vaUnmapBuffer(display, image.buf);
  libva.vaDestroyImage(display, image.image_id);
  return 1;
}
