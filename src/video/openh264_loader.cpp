#include "openh264_loader.hpp"
#include <openmedia/log.hpp>

namespace openmedia {

auto OpenH264Loader::getInstance() -> OpenH264Loader& {
  static OpenH264Loader instance;
  return instance;
}

auto OpenH264Loader::load() -> bool {
  if (loaded_) return true;
  std::lock_guard<std::mutex> lock(mutex_);
  if (loaded_) return true;

#if defined(_WIN32)
  lib_.open("openh264.dll");
  if (!lib_.success()) {
    lib_.open("openh264-2.6.0-win64.dll");
  }
#elif defined(__APPLE__)
  lib_.open("libopenh264.dylib");
#else
  lib_.open("libopenh264.so");
#endif

  if (!lib_.success()) {
    return false;
  }

  WelsCreateSVCEncoder = lib_.getProcAddress<PFN_WelsCreateSVCEncoder>("WelsCreateSVCEncoder");
  WelsDestroySVCEncoder = lib_.getProcAddress<PFN_WelsDestroySVCEncoder>("WelsDestroySVCEncoder");
  WelsCreateDecoder = lib_.getProcAddress<PFN_WelsCreateDecoder>("WelsCreateDecoder");
  WelsDestroyDecoder = lib_.getProcAddress<PFN_WelsDestroyDecoder>("WelsDestroyDecoder");

  if (!WelsCreateSVCEncoder || !WelsDestroySVCEncoder || !WelsCreateDecoder || !WelsDestroyDecoder) {
    return false;
  }

  loaded_ = true;
  return true;
}

auto OpenH264Loader::isLoaded() const -> bool {
  return loaded_;
}

} // namespace openmedia
