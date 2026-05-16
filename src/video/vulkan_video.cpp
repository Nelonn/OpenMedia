#include <openmedia/hw_vulkan.h>
#include <hw_vulkan_priv.hpp>
#include <openmedia/video.hpp>
#include <codecs.hpp>
#include <vector>
#include <memory>
#include <cstring>
#include <algorithm>
#include <h264.h>
#include <h265_stream.h>
#include <util/io_util.hpp>

namespace openmedia {

struct VulkanDPBEntry {
  OMVulkanPicture picture = {};
  int32_t poc = 0;
  uint32_t frame_num = 0;
  bool is_reference = false;
};

class VulkanDecoder final : public Decoder {
  OMVulkanContext* hw_context_ = nullptr;
  bool initialized_ = false;
  VideoFormat output_format_ = {};
  OMCodecId codec_id_ = OM_CODEC_NONE;
  uint32_t width_ = 0;
  uint32_t height_ = 0;

  VkVideoProfileInfoKHR video_profile_ = {VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
  VkVideoDecodeH264ProfileInfoKHR h264_profile_ = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR};
  VkVideoDecodeH265ProfileInfoKHR h265_profile_ = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR};
  VkVideoDecodeAV1ProfileInfoKHR av1_profile_ = {VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PROFILE_INFO_KHR};
  VkVideoDecodeVP9ProfileInfoKHR vp9_profile_ = {VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_PROFILE_INFO_KHR};

  VkVideoSessionKHR video_session_ = VK_NULL_HANDLE;
  VkVideoSessionParametersKHR session_params_ = VK_NULL_HANDLE;
  std::vector<VkDeviceMemory> session_memory_;

  bool coincide_supported_ = false;
  VkImage dpb_image_ = VK_NULL_HANDLE;
  VkDeviceMemory dpb_memory_ = VK_NULL_HANDLE;
  VkImageView dpb_image_view_ = VK_NULL_HANDLE;
  std::vector<VulkanDPBEntry> dpb_slots_;

  VkImage output_image_ = VK_NULL_HANDLE;
  VkDeviceMemory output_memory_ = VK_NULL_HANDLE;
  VkImageView output_view_ = VK_NULL_HANDLE;
  VkImageLayout output_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

  uint32_t next_slot_ = 0;
  static constexpr uint32_t MAX_DPB_SLOTS = 16;
  uint32_t min_bitstream_alignment_ = 128;

  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  std::vector<VkCommandBuffer> command_buffers_;
  VkFence decode_fence_ = VK_NULL_HANDLE;
  VkBuffer bitstream_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory bitstream_memory_ = VK_NULL_HANDLE;
  void* bitstream_ptr_ = nullptr;
  static constexpr size_t BITSTREAM_SIZE = 8 * 1024 * 1024;

  // Bitstream parsers
  h264::SPS h264_sps_ = {};
  h264::PPS h264_pps_ = {};
  bool has_h264_sps_ = false;
  bool has_h264_pps_ = false;

  h265_stream_t* h265_stream_ = nullptr;
  bool has_h265_sps_ = false;
  bool has_h265_pps_ = false;
  bool first_decode_ = true;

  OMVulkanPicture output_pic_proxy_ = {};

public:
  VulkanDecoder() = default;
  ~VulkanDecoder() override { release(); }

#define VK(name) hw_context_->name

  auto configure(const DecoderOptions& options) -> OMError override {
    if (!options.hw_device.has_value() || !options.hw_device->context || options.hw_device->type != HWDeviceType::VULKAN) {
      release();
      return OM_CODEC_HWACCEL_FAILED;
    }
    hw_context_ = static_cast<OMVulkanContext*>(options.hw_device->context);
    release();

    codec_id_ = options.format.codec_id;
    width_ = options.format.video.width;
    height_ = options.format.video.height;
    first_decode_ = true;

    video_profile_ = {VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
    video_profile_.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    video_profile_.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    video_profile_.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;

    if (codec_id_ == OM_CODEC_H264) {
      h264_profile_ = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR};
      h264_profile_.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN;
      h264_profile_.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
      video_profile_.pNext = &h264_profile_;
      video_profile_.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    } else if (codec_id_ == OM_CODEC_H265) {
      h265_profile_ = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR};
      h265_profile_.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
      video_profile_.pNext = &h265_profile_;
      video_profile_.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
    } else if (codec_id_ == OM_CODEC_AV1) {
      av1_profile_ = {VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PROFILE_INFO_KHR};
      av1_profile_.stdProfile = STD_VIDEO_AV1_PROFILE_MAIN;
      video_profile_.pNext = &av1_profile_;
      video_profile_.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR;
    } else if (codec_id_ == OM_CODEC_VP9) {
      vp9_profile_ = {VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_PROFILE_INFO_KHR};
      vp9_profile_.stdProfile = STD_VIDEO_VP9_PROFILE_0;
      video_profile_.pNext = &vp9_profile_;
      video_profile_.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR;
    } else {
      return OM_CODEC_NOT_SUPPORTED;
    }

    VkVideoDecodeH264CapabilitiesKHR h264_caps = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR};
    VkVideoDecodeH265CapabilitiesKHR h265_caps = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR};
    VkVideoDecodeAV1CapabilitiesKHR av1_caps = {VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_CAPABILITIES_KHR};
    VkVideoDecodeVP9CapabilitiesKHR vp9_caps = {VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_CAPABILITIES_KHR};

    VkVideoCapabilitiesKHR video_caps = {VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR};
    VkVideoDecodeCapabilitiesKHR decode_caps = {VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR};
    video_caps.pNext = &decode_caps;
    
    if (codec_id_ == OM_CODEC_H264) decode_caps.pNext = &h264_caps;
    else if (codec_id_ == OM_CODEC_H265) decode_caps.pNext = &h265_caps;
    else if (codec_id_ == OM_CODEC_AV1) decode_caps.pNext = &av1_caps;
    else if (codec_id_ == OM_CODEC_VP9) decode_caps.pNext = &vp9_caps;

    if (VK(vkGetPhysicalDeviceVideoCapabilitiesKHR)(hw_context_->vk_physical_device, &video_profile_, &video_caps) != VK_SUCCESS) {
      return OM_CODEC_HWACCEL_FAILED;
    }

    coincide_supported_ = decode_caps.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR;
    min_bitstream_alignment_ = video_caps.minBitstreamBufferSizeAlignment;

    VkVideoSessionCreateInfoKHR session_info = {VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR};
    session_info.pVideoProfile = &video_profile_;
    session_info.maxCodedExtent = {width_, height_};
    session_info.referencePictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    session_info.pictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    session_info.maxDpbSlots = MAX_DPB_SLOTS;
    session_info.maxActiveReferencePictures = MAX_DPB_SLOTS;
    session_info.queueFamilyIndex = hw_context_->video_decode_queue_family_index;
    session_info.pStdHeaderVersion = &video_caps.stdHeaderVersion;

    if (VK(vkCreateVideoSessionKHR)(hw_context_->vk_device, &session_info, hw_context_->allocator, &video_session_) != VK_SUCCESS) {
      return OM_CODEC_HWACCEL_FAILED;
    }

    uint32_t mem_req_count = 0;
    VK(vkGetVideoSessionMemoryRequirementsKHR)(hw_context_->vk_device, video_session_, &mem_req_count, nullptr);
    std::vector<VkVideoSessionMemoryRequirementsKHR> mem_reqs(mem_req_count, {VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR});
    VK(vkGetVideoSessionMemoryRequirementsKHR)(hw_context_->vk_device, video_session_, &mem_req_count, mem_reqs.data());

    std::vector<VkBindVideoSessionMemoryInfoKHR> bind_infos;
    for (const auto& req : mem_reqs) {
      VkMemoryAllocateInfo alloc_info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      alloc_info.allocationSize = req.memoryRequirements.size;
      alloc_info.memoryTypeIndex = findMemoryType(req.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      VkDeviceMemory memory;
      VK(vkAllocateMemory)(hw_context_->vk_device, &alloc_info, hw_context_->allocator, &memory);
      session_memory_.push_back(memory);

      VkBindVideoSessionMemoryInfoKHR bind_info = {VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR};
      bind_info.sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR;
      bind_info.memoryBindIndex = req.memoryBindIndex;
      bind_info.memory = memory;
      bind_info.memorySize = req.memoryRequirements.size;
      bind_infos.push_back(bind_info);
    }
    VK(vkBindVideoSessionMemoryKHR)(hw_context_->vk_device, video_session_, (uint32_t)bind_infos.size(), bind_infos.data());

    // DPB Image
    VkVideoProfileListInfoKHR profile_list = {VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR};
    profile_list.pProfiles = &video_profile_;
    profile_list.profileCount = 1;

    VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.pNext = &profile_list;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    image_info.extent = {width_, height_, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = MAX_DPB_SLOTS;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;
    if (coincide_supported_) image_info.usage |= VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK(vkCreateImage)(hw_context_->vk_device, &image_info, hw_context_->allocator, &dpb_image_);

    VkMemoryRequirements img_reqs;
    VK(vkGetImageMemoryRequirements)(hw_context_->vk_device, dpb_image_, &img_reqs);
    VkMemoryAllocateInfo img_alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    img_alloc.allocationSize = img_reqs.size;
    img_alloc.memoryTypeIndex = findMemoryType(img_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK(vkAllocateMemory)(hw_context_->vk_device, &img_alloc, hw_context_->allocator, &dpb_memory_);
    VK(vkBindImageMemory)(hw_context_->vk_device, dpb_image_, dpb_memory_, 0);

    VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = dpb_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view_info.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = MAX_DPB_SLOTS;
    VK(vkCreateImageView)(hw_context_->vk_device, &view_info, hw_context_->allocator, &dpb_image_view_);

    dpb_slots_.resize(MAX_DPB_SLOTS);
    for (uint32_t i = 0; i < MAX_DPB_SLOTS; ++i) {
      dpb_slots_[i].picture.image = dpb_image_;
      dpb_slots_[i].picture.view = dpb_image_view_;
      dpb_slots_[i].picture.layer = i;
      dpb_slots_[i].picture.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    if (!coincide_supported_) {
      image_info.arrayLayers = 1;
      image_info.usage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      VK(vkCreateImage)(hw_context_->vk_device, &image_info, hw_context_->allocator, &output_image_);
      VK(vkGetImageMemoryRequirements)(hw_context_->vk_device, output_image_, &img_reqs);
      img_alloc.allocationSize = img_reqs.size;
      img_alloc.memoryTypeIndex = findMemoryType(img_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      VK(vkAllocateMemory)(hw_context_->vk_device, &img_alloc, hw_context_->allocator, &output_memory_);
      VK(vkBindImageMemory)(hw_context_->vk_device, output_image_, output_memory_, 0);

      view_info.image = output_image_;
      view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view_info.subresourceRange.layerCount = 1;
      VK(vkCreateImageView)(hw_context_->vk_device, &view_info, hw_context_->allocator, &output_view_);
      
      output_pic_proxy_.image = output_image_;
      output_pic_proxy_.view = output_view_;
      output_pic_proxy_.memory = output_memory_;
      output_pic_proxy_.layer = 0;
      output_pic_proxy_.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = hw_context_->video_decode_queue_family_index;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK(vkCreateCommandPool)(hw_context_->vk_device, &pool_info, hw_context_->allocator, &command_pool_);

    VkCommandBufferAllocateInfo cb_alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb_alloc.commandPool = command_pool_;
    cb_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_alloc.commandBufferCount = 1;
    command_buffers_.resize(1);
    VK(vkAllocateCommandBuffers)(hw_context_->vk_device, &cb_alloc, command_buffers_.data());

    VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK(vkCreateFence)(hw_context_->vk_device, &fence_info, hw_context_->allocator, &decode_fence_);

    VkBufferCreateInfo bit_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bit_info.pNext = &profile_list;
    bit_info.size = BITSTREAM_SIZE;
    bit_info.usage = VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR;
    VK(vkCreateBuffer)(hw_context_->vk_device, &bit_info, hw_context_->allocator, &bitstream_buffer_);

    VkMemoryRequirements bit_mem_reqs;
    VK(vkGetBufferMemoryRequirements)(hw_context_->vk_device, bitstream_buffer_, &bit_mem_reqs);
    VkMemoryAllocateInfo bit_alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    bit_alloc.allocationSize = bit_mem_reqs.size;
    bit_alloc.memoryTypeIndex = findMemoryType(bit_mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK(vkAllocateMemory)(hw_context_->vk_device, &bit_alloc, hw_context_->allocator, &bitstream_memory_);
    VK(vkBindBufferMemory)(hw_context_->vk_device, bitstream_buffer_, bitstream_memory_, 0);
    VK(vkMapMemory)(hw_context_->vk_device, bitstream_memory_, 0, BITSTREAM_SIZE, 0, &bitstream_ptr_);

    if (codec_id_ == OM_CODEC_H265) h265_stream_ = h265_new();
    if (codec_id_ == OM_CODEC_AV1) updateSessionParametersAV1();

    output_format_.width = width_;
    output_format_.height = height_;
    output_format_.format = OM_FORMAT_NV12;
    initialized_ = true;
    return OM_SUCCESS;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    if (!initialized_) return Err(OM_COMMON_NOT_INITIALIZED);
    if (packet.bytes.empty()) return Ok(std::vector<Frame>{});

    if (codec_id_ == OM_CODEC_H264) {
      h264::Bitstream bs;
      bs.init(packet.bytes.data(), packet.bytes.size());
      while (h264::find_next_nal(&bs)) {
        h264::NALHeader nal;
        if (!h264::read_nal_header(&nal, &bs)) continue;
        if (nal.type == h264::NAL_UNIT_TYPE_SPS) {
          h264::read_sps(&h264_sps_, &bs);
          has_h264_sps_ = true;
          updateSessionParametersH264();
        } else if (nal.type == h264::NAL_UNIT_TYPE_PPS) {
          h264::read_pps(&h264_pps_, &bs);
          has_h264_pps_ = true;
          updateSessionParametersH264();
        } else if (nal.type == h264::NAL_UNIT_TYPE_CODED_SLICE_IDR || nal.type == h264::NAL_UNIT_TYPE_CODED_SLICE_NON_IDR) {
          if (!has_h264_sps_ || !has_h264_pps_ || !session_params_) continue;
          h264::SliceHeader slice;
          h264::read_slice_header(&slice, &nal, &h264_pps_, &h264_sps_, &bs);
          
          uint32_t current_idx = next_slot_;
          VulkanDPBEntry* slot = &dpb_slots_[current_idx];
          next_slot_ = (next_slot_ + 1) % MAX_DPB_SLOTS;

          recordDecodeH264(slot, current_idx, nal, slice, packet);
          
          Frame frame = {};
          frame.pts = packet.pts;
          frame.dts = packet.dts;
          Picture pic;
          pic.format = output_format_.format;
          pic.width = output_format_.width;
          pic.height = output_format_.height;
          pic.buffer = std::make_shared<VulkanHardwarePicture>(coincide_supported_ ? &slot->picture : &output_pic_proxy_);
          frame.data = std::move(pic);
          return Ok(std::vector<Frame>{std::move(frame)});
        }
      }
    } else if (codec_id_ == OM_CODEC_H265) {
      int nal_start, nal_end;
      uint8_t* p = const_cast<uint8_t*>(packet.bytes.data());
      int sz = static_cast<int>(packet.bytes.size());
      while (find_nal_unit(p, sz, &nal_start, &nal_end) > 0) {
        read_debug_nal_unit(h265_stream_, p + nal_start, nal_end - nal_start);
        if (h265_stream_->nal->nal_unit_type == NAL_UNIT_SPS) {
          has_h265_sps_ = true;
          updateSessionParametersH265();
        } else if (h265_stream_->nal->nal_unit_type == NAL_UNIT_PPS) {
          has_h265_pps_ = true;
          updateSessionParametersH265();
        } else if (h265_stream_->nal->nal_unit_type >= NAL_UNIT_CODED_SLICE_TRAIL_N && h265_stream_->nal->nal_unit_type <= NAL_UNIT_CODED_SLICE_RASL_R) {
          if (!has_h265_sps_ || !has_h265_pps_ || !session_params_) continue;
          
          uint32_t current_idx = next_slot_;
          VulkanDPBEntry* slot = &dpb_slots_[current_idx];
          next_slot_ = (next_slot_ + 1) % MAX_DPB_SLOTS;

          recordDecodeH265(slot, current_idx, packet);
          Frame frame = {};
          frame.pts = packet.pts;
          frame.dts = packet.dts;
          Picture pic;
          pic.format = output_format_.format;
          pic.width = output_format_.width;
          pic.height = output_format_.height;
          pic.buffer = std::make_shared<VulkanHardwarePicture>(coincide_supported_ ? &slot->picture : &output_pic_proxy_);
          frame.data = std::move(pic);
          return Ok(std::vector<Frame>{std::move(frame)});
        }
        p += nal_end; sz -= nal_end;
      }
    }
    return Ok(std::vector<Frame>{});
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!initialized_) return std::nullopt;
    DecodingInfo info = {};
    info.media_type = OM_MEDIA_VIDEO;
    info.video_format = output_format_;
    return info;
  }

  void flush() override { dpb_slots_.clear(); }

private:
  uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    VK(vkGetPhysicalDeviceMemoryProperties)(hw_context_->vk_physical_device, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
      if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) return i;
    }
    return 0;
  }

  void updateSessionParametersH264() {
    if (!has_h264_sps_ || !has_h264_pps_) return;
    if (session_params_) {
      VK(vkDestroyVideoSessionParametersKHR)(hw_context_->vk_device, session_params_, hw_context_->allocator);
      session_params_ = nullptr;
    }

    StdVideoH264SequenceParameterSet sps = {};
    sps.profile_idc = (StdVideoH264ProfileIdc)h264_sps_.profile_idc;
    sps.level_idc = (StdVideoH264LevelIdc)h264_sps_.level_idc;
    sps.seq_parameter_set_id = (uint8_t)h264_sps_.seq_parameter_set_id;
    sps.chroma_format_idc = (StdVideoH264ChromaFormatIdc)h264_sps_.chroma_format_idc;
    sps.pic_width_in_mbs_minus1 = (uint16_t)h264_sps_.pic_width_in_mbs_minus1;
    sps.pic_height_in_map_units_minus1 = (uint16_t)h264_sps_.pic_height_in_map_units_minus1;
    sps.bit_depth_luma_minus8 = (uint8_t)h264_sps_.bit_depth_luma_minus8;
    sps.bit_depth_chroma_minus8 = (uint8_t)h264_sps_.bit_depth_chroma_minus8;
    sps.log2_max_frame_num_minus4 = (uint8_t)h264_sps_.log2_max_frame_num_minus4;
    sps.pic_order_cnt_type = (StdVideoH264PocType)h264_sps_.pic_order_cnt_type;
    sps.log2_max_pic_order_cnt_lsb_minus4 = (uint8_t)h264_sps_.log2_max_pic_order_cnt_lsb_minus4;
    sps.flags.frame_mbs_only_flag = h264_sps_.frame_mbs_only_flag;
    sps.flags.direct_8x8_inference_flag = h264_sps_.direct_8x8_inference_flag;
    sps.flags.vui_parameters_present_flag = h264_sps_.vui_parameters_present_flag;

    StdVideoH264PictureParameterSet pps = {};
    pps.seq_parameter_set_id = (uint8_t)h264_pps_.seq_parameter_set_id;
    pps.pic_parameter_set_id = (uint8_t)h264_pps_.pic_parameter_set_id;
    pps.num_ref_idx_l0_default_active_minus1 = (uint8_t)h264_pps_.num_ref_idx_l0_active_minus1;
    pps.num_ref_idx_l1_default_active_minus1 = (uint8_t)h264_pps_.num_ref_idx_l1_active_minus1;
    pps.weighted_bipred_idc = (StdVideoH264WeightedBipredIdc)h264_pps_.weighted_bipred_idc;
    pps.pic_init_qp_minus26 = (int8_t)h264_pps_.pic_init_qp_minus26;
    pps.chroma_qp_index_offset = (int8_t)h264_pps_.chroma_qp_index_offset;
    pps.second_chroma_qp_index_offset = (int8_t)h264_pps_.second_chroma_qp_index_offset;
    pps.flags.entropy_coding_mode_flag = h264_pps_.entropy_coding_mode_flag;
    pps.flags.weighted_pred_flag = h264_pps_.weighted_pred_flag;
    pps.flags.deblocking_filter_control_present_flag = h264_pps_.deblocking_filter_control_present_flag;
    pps.flags.constrained_intra_pred_flag = h264_pps_.constrained_intra_pred_flag;
    pps.flags.redundant_pic_cnt_present_flag = h264_pps_.redundant_pic_cnt_present_flag;
    pps.flags.transform_8x8_mode_flag = h264_pps_.transform_8x8_mode_flag;

    VkVideoDecodeH264SessionParametersAddInfoKHR h264_add_info = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR};
    h264_add_info.stdSPSCount = 1;
    h264_add_info.pStdSPSs = &sps;
    h264_add_info.stdPPSCount = 1;
    h264_add_info.pStdPPSs = &pps;

    VkVideoDecodeH264SessionParametersCreateInfoKHR h264_params_info = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR};
    h264_params_info.maxStdSPSCount = 1;
    h264_params_info.maxStdPPSCount = 1;
    h264_params_info.pParametersAddInfo = &h264_add_info;

    VkVideoSessionParametersCreateInfoKHR params_info = {VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR};
    params_info.pNext = &h264_params_info;
    params_info.videoSession = video_session_;
    VK(vkCreateVideoSessionParametersKHR)(hw_context_->vk_device, &params_info, hw_context_->allocator, &session_params_);
  }

  void updateSessionParametersH265() {
    if (!has_h265_sps_ || !has_h265_pps_) return;
    if (session_params_) {
      VK(vkDestroyVideoSessionParametersKHR)(hw_context_->vk_device, session_params_, hw_context_->allocator);
      session_params_ = nullptr;
    }

    StdVideoH265SequenceParameterSet sps = {};
    sps.sps_video_parameter_set_id = h265_stream_->sps->sps_video_parameter_set_id;
    sps.sps_seq_parameter_set_id = h265_stream_->sps->sps_seq_parameter_set_id;
    sps.chroma_format_idc = (StdVideoH265ChromaFormatIdc)h265_stream_->sps->chroma_format_idc;
    sps.pic_width_in_luma_samples = (uint16_t)h265_stream_->sps->pic_width_in_luma_samples;
    sps.pic_height_in_luma_samples = (uint16_t)h265_stream_->sps->pic_height_in_luma_samples;
    sps.bit_depth_luma_minus8 = (uint8_t)h265_stream_->sps->bit_depth_luma_minus8;
    sps.bit_depth_chroma_minus8 = (uint8_t)h265_stream_->sps->bit_depth_chroma_minus8;
    sps.log2_max_pic_order_cnt_lsb_minus4 = (uint8_t)h265_stream_->sps->log2_max_pic_order_cnt_lsb_minus4;
    sps.flags.amp_enabled_flag = h265_stream_->sps->amp_enabled_flag;
    sps.flags.sample_adaptive_offset_enabled_flag = h265_stream_->sps->sample_adaptive_offset_enabled_flag;
    sps.flags.pcm_enabled_flag = h265_stream_->sps->pcm_enabled_flag;

    StdVideoH265PictureParameterSet pps = {};
    pps.pps_pic_parameter_set_id = h265_stream_->pps->pps_pic_parameter_set_id;
    pps.pps_seq_parameter_set_id = h265_stream_->pps->pps_seq_parameter_set_id;
    pps.flags.entropy_coding_sync_enabled_flag = h265_stream_->pps->entropy_coding_sync_enabled_flag;
    pps.flags.weighted_pred_flag = h265_stream_->pps->weighted_pred_flag;
    pps.flags.weighted_bipred_flag = h265_stream_->pps->weighted_bipred_flag;
    pps.flags.transform_skip_enabled_flag = h265_stream_->pps->transform_skip_enabled_flag;
    pps.flags.cu_qp_delta_enabled_flag = h265_stream_->pps->cu_qp_delta_enabled_flag;
    pps.flags.pps_slice_chroma_qp_offsets_present_flag = h265_stream_->pps->pps_slice_chroma_qp_offsets_present_flag;
    pps.flags.weighted_pred_flag = h265_stream_->pps->weighted_pred_flag;
    pps.flags.weighted_bipred_flag = h265_stream_->pps->weighted_bipred_flag;
    pps.flags.transquant_bypass_enabled_flag = h265_stream_->pps->transquant_bypass_enabled_flag;
    pps.flags.tiles_enabled_flag = h265_stream_->pps->tiles_enabled_flag;
    pps.flags.entropy_coding_sync_enabled_flag = h265_stream_->pps->entropy_coding_sync_enabled_flag;

    VkVideoDecodeH265SessionParametersAddInfoKHR h265_add_info = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR};
    h265_add_info.stdSPSCount = 1;
    h265_add_info.pStdSPSs = &sps;
    h265_add_info.stdPPSCount = 1;
    h265_add_info.pStdPPSs = &pps;

    VkVideoDecodeH265SessionParametersCreateInfoKHR h265_params_info = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR};
    h265_params_info.maxStdVPSCount = 0;
    h265_params_info.maxStdSPSCount = 1;
    h265_params_info.maxStdPPSCount = 1;
    h265_params_info.pParametersAddInfo = &h265_add_info;

    VkVideoSessionParametersCreateInfoKHR params_info = {VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR};
    params_info.pNext = &h265_params_info;
    params_info.videoSession = video_session_;
    VK(vkCreateVideoSessionParametersKHR)(hw_context_->vk_device, &params_info, hw_context_->allocator, &session_params_);
  }

  void updateSessionParametersAV1() {
    if (session_params_) {
      VK(vkDestroyVideoSessionParametersKHR)(hw_context_->vk_device, session_params_, hw_context_->allocator);
      session_params_ = nullptr;
    }

    StdVideoAV1SequenceHeader seq = {};

    VkVideoDecodeAV1SessionParametersCreateInfoKHR av1_params_info = {VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_SESSION_PARAMETERS_CREATE_INFO_KHR};
    av1_params_info.pStdSequenceHeader = &seq;

    VkVideoSessionParametersCreateInfoKHR params_info = {VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR};
    params_info.pNext = &av1_params_info;
    params_info.videoSession = video_session_;
    VK(vkCreateVideoSessionParametersKHR)(hw_context_->vk_device, &params_info, hw_context_->allocator, &session_params_);
  }

  void recordDecodeH264(VulkanDPBEntry* slot, uint32_t slot_idx, const h264::NALHeader& nal, const h264::SliceHeader& slice, const Packet& packet) {
    size_t aligned_size = (packet.bytes.size() + min_bitstream_alignment_ - 1) & ~(min_bitstream_alignment_ - 1);
    std::memcpy(bitstream_ptr_, packet.bytes.data(), packet.bytes.size());

    VkCommandBuffer cb = command_buffers_[0];
    VK(vkResetCommandBuffer)(cb, 0);
    VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK(vkBeginCommandBuffer)(cb, &begin_info);

    VkImageMemoryBarrier2 barriers[2];
    uint32_t barrier_count = 0;

    if (slot->picture.layout != VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR) {
      barriers[barrier_count] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      barriers[barrier_count].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
      barriers[barrier_count].srcAccessMask = 0;
      barriers[barrier_count].dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
      barriers[barrier_count].dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
      barriers[barrier_count].oldLayout = slot->picture.layout;
      barriers[barrier_count].newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
      barriers[barrier_count].image = dpb_image_;
      barriers[barrier_count].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, slot_idx, 1};
      barrier_count++;
      slot->picture.layout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
    }

    if (!coincide_supported_ && output_pic_proxy_.layout != VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR) {
      barriers[barrier_count] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      barriers[barrier_count].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
      barriers[barrier_count].srcAccessMask = 0;
      barriers[barrier_count].dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
      barriers[barrier_count].dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
      barriers[barrier_count].oldLayout = output_pic_proxy_.layout;
      barriers[barrier_count].newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR;
      barriers[barrier_count].image = output_image_;
      barriers[barrier_count].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      barrier_count++;
      output_pic_proxy_.layout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR;
    }

    if (barrier_count > 0) {
      VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.imageMemoryBarrierCount = barrier_count;
      dep.pImageMemoryBarriers = barriers;
      VK(vkCmdPipelineBarrier2KHR)(cb, &dep);
    }

    VkVideoDecodeH264PictureInfoKHR h264_pic = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR};
    StdVideoDecodeH264PictureInfo std_pic = {};
    std_pic.pic_parameter_set_id = (uint8_t)h264_pps_.pic_parameter_set_id;
    std_pic.seq_parameter_set_id = (uint8_t)h264_sps_.seq_parameter_set_id;
    std_pic.frame_num = (uint16_t)slice.frame_num;
    std_pic.PicOrderCnt[0] = (int32_t)packet.pts;
    std_pic.PicOrderCnt[1] = (int32_t)packet.pts;
    std_pic.idr_pic_id = (uint16_t)slice.idr_pic_id;
    std_pic.flags.is_intra = (nal.type == h264::NAL_UNIT_TYPE_CODED_SLICE_IDR);
    std_pic.flags.IdrPicFlag = std_pic.flags.is_intra;
    std_pic.flags.is_reference = 1;
    h264_pic.pStdPictureInfo = &std_pic;
    uint32_t slice_offset = 0;
    h264_pic.sliceCount = 1;
    h264_pic.pSliceOffsets = &slice_offset;

    VkVideoPictureResourceInfoKHR dst_pic = {VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
    dst_pic.imageViewBinding = coincide_supported_ ? dpb_image_view_ : output_view_;
    dst_pic.codedExtent = {width_, height_};
    dst_pic.baseArrayLayer = coincide_supported_ ? slot_idx : 0;

    VkVideoReferenceSlotInfoKHR setup_slot = {VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
    setup_slot.slotIndex = (int32_t)slot_idx;
    VkVideoPictureResourceInfoKHR setup_pic = {VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
    setup_pic.imageViewBinding = dpb_image_view_;
    setup_pic.codedExtent = {width_, height_};
    setup_pic.baseArrayLayer = slot_idx;
    setup_slot.pPictureResource = &setup_pic;
    
    StdVideoDecodeH264ReferenceInfo std_ref_info = {};
    std_ref_info.FrameNum = (uint16_t)slice.frame_num;
    std_ref_info.PicOrderCnt[0] = (int32_t)packet.pts;
    std_ref_info.PicOrderCnt[1] = (int32_t)packet.pts;
    VkVideoDecodeH264DpbSlotInfoKHR dpb_slot_info = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR};
    dpb_slot_info.pStdReferenceInfo = &std_ref_info;
    setup_slot.pNext = &dpb_slot_info;

    VkVideoReferenceSlotInfoKHR active_slots[MAX_DPB_SLOTS + 1];
    VkVideoPictureResourceInfoKHR active_pics[MAX_DPB_SLOTS + 1];
    VkVideoDecodeH264DpbSlotInfoKHR active_dpbs[MAX_DPB_SLOTS + 1];
    StdVideoDecodeH264ReferenceInfo active_stds[MAX_DPB_SLOTS + 1];
    uint32_t num_active = 0;

    for (uint32_t i = 0; i < MAX_DPB_SLOTS; ++i) {
      if (dpb_slots_[i].is_reference && i != slot_idx) {
        active_stds[num_active] = {};
        active_stds[num_active].FrameNum = (uint16_t)dpb_slots_[i].frame_num;
        active_stds[num_active].PicOrderCnt[0] = dpb_slots_[i].poc;
        active_stds[num_active].PicOrderCnt[1] = dpb_slots_[i].poc;

        active_dpbs[num_active] = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR};
        active_dpbs[num_active].pStdReferenceInfo = &active_stds[num_active];

        active_pics[num_active] = {VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
        active_pics[num_active].sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
        active_pics[num_active].imageViewBinding = dpb_image_view_;
        active_pics[num_active].codedExtent = {width_, height_};
        active_pics[num_active].baseArrayLayer = i;

        active_slots[num_active] = {VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
        active_slots[num_active].slotIndex = (int32_t)i;
        active_slots[num_active].pPictureResource = &active_pics[num_active];
        active_slots[num_active].pNext = &active_dpbs[num_active];
        num_active++;
      }
    }
    
    uint32_t coding_slot_count = num_active;
    active_slots[coding_slot_count] = setup_slot;
    active_slots[coding_slot_count].slotIndex = -1;
    coding_slot_count++;

    VkVideoBeginCodingInfoKHR begin_coding = {VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
    begin_coding.videoSession = video_session_;
    begin_coding.videoSessionParameters = session_params_;
    begin_coding.referenceSlotCount = coding_slot_count;
    begin_coding.pReferenceSlots = active_slots;
    VK(vkCmdBeginVideoCodingKHR)(cb, &begin_coding);

    if (first_decode_) {
      VkVideoCodingControlInfoKHR control = {VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR};
      control.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
      VK(vkCmdControlVideoCodingKHR)(cb, &control);
      first_decode_ = false;
    }

    VkVideoDecodeInfoKHR decode_info = {VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR};
    decode_info.pNext = &h264_pic;
    decode_info.srcBuffer = bitstream_buffer_;
    decode_info.srcBufferRange = aligned_size;
    decode_info.dstPictureResource = dst_pic;
    decode_info.pSetupReferenceSlot = &setup_slot;
    decode_info.referenceSlotCount = num_active;
    decode_info.pReferenceSlots = num_active > 0 ? active_slots : nullptr;

    VK(vkCmdDecodeVideoKHR)(cb, &decode_info);

    VkVideoEndCodingInfoKHR end_coding = {VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
    VK(vkCmdEndVideoCodingKHR)(cb, &end_coding);

    slot->poc = (int32_t)packet.pts;
    slot->frame_num = (uint16_t)slice.frame_num;
    slot->is_reference = true;

    VK(vkEndCommandBuffer)(cb);

    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    VK(vkQueueSubmit)(hw_context_->video_decode_queue, 1, &submit, decode_fence_);
    VK(vkWaitForFences)(hw_context_->vk_device, 1, &decode_fence_, VK_TRUE, UINT64_MAX);
    VK(vkResetFences)(hw_context_->vk_device, 1, &decode_fence_);
  }

  void recordDecodeH265(VulkanDPBEntry* slot, uint32_t slot_idx, const Packet& packet) {
    size_t aligned_size = (packet.bytes.size() + min_bitstream_alignment_ - 1) & ~(min_bitstream_alignment_ - 1);
    std::memcpy(bitstream_ptr_, packet.bytes.data(), packet.bytes.size());

    VkCommandBuffer cb = command_buffers_[0];
    VK(vkResetCommandBuffer)(cb, 0);
    VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK(vkBeginCommandBuffer)(cb, &begin_info);

    VkImageMemoryBarrier2 barriers[2];
    uint32_t barrier_count = 0;

    if (slot->picture.layout != VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR) {
      barriers[barrier_count] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      barriers[barrier_count].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
      barriers[barrier_count].srcAccessMask = 0;
      barriers[barrier_count].dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
      barriers[barrier_count].dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
      barriers[barrier_count].oldLayout = slot->picture.layout;
      barriers[barrier_count].newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
      barriers[barrier_count].image = dpb_image_;
      barriers[barrier_count].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, slot_idx, 1};
      barrier_count++;
      slot->picture.layout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
    }

    if (!coincide_supported_ && output_pic_proxy_.layout != VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR) {
      barriers[barrier_count] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      barriers[barrier_count].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
      barriers[barrier_count].srcAccessMask = 0;
      barriers[barrier_count].dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
      barriers[barrier_count].dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
      barriers[barrier_count].oldLayout = output_pic_proxy_.layout;
      barriers[barrier_count].newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR;
      barriers[barrier_count].image = output_image_;
      barriers[barrier_count].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      barrier_count++;
      output_pic_proxy_.layout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR;
    }

    if (barrier_count > 0) {
      VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.imageMemoryBarrierCount = barrier_count;
      dep.pImageMemoryBarriers = barriers;
      VK(vkCmdPipelineBarrier2KHR)(cb, &dep);
    }

    VkVideoDecodeH265PictureInfoKHR h265_pic = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PICTURE_INFO_KHR};
    StdVideoDecodeH265PictureInfo std_pic = {};
    std_pic.PicOrderCntVal = (int32_t)packet.pts;
    std_pic.flags.IrapPicFlag = (h265_stream_->nal->nal_unit_type >= NAL_UNIT_CODED_SLICE_BLA_W_LP && h265_stream_->nal->nal_unit_type <= NAL_UNIT_CODED_SLICE_CRA);
    std_pic.flags.IdrPicFlag = (h265_stream_->nal->nal_unit_type == NAL_UNIT_CODED_SLICE_IDR_W_RADL || h265_stream_->nal->nal_unit_type == NAL_UNIT_CODED_SLICE_IDR_N_LP);
    std_pic.flags.IsReference = 1;
    h265_pic.pStdPictureInfo = &std_pic;
    uint32_t slice_offset = 0;
    h265_pic.sliceSegmentCount = 1;
    h265_pic.pSliceSegmentOffsets = &slice_offset;

    VkVideoPictureResourceInfoKHR dst_pic = {VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
    dst_pic.imageViewBinding = coincide_supported_ ? dpb_image_view_ : output_view_;
    dst_pic.codedExtent = {width_, height_};
    dst_pic.baseArrayLayer = coincide_supported_ ? slot_idx : 0;

    VkVideoReferenceSlotInfoKHR setup_slot = {VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
    setup_slot.slotIndex = (int32_t)slot_idx;
    VkVideoPictureResourceInfoKHR setup_pic = {VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
    setup_pic.imageViewBinding = dpb_image_view_;
    setup_pic.codedExtent = {width_, height_};
    setup_pic.baseArrayLayer = slot_idx;
    setup_slot.pPictureResource = &setup_pic;
    
    StdVideoDecodeH265ReferenceInfo std_ref_info = {};
    std_ref_info.PicOrderCntVal = (int32_t)packet.pts;
    VkVideoDecodeH265DpbSlotInfoKHR dpb_slot_info = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_DPB_SLOT_INFO_KHR};
    dpb_slot_info.pStdReferenceInfo = &std_ref_info;
    setup_slot.pNext = &dpb_slot_info;

    VkVideoReferenceSlotInfoKHR active_slots[MAX_DPB_SLOTS + 1];
    VkVideoPictureResourceInfoKHR active_pics[MAX_DPB_SLOTS + 1];
    VkVideoDecodeH265DpbSlotInfoKHR active_dpbs[MAX_DPB_SLOTS + 1];
    StdVideoDecodeH265ReferenceInfo active_stds[MAX_DPB_SLOTS + 1];
    uint32_t num_active = 0;

    for (uint32_t i = 0; i < MAX_DPB_SLOTS; ++i) {
      if (dpb_slots_[i].is_reference && i != slot_idx) {
        active_stds[num_active] = {};
        active_stds[num_active].PicOrderCntVal = dpb_slots_[i].poc;

        active_dpbs[num_active] = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_DPB_SLOT_INFO_KHR};
        active_dpbs[num_active].pStdReferenceInfo = &active_stds[num_active];

        active_pics[num_active] = {VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
        active_pics[num_active].imageViewBinding = dpb_image_view_;
        active_pics[num_active].codedExtent = {width_, height_};
        active_pics[num_active].baseArrayLayer = i;

        active_slots[num_active] = {VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
        active_slots[num_active].slotIndex = (int32_t)i;
        active_slots[num_active].pPictureResource = &active_pics[num_active];
        active_slots[num_active].pNext = &active_dpbs[num_active];
        
        num_active++;
      }
    }
    
    uint32_t coding_slot_count = num_active;
    active_slots[coding_slot_count] = setup_slot;
    active_slots[coding_slot_count].slotIndex = -1;
    coding_slot_count++;

    VkVideoBeginCodingInfoKHR begin_coding = {VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
    begin_coding.videoSession = video_session_;
    begin_coding.videoSessionParameters = session_params_;
    begin_coding.referenceSlotCount = coding_slot_count;
    begin_coding.pReferenceSlots = active_slots;
    VK(vkCmdBeginVideoCodingKHR)(cb, &begin_coding);

    if (first_decode_) {
      VkVideoCodingControlInfoKHR control = {VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR};
      control.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
      VK(vkCmdControlVideoCodingKHR)(cb, &control);
      first_decode_ = false;
    }

    VkVideoDecodeInfoKHR decode_info = {VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR};
    decode_info.pNext = &h265_pic;
    decode_info.srcBuffer = bitstream_buffer_;
    decode_info.srcBufferRange = aligned_size;
    decode_info.dstPictureResource = dst_pic;
    decode_info.pSetupReferenceSlot = &setup_slot;
    decode_info.referenceSlotCount = num_active;
    decode_info.pReferenceSlots = num_active > 0 ? active_slots : nullptr;

    VK(vkCmdDecodeVideoKHR)(cb, &decode_info);

    VkVideoEndCodingInfoKHR end_coding = {VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
    VK(vkCmdEndVideoCodingKHR)(cb, &end_coding);

    slot->poc = (int32_t)packet.pts;
    slot->is_reference = true;

    VK(vkEndCommandBuffer)(cb);

    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    VK(vkQueueSubmit)(hw_context_->video_decode_queue, 1, &submit, decode_fence_);
    VK(vkWaitForFences)(hw_context_->vk_device, 1, &decode_fence_, VK_TRUE, UINT64_MAX);
    VK(vkResetFences)(hw_context_->vk_device, 1, &decode_fence_);
  }

  void recordDecodeAV1(VulkanDPBEntry* slot, uint32_t slot_idx, const uint8_t* data, size_t size) { }
  void recordDecodeVP9(VulkanDPBEntry* slot, uint32_t slot_idx, const uint8_t* data, size_t size) { }

  void release() {
    if (!hw_context_) {
      initialized_ = false;
      return;
    }
    if (command_pool_) {
      VK(vkDestroyCommandPool)(hw_context_->vk_device, command_pool_, hw_context_->allocator);
      command_pool_ = VK_NULL_HANDLE;
    }
    if (decode_fence_) {
      VK(vkDestroyFence)(hw_context_->vk_device, decode_fence_, hw_context_->allocator);
      decode_fence_ = VK_NULL_HANDLE;
    }
    if (bitstream_buffer_) {
      VK(vkDestroyBuffer)(hw_context_->vk_device, bitstream_buffer_, hw_context_->allocator);
      bitstream_buffer_ = VK_NULL_HANDLE;
    }
    if (bitstream_memory_) {
      VK(vkUnmapMemory)(hw_context_->vk_device, bitstream_memory_);
      VK(vkFreeMemory)(hw_context_->vk_device, bitstream_memory_, hw_context_->allocator);
      bitstream_memory_ = VK_NULL_HANDLE;
    }
    if (video_session_) {
      VK(vkDestroyVideoSessionKHR)(hw_context_->vk_device, video_session_, hw_context_->allocator);
      video_session_ = VK_NULL_HANDLE;
    }
    if (session_params_) {
      VK(vkDestroyVideoSessionParametersKHR)(hw_context_->vk_device, session_params_, hw_context_->allocator);
      session_params_ = VK_NULL_HANDLE;
    }
    for (auto mem : session_memory_) {
      VK(vkFreeMemory)(hw_context_->vk_device, mem, hw_context_->allocator);
    }
    session_memory_.clear();
    
    if (dpb_image_view_) VK(vkDestroyImageView)(hw_context_->vk_device, dpb_image_view_, hw_context_->allocator);
    if (dpb_image_) VK(vkDestroyImage)(hw_context_->vk_device, dpb_image_, hw_context_->allocator);
    if (dpb_memory_) VK(vkFreeMemory)(hw_context_->vk_device, dpb_memory_, hw_context_->allocator);
    dpb_image_view_ = VK_NULL_HANDLE;
    dpb_image_ = VK_NULL_HANDLE;
    dpb_memory_ = VK_NULL_HANDLE;

    if (output_view_) VK(vkDestroyImageView)(hw_context_->vk_device, output_view_, hw_context_->allocator);
    if (output_image_) VK(vkDestroyImage)(hw_context_->vk_device, output_image_, hw_context_->allocator);
    if (output_memory_) VK(vkFreeMemory)(hw_context_->vk_device, output_memory_, hw_context_->allocator);
    output_view_ = VK_NULL_HANDLE;
    output_image_ = VK_NULL_HANDLE;
    output_memory_ = VK_NULL_HANDLE;

    if (h265_stream_) {
      h265_free(h265_stream_);
      h265_stream_ = nullptr;
    }
    initialized_ = false;
  }
};

class VulkanEncoder final : public Encoder {
  OMVulkanContext* hw_context_ = nullptr;
  bool initialized_ = false;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  OMCodecId codec_id_ = OM_CODEC_NONE;
  uint32_t frame_count_ = 0;

  VkVideoSessionKHR video_session_ = VK_NULL_HANDLE;
  VkVideoSessionParametersKHR session_params_ = VK_NULL_HANDLE;
  std::vector<VkDeviceMemory> session_memory_;

  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  std::vector<VkCommandBuffer> command_buffers_;
  VkFence encode_fence_ = VK_NULL_HANDLE;
  
  VkBuffer bitstream_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory bitstream_memory_ = VK_NULL_HANDLE;
  void* bitstream_ptr_ = nullptr;
  static constexpr size_t BITSTREAM_SIZE = 8 * 1024 * 1024;

public:
  VulkanEncoder() = default;
  ~VulkanEncoder() override { release(); }

#define VK(name) hw_context_->name

  auto configure(const EncoderOptions& options) -> OMError override {
    if (!options.hw_device.has_value() || !options.hw_device->context || options.hw_device->type != HWDeviceType::VULKAN) {
      release();
      return OM_CODEC_HWACCEL_FAILED;
    }
    hw_context_ = static_cast<OMVulkanContext*>(options.hw_device->context);
    release();

    codec_id_ = options.format.codec_id;
    width_ = options.format.video.width;
    height_ = options.format.video.height;

    VkVideoProfileInfoKHR video_profile = {VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
    if (codec_id_ == OM_CODEC_H264) {
      video_profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
    } else if (codec_id_ == OM_CODEC_H265) {
      video_profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H265_BIT_KHR;
    } else if (codec_id_ == OM_CODEC_AV1) {
      video_profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_AV1_BIT_KHR;
    } else {
      return OM_CODEC_NOT_SUPPORTED;
    }

    VkVideoSessionCreateInfoKHR session_info = {VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR};
    session_info.pVideoProfile = &video_profile;
    session_info.maxCodedExtent = {width_, height_};
    session_info.referencePictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    session_info.pictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    session_info.queueFamilyIndex = hw_context_->video_encode_queue_family_index;

    VK(vkCreateVideoSessionKHR)(hw_context_->vk_device, &session_info, hw_context_->allocator, &video_session_);

    // Memory, Pools, Buffers...
    VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = hw_context_->video_encode_queue_family_index;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK(vkCreateCommandPool)(hw_context_->vk_device, &pool_info, hw_context_->allocator, &command_pool_);

    VkCommandBufferAllocateInfo cb_alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cb_alloc.commandPool = command_pool_;
    cb_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_alloc.commandBufferCount = 1;
    command_buffers_.resize(1);
    VK(vkAllocateCommandBuffers)(hw_context_->vk_device, &cb_alloc, command_buffers_.data());

    VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK(vkCreateFence)(hw_context_->vk_device, &fence_info, hw_context_->allocator, &encode_fence_);

    VkBufferCreateInfo bit_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bit_info.size = BITSTREAM_SIZE;
    bit_info.usage = VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR;
    VK(vkCreateBuffer)(hw_context_->vk_device, &bit_info, hw_context_->allocator, &bitstream_buffer_);

    VkMemoryRequirements bit_mem_reqs;
    VK(vkGetBufferMemoryRequirements)(hw_context_->vk_device, bitstream_buffer_, &bit_mem_reqs);
    VkMemoryAllocateInfo bit_alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    bit_alloc.allocationSize = bit_mem_reqs.size;
    bit_alloc.memoryTypeIndex = findMemoryType(bit_mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK(vkAllocateMemory)(hw_context_->vk_device, &bit_alloc, hw_context_->allocator, &bitstream_memory_);
    VK(vkBindBufferMemory)(hw_context_->vk_device, bitstream_buffer_, bitstream_memory_, 0);
    VK(vkMapMemory)(hw_context_->vk_device, bitstream_memory_, 0, BITSTREAM_SIZE, 0, &bitstream_ptr_);

    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> EncodingInfo override { return {}; }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    if (!initialized_) return Err(OM_COMMON_NOT_INITIALIZED);
    if (!std::holds_alternative<Picture>(frame.data)) return Ok(std::vector<Packet>{});
    const auto& pic = std::get<Picture>(frame.data);

    VkCommandBuffer cb = command_buffers_[0];
    VK(vkResetCommandBuffer)(cb, 0);
    VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK(vkBeginCommandBuffer)(cb, &begin_info);

    VkVideoBeginCodingInfoKHR begin_coding = {VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
    begin_coding.videoSession = video_session_;
    begin_coding.videoSessionParameters = session_params_;
    VK(vkCmdBeginVideoCodingKHR)(cb, &begin_coding);

    VkVideoEncodeInfoKHR encode_info = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR};
    encode_info.dstBuffer = bitstream_buffer_;
    encode_info.dstBufferRange = BITSTREAM_SIZE;
    
    // Setup src picture resource (simplified for now)
    encode_info.srcPictureResource.imageViewBinding = VK_NULL_HANDLE; // Should come from frame.data
    encode_info.srcPictureResource.codedOffset = {0, 0};
    encode_info.srcPictureResource.codedExtent = {width_, height_};
    
    VK(vkCmdEncodeVideoKHR)(cb, &encode_info);

    VkVideoEndCodingInfoKHR end_coding = {VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
    VK(vkCmdEndVideoCodingKHR)(cb, &end_coding);

    VK(vkEndCommandBuffer)(cb);

    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    VK(vkQueueSubmit)(hw_context_->video_encode_queue, 1, &submit, encode_fence_);
    VK(vkQueueWaitIdle)(hw_context_->video_encode_queue);
    VK(vkResetFences)(hw_context_->vk_device, 1, &encode_fence_);

    uint32_t encoded_size = 0; // In reality, we'd query the coded buffer segment or status
    Packet pkt;
    pkt.allocate(encoded_size);
    std::memcpy(pkt.bytes.data(), bitstream_ptr_, encoded_size);
    pkt.pts = frame.pts;
    pkt.dts = frame.pts;
    frame_count_++;
    return Ok(std::vector<Packet>{std::move(pkt)});
  }

  auto updateBitrate(const RateControlParams& rc) -> OMError override { return OM_SUCCESS; }

private:
  uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    VK(vkGetPhysicalDeviceMemoryProperties)(hw_context_->vk_physical_device, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
      if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) return i;
    }
    return 0;
  }

  void release() {
    if (command_pool_) VK(vkDestroyCommandPool)(hw_context_->vk_device, command_pool_, hw_context_->allocator);
    if (encode_fence_) VK(vkDestroyFence)(hw_context_->vk_device, encode_fence_, hw_context_->allocator);
    if (bitstream_buffer_) VK(vkDestroyBuffer)(hw_context_->vk_device, bitstream_buffer_, hw_context_->allocator);
    if (bitstream_memory_) {
      VK(vkUnmapMemory)(hw_context_->vk_device, bitstream_memory_);
      VK(vkFreeMemory)(hw_context_->vk_device, bitstream_memory_, hw_context_->allocator);
    }
    if (video_session_) VK(vkDestroyVideoSessionKHR)(hw_context_->vk_device, video_session_, hw_context_->allocator);
    if (session_params_) VK(vkDestroyVideoSessionParametersKHR)(hw_context_->vk_device, session_params_, hw_context_->allocator);
    for (auto mem : session_memory_) VK(vkFreeMemory)(hw_context_->vk_device, mem, hw_context_->allocator);
    session_memory_.clear();
    initialized_ = false;
  }
};

const CodecDescriptor CODEC_VULKAN_H264 = {
    .codec_id = OM_CODEC_H264,
    .type = OM_MEDIA_VIDEO,
    .name = "vulkan_h264",
    .long_name = "Vulkan H.264/AVC Codec",
    .vendor = "Vulkan",
    .flags = HARDWARE,
    .decoder_factory = [] { return std::make_unique<VulkanDecoder>(); },
    .encoder_factory = [] { return std::make_unique<VulkanEncoder>(); },
};

const CodecDescriptor CODEC_VULKAN_H265 = {
    .codec_id = OM_CODEC_H265,
    .type = OM_MEDIA_VIDEO,
    .name = "vulkan_h265",
    .long_name = "Vulkan H.265/HEVC Codec",
    .vendor = "Vulkan",
    .flags = HARDWARE,
    .decoder_factory = [] { return std::make_unique<VulkanDecoder>(); },
    .encoder_factory = [] { return std::make_unique<VulkanEncoder>(); },
};

const CodecDescriptor CODEC_VULKAN_AV1 = {
    .codec_id = OM_CODEC_AV1,
    .type = OM_MEDIA_VIDEO,
    .name = "vulkan_av1",
    .long_name = "Vulkan AV1 Codec",
    .vendor = "Vulkan",
    .flags = HARDWARE,
    .decoder_factory = [] { return std::make_unique<VulkanDecoder>(); },
    .encoder_factory = [] { return std::make_unique<VulkanEncoder>(); },
};

const CodecDescriptor CODEC_VULKAN_VP9 = {
    .codec_id = OM_CODEC_VP9,
    .type = OM_MEDIA_VIDEO,
    .name = "vulkan_vp9",
    .long_name = "Vulkan VP9 Codec",
    .vendor = "Vulkan",
    .flags = HARDWARE,
    .decoder_factory = [] { return std::make_unique<VulkanDecoder>(); },
};

} // namespace openmedia
