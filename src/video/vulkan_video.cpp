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

class VulkanHardwarePicture : public HardwarePicture {
public:
  OMVulkanPicture* picture = nullptr;
  VulkanHardwarePicture(OMVulkanPicture* pic)
      : HardwarePicture(HWDeviceType::VULKAN), picture(pic) {}
};

struct VulkanDPBEntry {
  OMVulkanPicture* picture = nullptr;
  int32_t poc = 0;
  uint32_t frame_num = 0;
  bool is_long_term = false;
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

  std::vector<OMVulkanPicture*> surface_pool_;
  size_t surface_idx_ = 0;
  std::vector<VulkanDPBEntry> dpb_;

  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  std::vector<VkCommandBuffer> command_buffers_;
  VkFence decode_fence_ = VK_NULL_HANDLE;
  VkBuffer bitstream_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory bitstream_memory_ = VK_NULL_HANDLE;
  void* bitstream_ptr_ = nullptr;
  static constexpr size_t BITSTREAM_SIZE = 4 * 1024 * 1024;

  // Bitstream parsers
  h264::SPS h264_sps_ = {};
  h264::PPS h264_pps_ = {};
  bool has_h264_sps_ = false;
  bool has_h264_pps_ = false;

  h265_stream_t* h265_stream_ = nullptr;
  bool has_h265_sps_ = false;
  bool has_h265_pps_ = false;

public:
  VulkanDecoder() = default;
  ~VulkanDecoder() override { release(); }

  auto configure(const DecoderOptions& options) -> OMError override {
    release();
    if (!options.hw_device.has_value() || options.hw_device->type != HWDeviceType::VULKAN) {
      return OM_CODEC_HWACCEL_FAILED;
    }
    hw_context_ = static_cast<OMVulkanContext*>(options.hw_device->context);
    codec_id_ = options.format.codec_id;
    width_ = options.format.video.width;
    height_ = options.format.video.height;

    if (codec_id_ == OM_CODEC_H264) {
      h264_profile_.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN;
      video_profile_.pNext = &h264_profile_;
      video_profile_.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    } else if (codec_id_ == OM_CODEC_H265) {
      h265_profile_.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
      video_profile_.pNext = &h265_profile_;
      video_profile_.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
    } else if (codec_id_ == OM_CODEC_AV1) {
      av1_profile_.stdProfile = STD_VIDEO_AV1_PROFILE_MAIN;
      video_profile_.pNext = &av1_profile_;
      video_profile_.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR;
    } else if (codec_id_ == OM_CODEC_VP9) {
      vp9_profile_.stdProfile = STD_VIDEO_VP9_PROFILE_0;
      video_profile_.pNext = &vp9_profile_;
      video_profile_.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR;
    } else {
      return OM_CODEC_NOT_SUPPORTED;
    }

    VkVideoSessionCreateInfoKHR session_info = {VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR};
    session_info.pVideoProfile = &video_profile_;
    session_info.maxCodedExtent = {width_, height_};
    session_info.referencePictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    session_info.pictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    session_info.maxDpbSlots = 16;
    session_info.maxActiveReferencePictures = 16;
    session_info.queueFamilyIndex = hw_context_->video_decode_queue_family_index;

    if (vkCreateVideoSessionKHR(hw_context_->vk_device, &session_info, hw_context_->allocator, &video_session_) != VK_SUCCESS) {
      return OM_CODEC_HWACCEL_FAILED;
    }

    uint32_t mem_req_count = 0;
    vkGetVideoSessionMemoryRequirementsKHR(hw_context_->vk_device, video_session_, &mem_req_count, nullptr);
    std::vector<VkVideoSessionMemoryRequirementsKHR> mem_reqs(mem_req_count, {VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR});
    vkGetVideoSessionMemoryRequirementsKHR(hw_context_->vk_device, video_session_, &mem_req_count, mem_reqs.data());

    std::vector<VkBindVideoSessionMemoryInfoKHR> bind_infos;
    for (const auto& req : mem_reqs) {
      VkMemoryAllocateInfo alloc_info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      alloc_info.allocationSize = req.memoryRequirements.size;
      alloc_info.memoryTypeIndex = findMemoryType(req.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      VkDeviceMemory memory;
      vkAllocateMemory(hw_context_->vk_device, &alloc_info, hw_context_->allocator, &memory);
      session_memory_.push_back(memory);

      VkBindVideoSessionMemoryInfoKHR bind_info = {VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR};
      bind_info.memoryBindIndex = req.memoryBindIndex;
      bind_info.memory = memory;
      bind_info.memorySize = req.memoryRequirements.size;
      bind_infos.push_back(bind_info);
    }
    vkBindVideoSessionMemoryKHR(hw_context_->vk_device, video_session_, (uint32_t)bind_infos.size(), bind_infos.data());

    VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = hw_context_->video_decode_queue_family_index;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(hw_context_->vk_device, &pool_info, hw_context_->allocator, &command_pool_);

    VkCommandBufferAllocateInfo alloc_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc_info.commandPool = command_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    command_buffers_.resize(1);
    vkAllocateCommandBuffers(hw_context_->vk_device, &alloc_info, command_buffers_.data());

    VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(hw_context_->vk_device, &fence_info, hw_context_->allocator, &decode_fence_);

    VkBufferCreateInfo bit_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bit_info.size = BITSTREAM_SIZE;
    bit_info.usage = VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR;
    vkCreateBuffer(hw_context_->vk_device, &bit_info, hw_context_->allocator, &bitstream_buffer_);

    VkMemoryRequirements bit_mem_reqs;
    vkGetBufferMemoryRequirements(hw_context_->vk_device, bitstream_buffer_, &bit_mem_reqs);
    VkMemoryAllocateInfo bit_alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    bit_alloc.allocationSize = bit_mem_reqs.size;
    bit_alloc.memoryTypeIndex = findMemoryType(bit_mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(hw_context_->vk_device, &bit_alloc, hw_context_->allocator, &bitstream_memory_);
    vkBindBufferMemory(hw_context_->vk_device, bitstream_buffer_, bitstream_memory_, 0);
    vkMapMemory(hw_context_->vk_device, bitstream_memory_, 0, BITSTREAM_SIZE, 0, &bitstream_ptr_);

    if (codec_id_ == OM_CODEC_H265) h265_stream_ = h265_new();

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
          if (!has_h264_sps_ || !has_h264_pps_) continue;
          h264::SliceHeader slice;
          h264::read_slice_header(&slice, &nal, &h264_pps_, &h264_sps_, &bs);
          
          OMVulkanPicture* surface = hw_context_ ? HWVulkanContext_createPicture(hw_context_) : nullptr;
          if (!surface) return Err(OM_COMMON_OUT_OF_MEMORY);
          recordDecodeH264(surface, slice, packet);
          
          Frame frame = {};
          frame.pts = packet.pts;
          frame.dts = packet.dts;
          Picture pic;
          pic.format = output_format_.format;
          pic.width = output_format_.width;
          pic.height = output_format_.height;
          pic.buffer = VulkanHardwarePicture(surface);
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
          if (!has_h265_sps_ || !has_h265_pps_) continue;
          OMVulkanPicture* surface = HWVulkanContext_createPicture(hw_context_);
          if (!surface) return Err(OM_COMMON_OUT_OF_MEMORY);
          recordDecodeH265(surface, packet);
          Frame frame = {};
          frame.pts = packet.pts;
          frame.dts = packet.dts;
          Picture pic;
          pic.format = output_format_.format;
          pic.width = output_format_.width;
          pic.height = output_format_.height;
          pic.buffer = VulkanHardwarePicture(surface);
          frame.data = std::move(pic);
          return Ok(std::vector<Frame>{std::move(frame)});
        }
        p += nal_end; sz -= nal_end;
      }
    } else if (codec_id_ == OM_CODEC_AV1) {
      const uint8_t* p = packet.bytes.data();
      size_t sz = packet.bytes.size();
      while (sz > 0) {
        uint8_t header = *p;
        bool obu_extension_flag = (header >> 2) & 1;
        bool obu_has_size_field = (header >> 1) & 1;
        uint8_t obu_type = (header >> 3) & 0xF;
        p++; sz--;
        if (obu_extension_flag) { p++; sz--; }
        size_t obu_size_len = 0;
        uint32_t obu_size = obu_has_size_field ? read_leb128(p, sz, &obu_size_len) : (uint32_t)sz;
        p += obu_size_len; sz -= obu_size_len;
        if (obu_type == 6 /* FRAME */ || obu_type == 3 /* FRAME_HEADER */) {
          OMVulkanPicture* surface = HWVulkanContext_createPicture(hw_context_);
          recordDecodeAV1(surface, p, obu_size);
          Frame frame = {};
          frame.pts = packet.pts;
          Picture pic;
          pic.format = output_format_.format;
          pic.width = output_format_.width;
          pic.height = output_format_.height;
          pic.buffer = VulkanHardwarePicture(surface);
          frame.data = std::move(pic);
          return Ok(std::vector<Frame>{std::move(frame)});
        }
        if (obu_size > sz) break;
        p += obu_size; sz -= obu_size;
      }
    } else if (codec_id_ == OM_CODEC_VP9) {
      OMVulkanPicture* surface = HWVulkanContext_createPicture(hw_context_);
      recordDecodeVP9(surface, packet.bytes.data(), packet.bytes.size());
      Frame frame = {};
      frame.pts = packet.pts;
      Picture pic;
      pic.format = output_format_.format;
      pic.width = output_format_.width;
      pic.height = output_format_.height;
      pic.buffer = VulkanHardwarePicture(surface);
      frame.data = std::move(pic);
      return Ok(std::vector<Frame>{std::move(frame)});
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

  void flush() override { dpb_.clear(); }

private:
  uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(hw_context_->vk_physical_device, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
      if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) return i;
    }
    return 0;
  }

  void updateSessionParametersH264() {
    if (!has_h264_sps_ || !has_h264_pps_) return;
    if (session_params_) vkDestroyVideoSessionParametersKHR(hw_context_->vk_device, session_params_, hw_context_->allocator);

    StdVideoH264SequenceParameterSet sps = {};
    sps.pic_width_in_mbs_minus1 = (uint16_t)h264_sps_.pic_width_in_mbs_minus1;
    sps.pic_height_in_map_units_minus1 = (uint16_t)h264_sps_.pic_height_in_map_units_minus1;
    sps.bit_depth_luma_minus8 = (uint8_t)h264_sps_.bit_depth_luma_minus8;
    sps.bit_depth_chroma_minus8 = (uint8_t)h264_sps_.bit_depth_chroma_minus8;
    sps.log2_max_frame_num_minus4 = (uint8_t)h264_sps_.log2_max_frame_num_minus4;
    sps.pic_order_cnt_type = (StdVideoH264PocType)h264_sps_.pic_order_cnt_type;
    sps.log2_max_pic_order_cnt_lsb_minus4 = (uint8_t)h264_sps_.log2_max_pic_order_cnt_lsb_minus4;
    sps.flags.frame_mbs_only_flag = h264_sps_.frame_mbs_only_flag;
    sps.flags.direct_8x8_inference_flag = h264_sps_.direct_8x8_inference_flag;

    StdVideoH264PictureParameterSet pps = {};
    pps.seq_parameter_set_id = 0;
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

    VkVideoDecodeH264SessionParametersAddInfoKHR h264_add = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR};
    h264_add.stdSPSCount = 1;
    h264_add.pStdSPSs = &sps;
    h264_add.stdPPSCount = 1;
    h264_add.pStdPPSs = &pps;

    VkVideoSessionParametersCreateInfoKHR params_info = {VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR};
    params_info.pNext = &h264_add;
    params_info.videoSession = video_session_;
    vkCreateVideoSessionParametersKHR(hw_context_->vk_device, &params_info, hw_context_->allocator, &session_params_);
  }

  void updateSessionParametersH265() {
    if (!has_h265_sps_ || !has_h265_pps_) return;
    if (session_params_) vkDestroyVideoSessionParametersKHR(hw_context_->vk_device, session_params_, hw_context_->allocator);

    StdVideoH265SequenceParameterSet sps = {};
    sps.pic_width_in_luma_samples = (uint16_t)h265_stream_->sps->pic_width_in_luma_samples;
    sps.pic_height_in_luma_samples = (uint16_t)h265_stream_->sps->pic_height_in_luma_samples;
    sps.bit_depth_luma_minus8 = (uint8_t)h265_stream_->sps->bit_depth_luma_minus8;
    sps.bit_depth_chroma_minus8 = (uint8_t)h265_stream_->sps->bit_depth_chroma_minus8;
    sps.log2_max_pic_order_cnt_lsb_minus4 = (uint8_t)h265_stream_->sps->log2_max_pic_order_cnt_lsb_minus4;

    StdVideoH265PictureParameterSet pps = {};
    pps.flags.entropy_coding_sync_enabled_flag = h265_stream_->pps->entropy_coding_sync_enabled_flag;
    pps.flags.weighted_pred_flag = h265_stream_->pps->weighted_pred_flag;
    pps.flags.weighted_bipred_flag = h265_stream_->pps->weighted_bipred_flag;
    pps.flags.transform_skip_enabled_flag = h265_stream_->pps->transform_skip_enabled_flag;

    VkVideoDecodeH265SessionParametersAddInfoKHR h265_add = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR};
    h265_add.stdSPSCount = 1;
    h265_add.pStdSPSs = &sps;
    h265_add.stdPPSCount = 1;
    h265_add.pStdPPSs = &pps;

    VkVideoSessionParametersCreateInfoKHR params_info = {VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR};
    params_info.pNext = &h265_add;
    params_info.videoSession = video_session_;
    vkCreateVideoSessionParametersKHR(hw_context_->vk_device, &params_info, hw_context_->allocator, &session_params_);
  }

  void recordDecodeH264(OMVulkanPicture* surface, const h264::SliceHeader& slice, const Packet& packet) {
    if (packet.bytes.size() > BITSTREAM_SIZE) return;
    std::memcpy(bitstream_ptr_, packet.bytes.data(), packet.bytes.size());

    VkCommandBuffer cb = command_buffers_[0];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &begin_info);

    VkVideoBeginCodingInfoKHR begin_coding = {VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
    begin_coding.videoSession = video_session_;
    begin_coding.videoSessionParameters = session_params_;
    vkCmdBeginVideoCodingKHR(cb, &begin_coding);

    VkVideoDecodeH264PictureInfoKHR h264_pic = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR};
    StdVideoDecodeH264PictureInfo std_pic = {};
    std_pic.frame_num = (uint16_t)slice.frame_num;
    std_pic.PicOrderCnt[0] = (int32_t)packet.pts;
    h264_pic.pStdPictureInfo = &std_pic;

    VkVideoDecodeInfoKHR decode_info = {VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR};
    decode_info.pNext = &h264_pic;
    decode_info.srcBuffer = bitstream_buffer_;
    decode_info.srcBufferOffset = 0;
    decode_info.srcBufferRange = packet.bytes.size();
    decode_info.dstPictureResource.imageViewBinding = surface->view;
    decode_info.dstPictureResource.codedOffset = {0, 0};
    decode_info.dstPictureResource.codedExtent = {width_, height_};

    vkCmdDecodeVideoKHR(cb, &decode_info);

    VkVideoEndCodingInfoKHR end_coding = {VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
    vkCmdEndVideoCodingKHR(cb, &end_coding);

    vkEndCommandBuffer(cb);

    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    vkQueueSubmit(hw_context_->video_decode_queue, 1, &submit, decode_fence_);
    vkWaitForFences(hw_context_->vk_device, 1, &decode_fence_, VK_TRUE, UINT64_MAX);
    vkResetFences(hw_context_->vk_device, 1, &decode_fence_);
  }

  void recordDecodeH265(OMVulkanPicture* surface, const Packet& packet) {
    if (packet.bytes.size() > BITSTREAM_SIZE) return;
    std::memcpy(bitstream_ptr_, packet.bytes.data(), packet.bytes.size());

    VkCommandBuffer cb = command_buffers_[0];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &begin_info);

    VkVideoBeginCodingInfoKHR begin_coding = {VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
    begin_coding.videoSession = video_session_;
    begin_coding.videoSessionParameters = session_params_;
    vkCmdBeginVideoCodingKHR(cb, &begin_coding);

    VkVideoDecodeH265PictureInfoKHR h265_pic = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PICTURE_INFO_KHR};
    StdVideoDecodeH265PictureInfo std_pic = {};
    std_pic.PicOrderCntVal = (int32_t)packet.pts;
    h265_pic.pStdPictureInfo = &std_pic;

    VkVideoDecodeInfoKHR decode_info = {VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR};
    decode_info.pNext = &h265_pic;
    decode_info.srcBuffer = bitstream_buffer_;
    decode_info.srcBufferOffset = 0;
    decode_info.srcBufferRange = packet.bytes.size();
    decode_info.dstPictureResource.imageViewBinding = surface->view;
    decode_info.dstPictureResource.codedOffset = {0, 0};
    decode_info.dstPictureResource.codedExtent = {width_, height_};

    vkCmdDecodeVideoKHR(cb, &decode_info);

    VkVideoEndCodingInfoKHR end_coding = {VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
    vkCmdEndVideoCodingKHR(cb, &end_coding);

    vkEndCommandBuffer(cb);

    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    vkQueueSubmit(hw_context_->video_decode_queue, 1, &submit, decode_fence_);
    vkWaitForFences(hw_context_->vk_device, 1, &decode_fence_, VK_TRUE, UINT64_MAX);
    vkResetFences(hw_context_->vk_device, 1, &decode_fence_);
  }

  void recordDecodeAV1(OMVulkanPicture* surface, const uint8_t* data, size_t size) {
    if (size > BITSTREAM_SIZE) return;
    std::memcpy(bitstream_ptr_, data, size);

    VkCommandBuffer cb = command_buffers_[0];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &begin_info);

    VkVideoBeginCodingInfoKHR begin_coding = {VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
    begin_coding.videoSession = video_session_;
    begin_coding.videoSessionParameters = session_params_;
    vkCmdBeginVideoCodingKHR(cb, &begin_coding);

    VkVideoDecodeAV1PictureInfoKHR av1_pic = {VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PICTURE_INFO_KHR};
    StdVideoDecodeAV1PictureInfo std_pic = {};
    av1_pic.pStdPictureInfo = &std_pic;

    VkVideoDecodeInfoKHR decode_info = {VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR};
    decode_info.pNext = &av1_pic;
    decode_info.srcBuffer = bitstream_buffer_;
    decode_info.srcBufferOffset = 0;
    decode_info.srcBufferRange = size;
    decode_info.dstPictureResource.imageViewBinding = surface->view;
    decode_info.dstPictureResource.codedOffset = {0, 0};
    decode_info.dstPictureResource.codedExtent = {width_, height_};

    vkCmdDecodeVideoKHR(cb, &decode_info);

    VkVideoEndCodingInfoKHR end_coding = {VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
    vkCmdEndVideoCodingKHR(cb, &end_coding);

    vkEndCommandBuffer(cb);

    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    vkQueueSubmit(hw_context_->video_decode_queue, 1, &submit, decode_fence_);
    vkWaitForFences(hw_context_->vk_device, 1, &decode_fence_, VK_TRUE, UINT64_MAX);
    vkResetFences(hw_context_->vk_device, 1, &decode_fence_);
  }

  void recordDecodeVP9(OMVulkanPicture* surface, const uint8_t* data, size_t size) {
    if (size > BITSTREAM_SIZE) return;
    std::memcpy(bitstream_ptr_, data, size);

    VkCommandBuffer cb = command_buffers_[0];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &begin_info);

    VkVideoBeginCodingInfoKHR begin_coding = {VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
    begin_coding.videoSession = video_session_;
    begin_coding.videoSessionParameters = session_params_;
    vkCmdBeginVideoCodingKHR(cb, &begin_coding);

    VkVideoDecodeVP9PictureInfoKHR vp9_pic = {VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_PICTURE_INFO_KHR};
    StdVideoDecodeVP9PictureInfo std_pic = {};
    vp9_pic.pStdPictureInfo = &std_pic;

    VkVideoDecodeInfoKHR decode_info = {VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR};
    decode_info.pNext = &vp9_pic;
    decode_info.srcBuffer = bitstream_buffer_;
    decode_info.srcBufferOffset = 0;
    decode_info.srcBufferRange = size;
    decode_info.dstPictureResource.imageViewBinding = surface->view;
    decode_info.dstPictureResource.codedOffset = {0, 0};
    decode_info.dstPictureResource.codedExtent = {width_, height_};

    vkCmdDecodeVideoKHR(cb, &decode_info);

    VkVideoEndCodingInfoKHR end_coding = {VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
    vkCmdEndVideoCodingKHR(cb, &end_coding);

    vkEndCommandBuffer(cb);

    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    vkQueueSubmit(hw_context_->video_decode_queue, 1, &submit, decode_fence_);
    vkWaitForFences(hw_context_->vk_device, 1, &decode_fence_, VK_TRUE, UINT64_MAX);
    vkResetFences(hw_context_->vk_device, 1, &decode_fence_);
  }

  void release() {
    if (command_pool_) {
      vkDestroyCommandPool(hw_context_->vk_device, command_pool_, hw_context_->allocator);
      command_pool_ = VK_NULL_HANDLE;
    }
    if (decode_fence_) {
      vkDestroyFence(hw_context_->vk_device, decode_fence_, hw_context_->allocator);
      decode_fence_ = VK_NULL_HANDLE;
    }
    if (bitstream_buffer_) {
      vkDestroyBuffer(hw_context_->vk_device, bitstream_buffer_, hw_context_->allocator);
      bitstream_buffer_ = VK_NULL_HANDLE;
    }
    if (bitstream_memory_) {
      vkUnmapMemory(hw_context_->vk_device, bitstream_memory_);
      vkFreeMemory(hw_context_->vk_device, bitstream_memory_, hw_context_->allocator);
      bitstream_memory_ = VK_NULL_HANDLE;
    }
    if (video_session_) {
      vkDestroyVideoSessionKHR(hw_context_->vk_device, video_session_, hw_context_->allocator);
      video_session_ = VK_NULL_HANDLE;
    }
    if (session_params_) {
      vkDestroyVideoSessionParametersKHR(hw_context_->vk_device, session_params_, hw_context_->allocator);
      session_params_ = VK_NULL_HANDLE;
    }
    for (auto mem : session_memory_) {
      vkFreeMemory(hw_context_->vk_device, mem, hw_context_->allocator);
    }
    session_memory_.clear();
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
  static constexpr size_t BITSTREAM_SIZE = 4 * 1024 * 1024;

public:
  VulkanEncoder() = default;
  ~VulkanEncoder() override { release(); }

  auto configure(const EncoderOptions& options) -> OMError override {
    release();
    if (!options.hw_device.has_value() || options.hw_device->type != HWDeviceType::VULKAN) return OM_CODEC_HWACCEL_FAILED;
    hw_context_ = static_cast<OMVulkanContext*>(options.hw_device->context);
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

    vkCreateVideoSessionKHR(hw_context_->vk_device, &session_info, hw_context_->allocator, &video_session_);

    // Memory, Pools, Buffers...
    VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.queueFamilyIndex = hw_context_->video_encode_queue_family_index;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(hw_context_->vk_device, &pool_info, hw_context_->allocator, &command_pool_);

    VkCommandBufferAllocateInfo alloc_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc_info.commandPool = command_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    command_buffers_.resize(1);
    vkAllocateCommandBuffers(hw_context_->vk_device, &alloc_info, command_buffers_.data());

    VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(hw_context_->vk_device, &fence_info, hw_context_->allocator, &encode_fence_);

    VkBufferCreateInfo bit_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bit_info.size = BITSTREAM_SIZE;
    bit_info.usage = VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR;
    vkCreateBuffer(hw_context_->vk_device, &bit_info, hw_context_->allocator, &bitstream_buffer_);

    VkMemoryRequirements bit_mem_reqs;
    vkGetBufferMemoryRequirements(hw_context_->vk_device, bitstream_buffer_, &bit_mem_reqs);
    VkMemoryAllocateInfo bit_alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    bit_alloc.allocationSize = bit_mem_reqs.size;
    bit_alloc.memoryTypeIndex = findMemoryType(bit_mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(hw_context_->vk_device, &bit_alloc, hw_context_->allocator, &bitstream_memory_);
    vkBindBufferMemory(hw_context_->vk_device, bitstream_buffer_, bitstream_memory_, 0);
    vkMapMemory(hw_context_->vk_device, bitstream_memory_, 0, BITSTREAM_SIZE, 0, &bitstream_ptr_);

    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> EncodingInfo override { return {}; }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    if (!initialized_) return Err(OM_COMMON_NOT_INITIALIZED);
    if (!std::holds_alternative<Picture>(frame.data)) return Ok(std::vector<Packet>{});
    const auto& pic = std::get<Picture>(frame.data);

    VkCommandBuffer cb = command_buffers_[0];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &begin_info);

    VkVideoBeginCodingInfoKHR begin_coding = {VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
    begin_coding.videoSession = video_session_;
    begin_coding.videoSessionParameters = session_params_;
    vkCmdBeginVideoCodingKHR(cb, &begin_coding);

    VkVideoEncodeInfoKHR encode_info = {VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR};
    encode_info.dstBuffer = bitstream_buffer_;
    encode_info.dstBufferRange = BITSTREAM_SIZE;
    
    // Setup src picture resource (simplified for now)
    encode_info.srcPictureResource.imageViewBinding = VK_NULL_HANDLE; // Should come from frame.data
    encode_info.srcPictureResource.codedOffset = {0, 0};
    encode_info.srcPictureResource.codedExtent = {width_, height_};
    
    vkCmdEncodeVideoKHR(cb, &encode_info);

    VkVideoEndCodingInfoKHR end_coding = {VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
    vkCmdEndVideoCodingKHR(cb, &end_coding);

    vkEndCommandBuffer(cb);

    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    vkQueueSubmit(hw_context_->video_encode_queue, 1, &submit, encode_fence_);
    vkQueueWaitIdle(hw_context_->video_encode_queue);
    vkResetFences(hw_context_->vk_device, 1, &encode_fence_);

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
    vkGetPhysicalDeviceMemoryProperties(hw_context_->vk_physical_device, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
      if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) return i;
    }
    return 0;
  }

  void release() {
    if (command_pool_) vkDestroyCommandPool(hw_context_->vk_device, command_pool_, hw_context_->allocator);
    if (encode_fence_) vkDestroyFence(hw_context_->vk_device, encode_fence_, hw_context_->allocator);
    if (bitstream_buffer_) vkDestroyBuffer(hw_context_->vk_device, bitstream_buffer_, hw_context_->allocator);
    if (bitstream_memory_) {
      vkUnmapMemory(hw_context_->vk_device, bitstream_memory_);
      vkFreeMemory(hw_context_->vk_device, bitstream_memory_, hw_context_->allocator);
    }
    if (video_session_) vkDestroyVideoSessionKHR(hw_context_->vk_device, video_session_, hw_context_->allocator);
    if (session_params_) vkDestroyVideoSessionParametersKHR(hw_context_->vk_device, session_params_, hw_context_->allocator);
    for (auto mem : session_memory_) vkFreeMemory(hw_context_->vk_device, mem, hw_context_->allocator);
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
