#pragma once

#include <openmedia/hw_vulkan.h>

struct OMVulkanContext {
  VkDevice vk_device = VK_NULL_HANDLE;

  explicit OMVulkanContext(OMVulkanInit init);
};
