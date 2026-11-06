#include <openmedia/hw_cuda.h>
#include <cstdlib>
#include <new>
#include <openmedia/log.hpp>
#include <openmedia/video.hpp>
#include "video/nv_loader.hpp"

using namespace openmedia;

namespace openmedia {
class NVDecPicture;
}

struct OMCudaContext {
  CUcontext cu_context = nullptr;
  CUstream cu_stream = nullptr;
  int device_index = 0;
  bool owns_context = false;

  ~OMCudaContext() {
    auto* cu = NVLoader::getInstance().cuda();
    if (cu) {
      if (cu_stream) cu->cuStreamDestroy(cu_stream);
      if (owns_context && cu_context) cu->cuCtxDestroy(cu_context);
    }
  }
};

OMCudaContext* HWCudaContext_create(OMCudaInit init) {
  if (!NVLoader::getInstance().load()) {
    openmedia::log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "CUDA: Failed to load NVIDIA libraries");
    return nullptr;
  }
  auto* cu = NVLoader::getInstance().cuda();

  CUresult res = cu->cuInit(0);
  if (res != CUDA_SUCCESS) {
    openmedia::log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "CUDA: cuInit failed with error {}", (int) res);
    return nullptr;
  }

  auto* context = new (std::nothrow) OMCudaContext();
  if (!context) return nullptr;

  context->device_index = init.device_index;

  if (init.cu_context) {
    context->cu_context = (CUcontext) init.cu_context;
    context->owns_context = false;
  } else {
    CUdevice device;
    res = cu->cuDeviceGet(&device, context->device_index);
    if (res != CUDA_SUCCESS) {
      openmedia::log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "CUDA: cuDeviceGet failed with error {}", (int) res);
      delete context;
      return nullptr;
    }
    res = cu->cuCtxCreate(&context->cu_context, 0, device);
    if (res != CUDA_SUCCESS) {
      openmedia::log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "CUDA: cuCtxCreate failed with error {}", (int) res);
      delete context;
      return nullptr;
    }
    context->owns_context = true;
  }

  if (cu->cuStreamCreate(&context->cu_stream, 0) != CUDA_SUCCESS) {
    openmedia::log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "CUDA: cuStreamCreate failed");
    delete context;
    return nullptr;
  }

  return context;
}

void HWCudaContext_delete(OMCudaContext* context) {
  delete context;
}

void* HWCudaContext_getContext(OMCudaContext* context) {
  return context ? (void*) context->cu_context : nullptr;
}

void* HWCudaContext_getStream(OMCudaContext* context) {
  return context ? (void*) context->cu_stream : nullptr;
}

void HWCudaContext_copyToHost(OMCudaContext* context,
                              OMCudaPicture* picture,
                              void* dst_y, uint32_t dst_y_pitch,
                              void* dst_uv, uint32_t dst_uv_pitch,
                              uint32_t width, uint32_t height) {
  if (!context || !picture) return;
  auto* cu = NVLoader::getInstance().cuda();
  if (!cu) return;

  CUDA_MEMCPY2D copy = {};

  // Y Plane
  copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
  copy.srcDevice = (CUdeviceptr) picture->data;
  copy.srcPitch = picture->pitch;
  copy.dstMemoryType = CU_MEMORYTYPE_HOST;
  copy.dstHost = dst_y;
  copy.dstPitch = dst_y_pitch;
  copy.WidthInBytes = width;
  copy.Height = height;
  cu->cuMemcpy2DAsync(&copy, context->cu_stream);

  // UV Plane
  copy.srcDevice = (CUdeviceptr) picture->data + (size_t) picture->pitch * height;
  copy.dstHost = dst_uv;
  copy.dstPitch = dst_uv_pitch;
  copy.Height = (height + 1) / 2;
  cu->cuMemcpy2DAsync(&copy, context->cu_stream);

  cu->cuStreamSynchronize(context->cu_stream);
}

void HWCudaContext_copyPlanesToHost(OMCudaContext* context,
                                    OMCudaPicture* picture,
                                    uint32_t format,
                                    uint32_t width, uint32_t height,
                                    void* const dst[3], const uint32_t dst_pitch[3]) {
  if (!context || !picture || !dst || !dst_pitch) return;
  auto* cu = NVLoader::getInstance().cuda();
  if (!cu) return;

  // NVDecPicture keeps plane k at k * pitch * height, whatever the format.
  const auto fmt = static_cast<OMPixelFormat>(format);
  const uint32_t planes = openmedia::getNumPlanes(fmt);
  for (uint32_t plane = 0; plane < planes && plane < 3; ++plane) {
    const auto [plane_width, plane_height] = openmedia::getPlaneDimensions(fmt, plane, width, height);
    CUDA_MEMCPY2D copy = {};
    copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.srcDevice = (CUdeviceptr) picture->data + (size_t) picture->pitch * height * plane;
    copy.srcPitch = picture->pitch;
    copy.dstMemoryType = CU_MEMORYTYPE_HOST;
    copy.dstHost = dst[plane];
    copy.dstPitch = dst_pitch[plane];
    copy.WidthInBytes = (size_t) plane_width * openmedia::getBytesPerPixel(fmt, static_cast<uint8_t>(plane));
    copy.Height = plane_height;
    cu->cuMemcpy2DAsync(&copy, context->cu_stream);
  }
  cu->cuStreamSynchronize(context->cu_stream);
}
