#pragma once

#include <d3d12.h>
#include <d3d12video.h>
#include <openmedia/macro.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct OMDX12Init {
  ID3D12Device* device;
  ID3D12CommandQueue* command_queue;
  int adapter_index;
} OMDX12Init;

typedef struct OMDX12Picture OMDX12Picture;

typedef struct OMDX12Context OMDX12Context;

OPENMEDIA_ABI
OMDX12Context* HWD3D12Context_create(OMDX12Init init);

OPENMEDIA_ABI
void HWD3D12Context_delete(OMDX12Context* context);

OPENMEDIA_ABI
ID3D12Device* HWD3D12Context_getDevice(OMDX12Context* context);

OPENMEDIA_ABI
ID3D12VideoDevice* HWD3D12Context_getVideoDevice(OMDX12Context* context);

OPENMEDIA_ABI
ID3D12VideoDecodeCommandList* HWD3D12Context_getDecodeCommandList(OMDX12Context* context);

OPENMEDIA_ABI
OMDX12Picture* HWD3D12Context_createPicture(OMDX12Context* context);

OPENMEDIA_ABI
void HWD3D12Picture_delete(OMDX12Picture* picture);

struct OMDX12Picture {
  ID3D12Resource* texture;
  D3D12_VIDEO_DECODE_REFERENCE_FRAMES reference_frames;
};

#if defined(__cplusplus)
}
#endif
