#pragma once

#include <vulkan/vulkan.h>
#include <vk_video/vulkan_video_codecs_common.h>
#include <vk_video/vulkan_video_codec_h264std.h>
#include <vk_video/vulkan_video_codec_h264std_decode.h>
#include <vk_video/vulkan_video_codec_h264std_encode.h>
#include <vk_video/vulkan_video_codec_h265std.h>
#include <vk_video/vulkan_video_codec_h265std_decode.h>
#include <vk_video/vulkan_video_codec_h265std_encode.h>
#include <vk_video/vulkan_video_codec_av1std.h>
#include <vk_video/vulkan_video_codec_av1std_decode.h>
#include <vk_video/vulkan_video_codec_av1std_encode.h>
#include <vk_video/vulkan_video_codec_vp9std.h>
#include <vk_video/vulkan_video_codec_vp9std_decode.h>
#include <openmedia/macro.h>

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct OMVulkanInit {
  PFN_vkGetInstanceProcAddr proc;
  const VkAllocationCallbacks* allocator;
  VkInstance instance;
  VkPhysicalDevice physical_device;
  VkDevice device;
  uint32_t queue_family_index;
  uint32_t video_decode_queue_family_index;
  uint32_t video_encode_queue_family_index;
} OMVulkanInit;

typedef struct OMVulkanPicture OMVulkanPicture;

typedef struct OMVulkanContext OMVulkanContext;

OPENMEDIA_ABI
OMVulkanContext* HWVulkanContext_create(OMVulkanInit init);

OPENMEDIA_ABI
void HWVulkanContext_delete(OMVulkanContext* context);

OPENMEDIA_ABI
OMVulkanPicture* HWVulkanContext_createPicture(OMVulkanContext* context);

OPENMEDIA_ABI
void HWVulkanPicture_delete(OMVulkanPicture* picture);

OPENMEDIA_ABI
void HWVulkanContext_resolvePicture(OMVulkanContext* context, OMVulkanPicture* src, void* dst_y, uint32_t stride_y, void* dst_uv, uint32_t stride_uv, uint32_t width, uint32_t height);

struct OMVulkanPicture {
  VkImage image;
  VkImageView view;
  VkDeviceMemory memory;
  uint32_t layer;
  VkImageLayout layout;
};

#if defined(__cplusplus)
} // extern "C"

#include <openmedia/video.hpp>

namespace openmedia {

class OPENMEDIA_ABI VulkanHardwarePicture : public HardwarePicture {
public:
  OMVulkanPicture* picture = nullptr;
  VulkanHardwarePicture(OMVulkanPicture* pic)
      : HardwarePicture(HWDeviceType::VULKAN), picture(pic) {}
};

} // namespace openmedia

extern "C" {
#endif

#if defined(__cplusplus)
}
#endif
