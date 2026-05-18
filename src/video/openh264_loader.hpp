#pragma once

#include <wels/codec_api.h>
#include <util/dynamic_loader.hpp>
#include <mutex>
#include <util/cpp.hpp>

namespace openmedia {

class OpenH264Loader {
public:
  static auto getInstance() -> OpenH264Loader&;

  auto load() -> bool;
  auto isLoaded() const -> bool;

  using PFN_WelsCreateSVCEncoder = fn_ptr<int EXTAPI(ISVCEncoder**)>;
  using PFN_WelsDestroySVCEncoder = fn_ptr<void EXTAPI(ISVCEncoder*)>;
  using PFN_WelsCreateDecoder = fn_ptr<long EXTAPI(ISVCDecoder**)>;
  using PFN_WelsDestroyDecoder = fn_ptr<void EXTAPI(ISVCDecoder*)>;

  PFN_WelsCreateSVCEncoder WelsCreateSVCEncoder = nullptr;
  PFN_WelsDestroySVCEncoder WelsDestroySVCEncoder = nullptr;
  PFN_WelsCreateDecoder WelsCreateDecoder = nullptr;
  PFN_WelsDestroyDecoder WelsDestroyDecoder = nullptr;

private:
  OpenH264Loader() = default;
  DynamicLoader lib_;
  bool loaded_ = false;
  std::mutex mutex_;
};

} // namespace openmedia
