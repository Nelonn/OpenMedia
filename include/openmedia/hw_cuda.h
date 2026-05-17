#pragma once

#include <openmedia/macro.h>
#include <cstdint>

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct OMCudaContext OMCudaContext;

typedef struct OMCudaPicture {
  uint64_t data;
  uint32_t pitch;
} OMCudaPicture;

typedef struct OMCudaInit {
  int device_index;
  void* cu_context;
} OMCudaInit;

OPENMEDIA_ABI
OMCudaContext* HWCudaContext_create(OMCudaInit init);

OPENMEDIA_ABI
void HWCudaContext_delete(OMCudaContext* context);

OPENMEDIA_ABI
void* HWCudaContext_getContext(OMCudaContext* context);

OPENMEDIA_ABI
void* HWCudaContext_getStream(OMCudaContext* context);

OPENMEDIA_ABI
void HWCudaContext_copyToHost(OMCudaContext* context,
                              OMCudaPicture* picture,
                              void* dst_y, uint32_t dst_y_pitch,
                              void* dst_uv, uint32_t dst_uv_pitch,
                              uint32_t width, uint32_t height);

#if defined(__cplusplus)
} // extern "C"
#endif

#if defined(__cplusplus)
#include <openmedia/video.hpp>
#include <memory>

namespace openmedia {

class OPENMEDIA_ABI CudaHardwarePicture : public HardwarePicture {
public:
  uint64_t data = 0;
  uint32_t pitch = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  OMPixelFormat format = OM_FORMAT_NV12;

  explicit CudaHardwarePicture() : HardwarePicture(HWDeviceType::CUDA) {}

  virtual auto getOMPicture() -> OMCudaPicture* = 0;
};

} // namespace openmedia
#endif
