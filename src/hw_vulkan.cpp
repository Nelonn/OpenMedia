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
  GET_INST_FN(vkGetPhysicalDeviceVideoCapabilitiesKHR);
  GET_INST_FN(vkGetPhysicalDeviceVideoFormatPropertiesKHR);

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
  GET_DEV_FN(vkCreateImage);
  GET_DEV_FN(vkDestroyImage);
  GET_DEV_FN(vkCreateImageView);
  GET_DEV_FN(vkDestroyImageView);
  GET_DEV_FN(vkGetBufferMemoryRequirements);
  GET_DEV_FN(vkGetImageMemoryRequirements);
  GET_DEV_FN(vkAllocateMemory);
  GET_DEV_FN(vkFreeMemory);
  GET_DEV_FN(vkBindBufferMemory);
  GET_DEV_FN(vkBindImageMemory);
  GET_DEV_FN(vkMapMemory);
  GET_DEV_FN(vkUnmapMemory);
  GET_DEV_FN(vkInvalidateMappedMemoryRanges);

  GET_DEV_FN(vkCreateFence);
  GET_DEV_FN(vkDestroyFence);
  GET_DEV_FN(vkWaitForFences);
  GET_DEV_FN(vkResetFences);

  GET_DEV_FN(vkCreateVideoSessionKHR);
  GET_DEV_FN(vkDestroyVideoSessionKHR);
  GET_DEV_FN(vkGetVideoSessionMemoryRequirementsKHR);
  GET_DEV_FN(vkBindVideoSessionMemoryKHR);
  GET_DEV_FN(vkCreateVideoSessionParametersKHR);
  GET_DEV_FN(vkUpdateVideoSessionParametersKHR);
  GET_DEV_FN(vkDestroyVideoSessionParametersKHR);
  GET_DEV_FN(vkCmdBeginVideoCodingKHR);
  GET_DEV_FN(vkCmdControlVideoCodingKHR);
  GET_DEV_FN(vkCmdEndVideoCodingKHR);
  GET_DEV_FN(vkCmdPipelineBarrier2KHR);
  GET_DEV_FN(vkCmdDecodeVideoKHR);
  GET_DEV_FN(vkCmdCopyImageToBuffer);
  GET_DEV_FN(vkCmdEncodeVideoKHR);

  #undef GET_INST_FN
  #undef GET_DEV_FN

  if (queue_family_index != 0xFFFFFFFF)
    this->vkGetDeviceQueue(vk_device, queue_family_index, 0, &queue);
  if (video_decode_queue_family_index != 0xFFFFFFFF)
    this->vkGetDeviceQueue(vk_device, video_decode_queue_family_index, 0, &video_decode_queue);
  if (video_encode_queue_family_index != 0xFFFFFFFF)
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

  void HWVulkanContext_resolvePicture(OMVulkanContext* context, OMVulkanPicture* src, void* dst_y, uint32_t stride_y, void* dst_uv, uint32_t stride_uv, uint32_t width, uint32_t height) {
  if (!context || !src || !src->image) return;

  uint32_t src_family = context->video_decode_queue_family_index;
  uint32_t dst_family = context->queue_family_index;

  size_t size = (size_t)width * height * 3 / 2;
  VkBuffer staging_buffer;
  VkDeviceMemory staging_memory;

  VkBufferCreateInfo buf_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buf_info.size = size;
  buf_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  context->vkCreateBuffer(context->vk_device, &buf_info, context->allocator, &staging_buffer);

  VkMemoryRequirements reqs;
  context->vkGetBufferMemoryRequirements(context->vk_device, staging_buffer, &reqs);
  VkMemoryAllocateInfo alloc_info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc_info.allocationSize = reqs.size;
  
  VkPhysicalDeviceMemoryProperties mem_props;
  context->vkGetPhysicalDeviceMemoryProperties(context->vk_physical_device, &mem_props);
  bool is_coherent = false;
  for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
    if ((reqs.memoryTypeBits & (1 << i)) && (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
      alloc_info.memoryTypeIndex = i;
      is_coherent = (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      break;
    }
  }
  context->vkAllocateMemory(context->vk_device, &alloc_info, context->allocator, &staging_memory);
  context->vkBindBufferMemory(context->vk_device, staging_buffer, staging_memory, 0);

  VkCommandPool decode_pool, graphics_pool;
  VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = src_family;
  context->vkCreateCommandPool(context->vk_device, &pool_info, context->allocator, &decode_pool);
  pool_info.queueFamilyIndex = dst_family;
  context->vkCreateCommandPool(context->vk_device, &pool_info, context->allocator, &graphics_pool);

  VkCommandBuffer decode_cb, graphics_cb;
  VkCommandBufferAllocateInfo cb_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cb_info.commandPool = decode_pool;
  cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cb_info.commandBufferCount = 1;
  context->vkAllocateCommandBuffers(context->vk_device, &cb_info, &decode_cb);
  cb_info.commandPool = graphics_pool;
  context->vkAllocateCommandBuffers(context->vk_device, &cb_info, &graphics_cb);

  VkFence fence;
  VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  context->vkCreateFence(context->vk_device, &fence_info, context->allocator, &fence);

  VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  
  // 1. Release from decode queue
  context->vkBeginCommandBuffer(decode_cb, &begin_info);
  VkImageMemoryBarrier2 release_barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  release_barrier.srcStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
  release_barrier.srcAccessMask = VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
  release_barrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
  release_barrier.dstAccessMask = 0;
  release_barrier.oldLayout = src->layout;
  release_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  release_barrier.srcQueueFamilyIndex = src_family;
  release_barrier.dstQueueFamilyIndex = dst_family;
  release_barrier.image = src->image;
  release_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, src->layer, 1};

  VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &release_barrier;
  context->vkCmdPipelineBarrier2KHR(decode_cb, &dep);
  context->vkEndCommandBuffer(decode_cb);

  VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &decode_cb;
  context->vkQueueSubmit(context->video_decode_queue, 1, &submit, fence);
  context->vkWaitForFences(context->vk_device, 1, &fence, VK_TRUE, UINT64_MAX);
  context->vkResetFences(context->vk_device, 1, &fence);

  // 2. Acquire and copy on graphics queue
  context->vkBeginCommandBuffer(graphics_cb, &begin_info);
  VkImageMemoryBarrier2 acquire_barrier = release_barrier;
  acquire_barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
  acquire_barrier.srcAccessMask = 0;
  acquire_barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  acquire_barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  dep.pImageMemoryBarriers = &acquire_barrier;
  context->vkCmdPipelineBarrier2KHR(graphics_cb, &dep);

  VkBufferImageCopy regions[2] = {};
  regions[0].bufferOffset = 0;
  regions[0].imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, src->layer, 1};
  regions[0].imageExtent = {width, height, 1};
  regions[1].bufferOffset = (size_t)width * height;
  regions[1].imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, src->layer, 1};
  regions[1].imageExtent = {width / 2, height / 2, 1};
  
  context->vkCmdCopyImageToBuffer(graphics_cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging_buffer, 2, regions);

  // 3. Release from graphics queue back to decode
  release_barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
  release_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  release_barrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
  release_barrier.dstAccessMask = 0;
  release_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  release_barrier.newLayout = src->layout;
  release_barrier.srcQueueFamilyIndex = dst_family;
  release_barrier.dstQueueFamilyIndex = src_family;
  dep.pImageMemoryBarriers = &release_barrier;
  context->vkCmdPipelineBarrier2KHR(graphics_cb, &dep);
  context->vkEndCommandBuffer(graphics_cb);

  submit.pCommandBuffers = &graphics_cb;
  context->vkQueueSubmit(context->queue, 1, &submit, fence);
  context->vkWaitForFences(context->vk_device, 1, &fence, VK_TRUE, UINT64_MAX);
  context->vkResetFences(context->vk_device, 1, &fence);

  // 4. Acquire back on decode queue
  context->vkBeginCommandBuffer(decode_cb, &begin_info);
  acquire_barrier = release_barrier;
  acquire_barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
  acquire_barrier.srcAccessMask = 0;
  acquire_barrier.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
  acquire_barrier.dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
  dep.pImageMemoryBarriers = &acquire_barrier;
  context->vkCmdPipelineBarrier2KHR(decode_cb, &dep);
  context->vkEndCommandBuffer(decode_cb);
  submit.pCommandBuffers = &decode_cb;
  context->vkQueueSubmit(context->video_decode_queue, 1, &submit, fence);
  context->vkWaitForFences(context->vk_device, 1, &fence, VK_TRUE, UINT64_MAX);

  if (!is_coherent) {
    VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = staging_memory;
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    context->vkInvalidateMappedMemoryRanges(context->vk_device, 1, &range);
  }

  void* data;
  context->vkMapMemory(context->vk_device, staging_memory, 0, size, 0, &data);
  
  uint8_t* src_ptr = (uint8_t*)data;
  uint8_t* d_y = (uint8_t*)dst_y;
  for (uint32_t i = 0; i < height; ++i) {
    std::memcpy(d_y + (size_t)i * stride_y, src_ptr + (size_t)i * width, width);
  }
  
  uint8_t* src_uv = src_ptr + (size_t)width * height;
  uint8_t* d_uv = (uint8_t*)dst_uv;
  for (uint32_t i = 0; i < height / 2; ++i) {
    std::memcpy(d_uv + (size_t)i * stride_uv, src_uv + (size_t)i * width, width);
  }

  context->vkUnmapMemory(context->vk_device, staging_memory);
  context->vkDestroyFence(context->vk_device, fence, context->allocator);
  context->vkDestroyCommandPool(context->vk_device, decode_pool, context->allocator);
  context->vkDestroyCommandPool(context->vk_device, graphics_pool, context->allocator);
  context->vkFreeMemory(context->vk_device, staging_memory, context->allocator);
  context->vkDestroyBuffer(context->vk_device, staging_buffer, context->allocator);
}

