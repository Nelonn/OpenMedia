#include "hw_vaapi_priv.hpp"
#include <cstdlib>
#include <new>

using namespace openmedia;

OMVAAPIContext* HWVAAPIContext_create(OMVAAPIInit init) {
  auto* context = static_cast<OMVAAPIContext*>(std::malloc(sizeof(OMVAAPIContext)));
  if (!context) return nullptr;

  new (context) OMVAAPIContext();

  if (!context->initialize(init)) {
    context->~OMVAAPIContext();
    std::free(context);
    return nullptr;
  }

  return context;
}

void HWVAAPIContext_delete(OMVAAPIContext* context) {
  if (!context) return;
  context->~OMVAAPIContext();
  std::free(context);
}

VADisplay HWVAAPIContext_getDisplay(OMVAAPIContext* context) {
  if (!context) return nullptr;
  return context->display;
}
