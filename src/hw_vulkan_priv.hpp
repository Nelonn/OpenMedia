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

  // Global/Instance functions
  PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
  PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
  PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR vkGetPhysicalDeviceVideoCapabilitiesKHR = nullptr;

  // Device functions
  PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
  PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
  PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
  PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
  PFN_vkResetCommandBuffer vkResetCommandBuffer = nullptr;
  PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
  PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
  PFN_vkQueueSubmit vkQueueSubmit = nullptr;
  PFN_vkQueueWaitIdle vkQueueWaitIdle = nullptr;

  // Memory/Buffer/Image functions
  PFN_vkCreateBuffer vkCreateBuffer = nullptr;
  PFN_vkDestroyBuffer vkDestroyBuffer = nullptr;
  PFN_vkCreateImage vkCreateImage = nullptr;
  PFN_vkDestroyImage vkDestroyImage = nullptr;
  PFN_vkCreateImageView vkCreateImageView = nullptr;
  PFN_vkDestroyImageView vkDestroyImageView = nullptr;
  PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
  PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
  PFN_vkAllocateMemory vkAllocateMemory = nullptr;
  PFN_vkFreeMemory vkFreeMemory = nullptr;
  PFN_vkBindBufferMemory vkBindBufferMemory = nullptr;
  PFN_vkBindImageMemory vkBindImageMemory = nullptr;
  PFN_vkMapMemory vkMapMemory = nullptr;
  PFN_vkUnmapMemory vkUnmapMemory = nullptr;

  // Synchronization functions
  PFN_vkCreateFence vkCreateFence = nullptr;
  PFN_vkDestroyFence vkDestroyFence = nullptr;
  PFN_vkWaitForFences vkWaitForFences = nullptr;
  PFN_vkResetFences vkResetFences = nullptr;

  // Video functions
  PFN_vkCreateVideoSessionKHR vkCreateVideoSessionKHR = nullptr;
  PFN_vkDestroyVideoSessionKHR vkDestroyVideoSessionKHR = nullptr;
  PFN_vkGetVideoSessionMemoryRequirementsKHR vkGetVideoSessionMemoryRequirementsKHR = nullptr;
  PFN_vkBindVideoSessionMemoryKHR vkBindVideoSessionMemoryKHR = nullptr;
  PFN_vkCreateVideoSessionParametersKHR vkCreateVideoSessionParametersKHR = nullptr;
  PFN_vkUpdateVideoSessionParametersKHR vkUpdateVideoSessionParametersKHR = nullptr;
  PFN_vkDestroyVideoSessionParametersKHR vkDestroyVideoSessionParametersKHR = nullptr;
  PFN_vkCmdBeginVideoCodingKHR vkCmdBeginVideoCodingKHR = nullptr;
  PFN_vkCmdControlVideoCodingKHR vkCmdControlVideoCodingKHR = nullptr;
  PFN_vkCmdEndVideoCodingKHR vkCmdEndVideoCodingKHR = nullptr;
  PFN_vkCmdDecodeVideoKHR vkCmdDecodeVideoKHR = nullptr;
  PFN_vkCmdEncodeVideoKHR vkCmdEncodeVideoKHR = nullptr;

  explicit OMVulkanContext(OMVulkanInit init);
};
