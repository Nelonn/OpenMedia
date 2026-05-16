#include "hw_vulkan_priv.hpp"

#include <memory>
#include <cstring>

OMVulkanContext::OMVulkanContext(OMVulkanInit init)
    : vk_instance(init.instance),
      vk_physical_device(init.physical_device),
      vk_device(init.device),
      allocator(init.allocator),
      queue_family_index(init.queue_family_index),
      video_decode_queue_family_index(init.video_decode_queue_family_index),
      video_encode_queue_family_index(init.video_encode_queue_family_index) {
  vkGetDeviceQueue(vk_device, queue_family_index, 0, &queue);
  vkGetDeviceQueue(vk_device, video_decode_queue_family_index, 0, &video_decode_queue);
  vkGetDeviceQueue(vk_device, video_encode_queue_family_index, 0, &video_encode_queue);
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
  auto* picture = static_cast<OMVulkanPicture*>(malloc(sizeof(OMVulkanPicture)));
  if (!picture) return nullptr;
  memset(picture, 0, sizeof(OMVulkanPicture));
  return picture;
}
