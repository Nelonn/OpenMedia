#include <openmedia/hw_vulkan.h>
#include "hw_vulkan_priv.hpp"
#include <openmedia/video.hpp>
#include <codecs.hpp>
#include <vector>
#include <memory>
#include <cstring>
#include <algorithm>
#include <array>
#include <span>
#include <h264.h>
#include <h265_stream.h>
#include <util/io_util.hpp>
#include <cstdio>

namespace openmedia {

struct VulkanDPBEntry {
  OMVulkanPicture picture = {};
  int32_t poc = 0;
  uint32_t frame_num = 0;
  bool is_reference = false;
};

template<typename T>
static auto alignUp(T value, T alignment) -> T {
  if (alignment <= 1) return value;
  return ((value + alignment - 1) / alignment) * alignment;
}

static auto isAnnexB(std::span<const uint8_t> data) -> bool {
  return data.size() >= 3 && data[0] == 0 && data[1] == 0 &&
         (data[2] == 1 || (data.size() >= 4 && data[2] == 0 && data[3] == 1));
}

static auto h264LevelIdc(int level) -> StdVideoH264LevelIdc {
  switch (level) {
    case 0: return STD_VIDEO_H264_LEVEL_IDC_1_0;
    case 10: return STD_VIDEO_H264_LEVEL_IDC_1_0;
    case 11: return STD_VIDEO_H264_LEVEL_IDC_1_1;
    case 12: return STD_VIDEO_H264_LEVEL_IDC_1_2;
    case 13: return STD_VIDEO_H264_LEVEL_IDC_1_3;
    case 20: return STD_VIDEO_H264_LEVEL_IDC_2_0;
    case 21: return STD_VIDEO_H264_LEVEL_IDC_2_1;
    case 22: return STD_VIDEO_H264_LEVEL_IDC_2_2;
    case 30: return STD_VIDEO_H264_LEVEL_IDC_3_0;
    case 31: return STD_VIDEO_H264_LEVEL_IDC_3_1;
    case 32: return STD_VIDEO_H264_LEVEL_IDC_3_2;
    case 40: return STD_VIDEO_H264_LEVEL_IDC_4_0;
    case 41: return STD_VIDEO_H264_LEVEL_IDC_4_1;
    case 42: return STD_VIDEO_H264_LEVEL_IDC_4_2;
    case 50: return STD_VIDEO_H264_LEVEL_IDC_5_0;
    case 51: return STD_VIDEO_H264_LEVEL_IDC_5_1;
    case 52: return STD_VIDEO_H264_LEVEL_IDC_5_2;
    case 60: return STD_VIDEO_H264_LEVEL_IDC_6_0;
    case 61: return STD_VIDEO_H264_LEVEL_IDC_6_1;
    case 62: return STD_VIDEO_H264_LEVEL_IDC_6_2;
    default: return static_cast<StdVideoH264LevelIdc>(level);
  }
}

class VulkanDecoder final : public Decoder {
  OMVulkanContext* hw_context_ = nullptr;
  bool initialized_ = false;
  VideoFormat output_format_ = {};
  OMCodecId codec_id_ = OM_CODEC_NONE;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t padded_width_ = 0;
  uint32_t padded_height_ = 0;

  VkVideoProfileInfoKHR video_profile_ = {VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
  VkVideoDecodeH264ProfileInfoKHR h264_profile_ = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR};
  VkVideoDecodeH265ProfileInfoKHR h265_profile_ = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR};

  VkVideoSessionKHR video_session_ = VK_NULL_HANDLE;
  VkVideoSessionParametersKHR session_params_ = VK_NULL_HANDLE;
  std::vector<VkDeviceMemory> session_memory_;

  bool coincide_supported_ = false;
  bool distinct_supported_ = false;
  VkFormat dpb_format_ = VK_FORMAT_UNDEFINED;
  VkImageTiling dpb_tiling_ = VK_IMAGE_TILING_OPTIMAL;
  VkFormat out_format_ = VK_FORMAT_UNDEFINED;
  VkImageTiling out_tiling_ = VK_IMAGE_TILING_OPTIMAL;
  uint32_t dpb_slot_count_ = 0;
  
  VkImage dpb_image_ = VK_NULL_HANDLE;
  VkDeviceMemory dpb_memory_ = VK_NULL_HANDLE;
  VkImageView dpb_image_view_ = VK_NULL_HANDLE;
  std::vector<VulkanDPBEntry> dpb_slots_;

  VkImage output_image_ = VK_NULL_HANDLE;
  VkDeviceMemory output_memory_ = VK_NULL_HANDLE;
  VkImageView output_view_ = VK_NULL_HANDLE;
  OMVulkanPicture output_pic_proxy_ = {};

  uint32_t next_slot_ = 0;
  uint32_t next_ref_ = 0;
  std::vector<uint8_t> reference_usage_;
  static constexpr uint32_t MAX_DPB_SLOTS = 16;
  uint32_t min_bitstream_alignment_ = 128;

  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  std::vector<VkCommandBuffer> command_buffers_;
  VkFence decode_fence_ = VK_NULL_HANDLE;
  VkBuffer bitstream_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory bitstream_memory_ = VK_NULL_HANDLE;
  void* bitstream_ptr_ = nullptr;
  static constexpr size_t BITSTREAM_SIZE = 8 * 1024 * 1024;

  h264::SPS h264_sps_[32] = {};
  h264::PPS h264_pps_[256] = {};
  bool h264_sps_valid_[32] = {};
  bool h264_pps_valid_[256] = {};
  bool has_h264_sps_ = false;
  bool has_h264_pps_ = false;
  uint8_t h264_nal_length_size_ = 0;
  int prev_pic_order_cnt_lsb_ = 0;
  int prev_pic_order_cnt_msb_ = 0;
  bool have_prev_poc_ = false;

  h265_stream_t* h265_stream_ = nullptr;
  bool has_h265_sps_ = false;
  bool has_h265_pps_ = false;
  bool first_decode_ = true;

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
    has_h264_sps_ = false;
    has_h264_pps_ = false;
    std::memset(h264_sps_valid_, 0, sizeof(h264_sps_valid_));
    std::memset(h264_pps_valid_, 0, sizeof(h264_pps_valid_));
    h264_nal_length_size_ = 0;
    prev_pic_order_cnt_lsb_ = 0;
    prev_pic_order_cnt_msb_ = 0;
    have_prev_poc_ = false;
    reference_usage_.clear();
    next_ref_ = 0;
    next_slot_ = 0;

    video_profile_ = {VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
    video_profile_.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    video_profile_.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    video_profile_.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;

    if (codec_id_ == OM_CODEC_H264) {
      h264_profile_ = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR};
      if (options.format.profile == 66) h264_profile_.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_BASELINE;
      else if (options.format.profile == 100) h264_profile_.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
      else h264_profile_.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN;
      h264_profile_.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_INTERLACED_INTERLEAVED_LINES_BIT_KHR;
      video_profile_.pNext = &h264_profile_;
      video_profile_.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
      parseH264Extradata(options.extradata);
      for (uint32_t i = 0; i < 32; ++i) {
        if (!h264_sps_valid_[i]) continue;
        h264_profile_.stdProfileIdc = (StdVideoH264ProfileIdc)h264_sps_[i].profile_idc;
        break;
      }
    } else if (codec_id_ == OM_CODEC_H265) {
      h265_profile_ = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR};
      if (options.format.profile == 2) h265_profile_.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN_10;
      else h265_profile_.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
      video_profile_.pNext = &h265_profile_;
      video_profile_.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
    } else {
      return OM_CODEC_NOT_SUPPORTED;
    }

    VkVideoDecodeH264CapabilitiesKHR h264_caps = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR};
    VkVideoDecodeH265CapabilitiesKHR h265_caps = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR};
    VkVideoCapabilitiesKHR video_caps = {VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR};
    VkVideoDecodeCapabilitiesKHR decode_caps = {VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR};
    video_caps.pNext = &decode_caps;
    if (codec_id_ == OM_CODEC_H264) decode_caps.pNext = &h264_caps;
    else decode_caps.pNext = &h265_caps;

    if (VK(vkGetPhysicalDeviceVideoCapabilitiesKHR)(hw_context_->vk_physical_device, &video_profile_, &video_caps) != VK_SUCCESS) {
      return OM_CODEC_HWACCEL_FAILED;
    }

    coincide_supported_ = (decode_caps.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR) != 0;
    distinct_supported_ = (decode_caps.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_DISTINCT_BIT_KHR) != 0;
    if (!coincide_supported_ && !distinct_supported_) return OM_CODEC_HWACCEL_FAILED;

    min_bitstream_alignment_ = alignUp(video_caps.minBitstreamBufferOffsetAlignment, video_caps.minBitstreamBufferSizeAlignment);
    padded_width_ = alignUp(width_, 16u);
    padded_height_ = alignUp(height_, 16u);
    if (has_h264_sps_) {
      for (uint32_t i = 0; i < 32; ++i) {
        if (!h264_sps_valid_[i]) continue;
        const auto& sps = h264_sps_[i];
        padded_width_ = static_cast<uint32_t>((sps.pic_width_in_mbs_minus1 + 1) * 16);
        padded_height_ = static_cast<uint32_t>((sps.pic_height_in_map_units_minus1 + 1) * 16);
        break;
      }
    }
    if (video_caps.pictureAccessGranularity.width > 1) {
      padded_width_ = alignUp(padded_width_, video_caps.pictureAccessGranularity.width);
      padded_height_ = alignUp(padded_height_, video_caps.pictureAccessGranularity.height);
    }

    VkVideoProfileListInfoKHR profile_list = {VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR, nullptr, 1, &video_profile_};
    dpb_format_ = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    dpb_tiling_ = VK_IMAGE_TILING_OPTIMAL;
    out_format_ = dpb_format_;
    out_tiling_ = dpb_tiling_;

    dpb_slot_count_ = MAX_DPB_SLOTS + 1;
    if (codec_id_ == OM_CODEC_H264 && has_h264_sps_) {
      for (uint32_t i = 0; i < 32; ++i) {
        if (!h264_sps_valid_[i]) continue;
        dpb_slot_count_ = std::max(2u, static_cast<uint32_t>(h264_sps_[i].num_ref_frames + 1));
        break;
      }
    }
    dpb_slot_count_ = std::min(dpb_slot_count_, std::min(video_caps.maxDpbSlots, MAX_DPB_SLOTS + 1));

    VkVideoSessionCreateInfoKHR session_info = {VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR};
    session_info.pVideoProfile = &video_profile_;
    session_info.maxCodedExtent = {padded_width_, padded_height_};
    session_info.referencePictureFormat = dpb_format_;
    session_info.pictureFormat = out_format_;
    session_info.maxDpbSlots = dpb_slot_count_;
    session_info.maxActiveReferencePictures = std::min(video_caps.maxActiveReferencePictures, MAX_DPB_SLOTS * 2u);
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
      bind_infos.push_back({VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR, nullptr, req.memoryBindIndex, memory, 0, req.memoryRequirements.size});
    }
    VK(vkBindVideoSessionMemoryKHR)(hw_context_->vk_device, video_session_, (uint32_t)bind_infos.size(), bind_infos.data());

    if (codec_id_ == OM_CODEC_H264 && has_h264_sps_ && has_h264_pps_) {
      updateSessionParametersH264();
      if (!session_params_) return OM_CODEC_HWACCEL_FAILED;
    }

    VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &profile_list};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = dpb_format_;
    image_info.extent = {padded_width_, padded_height_, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = dpb_slot_count_;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = dpb_tiling_;
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
    view_info.format = dpb_format_;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, dpb_slot_count_};
    VkImageViewUsageCreateInfo dpb_view_usage = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    dpb_view_usage.usage = VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;
    if (coincide_supported_) dpb_view_usage.usage |= VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR;
    view_info.pNext = &dpb_view_usage;
    VK(vkCreateImageView)(hw_context_->vk_device, &view_info, hw_context_->allocator, &dpb_image_view_);

    dpb_slots_.resize(dpb_slot_count_);
    for (uint32_t i = 0; i < dpb_slot_count_; ++i) {
      dpb_slots_[i].picture.image = dpb_image_;
      dpb_slots_[i].picture.view = dpb_image_view_;
      dpb_slots_[i].picture.memory = dpb_memory_;
      dpb_slots_[i].picture.layer = i;
      dpb_slots_[i].picture.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    if (!coincide_supported_) {
      image_info.arrayLayers = 1;
      image_info.format = out_format_;
      image_info.tiling = out_tiling_;
      image_info.usage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      image_info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
      VK(vkCreateImage)(hw_context_->vk_device, &image_info, hw_context_->allocator, &output_image_);
      VK(vkGetImageMemoryRequirements)(hw_context_->vk_device, output_image_, &img_reqs);
      img_alloc.allocationSize = img_reqs.size;
      img_alloc.memoryTypeIndex = findMemoryType(img_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      VK(vkAllocateMemory)(hw_context_->vk_device, &img_alloc, hw_context_->allocator, &output_memory_);
      VK(vkBindImageMemory)(hw_context_->vk_device, output_image_, output_memory_, 0);
      view_info.image = output_image_;
      view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view_info.format = out_format_;
      view_info.subresourceRange.layerCount = 1;
      VkImageViewUsageCreateInfo output_view_usage = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
      output_view_usage.usage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR;
      view_info.pNext = &output_view_usage;
      VK(vkCreateImageView)(hw_context_->vk_device, &view_info, hw_context_->allocator, &output_view_);
      output_pic_proxy_ = {output_image_, output_view_, output_memory_, 0, VK_IMAGE_LAYOUT_UNDEFINED};
    }

    VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = hw_context_->video_decode_queue_family_index;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK(vkCreateCommandPool)(hw_context_->vk_device, &pool_info, hw_context_->allocator, &command_pool_);

    command_buffers_.resize(1);
    VkCommandBufferAllocateInfo cb_alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, command_pool_, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VK(vkAllocateCommandBuffers)(hw_context_->vk_device, &cb_alloc, command_buffers_.data());

    VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK(vkCreateFence)(hw_context_->vk_device, &fence_info, hw_context_->allocator, &decode_fence_);

    VkBufferCreateInfo bit_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &profile_list, 0, BITSTREAM_SIZE, VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR};
    VK(vkCreateBuffer)(hw_context_->vk_device, &bit_info, hw_context_->allocator, &bitstream_buffer_);
    VkMemoryRequirements bit_mem_reqs;
    VK(vkGetBufferMemoryRequirements)(hw_context_->vk_device, bitstream_buffer_, &bit_mem_reqs);
    VkMemoryAllocateInfo bit_alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, bit_mem_reqs.size, findMemoryType(bit_mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VK(vkAllocateMemory)(hw_context_->vk_device, &bit_alloc, hw_context_->allocator, &bitstream_memory_);
    VK(vkBindBufferMemory)(hw_context_->vk_device, bitstream_buffer_, bitstream_memory_, 0);
    VK(vkMapMemory)(hw_context_->vk_device, bitstream_memory_, 0, BITSTREAM_SIZE, 0, &bitstream_ptr_);

    if (codec_id_ == OM_CODEC_H265) h265_stream_ = h265_new();
    output_format_ = {OM_FORMAT_NV12, width_, height_};
    initialized_ = true;
    return OM_SUCCESS;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    if (!initialized_) return Err(OM_COMMON_NOT_INITIALIZED);
    if (packet.bytes.empty()) return Ok(std::vector<Frame>{});

    if (codec_id_ == OM_CODEC_H264) {
      std::span<const uint8_t> bitstream = packet.bytes;
      if (bitstream.empty() || bitstream.size() > BITSTREAM_SIZE) return Err(OM_CODEC_HWACCEL_FAILED);

      h264::Bitstream bs;
      bs.init(bitstream.data(), bitstream.size());
      std::vector<uint32_t> slice_offsets;
      h264::SliceHeader last_slice = {};
      h264::NALHeader last_nal = {};

      while (h264::find_next_nal(&bs)) {
        uint32_t nal_offset = (uint32_t)bs.byte_offset() - 3;
        h264::NALHeader nal;
        if (!h264::read_nal_header(&nal, &bs)) continue;
        if (nal.type == h264::NAL_UNIT_TYPE_SPS) {
          h264::SPS sps;
          h264::read_sps(&sps, &bs);
          if (sps.seq_parameter_set_id >= 0 && sps.seq_parameter_set_id < 32) {
            h264_sps_[sps.seq_parameter_set_id] = sps;
            h264_sps_valid_[sps.seq_parameter_set_id] = true;
            has_h264_sps_ = true;
            updateSessionParametersH264();
          }
        } else if (nal.type == h264::NAL_UNIT_TYPE_PPS) {
          h264::PPS pps;
          h264::read_pps(&pps, &bs);
          if (pps.pic_parameter_set_id >= 0 && pps.pic_parameter_set_id < 256) {
            h264_pps_[pps.pic_parameter_set_id] = pps;
            h264_pps_valid_[pps.pic_parameter_set_id] = true;
            has_h264_pps_ = true;
            updateSessionParametersH264();
          }
        } else if (nal.type == h264::NAL_UNIT_TYPE_CODED_SLICE_IDR || nal.type == h264::NAL_UNIT_TYPE_CODED_SLICE_NON_IDR) {
          if (!has_h264_sps_ || !has_h264_pps_ || !session_params_) continue;
          h264::read_slice_header(&last_slice, &nal, h264_pps_, h264_sps_, &bs);
          slice_offsets.push_back(nal_offset);
          last_nal = nal;
        }
      }

      if (!slice_offsets.empty()) {
          const bool is_intra = last_nal.type == h264::NAL_UNIT_TYPE_CODED_SLICE_IDR;
          const bool is_reference = last_nal.idc != h264::NAL_REF_IDC_PRIORITY_DISPOSABLE;
          if (is_intra) {
            for (auto& dpb : dpb_slots_) dpb.is_reference = false;
            reference_usage_.clear();
            next_ref_ = 0;
            next_slot_ = 0;
            prev_pic_order_cnt_lsb_ = 0;
            prev_pic_order_cnt_msb_ = 0;
            have_prev_poc_ = false;
          }

          uint32_t current_idx = next_slot_;
          VulkanDPBEntry* slot = &dpb_slots_[current_idx];
          const int32_t poc = computeH264Poc(last_slice);
          recordDecodeH264(slot, current_idx, last_nal, last_slice, bitstream, poc, is_reference, slice_offsets);

          slot->poc = poc;
          slot->frame_num = (uint32_t)last_slice.frame_num;
          slot->is_reference = is_reference;
          if (is_reference && dpb_slot_count_ > 1) {
            if (next_ref_ >= reference_usage_.size()) reference_usage_.resize(next_ref_ + 1);
            reference_usage_[next_ref_] = static_cast<uint8_t>(current_idx);
            next_ref_ = (next_ref_ + 1) % (dpb_slot_count_ - 1);
            next_slot_ = (next_slot_ + 1) % dpb_slot_count_;
          }
          
          Frame frame = {};
          frame.pts = packet.pts;
          frame.dts = packet.dts;
          Picture pic(output_format_.format, output_format_.width, output_format_.height);
          pic.buffer = std::make_shared<VulkanHardwarePicture>(coincide_supported_ ? &slot->picture : &output_pic_proxy_);
          frame.data = std::move(pic);
          return Ok(std::vector<Frame>{std::move(frame)});
      }
    } else if (codec_id_ == OM_CODEC_H265) {
        // ... (H265 logic if any)
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
  void flush() override { }

private:
  uint32_t findMemoryType(uint32_t filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties m;
    VK(vkGetPhysicalDeviceMemoryProperties)(hw_context_->vk_physical_device, &m);
    for (uint32_t i = 0; i < m.memoryTypeCount; i++) if ((filter & (1 << i)) && (m.memoryTypes[i].propertyFlags & props) == props) return i;
    return 0;
  }

  void storeH264Nal(std::span<const uint8_t> nal_data) {
    if (nal_data.empty()) return;
    h264::Bitstream bs;
    bs.init(nal_data.data(), nal_data.size());
    h264::NALHeader nal;
    if (!h264::read_nal_header(&nal, &bs)) return;
    if (nal.type == h264::NAL_UNIT_TYPE_SPS) {
      h264::SPS sps = {};
      h264::read_sps(&sps, &bs);
      if (sps.seq_parameter_set_id >= 0 && sps.seq_parameter_set_id < 32) {
        h264_sps_[sps.seq_parameter_set_id] = sps;
        h264_sps_valid_[sps.seq_parameter_set_id] = true;
        has_h264_sps_ = true;
      }
    } else if (nal.type == h264::NAL_UNIT_TYPE_PPS) {
      h264::PPS pps = {};
      h264::read_pps(&pps, &bs);
      if (pps.pic_parameter_set_id >= 0 && pps.pic_parameter_set_id < 256) {
        h264_pps_[pps.pic_parameter_set_id] = pps;
        h264_pps_valid_[pps.pic_parameter_set_id] = true;
        has_h264_pps_ = true;
      }
    }
  }

  void parseH264Extradata(std::span<const uint8_t> extradata) {
    if (extradata.empty()) return;
    if (extradata.size() >= 7 && extradata[0] == 1) {
      h264_nal_length_size_ = static_cast<uint8_t>((extradata[4] & 0x03u) + 1);
      size_t offset = 5;
      const uint8_t sps_count = extradata[offset++] & 0x1fu;
      for (uint8_t i = 0; i < sps_count && offset + 2 <= extradata.size(); ++i) {
        const size_t size = (static_cast<size_t>(extradata[offset]) << 8u) | extradata[offset + 1];
        offset += 2;
        if (offset + size > extradata.size()) return;
        storeH264Nal(extradata.subspan(offset, size));
        offset += size;
      }
      if (offset >= extradata.size()) return;
      const uint8_t pps_count = extradata[offset++];
      for (uint8_t i = 0; i < pps_count && offset + 2 <= extradata.size(); ++i) {
        const size_t size = (static_cast<size_t>(extradata[offset]) << 8u) | extradata[offset + 1];
        offset += 2;
        if (offset + size > extradata.size()) return;
        storeH264Nal(extradata.subspan(offset, size));
        offset += size;
      }
      return;
    }

    if (!isAnnexB(extradata)) return;
    h264::Bitstream bs;
    bs.init(extradata.data(), extradata.size());
    while (h264::find_next_nal(&bs)) {
      const size_t start = static_cast<size_t>(bs.byte_offset());
      const uint8_t* nal_start = extradata.data() + start;
      const uint8_t* nal_end = extradata.data() + extradata.size();
      for (const uint8_t* p = nal_start; p + 3 <= extradata.data() + extradata.size(); ++p) {
        if (p[0] == 0 && p[1] == 0 && (p[2] == 1 || (p + 4 <= extradata.data() + extradata.size() && p[2] == 0 && p[3] == 1))) {
          nal_end = p;
          break;
        }
      }
      storeH264Nal({nal_start, static_cast<size_t>(nal_end - nal_start)});
    }
  }

  auto computeH264Poc(const h264::SliceHeader& slice) -> int32_t {
    if (slice.pic_parameter_set_id < 0 || slice.pic_parameter_set_id >= 256 || !h264_pps_valid_[slice.pic_parameter_set_id]) {
      return slice.pic_order_cnt_lsb;
    }
    const auto& pps = h264_pps_[slice.pic_parameter_set_id];
    if (pps.seq_parameter_set_id < 0 || pps.seq_parameter_set_id >= 32 || !h264_sps_valid_[pps.seq_parameter_set_id]) {
      return slice.pic_order_cnt_lsb;
    }
    const auto& sps = h264_sps_[pps.seq_parameter_set_id];
    if (sps.pic_order_cnt_type != 0) return slice.pic_order_cnt_lsb;

    const int max_pic_order_cnt_lsb = 1 << (sps.log2_max_pic_order_cnt_lsb_minus4 + 4);
    int pic_order_cnt_msb = 0;
    if (have_prev_poc_) {
      if (slice.pic_order_cnt_lsb < prev_pic_order_cnt_lsb_ &&
          (prev_pic_order_cnt_lsb_ - slice.pic_order_cnt_lsb) >= max_pic_order_cnt_lsb / 2) {
        pic_order_cnt_msb = prev_pic_order_cnt_msb_ + max_pic_order_cnt_lsb;
      } else if (slice.pic_order_cnt_lsb > prev_pic_order_cnt_lsb_ &&
                 (slice.pic_order_cnt_lsb - prev_pic_order_cnt_lsb_) > max_pic_order_cnt_lsb / 2) {
        pic_order_cnt_msb = prev_pic_order_cnt_msb_ - max_pic_order_cnt_lsb;
      } else {
        pic_order_cnt_msb = prev_pic_order_cnt_msb_;
      }
    }
    prev_pic_order_cnt_lsb_ = slice.pic_order_cnt_lsb;
    prev_pic_order_cnt_msb_ = pic_order_cnt_msb;
    have_prev_poc_ = true;
    return pic_order_cnt_msb + slice.pic_order_cnt_lsb;
  }

  void updateSessionParametersH264() {
    if (!video_session_ || !has_h264_sps_ || !has_h264_pps_) return;
    if (session_params_) {
      VK(vkDestroyVideoSessionParametersKHR)(hw_context_->vk_device, session_params_, hw_context_->allocator);
      session_params_ = VK_NULL_HANDLE;
    }

    std::vector<StdVideoH264SequenceParameterSet> spss;
    std::vector<StdVideoH264SequenceParameterSetVui> vuis(32);
    std::vector<StdVideoH264HrdParameters> hrds(32);
    std::vector<StdVideoH264ScalingLists> sps_scaling_lists(32);
    for (int i = 0; i < 32; ++i) if (h264_sps_valid_[i]) {
        const auto& d = h264_sps_[i];
        StdVideoH264SequenceParameterSet s = {};
        s.flags.constraint_set0_flag = d.constraint_set0_flag;
        s.flags.constraint_set1_flag = d.constraint_set1_flag;
        s.flags.constraint_set2_flag = d.constraint_set2_flag;
        s.flags.constraint_set3_flag = d.constraint_set3_flag;
        s.flags.constraint_set4_flag = d.constraint_set4_flag;
        s.flags.constraint_set5_flag = d.constraint_set5_flag;
        s.flags.frame_mbs_only_flag = d.frame_mbs_only_flag;
        s.flags.mb_adaptive_frame_field_flag = d.mb_adaptive_frame_field_flag;
        s.flags.direct_8x8_inference_flag = d.direct_8x8_inference_flag;
        s.flags.delta_pic_order_always_zero_flag = d.delta_pic_order_always_zero_flag;
        s.flags.separate_colour_plane_flag = d.separate_colour_plane_flag;
        s.flags.gaps_in_frame_num_value_allowed_flag = d.gaps_in_frame_num_value_allowed_flag;
        s.flags.qpprime_y_zero_transform_bypass_flag = d.qpprime_y_zero_transform_bypass_flag;
        s.flags.frame_cropping_flag = d.frame_cropping_flag;
        s.flags.seq_scaling_matrix_present_flag = d.seq_scaling_matrix_present_flag;
        s.flags.vui_parameters_present_flag = d.vui_parameters_present_flag;
        s.profile_idc = (StdVideoH264ProfileIdc)d.profile_idc;
        s.level_idc = h264LevelIdc(d.level_idc);
        s.chroma_format_idc = STD_VIDEO_H264_CHROMA_FORMAT_IDC_420;
        s.seq_parameter_set_id = (uint8_t)d.seq_parameter_set_id;
        s.bit_depth_luma_minus8 = (uint8_t)d.bit_depth_luma_minus8;
        s.bit_depth_chroma_minus8 = (uint8_t)d.bit_depth_chroma_minus8;
        s.log2_max_frame_num_minus4 = (uint8_t)d.log2_max_frame_num_minus4;
        s.pic_order_cnt_type = (StdVideoH264PocType)d.pic_order_cnt_type;
        s.offset_for_non_ref_pic = d.offset_for_non_ref_pic;
        s.offset_for_top_to_bottom_field = d.offset_for_top_to_bottom_field;
        s.log2_max_pic_order_cnt_lsb_minus4 = (uint8_t)d.log2_max_pic_order_cnt_lsb_minus4;
        s.num_ref_frames_in_pic_order_cnt_cycle = (uint8_t)d.num_ref_frames_in_pic_order_cnt_cycle;
        s.max_num_ref_frames = (uint8_t)d.num_ref_frames;
        s.pic_width_in_mbs_minus1 = (uint16_t)d.pic_width_in_mbs_minus1;
        s.pic_height_in_map_units_minus1 = (uint16_t)d.pic_height_in_map_units_minus1;
        s.frame_crop_left_offset = (uint32_t)d.frame_crop_left_offset;
        s.frame_crop_right_offset = (uint32_t)d.frame_crop_right_offset;
        s.frame_crop_top_offset = (uint32_t)d.frame_crop_top_offset;
        s.frame_crop_bottom_offset = (uint32_t)d.frame_crop_bottom_offset;
        s.pOffsetForRefFrame = d.offset_for_ref_frame;
        if (d.seq_scaling_matrix_present_flag) {
          auto& sl = sps_scaling_lists[i];
          s.pScalingLists = &sl;
          for (uint32_t j = 0; j < 8; ++j) sl.scaling_list_present_mask |= d.seq_scaling_list_present_flag[j] << j;
          for (uint32_t j = 0; j < 6; ++j) {
            sl.use_default_scaling_matrix_mask |= d.UseDefaultScalingMatrix4x4Flag[j] << j;
            for (uint32_t k = 0; k < 16; ++k) sl.ScalingList4x4[j][k] = (uint8_t)d.ScalingList4x4[j][k];
          }
          for (uint32_t j = 0; j < 2; ++j) {
            sl.use_default_scaling_matrix_mask |= d.UseDefaultScalingMatrix8x8Flag[j] << (j + 6);
            for (uint32_t k = 0; k < 64; ++k) sl.ScalingList8x8[j][k] = (uint8_t)d.ScalingList8x8[j][k];
          }
        }
        if (d.vui_parameters_present_flag) {
          auto& vui = vuis[i];
          auto& hrd = hrds[i];
          s.pSequenceParameterSetVui = &vui;
          vui.flags.aspect_ratio_info_present_flag = d.vui.aspect_ratio_info_present_flag;
          vui.flags.overscan_info_present_flag = d.vui.overscan_info_present_flag;
          vui.flags.overscan_appropriate_flag = d.vui.overscan_appropriate_flag;
          vui.flags.video_signal_type_present_flag = d.vui.video_signal_type_present_flag;
          vui.flags.video_full_range_flag = d.vui.video_full_range_flag;
          vui.flags.color_description_present_flag = d.vui.colour_description_present_flag;
          vui.flags.chroma_loc_info_present_flag = d.vui.chroma_loc_info_present_flag;
          vui.flags.timing_info_present_flag = d.vui.timing_info_present_flag;
          vui.flags.fixed_frame_rate_flag = d.vui.fixed_frame_rate_flag;
          vui.flags.bitstream_restriction_flag = d.vui.bitstream_restriction_flag;
          vui.flags.nal_hrd_parameters_present_flag = d.vui.nal_hrd_parameters_present_flag;
          vui.flags.vcl_hrd_parameters_present_flag = d.vui.vcl_hrd_parameters_present_flag;
          vui.aspect_ratio_idc = (StdVideoH264AspectRatioIdc)d.vui.aspect_ratio_idc;
          vui.sar_width = (uint16_t)d.vui.sar_width;
          vui.sar_height = (uint16_t)d.vui.sar_height;
          vui.video_format = (uint8_t)d.vui.video_format;
          vui.colour_primaries = (uint8_t)d.vui.colour_primaries;
          vui.transfer_characteristics = (uint8_t)d.vui.transfer_characteristics;
          vui.matrix_coefficients = (uint8_t)d.vui.matrix_coefficients;
          vui.num_units_in_tick = (uint32_t)d.vui.num_units_in_tick;
          vui.time_scale = (uint32_t)d.vui.time_scale;
          vui.max_num_reorder_frames = (uint8_t)d.vui.num_reorder_frames;
          vui.max_dec_frame_buffering = (uint8_t)d.vui.max_dec_frame_buffering;
          vui.chroma_sample_loc_type_top_field = (uint8_t)d.vui.chroma_sample_loc_type_top_field;
          vui.chroma_sample_loc_type_bottom_field = (uint8_t)d.vui.chroma_sample_loc_type_bottom_field;
          if (d.vui.nal_hrd_parameters_present_flag || d.vui.vcl_hrd_parameters_present_flag) {
            vui.pHrdParameters = &hrd;
            hrd.cpb_cnt_minus1 = (uint8_t)d.hrd.cpb_cnt_minus1;
            hrd.bit_rate_scale = (uint8_t)d.hrd.bit_rate_scale;
            hrd.cpb_size_scale = (uint8_t)d.hrd.cpb_size_scale;
            for (uint32_t j = 0; j < 32; ++j) {
              hrd.bit_rate_value_minus1[j] = (uint32_t)d.hrd.bit_rate_value_minus1[j];
              hrd.cpb_size_value_minus1[j] = (uint32_t)d.hrd.cpb_size_value_minus1[j];
              hrd.cbr_flag[j] = (uint8_t)d.hrd.cbr_flag[j];
            }
            hrd.initial_cpb_removal_delay_length_minus1 = (uint8_t)d.hrd.initial_cpb_removal_delay_length_minus1;
            hrd.cpb_removal_delay_length_minus1 = (uint8_t)d.hrd.cpb_removal_delay_length_minus1;
            hrd.dpb_output_delay_length_minus1 = (uint8_t)d.hrd.dpb_output_delay_length_minus1;
            hrd.time_offset_length = (uint8_t)d.hrd.time_offset_length;
          }
        }
        spss.push_back(s);
    }
    std::vector<StdVideoH264PictureParameterSet> ppss;
    std::vector<StdVideoH264ScalingLists> scaling_lists(256);
    for (int i = 0; i < 256; ++i) if (h264_pps_valid_[i]) {
        const auto& d = h264_pps_[i];
        if (d.seq_parameter_set_id < 0 || d.seq_parameter_set_id >= 32 || !h264_sps_valid_[d.seq_parameter_set_id]) continue;
        StdVideoH264PictureParameterSet p = {};
        p.flags.entropy_coding_mode_flag = d.entropy_coding_mode_flag;
        p.flags.bottom_field_pic_order_in_frame_present_flag = d.pic_order_present_flag;
        p.flags.weighted_pred_flag = d.weighted_pred_flag;
        p.flags.deblocking_filter_control_present_flag = d.deblocking_filter_control_present_flag;
        p.flags.constrained_intra_pred_flag = d.constrained_intra_pred_flag;
        p.flags.redundant_pic_cnt_present_flag = d.redundant_pic_cnt_present_flag;
        p.flags.transform_8x8_mode_flag = d.transform_8x8_mode_flag;
        p.flags.pic_scaling_matrix_present_flag = d.pic_scaling_matrix_present_flag;
        p.seq_parameter_set_id = (uint8_t)d.seq_parameter_set_id;
        p.pic_parameter_set_id = (uint8_t)d.pic_parameter_set_id;
        p.num_ref_idx_l0_default_active_minus1 = (uint8_t)d.num_ref_idx_l0_active_minus1;
        p.num_ref_idx_l1_default_active_minus1 = (uint8_t)d.num_ref_idx_l1_active_minus1;
        p.weighted_bipred_idc = (StdVideoH264WeightedBipredIdc)d.weighted_bipred_idc;
        p.pic_init_qp_minus26 = (int8_t)d.pic_init_qp_minus26;
        p.pic_init_qs_minus26 = (int8_t)d.pic_init_qs_minus26;
        p.chroma_qp_index_offset = (int8_t)d.chroma_qp_index_offset;
        p.second_chroma_qp_index_offset = (int8_t)d.second_chroma_qp_index_offset;
        auto& sl = scaling_lists[i];
        p.pScalingLists = &sl;
        for (uint32_t j = 0; j < 8; ++j) sl.scaling_list_present_mask |= d.pic_scaling_list_present_flag[j] << j;
        for (uint32_t j = 0; j < 6; ++j) {
          sl.use_default_scaling_matrix_mask |= d.UseDefaultScalingMatrix4x4Flag[j] << j;
          for (uint32_t k = 0; k < 16; ++k) sl.ScalingList4x4[j][k] = (uint8_t)d.ScalingList4x4[j][k];
        }
        for (uint32_t j = 0; j < 2; ++j) {
          sl.use_default_scaling_matrix_mask |= d.UseDefaultScalingMatrix8x8Flag[j] << (j + 6);
          for (uint32_t k = 0; k < 64; ++k) sl.ScalingList8x8[j][k] = (uint8_t)d.ScalingList8x8[j][k];
        }
        ppss.push_back(p);
    }
    if (spss.empty() || ppss.empty()) return;

    VkVideoDecodeH264SessionParametersAddInfoKHR add = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR};
    add.stdSPSCount = (uint32_t)spss.size();
    add.pStdSPSs = spss.data();
    add.stdPPSCount = (uint32_t)ppss.size();
    add.pStdPPSs = ppss.data();

    VkVideoDecodeH264SessionParametersCreateInfoKHR h264 = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR};
    h264.maxStdSPSCount = add.stdSPSCount;
    h264.maxStdPPSCount = add.stdPPSCount;
    h264.pParametersAddInfo = &add;
    VkVideoSessionParametersCreateInfoKHR info = {VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR};
    info.pNext = &h264;
    info.videoSession = video_session_;
    if (VK(vkCreateVideoSessionParametersKHR)(hw_context_->vk_device, &info, hw_context_->allocator, &session_params_) != VK_SUCCESS) {
      session_params_ = VK_NULL_HANDLE;
    }
  }

  void recordDecodeH264(VulkanDPBEntry* slot, uint32_t slot_idx, const h264::NALHeader& nal, const h264::SliceHeader& slice, std::span<const uint8_t> bitstream, int32_t poc, bool is_reference, const std::vector<uint32_t>& slice_offsets) {
    size_t aligned_size = alignUp(bitstream.size(), static_cast<size_t>(min_bitstream_alignment_));
    std::memcpy(bitstream_ptr_, bitstream.data(), bitstream.size());
    VkCommandBuffer cb = command_buffers_[0];
    VK(vkResetCommandBuffer)(cb, 0);
    VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK(vkBeginCommandBuffer)(cb, &begin_info);
    
    std::array<VkImageMemoryBarrier2, MAX_DPB_SLOTS + 2> b{};
    uint32_t bc = 0;
    if (first_decode_) {
      b[bc] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      b[bc].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      b[bc].srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
      b[bc].dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
      b[bc].dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
      b[bc].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      b[bc].newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
      b[bc].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b[bc].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b[bc].image = dpb_image_;
      b[bc].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, dpb_slot_count_};
      ++bc;
      for (auto& dpb : dpb_slots_) dpb.picture.layout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
    } else if (slot->picture.layout != VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR) {
      b[bc] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      b[bc].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      b[bc].srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
      b[bc].dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
      b[bc].dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
      b[bc].oldLayout = slot->picture.layout;
      b[bc].newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
      b[bc].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b[bc].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b[bc].image = dpb_image_;
      b[bc].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, slot_idx, 1};
      ++bc;
      slot->picture.layout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
    }
    if (!coincide_supported_) {
      b[bc] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      b[bc].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      b[bc].srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
      b[bc].dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
      b[bc].dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
      b[bc].oldLayout = output_pic_proxy_.layout;
      b[bc].newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR;
      b[bc].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b[bc].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b[bc].image = output_image_;
      b[bc].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      ++bc;
      output_pic_proxy_.layout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR;
    }
    if (bc > 0) {
      VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
      dep.imageMemoryBarrierCount = bc;
      dep.pImageMemoryBarriers = b.data();
      VK(vkCmdPipelineBarrier2KHR)(cb, &dep);
    }

    std::array<VkVideoReferenceSlotInfoKHR, MAX_DPB_SLOTS + 1> slot_infos{};
    std::array<VkVideoPictureResourceInfoKHR, MAX_DPB_SLOTS + 1> slot_pics{};
    std::array<VkVideoDecodeH264DpbSlotInfoKHR, MAX_DPB_SLOTS + 1> slot_h264{};
    std::array<StdVideoDecodeH264ReferenceInfo, MAX_DPB_SLOTS + 1> slot_stds{};
    for (uint32_t i = 0; i < dpb_slot_count_; ++i) {
      slot_stds[i] = {};
      slot_stds[i].FrameNum = (uint16_t)dpb_slots_[i].frame_num;
      slot_stds[i].PicOrderCnt[0] = dpb_slots_[i].poc;
      slot_stds[i].PicOrderCnt[1] = dpb_slots_[i].poc;
      slot_h264[i] = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR};
      slot_h264[i].pStdReferenceInfo = &slot_stds[i];
      slot_pics[i] = {VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
      slot_pics[i].codedExtent = {padded_width_, padded_height_};
      slot_pics[i].baseArrayLayer = i;
      slot_pics[i].imageViewBinding = dpb_image_view_;
      slot_infos[i] = {VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
      slot_infos[i].pNext = &slot_h264[i];
      slot_infos[i].slotIndex = (int32_t)i;
      slot_infos[i].pPictureResource = &slot_pics[i];
    }

    slot_stds[slot_idx].FrameNum = (uint16_t)slice.frame_num;
    slot_stds[slot_idx].PicOrderCnt[0] = poc;
    slot_stds[slot_idx].PicOrderCnt[1] = poc;

    std::array<VkVideoReferenceSlotInfoKHR, MAX_DPB_SLOTS + 1> begin_slots{};
    std::array<VkVideoReferenceSlotInfoKHR, MAX_DPB_SLOTS + 1> ref_slots{};
    uint32_t ref_count = 0;
    for (uint8_t ref_slot : reference_usage_) {
      if (ref_slot >= dpb_slot_count_ || ref_slot == slot_idx || !dpb_slots_[ref_slot].is_reference) continue;
      ref_slots[ref_count] = slot_infos[ref_slot];
      begin_slots[ref_count] = slot_infos[ref_slot];
      ++ref_count;
    }
    begin_slots[ref_count] = slot_infos[slot_idx];
    begin_slots[ref_count].slotIndex = -1;

    StdVideoDecodeH264PictureInfo std_pic = {};
    std_pic.pic_parameter_set_id = (uint16_t)slice.pic_parameter_set_id;
    std_pic.seq_parameter_set_id = (uint8_t)h264_pps_[slice.pic_parameter_set_id].seq_parameter_set_id;
    std_pic.frame_num = (uint16_t)slice.frame_num;
    std_pic.PicOrderCnt[0] = poc;
    std_pic.PicOrderCnt[1] = poc;
    std_pic.idr_pic_id = (uint16_t)slice.idr_pic_id;
    std_pic.flags.is_intra = (nal.type == h264::NAL_UNIT_TYPE_CODED_SLICE_IDR);
    std_pic.flags.is_reference = is_reference ? 1 : 0;
    std_pic.flags.IdrPicFlag = (std_pic.flags.is_intra && std_pic.flags.is_reference) ? 1 : 0;
    std_pic.flags.field_pic_flag = slice.field_pic_flag;
    std_pic.flags.bottom_field_flag = slice.bottom_field_flag;
    VkVideoDecodeH264PictureInfoKHR h264_pic = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR};
    h264_pic.pStdPictureInfo = &std_pic;
    h264_pic.sliceCount = (uint32_t)slice_offsets.size();
    h264_pic.pSliceOffsets = slice_offsets.data();

    VkVideoPictureResourceInfoKHR dst = {VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
    dst.codedExtent = {padded_width_, padded_height_};
    dst.baseArrayLayer = coincide_supported_ ? slot_idx : 0;
    dst.imageViewBinding = coincide_supported_ ? dpb_image_view_ : output_view_;

    VkVideoBeginCodingInfoKHR begin = {VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
    begin.videoSession = video_session_;
    begin.videoSessionParameters = session_params_;
    begin.referenceSlotCount = ref_count + 1;
    begin.pReferenceSlots = begin_slots.data();
    VK(vkCmdBeginVideoCodingKHR)(cb, &begin);
    if (first_decode_) {
        VkVideoCodingControlInfoKHR ctrl = {VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR, nullptr, VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR};
        VK(vkCmdControlVideoCodingKHR)(cb, &ctrl);
        first_decode_ = false;
    }
    VkVideoDecodeInfoKHR decode = {VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR};
    decode.pNext = &h264_pic;
    decode.srcBuffer = bitstream_buffer_;
    decode.srcBufferOffset = 0;
    decode.srcBufferRange = aligned_size;
    decode.dstPictureResource = dst;
    decode.pSetupReferenceSlot = &slot_infos[slot_idx];
    decode.referenceSlotCount = ref_count;
    decode.pReferenceSlots = ref_count == 0 ? nullptr : ref_slots.data();
    VK(vkCmdDecodeVideoKHR)(cb, &decode);
    VkVideoEndCodingInfoKHR end = {VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
    VK(vkCmdEndVideoCodingKHR)(cb, &end);
    VK(vkEndCommandBuffer)(cb);
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, 0, 1, &cb};
    VK(vkQueueSubmit)(hw_context_->video_decode_queue, 1, &submit, decode_fence_);
    VK(vkWaitForFences)(hw_context_->vk_device, 1, &decode_fence_, VK_TRUE, UINT64_MAX);
    VK(vkResetFences)(hw_context_->vk_device, 1, &decode_fence_);
  }

  void recordDecodeH265(VulkanDPBEntry* slot, uint32_t slot_idx, const Packet& packet, const std::vector<uint32_t>& slice_offsets) { }

  void release() {
    if (!hw_context_) return;
    if (command_pool_) VK(vkDestroyCommandPool)(hw_context_->vk_device, command_pool_, hw_context_->allocator);
    if (decode_fence_) VK(vkDestroyFence)(hw_context_->vk_device, decode_fence_, hw_context_->allocator);
    if (bitstream_buffer_) VK(vkDestroyBuffer)(hw_context_->vk_device, bitstream_buffer_, hw_context_->allocator);
    if (bitstream_memory_) { VK(vkUnmapMemory)(hw_context_->vk_device, bitstream_memory_); VK(vkFreeMemory)(hw_context_->vk_device, bitstream_memory_, hw_context_->allocator); }
    if (session_params_) VK(vkDestroyVideoSessionParametersKHR)(hw_context_->vk_device, session_params_, hw_context_->allocator);
    if (video_session_) VK(vkDestroyVideoSessionKHR)(hw_context_->vk_device, video_session_, hw_context_->allocator);
    for (auto m : session_memory_) VK(vkFreeMemory)(hw_context_->vk_device, m, hw_context_->allocator);
    session_memory_.clear();
    if (dpb_image_view_) VK(vkDestroyImageView)(hw_context_->vk_device, dpb_image_view_, hw_context_->allocator);
    if (dpb_image_) VK(vkDestroyImage)(hw_context_->vk_device, dpb_image_, hw_context_->allocator);
    if (dpb_memory_) VK(vkFreeMemory)(hw_context_->vk_device, dpb_memory_, hw_context_->allocator);
    if (output_view_) VK(vkDestroyImageView)(hw_context_->vk_device, output_view_, hw_context_->allocator);
    if (output_image_) VK(vkDestroyImage)(hw_context_->vk_device, output_image_, hw_context_->allocator);
    if (output_memory_) VK(vkFreeMemory)(hw_context_->vk_device, output_memory_, hw_context_->allocator);
    if (h265_stream_) h265_free(h265_stream_);
    command_pool_ = VK_NULL_HANDLE; decode_fence_ = VK_NULL_HANDLE; bitstream_buffer_ = VK_NULL_HANDLE; bitstream_memory_ = VK_NULL_HANDLE; bitstream_ptr_ = nullptr;
    video_session_ = VK_NULL_HANDLE; session_params_ = VK_NULL_HANDLE; dpb_image_view_ = VK_NULL_HANDLE; dpb_image_ = VK_NULL_HANDLE;
    dpb_memory_ = VK_NULL_HANDLE; output_view_ = VK_NULL_HANDLE; output_image_ = VK_NULL_HANDLE; output_memory_ = VK_NULL_HANDLE;
    h265_stream_ = nullptr; initialized_ = false;
  }
};

const CodecDescriptor CODEC_VULKAN_H264 = { .codec_id = OM_CODEC_H264, .type = OM_MEDIA_VIDEO, .name = "vulkan_h264", .long_name = "Vulkan H.264/AVC Codec", .vendor = "Vulkan", .flags = HARDWARE, .decoder_factory = [] { return std::make_unique<VulkanDecoder>(); } };
const CodecDescriptor CODEC_VULKAN_H265 = { .codec_id = OM_CODEC_H265, .type = OM_MEDIA_VIDEO, .name = "vulkan_h265", .long_name = "Vulkan H.265/HEVC Codec", .vendor = "Vulkan", .flags = HARDWARE, .decoder_factory = [] { return std::make_unique<VulkanDecoder>(); } };

} // namespace openmedia
