#pragma once

#include <openmedia/hw_dx11.h>
#include <openmedia/hw_dx12.h>
#include <openmedia/hw_vulkan.h>
#include <openmedia/video.hpp>

namespace openmedia {

class DX11HardwarePicture : public HardwarePicture {
public:
  OMDX11Picture* pic;
  explicit DX11HardwarePicture(OMDX11Picture* p)
      : HardwarePicture(HWDeviceType::DX11), pic(p) {}
  ~DX11HardwarePicture() override {
    // HWD3D11Picture_delete(pic); // Should we delete here? Depends on ownership.
  }
};

class DX12HardwarePicture : public HardwarePicture {
public:
  OMDX12Picture* pic;
  explicit DX12HardwarePicture(OMDX12Picture* p)
      : HardwarePicture(HWDeviceType::DX12), pic(p) {}
};

// VulkanHardwarePicture is already defined in include/openmedia/hw_vulkan.h

} // namespace openmedia
