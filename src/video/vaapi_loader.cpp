#include "vaapi_loader.hpp"

namespace openmedia {

auto LibVA::getInstance() -> LibVA& {
  static LibVA instance;
  return instance;
}

auto LibVA::load() -> bool {
  if (loaded_) return true;

  std::lock_guard<std::mutex> lock(load_mutex_);
  if (loaded_) return true;

  libva_.open("libva.so.2");
  if (!libva_.success()) {
    libva_.open("libva.so");
  }

  libva_drm_.open("libva-drm.so.2");
  if (!libva_drm_.success()) {
    libva_drm_.open("libva-drm.so");
  }

  if (!libva_.success() || !libva_drm_.success()) {
    return false;
  }

  vaGetDisplayDRM = libva_drm_.getProcAddress<PFN<VADisplay(int)>>("vaGetDisplayDRM");

  vaInitialize = libva_.getProcAddress<PFN<VAStatus(VADisplay, int*, int*)>>("vaInitialize");
  vaTerminate = libva_.getProcAddress<PFN<VAStatus(VADisplay)>>("vaTerminate");
  vaErrorStr = libva_.getProcAddress<PFN<const char*(VAStatus)>>("vaErrorStr");
  vaCreateConfig = libva_.getProcAddress<PFN<VAStatus(VADisplay, VAProfile, VAEntrypoint, VAConfigAttrib*, int, VAConfigID*)>>("vaCreateConfig");
  vaDestroyConfig = libva_.getProcAddress<PFN<VAStatus(VADisplay, VAConfigID)>>("vaDestroyConfig");
  vaCreateSurfaces = libva_.getProcAddress<PFN<VAStatus(VADisplay, unsigned int, int, int, VASurfaceID*, unsigned int, VASurfaceAttrib*, unsigned int)>>("vaCreateSurfaces");
  vaDestroySurfaces = libva_.getProcAddress<PFN<VAStatus(VADisplay, VASurfaceID*, int)>>("vaDestroySurfaces");
  vaCreateContext = libva_.getProcAddress<PFN<VAStatus(VADisplay, VAConfigID, int, int, int, VASurfaceID*, int, VAContextID*)>>("vaCreateContext");
  vaDestroyContext = libva_.getProcAddress<PFN<VAStatus(VADisplay, VAContextID)>>("vaDestroyContext");
  vaCreateBuffer = libva_.getProcAddress<PFN<VAStatus(VADisplay, VAContextID, VABufferType, unsigned int, unsigned int, void*, VABufferID*)>>("vaCreateBuffer");
  vaDestroyBuffer = libva_.getProcAddress<PFN<VAStatus(VADisplay, VABufferID)>>("vaDestroyBuffer");
  vaMapBuffer = libva_.getProcAddress<PFN<VAStatus(VADisplay, VABufferID, void**)>>("vaMapBuffer");
  vaUnmapBuffer = libva_.getProcAddress<PFN<VAStatus(VADisplay, VABufferID)>>("vaUnmapBuffer");
  vaBeginPicture = libva_.getProcAddress<PFN<VAStatus(VADisplay, VAContextID, VASurfaceID)>>("vaBeginPicture");
  vaRenderPicture = libva_.getProcAddress<PFN<VAStatus(VADisplay, VAContextID, VABufferID*, int)>>("vaRenderPicture");
  vaEndPicture = libva_.getProcAddress<PFN<VAStatus(VADisplay, VAContextID)>>("vaEndPicture");
  vaSyncSurface = libva_.getProcAddress<PFN<VAStatus(VADisplay, VASurfaceID)>>("vaSyncSurface");
  vaQueryConfigProfiles = libva_.getProcAddress<PFN<VAStatus(VADisplay, VAProfile*, int*)>>("vaQueryConfigProfiles");
  vaQueryConfigEntrypoints = libva_.getProcAddress<PFN<VAStatus(VADisplay, VAProfile, VAEntrypoint*, int*)>>("vaQueryConfigEntrypoints");
  vaGetConfigAttributes = libva_.getProcAddress<PFN<VAStatus(VADisplay, VAProfile, VAEntrypoint, VAConfigAttrib*, int)>>("vaGetConfigAttributes");
  vaQuerySurfaceStatus = libva_.getProcAddress<PFN<VAStatus(VADisplay, VASurfaceID, VAStatus*)>>("vaQuerySurfaceStatus");
  vaMaxNumConfigProfiles = libva_.getProcAddress<PFN<int(VADisplay)>>("vaMaxNumConfigProfiles");
  vaMaxNumConfigEntrypoints = libva_.getProcAddress<PFN<int(VADisplay)>>("vaMaxNumConfigEntrypoints");
  vaQueryImageFormats = libva_.getProcAddress<PFN<VAStatus(VADisplay, VAImageFormat*, int*)>>("vaQueryImageFormats");
  vaCreateImage = libva_.getProcAddress<PFN<VAStatus(VADisplay, VAImageFormat*, int, int, VAImage*)>>("vaCreateImage");
  vaDestroyImage = libva_.getProcAddress<PFN<VAStatus(VADisplay, VAImageID)>>("vaDestroyImage");
  vaDeriveImage = libva_.getProcAddress<PFN<VAStatus(VADisplay, VASurfaceID, VAImage*)>>("vaDeriveImage");
  vaGetImage = libva_.getProcAddress<PFN<VAStatus(VADisplay, VASurfaceID, int, int, unsigned int, unsigned int, VAImageID)>>("vaGetImage");
  vaPutImage = libva_.getProcAddress<PFN<VAStatus(VADisplay, VASurfaceID, VAImageID, int, int, unsigned int, unsigned int, int, int, unsigned int, unsigned int)>>("vaPutImage");
  vaMapBufferCoded = libva_.getProcAddress<PFN<VAStatus(VADisplay, VABufferID, void**)>>("vaMapBufferCoded"); 

  if (!vaInitialize || !vaTerminate || !vaCreateConfig || !vaCreateSurfaces || !vaCreateContext || !vaCreateBuffer || !vaRenderPicture || !vaSyncSurface || !vaMaxNumConfigProfiles) {
    return false;
  }

  loaded_ = true;
  return true;
}

auto LibVA::isLoaded() const -> bool {
  return loaded_;
}

} // namespace openmedia
