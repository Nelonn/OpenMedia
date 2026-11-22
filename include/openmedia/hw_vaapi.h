#pragma once

#include <openmedia/macro.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef void* VADisplay;

typedef struct OMVAAPIInit {
  VADisplay display;
  int drm_fd;
} OMVAAPIInit;

typedef struct OMVAAPIContext OMVAAPIContext;

OPENMEDIA_ABI
OMVAAPIContext* HWVAAPIContext_create(OMVAAPIInit init);

OPENMEDIA_ABI
void HWVAAPIContext_delete(OMVAAPIContext* context);

OPENMEDIA_ABI
VADisplay HWVAAPIContext_getDisplay(OMVAAPIContext* context);

OPENMEDIA_ABI
int HWVAAPIContext_copyToHost(OMVAAPIContext* context,
                              uint32_t surface,
                              uint8_t* y_plane,
                              uint32_t y_stride,
                              uint8_t* uv_plane,
                              uint32_t uv_stride,
                              uint32_t width,
                              uint32_t height);

// The DRM format modifiers a DMA-BUF of `drm_fourcc` (DRM_FORMAT_NV12 or
// DRM_FORMAT_P010) at about width x height should be laid out in for this
// display's encoders to read it without a copy: the layout the driver gives
// its own surfaces first, then linear. Both planes belong in one buffer --
// Mesa's encoders read the chroma through the luma plane's buffer -- so a
// producer that cannot allocate the format itself (GBM has no NV12 on AMD)
// lays out one linear buffer as NV12: luma rows, then chroma rows, one pitch. Writes up to `capacity` of them to
// `modifiers` and returns how many there were; 0 when the driver cannot say
// (no vaExportSurfaceHandle, or no such surfaces at all).
OPENMEDIA_ABI
int HWVAAPIContext_queryDmaBufModifiers(OMVAAPIContext* context,
                                        uint32_t drm_fourcc,
                                        uint32_t width,
                                        uint32_t height,
                                        uint64_t* modifiers,
                                        int capacity);

#if defined(__cplusplus)
}

#include <openmedia/video.hpp>

namespace openmedia {

class OPENMEDIA_ABI VAAPIHardwarePicture : public HardwarePicture {
public:
  VAAPIHardwarePicture(VADisplay display, uint32_t surface)
      : HardwarePicture(HWDeviceType::VAAPI), display_(display), surface_(surface) {}

  auto display() const noexcept -> VADisplay { return display_; }
  auto surface() const noexcept -> uint32_t { return surface_; }

private:
  VADisplay display_ = nullptr;
  uint32_t surface_ = 0xffffffffu;
};

} // namespace openmedia
#endif
