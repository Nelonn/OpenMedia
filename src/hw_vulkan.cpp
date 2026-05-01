#include "hw_vulkan_priv.hpp"

#include <memory>

OMVulkanContext::OMVulkanContext(OMVulkanInit init) {

}

OMVulkanContext* HWVulkanContext_create(OMVulkanInit init) {
  auto* context = static_cast<OMVulkanContext*>(malloc(sizeof(OMVulkanContext)));
  if (!context) return nullptr;
  new (context) OMVulkanContext(init);
  return context;
}

void HWVulkanContext_delete(OMVulkanContext* context) {
  if (!context) return;
  context->~OMVulkanContext();
  free(context);
}

OMVulkanPicture* HWVulkanContext_createPicture(OMVulkanContext* context) {
  return nullptr;
}
