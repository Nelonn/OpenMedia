#pragma once

#include <mutex>
#include <util/dynamic_loader.hpp>
#include <va/va.h>
#include <va/va_drm.h>

namespace openmedia {

class LibVA {
public:
  static auto getInstance() -> LibVA&;

  auto load() -> bool;
  auto isLoaded() const -> bool;

  PFN<VADisplay(int)> vaGetDisplayDRM = nullptr;
  PFN<VAStatus(VADisplay, int*, int*)> vaInitialize = nullptr;
  PFN<VAStatus(VADisplay)> vaTerminate = nullptr;
  PFN<const char*(VAStatus)> vaErrorStr = nullptr;
  PFN<VAStatus(VADisplay, VAProfile, VAEntrypoint, VAConfigAttrib*, int, VAConfigID*)> vaCreateConfig = nullptr;
  PFN<VAStatus(VADisplay, VAConfigID)> vaDestroyConfig = nullptr;
  PFN<VAStatus(VADisplay, unsigned int, int, int, VASurfaceID*, unsigned int, VASurfaceAttrib*, unsigned int)> vaCreateSurfaces = nullptr;
  PFN<VAStatus(VADisplay, VASurfaceID*, int)> vaDestroySurfaces = nullptr;
  PFN<VAStatus(VADisplay, VAConfigID, int, int, int, VASurfaceID*, int, VAContextID*)> vaCreateContext = nullptr;
  PFN<VAStatus(VADisplay, VAContextID)> vaDestroyContext = nullptr;
  PFN<VAStatus(VADisplay, VAContextID, VABufferType, unsigned int, unsigned int, void*, VABufferID*)> vaCreateBuffer = nullptr;
  PFN<VAStatus(VADisplay, VABufferID)> vaDestroyBuffer = nullptr;
  PFN<VAStatus(VADisplay, VABufferID, void**)> vaMapBuffer = nullptr;
  PFN<VAStatus(VADisplay, VABufferID)> vaUnmapBuffer = nullptr;
  PFN<VAStatus(VADisplay, VAContextID, VASurfaceID)> vaBeginPicture = nullptr;
  PFN<VAStatus(VADisplay, VAContextID, VABufferID*, int)> vaRenderPicture = nullptr;
  PFN<VAStatus(VADisplay, VAContextID)> vaEndPicture = nullptr;
  PFN<VAStatus(VADisplay, VASurfaceID)> vaSyncSurface = nullptr;
  PFN<VAStatus(VADisplay, VAProfile*, int*)> vaQueryConfigProfiles = nullptr;
  PFN<VAStatus(VADisplay, VAProfile, VAEntrypoint*, int*)> vaQueryConfigEntrypoints = nullptr;
  PFN<VAStatus(VADisplay, VAProfile, VAEntrypoint, VAConfigAttrib*, int)> vaGetConfigAttributes = nullptr;
  PFN<VAStatus(VADisplay, VASurfaceID, VAStatus*)> vaQuerySurfaceStatus = nullptr;
  PFN<int(VADisplay)> vaMaxNumConfigProfiles = nullptr;
  PFN<int(VADisplay)> vaMaxNumConfigEntrypoints = nullptr;
  PFN<VAStatus(VADisplay, VAImageFormat*, int*)> vaQueryImageFormats = nullptr;
  PFN<VAStatus(VADisplay, VAImageFormat*, int, int, VAImage*)> vaCreateImage = nullptr;
  PFN<VAStatus(VADisplay, VAImageID)> vaDestroyImage = nullptr;
  PFN<VAStatus(VADisplay, VASurfaceID, VAImage*)> vaDeriveImage = nullptr;
  PFN<VAStatus(VADisplay, VASurfaceID, int, int, unsigned int, unsigned int, VAImageID)> vaGetImage = nullptr;
  PFN<VAStatus(VADisplay, VASurfaceID, VAImageID, int, int, unsigned int, unsigned int, int, int, unsigned int, unsigned int)> vaPutImage = nullptr;
  PFN<VAStatus(VADisplay, VABufferID, void**)> vaMapBufferCoded = nullptr;

private:
  LibVA() = default;
  LibVA(const LibVA&) = delete;
  LibVA& operator=(const LibVA&) = delete;

  DynamicLoader libva_;
  DynamicLoader libva_drm_;
  bool loaded_ = false;
  std::mutex load_mutex_;
};

} // namespace openmedia
