#include "hw_vaapi_priv.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>
#include <va/va_drmcommon.h>

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

  // The derived mapping goes through write-combined, tiled memory on Intel, so it only says
  // what the surface holds; a linear image filled by vaGetImage is what gets read.
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

int HWVAAPIContext_queryDmaBufModifiers(OMVAAPIContext* context,
                                        uint32_t drm_fourcc,
                                        uint32_t width,
                                        uint32_t height,
                                        uint64_t* modifiers,
                                        int capacity) {
  if (!context || !context->display || !modifiers || capacity <= 0 || width == 0 || height == 0) return 0;
  auto& libva = openmedia::LibVA::getInstance();
  if (!libva.isLoaded() && !libva.load()) return 0;
  if (!libva.vaExportSurfaceHandle) return 0;

  constexpr uint32_t kNv12 = 0x3231564e; // DRM_FORMAT_NV12, 'NV12'
  constexpr uint32_t kP010 = 0x30313050; // DRM_FORMAT_P010, 'P010'
  if (drm_fourcc != kNv12 && drm_fourcc != kP010) return 0;
  const bool high = drm_fourcc == kP010;

  // A surface of the driver's own, exported: whatever layout it was given is
  // the one the driver reads best.
  VASurfaceAttrib attrib = {};
  attrib.type = VASurfaceAttribPixelFormat;
  attrib.flags = VA_SURFACE_ATTRIB_SETTABLE;
  attrib.value.type = VAGenericValueTypeInteger;
  attrib.value.value.i = static_cast<int>(high ? VA_FOURCC_P010 : VA_FOURCC_NV12);
  VASurfaceID surface = VA_INVALID_SURFACE;
  if (libva.vaCreateSurfaces(context->display, high ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420, width, height,
                             &surface, 1, &attrib, 1) != VA_STATUS_SUCCESS) {
    return 0;
  }
  VADRMPRIMESurfaceDescriptor descriptor = {};
  const VAStatus status =
      libva.vaExportSurfaceHandle(context->display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                  VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &descriptor);
  libva.vaDestroySurfaces(context->display, &surface, 1);
  if (status != VA_STATUS_SUCCESS) return 0;
  for (uint32_t i = 0; i < descriptor.num_objects; ++i) close(descriptor.objects[i].fd);
  if (descriptor.num_objects == 0) return 0;

  int count = 0;
  const uint64_t native = descriptor.objects[0].drm_format_modifier;
  modifiers[count++] = native;
  constexpr uint64_t kLinear = 0;
  if (native != kLinear && count < capacity) modifiers[count++] = kLinear;
  return count;
}
