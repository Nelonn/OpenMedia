#pragma once

#include <openmedia/hw_dx12.h>
#include <wrl.h>

using Microsoft::WRL::ComPtr;

struct OMDX12Context {
  ComPtr<ID3D12Device> device = nullptr;
  ComPtr<ID3D12CommandQueue> command_queue = nullptr;
  ComPtr<ID3D12VideoDevice> video_device = nullptr;
  ComPtr<ID3D12VideoDecodeCommandList> decode_command_list = nullptr;
  ComPtr<ID3D12Fence> fence = nullptr;
  HANDLE fence_event = nullptr;
  UINT64 fence_value = 0;
  int adapter_index = -1;
  bool owns_device = false;

  OMDX12Context() = default;

  ~OMDX12Context();

  auto initialize(const OMDX12Init& init) -> bool;

  auto signalFence() -> bool;

  auto waitForFence(UINT64 timeout_ms = 1000) -> bool;
};
