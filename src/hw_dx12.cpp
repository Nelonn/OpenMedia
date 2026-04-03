#include <openmedia/hw_dx12.h>
#include <openmedia/log.hpp>
#include <d3d12video.h>
#include <dxgidebug.h>
#include <dxgi1_6.h>
#include <memory>
#include <vector>

using namespace openmedia;

struct OMDX12Context {
  ID3D12Device* device = nullptr;
  ID3D12CommandQueue* command_queue = nullptr;
  ID3D12VideoDevice* video_device = nullptr;
  ID3D12VideoDecodeCommandList* decode_command_list = nullptr;
  ID3D12Fence* fence = nullptr;
  HANDLE fence_event = nullptr;
  UINT64 fence_value = 0;
  int adapter_index = -1;
  bool owns_device = false;

  OMDX12Context() = default;

  ~OMDX12Context() {
    if (fence_event) {
      CloseHandle(fence_event);
      fence_event = nullptr;
    }
    if (decode_command_list) {
      decode_command_list->Release();
      decode_command_list = nullptr;
    }
    if (video_device) {
      video_device->Release();
      video_device = nullptr;
    }
    if (fence) {
      fence->Release();
      fence = nullptr;
    }
    if (owns_device) {
      if (command_queue) {
        command_queue->Release();
        command_queue = nullptr;
      }
      if (device) {
        device->Release();
        device = nullptr;
      }
    }
  }

  bool initialize(const OMDX12Init& init) {
    adapter_index = init.adapter_index;

    if (init.device) {
      device = init.device;
      device->AddRef();

      if (init.command_queue) {
        command_queue = init.command_queue;
        command_queue->AddRef();
      }
    } else {
      owns_device = true;

      HRESULT hr = S_OK;

      IDXGIAdapter1* adapter = nullptr;
      if (adapter_index >= 0) {
        IDXGIFactory7* factory = nullptr;
        hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
          return false;
        }

        factory->EnumAdapters1(adapter_index, &adapter);
        factory->Release();

        if (!adapter) {
          return false;
        }
      }

      hr = D3D12CreateDevice(
        adapter,
        D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(&device)
      );

      if (adapter) {
        adapter->Release();
      }

      if (FAILED(hr)) {
        return false;
      }

      D3D12_COMMAND_QUEUE_DESC queue_desc = {};
      queue_desc.Type = D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE;
      queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
      queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
      queue_desc.NodeMask = 0;

      hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&command_queue));
      if (FAILED(hr)) {
        return false;
      }
    }

    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&video_device));
    if (FAILED(hr)) {
      return false;
    }

    hr = device->CreateCommandAllocator(
      D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE,
      IID_PPV_ARGS(&decode_command_list)
    );
    if (FAILED(hr)) {
      return false;
    }

    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) {
      return false;
    }

    fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event) {
      return false;
    }

    return true;
  }

  bool signal_fence() {
    if (!command_queue || !fence) return false;
    
    HRESULT hr = command_queue->Signal(fence, fence_value + 1);
    if (FAILED(hr)) {
      return false;
    }
    
    fence_value++;
    return true;
  }

  bool wait_for_fence(UINT64 timeout_ms = 1000) {
    if (!fence || !fence_event) return false;

    if (fence->GetCompletedValue() < fence_value) {
      HRESULT hr = fence->SetEventOnCompletion(fence_value, fence_event);
      if (FAILED(hr)) {
        return false;
      }

      DWORD result = WaitForSingleObject(fence_event, timeout_ms);
      if (result != WAIT_OBJECT_0) {
        return false;
      }
    }

    return true;
  }
};

/*struct OMDX12Picture {
  ID3D12Resource* texture = nullptr;
  D3D12_VIDEO_DECODE_REFERENCE_FRAMES reference_frames = {};

  ~OMDX12Picture() {
    if (texture) {
      texture->Release();
      texture = nullptr;
    }
    if (reference_frames.ppTexture2Ds) {
      for (UINT i = 0; i < reference_frames.NumTexture2Ds; i++) {
        if (reference_frames.ppTexture2Ds[i]) {
          reference_frames.ppTexture2Ds[i]->Release();
          reference_frames.ppTexture2Ds[i] = nullptr;
        }
      }
      free(reference_frames.ppTexture2Ds);
      reference_frames.ppTexture2Ds = nullptr;
    }
  }
};*/

OMDX12Context* HWD3D12Context_create(OMDX12Init init) {
  auto* context = static_cast<OMDX12Context*>(malloc(sizeof(OMDX12Context)));
  if (!context) return nullptr;

  new (context) OMDX12Context();

  if (!context->initialize(init)) {
    context->~OMDX12Context();
    free(context);
    return nullptr;
  }

  return context;
}

void HWD3D12Context_delete(OMDX12Context* context) {
  if (!context) return;
  context->~OMDX12Context();
  free(context);
}

ID3D12Device* HWD3D12Context_getDevice(OMDX12Context* context) {
  if (!context) return nullptr;
  return context->device;
}

ID3D12VideoDevice* HWD3D12Context_getVideoDevice(OMDX12Context* context) {
  if (!context) return nullptr;
  return context->video_device;
}

ID3D12VideoDecodeCommandList* HWD3D12Context_getDecodeCommandList(OMDX12Context* context) {
  if (!context) return nullptr;
  return context->decode_command_list;
}

OMDX12Picture* HWD3D12Context_createPicture(OMDX12Context* context) {
  if (!context || !context->video_device) return nullptr;

  auto* picture = static_cast<OMDX12Picture*>(malloc(sizeof(OMDX12Picture)));
  if (!picture) return nullptr;

  new (picture) OMDX12Picture();
  return picture;
}

void HWD3D12Picture_delete(OMDX12Picture* picture) {
  if (!picture) return;
  picture->~OMDX12Picture();
  free(picture);
}
