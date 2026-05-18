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
  if (!libva.vaDeriveImage || !libva.vaMapBuffer || !libva.vaUnmapBuffer || !libva.vaDestroyImage || !libva.vaGetImage) return 0;

  VAImage image = {};
  bool derived = false;
  
  // Try zero-copy derivation first
  VAStatus status = libva.vaDeriveImage(context->display, surface, &image);
  if (status == VA_STATUS_SUCCESS) {
    if (image.format.fourcc == VA_FOURCC_NV12) {
      derived = true;
    } else {
      libva.vaDestroyImage(context->display, image.image_id);
    }
  }

  // Fallback to copy via GetImage if derivation failed or wrong format
  if (!derived) {
    VAImageFormat format = {VA_FOURCC_NV12, VA_LSB_FIRST, 12};
    status = libva.vaCreateImage(context->display, &format, width, height, &image);
    if (status != VA_STATUS_SUCCESS) return 0;

    status = libva.vaGetImage(context->display, surface, 0, 0, width, height, image.image_id);
    if (status != VA_STATUS_SUCCESS) {
      libva.vaDestroyImage(context->display, image.image_id);
      return 0;
    }
  }

  void* mapped = nullptr;
  status = libva.vaMapBuffer(context->display, image.buf, &mapped);
  if (status != VA_STATUS_SUCCESS || !mapped) {
    libva.vaDestroyImage(context->display, image.image_id);
    return 0;
  }

  const auto* src = static_cast<const uint8_t*>(mapped);
  const uint32_t y_copy = std::min(width, y_stride);
  const uint32_t uv_copy = std::min(width, uv_stride);
  const uint32_t uv_height = (height + 1) / 2;

  const uint8_t* src_y = src + image.offsets[0];
  const uint8_t* src_uv = (image.num_planes > 1) ? src + image.offsets[1] : nullptr;

  for (uint32_t row = 0; row < height; ++row) {
    std::memcpy(y_plane + row * y_stride, src_y + row * image.pitches[0], y_copy);
  }
  
  if (src_uv) {
    for (uint32_t row = 0; row < uv_height; ++row) {
      std::memcpy(uv_plane + row * uv_stride, src_uv + row * image.pitches[1], uv_copy);
    }
  }

  libva.vaUnmapBuffer(context->display, image.buf);
  libva.vaDestroyImage(context->display, image.image_id);
  log(OM_CATEGORY_HARDWARE, OM_LEVEL_DEBUG, "[VAAPI] called HWVAAPIContext_copyToHost");
  return 1;
}
