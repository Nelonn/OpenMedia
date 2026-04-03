#include <openmedia/hw_dx11.h>
#include <openmedia/log.hpp>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <memory>
#include <vector>

using namespace openmedia;

struct OMDX11Context {
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* device_context = nullptr;
  ID3D11VideoDevice* video_device = nullptr;
  ID3D11VideoContext* video_context = nullptr;
  int adapter_index = -1;
  bool owns_device = false;

  OMDX11Context() = default;

  ~OMDX11Context() {
    if (video_context) {
      video_context->Release();
      video_context = nullptr;
    }
    if (video_device) {
      video_device->Release();
      video_device = nullptr;
    }
    if (owns_device) {
      if (device_context) {
        device_context->Release();
        device_context = nullptr;
      }
      if (device) {
        device->Release();
        device = nullptr;
      }
    }
  }

  bool initialize(const OMDX11Init& init) {
    adapter_index = init.adapter_index;

    if (init.device) {
      device = init.device;
      device->AddRef();

      if (init.device_context) {
        device_context = init.device_context;
        device_context->AddRef();
      } else {
        device->GetImmediateContext(&device_context);
      }
    } else {
      // Create our own D3D11 device
      owns_device = true;

      HRESULT hr = S_OK;
      UINT create_device_flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

#ifdef _DEBUG
      create_device_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

      D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
      };
      D3D_FEATURE_LEVEL feature_level;

      IDXGIAdapter* adapter = nullptr;
      if (adapter_index >= 0) {
        IDXGIFactory1* factory = nullptr;
        hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
          return false;
        }

        factory->EnumAdapters(adapter_index, &adapter);
        factory->Release();

        if (!adapter) {
          return false;
        }
      }

      hr = D3D11CreateDevice(
        adapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        create_device_flags,
        feature_levels,
        ARRAYSIZE(feature_levels),
        D3D11_SDK_VERSION,
        &device,
        &feature_level,
        &device_context
      );

      if (adapter) {
        adapter->Release();
      }

      if (FAILED(hr)) {
        return false;
      }
    }

    // Get video device interfaces
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&video_device));
    if (FAILED(hr)) {
      return false;
    }

    hr = device_context->QueryInterface(IID_PPV_ARGS(&video_context));
    if (FAILED(hr)) {
      return false;
    }

    return true;
  }
};

/*struct OMDX11Picture {
  ID3D11VideoDecoderOutputView* decoder_output = nullptr;
  ID3D11ShaderResourceView* shader_resource = nullptr;
  ID3D11Texture2D* texture = nullptr;

  ~OMDX11Picture() {
    if (decoder_output) {
      decoder_output->Release();
      decoder_output = nullptr;
    }
    if (shader_resource) {
      shader_resource->Release();
      shader_resource = nullptr;
    }
    if (texture) {
      texture->Release();
      texture = nullptr;
    }
  }
};*/

OMDX11Context* HWD3D11Context_create(OMDX11Init init) {
  auto* context = static_cast<OMDX11Context*>(malloc(sizeof(OMDX11Context)));
  if (!context) return nullptr;

  new (context) OMDX11Context();

  if (!context->initialize(init)) {
    context->~OMDX11Context();
    free(context);
    return nullptr;
  }

  return context;
}

void HWD3D11Context_delete(OMDX11Context* context) {
  if (!context) return;
  context->~OMDX11Context();
  free(context);
}

ID3D11Device* HWD3D11Context_getDevice(OMDX11Context* context) {
  if (!context) return nullptr;
  return context->device;
}

ID3D11VideoDevice* HWD3D11Context_getVideoDevice(OMDX11Context* context) {
  if (!context) return nullptr;
  return context->video_device;
}

ID3D11VideoContext* HWD3D11Context_getVideoContext(OMDX11Context* context) {
  if (!context) return nullptr;
  return context->video_context;
}

OMDX11Picture* HWD3D11Context_createPicture(OMDX11Context* context) {
  if (!context || !context->video_device) return nullptr;

  auto* picture = static_cast<OMDX11Picture*>(malloc(sizeof(OMDX11Picture)));
  if (!picture) return nullptr;

  new (picture) OMDX11Picture();
  return picture;
}

void HWD3D11Picture_delete(OMDX11Picture* picture) {
  if (!picture) return;
  picture->~OMDX11Picture();
  free(picture);
}
