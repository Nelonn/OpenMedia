#pragma once

#if defined(_WIN32)
#include <openmedia/hw_dx11.h>
#include <openmedia/hw_dx12.h>
#endif
#include <openmedia/hw_vulkan.h>
#include <openmedia/video.hpp>

namespace openmedia {

#if defined(_WIN32)
// Wraps a D3D11 surface that belongs to somebody else — the decoder's frame
// pool, an encoder's input, whatever produced it. Lifetime is the producer's
// business, so nothing is released here; a producer that needs to reclaim the
// surface derives from this and does it in its own destructor.
class DX11HardwarePicture : public HardwarePicture {
public:
  OMDX11Picture* pic;
  explicit DX11HardwarePicture(OMDX11Picture* p)
      : HardwarePicture(HWDeviceType::DX11, p), pic(p) {}
  ~DX11HardwarePicture() override = default;

  auto texture() const -> ID3D11Texture2D* { return pic ? pic->texture : nullptr; }
};

class DX12HardwarePicture : public HardwarePicture {
public:
  OMDX12Picture* pic;
  explicit DX12HardwarePicture(OMDX12Picture* p)
      : HardwarePicture(HWDeviceType::DX12, p), pic(p) {}
};
#endif

// VulkanHardwarePicture is already defined in include/openmedia/hw_vulkan.h

} // namespace openmedia
