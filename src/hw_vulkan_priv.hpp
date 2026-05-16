#pragma once

#include <openmedia/hw_vulkan.h>

struct OMVulkanContext {
  VkInstance vk_instance = VK_NULL_HANDLE;
  VkPhysicalDevice vk_physical_device = VK_NULL_HANDLE;
  VkDevice vk_device = VK_NULL_HANDLE;
  const VkAllocationCallbacks* allocator = nullptr;
  uint32_t queue_family_index = 0;
  uint32_t video_decode_queue_family_index = 0;
  uint32_t video_encode_queue_family_index = 0;
  VkQueue queue = VK_NULL_HANDLE;
  VkQueue video_decode_queue = VK_NULL_HANDLE;
  VkQueue video_encode_queue = VK_NULL_HANDLE;

  explicit OMVulkanContext(OMVulkanInit init);
};
