#pragma once

#include <d3d11.h>
#include <openmedia/macro.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct OMDX11Init {
  ID3D11Device* device;
  ID3D11DeviceContext* device_context;
  int adapter_index;
} OMDX11Init;

typedef struct OMDX11Picture OMDX11Picture;

typedef struct OMDX11Context OMDX11Context;

OPENMEDIA_ABI
OMDX11Context* HWD3D11Context_create(OMDX11Init init);

OPENMEDIA_ABI
void HWD3D11Context_delete(OMDX11Context* context);

OPENMEDIA_ABI
ID3D11Device* HWD3D11Context_getDevice(OMDX11Context* context);

OPENMEDIA_ABI
ID3D11VideoDevice* HWD3D11Context_getVideoDevice(OMDX11Context* context);

OPENMEDIA_ABI
ID3D11VideoContext* HWD3D11Context_getVideoContext(OMDX11Context* context);

OPENMEDIA_ABI
OMDX11Picture* HWD3D11Context_createPicture(OMDX11Context* context);

OPENMEDIA_ABI
void HWD3D11Picture_delete(OMDX11Picture* picture);

struct OMDX11Picture {
  ID3D11VideoDecoderOutputView* decoder_output;
  ID3D11ShaderResourceView* shader_resource;
  ID3D11Texture2D* texture;
};

#if defined(__cplusplus)
}
#endif
