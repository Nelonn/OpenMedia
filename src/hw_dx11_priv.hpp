#pragma once

#include <openmedia/hw_dx11.h>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

struct OMDX11Context {
  ComPtr<ID3D11Device> device = nullptr;
  ComPtr<ID3D11DeviceContext> device_context = nullptr;
  ComPtr<ID3D11VideoDevice> video_device = nullptr;
  ComPtr<ID3D11VideoContext> video_context = nullptr;
  int adapter_index = -1;
  bool owns_device = false;

  OMDX11Context() = default;

  ~OMDX11Context();

  auto initialize(const OMDX11Init& init) -> bool;
};
