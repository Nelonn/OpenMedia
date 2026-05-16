#pragma once

#include <openmedia/macro.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef void* VADisplay;

typedef struct OMVAAPIInit {
    VADisplay display;
    int drm_fd;
} OMVAAPIInit;

typedef struct OMVAAPIContext OMVAAPIContext;

OPENMEDIA_ABI
OMVAAPIContext* HWVAAPIContext_create(OMVAAPIInit init);

OPENMEDIA_ABI
void HWVAAPIContext_delete(OMVAAPIContext* context);

OPENMEDIA_ABI
VADisplay HWVAAPIContext_getDisplay(OMVAAPIContext* context);

#if defined(__cplusplus)
}
#endif
