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
      video_encode_queue_family_index(init.video_encode_queue_family_index),
      vkGetInstanceProcAddr(init.proc) {
  
#define GET_INST_FN(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(vk_instance, #name))
#define GET_DEV_FN(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(vk_device, #name))

  GET_INST_FN(vkGetDeviceProcAddr);
  GET_INST_FN(vkGetPhysicalDeviceMemoryProperties);

  GET_DEV_FN(vkGetDeviceQueue);
  GET_DEV_FN(vkCreateCommandPool);
  GET_DEV_FN(vkDestroyCommandPool);
  GET_DEV_FN(vkAllocateCommandBuffers);
  GET_DEV_FN(vkResetCommandBuffer);
  GET_DEV_FN(vkBeginCommandBuffer);
  GET_DEV_FN(vkEndCommandBuffer);
  GET_DEV_FN(vkQueueSubmit);
  GET_DEV_FN(vkQueueWaitIdle);

  GET_DEV_FN(vkCreateBuffer);
  GET_DEV_FN(vkDestroyBuffer);
  GET_DEV_FN(vkGetBufferMemoryRequirements);
  GET_DEV_FN(vkAllocateMemory);
  GET_DEV_FN(vkFreeMemory);
  GET_DEV_FN(vkBindBufferMemory);
  GET_DEV_FN(vkMapMemory);
  GET_DEV_FN(vkUnmapMemory);

  GET_DEV_FN(vkCreateFence);
  GET_DEV_FN(vkDestroyFence);
  GET_DEV_FN(vkWaitForFences);
  GET_DEV_FN(vkResetFences);

  GET_DEV_FN(vkCreateVideoSessionKHR);
  GET_DEV_FN(vkDestroyVideoSessionKHR);
  GET_DEV_FN(vkGetVideoSessionMemoryRequirementsKHR);
  GET_DEV_FN(vkBindVideoSessionMemoryKHR);
  GET_DEV_FN(vkCreateVideoSessionParametersKHR);
  GET_DEV_FN(vkDestroyVideoSessionParametersKHR);
  GET_DEV_FN(vkCmdBeginVideoCodingKHR);
  GET_DEV_FN(vkCmdEndVideoCodingKHR);
  GET_DEV_FN(vkCmdDecodeVideoKHR);
  GET_DEV_FN(vkCmdEncodeVideoKHR);

#undef GET_INST_FN
#undef GET_DEV_FN

  this->vkGetDeviceQueue(vk_device, queue_family_index, 0, &queue);
  this->vkGetDeviceQueue(vk_device, video_decode_queue_family_index, 0, &video_decode_queue);
  this->vkGetDeviceQueue(vk_device, video_encode_queue_family_index, 0, &video_encode_queue);
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
  std::memset(picture, 0, sizeof(OMVulkanPicture));
  return picture;
}

void HWVulkanPicture_delete(OMVulkanPicture* picture) {
  if (!picture) return;
  free(picture);
}

