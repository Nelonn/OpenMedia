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
