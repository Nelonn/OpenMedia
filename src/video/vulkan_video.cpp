#include <openmedia/hw_vulkan.h>
#include "hw_vulkan_priv.hpp"
#include <openmedia/video.hpp>
#include <codecs.hpp>
#include <vector>
#include <memory>
#include <cstring>
#include <algorithm>
#include <array>
#include <iterator>
#include <span>
#include <video/parser/h264_parser.hpp>
#include <video/parser/h265_parser.hpp>
#include <video/hdr_sei.hpp>
#include <util/color_codes.hpp>
#include <video/parser/vp9_parser.hpp>
#include <video/parser/av1_parser.hpp>
#include <util/io_util.hpp>
#include <util/bit_reader.hpp>
#include <cstdio>

// ---------------------------------------------------------------------------
// Known bad: AV1 decoding through Vulkan Video on AMD / Windows.
//
// This driver only decodes the FIRST TILE of an AV1 frame. Measured on a
// Radeon 890M, same source clip and encoder, the tile layout being the only
// difference between the two rows:
//
//   1 tile  : whole frame decoded, ~94% of pixels exact vs dav1d
//   2x2 tile: 48 of 1080 rows decoded, the rest left untouched
//   4K, 8x4 : 112 of 2160 rows decoded
//
// The same clips decode bit exact through dx11_av1 on this very GPU, and
// H.264 through Vulkan Video is bit exact too, so neither the hardware nor
// the shared Vulkan plumbing in this file is at fault. Prefer dx11_av1 on
// Windows.
//
// Ruled out by experiment, so do not spend time on them again:
//   * Bitstream layout. Uploading only the tile payloads packed back to back
//     (what DXVA wants) instead of the whole packet with absolute offsets
//     produces byte identical output. The driver does honour pTileOffsets[0] —
//     tile 0 lands correctly either way — it just ignores the rest.
//   * frameHeaderOffset: every interpretation gives byte identical output.
//   * filmGrainSupport, DPB_AND_OUTPUT_COINCIDE vs DISTINCT, passing
//     pSegmentation as null, CDEF strengths in coded vs derived form, coded
//     extent and picture access granularity, bitstream buffer size.
//   * The tile offsets and sizes themselves: all 32 were verified to cover the
//     tile group payload byte for byte, and dx11_av1 consumes the exact same
//     extraction without trouble.
//
// The validation layer is silent throughout. Still unexplained is the ~6% of
// pixels that differ even in the single tile case; that one may yet be ours.
// ---------------------------------------------------------------------------

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

// ISO/IEC 23001-8 colour codes; AV1 and VP9 carry the same values H.26x VUI
// does, so the tables are shared with the containers.
constexpr auto mapColorPrimaries = color_codes::primariesFromCode;
constexpr auto mapTransferCharacteristic = color_codes::transferFromCode;
constexpr auto mapMatrixCoefficients = color_codes::colorSpaceFromMatrix;

static auto mapVP9ColorSpace(uint8_t cs) -> std::pair<OMColorPrimaries, OMColorSpace> {
  switch (cs) {
    case video_parser::VP9_CS_BT_601:
    case video_parser::VP9_CS_SMPTE_170:
      return {OM_PRIMARIES_BT601, OM_COLOR_SPACE_BT601};
    case video_parser::VP9_CS_BT_709:
      return {OM_PRIMARIES_BT709, OM_COLOR_SPACE_BT709};
    case video_parser::VP9_CS_SMPTE_240:
      return {OM_PRIMARIES_SMPTE240M, OM_COLOR_SPACE_SMPTE240M};
    case video_parser::VP9_CS_BT_2020:
      return {OM_PRIMARIES_BT2020, OM_COLOR_SPACE_BT2020};
    case video_parser::VP9_CS_RGB:
      return {OM_PRIMARIES_BT709, OM_COLOR_SPACE_RGB};
    default:
      return {OM_PRIMARIES_UNKNOWN, OM_COLOR_SPACE_UNKNOWN};
  }
}

struct AV1DpbRef {
  int32_t dpb_slot = -1;
  uint32_t order_hint = 0;
  uint32_t saved_order_hints[8] = {};
  uint8_t frame_type = 0;
  bool disable_frame_end_update_cdf = false;
  bool segmentation_enabled = false;
  bool valid = false;
};

struct VP9DpbRef {
  int32_t dpb_slot = -1;
  bool valid = false;
};

class VulkanDecoder final : public Decoder {
  OMVulkanContext* hw_context_ = nullptr;
  bool initialized_ = false;
  VideoFormat output_format_ = {};
  OMCodecId codec_id_ = OM_CODEC_NONE;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t padded_width_ = 0;
  uint32_t padded_height_ = 0;
  uint8_t bit_depth_ = 8;
  // Chroma format of the stream. Derived from the bitstream, never assumed:
  // H.26x carry chroma_format_idc in the SPS, AV1 and VP9 carry subsampling
  // flags in their colour config.
  uint8_t subsampling_x_ = 1;
  uint8_t subsampling_y_ = 1;
  bool mono_chrome_ = false;
  // VP9 signals its chroma format per frame; remember the last one seen so
  // initSession() can pick a matching surface format.
  uint8_t vp9_subsampling_x_ = 1;
  uint8_t vp9_subsampling_y_ = 1;
  DecoderOptions options_ = {};

  OMMasteringDisplayMetadata mastering_display_ = {};
  OMContentLightLevel content_light_level_ = {};

  VkVideoProfileInfoKHR video_profile_ = {VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
  VkVideoDecodeH264ProfileInfoKHR h264_profile_ = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR};
  VkVideoDecodeH265ProfileInfoKHR h265_profile_ = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR};
  VkVideoDecodeAV1ProfileInfoKHR av1_profile_ = {VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PROFILE_INFO_KHR};
  VkVideoDecodeVP9ProfileInfoKHR vp9_profile_ = {VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_PROFILE_INFO_KHR};

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
  uint32_t max_active_refs_ = 0;

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
  video_parser::H264AccessUnitParser h264_parser_;

  bool has_h265_sps_ = false;
  bool has_h265_pps_ = false;
  video_parser::H265AccessUnitParser h265_parser_;
  video_parser::VP9FrameParser vp9_parser_;
  video_parser::AV1ObuParser av1_parser_;
  bool has_av1_seq_ = false;
  AV1DpbRef av1_refs_[8] = {};
  VP9DpbRef vp9_refs_[8] = {};
  bool first_decode_ = true;
  // A DPB slot only becomes active once a decode operation has used it as the
  // reconstructed picture. Until then it must be bound with slotIndex = -1.
  std::array<bool, MAX_DPB_SLOTS + 1> dpb_slot_active_ = {};

public:
  VulkanDecoder() = default;
  ~VulkanDecoder() override { release(); }

#define VK(name) hw_context_->name

  auto createFrame(VulkanDPBEntry* slot, int64_t pts, int64_t dts) -> Frame {
    Frame frame = {};
    frame.pts = pts;
    frame.dts = dts;
    Picture pic(output_format_.format, output_format_.width, output_format_.height);
    pic.color_space = output_format_.color_space;
    pic.transfer_char = output_format_.transfer_char;
    pic.color_primaries = output_format_.color_primaries;
    pic.color_range = output_format_.color_range;
    pic.mastering_display = output_format_.mastering_display;
    pic.content_light_level = output_format_.content_light_level;
    pic.buffer = std::make_shared<VulkanHardwarePicture>(coincide_supported_ ? &slot->picture : &output_pic_proxy_);
    frame.data = std::move(pic);
    return frame;
  }

  auto initSession(uint32_t width, uint32_t height, uint8_t bit_depth) -> OMError {
    if (command_pool_) { VK(vkDestroyCommandPool)(hw_context_->vk_device, command_pool_, hw_context_->allocator); command_pool_ = VK_NULL_HANDLE; }
    if (decode_fence_) { VK(vkDestroyFence)(hw_context_->vk_device, decode_fence_, hw_context_->allocator); decode_fence_ = VK_NULL_HANDLE; }
    if (bitstream_buffer_) { VK(vkDestroyBuffer)(hw_context_->vk_device, bitstream_buffer_, hw_context_->allocator); bitstream_buffer_ = VK_NULL_HANDLE; }
    if (bitstream_memory_) { VK(vkUnmapMemory)(hw_context_->vk_device, bitstream_memory_); VK(vkFreeMemory)(hw_context_->vk_device, bitstream_memory_, hw_context_->allocator); bitstream_memory_ = VK_NULL_HANDLE; bitstream_ptr_ = nullptr; }
    if (session_params_) { VK(vkDestroyVideoSessionParametersKHR)(hw_context_->vk_device, session_params_, hw_context_->allocator); session_params_ = VK_NULL_HANDLE; }
    if (video_session_) { VK(vkDestroyVideoSessionKHR)(hw_context_->vk_device, video_session_, hw_context_->allocator); video_session_ = VK_NULL_HANDLE; }
    for (auto m : session_memory_) VK(vkFreeMemory)(hw_context_->vk_device, m, hw_context_->allocator);
    session_memory_.clear();
    if (dpb_image_view_) { VK(vkDestroyImageView)(hw_context_->vk_device, dpb_image_view_, hw_context_->allocator); dpb_image_view_ = VK_NULL_HANDLE; }
    if (dpb_image_) { VK(vkDestroyImage)(hw_context_->vk_device, dpb_image_, hw_context_->allocator); dpb_image_ = VK_NULL_HANDLE; }
    if (dpb_memory_) { VK(vkFreeMemory)(hw_context_->vk_device, dpb_memory_, hw_context_->allocator); dpb_memory_ = VK_NULL_HANDLE; }
    if (output_view_) { VK(vkDestroyImageView)(hw_context_->vk_device, output_view_, hw_context_->allocator); output_view_ = VK_NULL_HANDLE; }
    if (output_image_) { VK(vkDestroyImage)(hw_context_->vk_device, output_image_, hw_context_->allocator); output_image_ = VK_NULL_HANDLE; }
    if (output_memory_) { VK(vkFreeMemory)(hw_context_->vk_device, output_memory_, hw_context_->allocator); output_memory_ = VK_NULL_HANDLE; }

    width_ = width;
    height_ = height;
    bit_depth_ = bit_depth;
    output_format_.width = width;
    output_format_.height = height;
    resolveChromaFormat();
    output_format_.format = pickOutputPixelFormat();

    video_profile_ = {VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
    video_profile_.chromaSubsampling = chromaSubsamplingFlag();
    if (bit_depth_ == 10) {
      video_profile_.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR;
      video_profile_.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR;
    } else if (bit_depth_ == 12) {
      video_profile_.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_12_BIT_KHR;
      video_profile_.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_12_BIT_KHR;
    } else {
      video_profile_.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
      video_profile_.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    }

    if (codec_id_ == OM_CODEC_H264) {
      video_profile_.pNext = &h264_profile_;
      video_profile_.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    } else if (codec_id_ == OM_CODEC_H265) {
      video_profile_.pNext = &h265_profile_;
      video_profile_.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
    } else if (codec_id_ == OM_CODEC_AV1) {
      video_profile_.pNext = &av1_profile_;
      video_profile_.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR;
    } else if (codec_id_ == OM_CODEC_VP9) {
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
      // A profile the driver has no support for at all. The usual cause is a
      // chroma format the hardware cannot decode: 4:2:2 and 4:4:4 decode is
      // absent from most Vulkan Video implementations. Say so rather than
      // reporting a generic acceleration failure.
      if (video_profile_.chromaSubsampling != VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR)
        return OM_CODEC_NOT_SUPPORTED;
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

    VkImageUsageFlags dpb_usage = VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;
    if (coincide_supported_)
      dpb_usage |= VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    dpb_format_ = pickVideoFormat(dpb_usage, profile_list);
    if (dpb_format_ == VK_FORMAT_UNDEFINED) return OM_CODEC_NOT_SUPPORTED;

    if (coincide_supported_) {
      out_format_ = dpb_format_;
    } else {
      out_format_ = pickVideoFormat(VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                    profile_list);
      if (out_format_ == VK_FORMAT_UNDEFINED) return OM_CODEC_NOT_SUPPORTED;
    }
    dpb_tiling_ = VK_IMAGE_TILING_OPTIMAL;
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
    max_active_refs_ = std::min(video_caps.maxActiveReferencePictures, dpb_slot_count_ - 1);

    VkVideoSessionCreateInfoKHR session_info = {VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR};
    session_info.pVideoProfile = &video_profile_;
    session_info.maxCodedExtent = {padded_width_, padded_height_};
    session_info.referencePictureFormat = dpb_format_;
    session_info.pictureFormat = out_format_;
    session_info.maxDpbSlots = dpb_slot_count_;
    session_info.maxActiveReferencePictures = max_active_refs_;
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

    uint32_t queue_families[] = { hw_context_->queue_family_index, hw_context_->video_decode_queue_family_index };
    bool multi_queue = queue_families[0] != queue_families[1];

    VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &profile_list};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = dpb_format_;
    image_info.extent = {padded_width_, padded_height_, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = dpb_slot_count_;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = dpb_tiling_;
    image_info.usage = VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;
    if (coincide_supported_) image_info.usage |= VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = multi_queue ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
    image_info.queueFamilyIndexCount = multi_queue ? 2 : 0;
    image_info.pQueueFamilyIndices = multi_queue ? queue_families : nullptr;
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
      dpb_slots_[i].picture.format = dpb_format_;
      dpb_slots_[i].is_reference = false;
      dpb_slots_[i].poc = 0;
      dpb_slots_[i].frame_num = 0;
    }

    if (!coincide_supported_) {
      image_info.arrayLayers = 1;
      image_info.format = out_format_;
      image_info.tiling = out_tiling_;
      image_info.usage = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      image_info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
      image_info.sharingMode = multi_queue ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
      image_info.queueFamilyIndexCount = multi_queue ? 2 : 0;
      image_info.pQueueFamilyIndices = multi_queue ? queue_families : nullptr;
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
      output_pic_proxy_ = {output_image_, output_view_, output_memory_, 0, VK_IMAGE_LAYOUT_UNDEFINED, out_format_};
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

    output_format_.format = (bit_depth_ == 12) ? OM_FORMAT_P012 : ((bit_depth_ == 10) ? OM_FORMAT_P010 : OM_FORMAT_NV12);
    output_format_.width = width_;
    output_format_.height = height_;

    first_decode_ = true;
    dpb_slot_active_.fill(false);
    for (int i = 0; i < 8; ++i) {
      av1_refs_[i] = {};
      vp9_refs_[i] = {};
    }
    reference_usage_.clear();
    next_ref_ = 0;
    next_slot_ = 0;

    return OM_SUCCESS;
  }

  auto configure(const DecoderOptions& options) -> OMError override {
    if (!options.hw_device.has_value() || !options.hw_device->context || options.hw_device->type != HWDeviceType::VULKAN) {
      release();
      return OM_CODEC_HWACCEL_FAILED;
    }
    hw_context_ = static_cast<OMVulkanContext*>(options.hw_device->context);
    release();

    options_ = options;
    codec_id_ = options.format.codec_id;
    width_ = options.format.video.width;
    height_ = options.format.video.height;
    bit_depth_ = 8;
    if (options.format.video.format == OM_FORMAT_P010 || options.format.video.format == OM_FORMAT_YUV420P10) bit_depth_ = 10;
    else if (options.format.video.format == OM_FORMAT_P012 || options.format.video.format == OM_FORMAT_YUV420P12) bit_depth_ = 12;

    first_decode_ = true;
    dpb_slot_active_.fill(false);
    has_h264_sps_ = false;
    has_h264_pps_ = false;
    has_av1_seq_ = false;
    std::memset(h264_sps_valid_, 0, sizeof(h264_sps_valid_));
    std::memset(h264_pps_valid_, 0, sizeof(h264_pps_valid_));
    reference_usage_.clear();
    next_ref_ = 0;
    next_slot_ = 0;
    for (int i = 0; i < 8; ++i) {
      av1_refs_[i] = {};
      vp9_refs_[i] = {};
    }

    mastering_display_ = options.format.video.mastering_display;
    content_light_level_ = options.format.video.content_light_level;

    output_format_ = {};
    output_format_.format = (bit_depth_ == 12) ? OM_FORMAT_P012 : ((bit_depth_ == 10) ? OM_FORMAT_P010 : OM_FORMAT_NV12);
    output_format_.width = width_;
    output_format_.height = height_;
    output_format_.color_space = options.format.video.color_space;
    output_format_.transfer_char = options.format.video.transfer_char;
    output_format_.color_primaries = options.format.video.color_primaries;
    output_format_.color_range = options.format.video.color_range;
    output_format_.mastering_display = mastering_display_;
    output_format_.content_light_level = content_light_level_;

    if (codec_id_ == OM_CODEC_H264) {
      h264_profile_ = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR};
      if (options.format.profile == 66) h264_profile_.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_BASELINE;
      else if (options.format.profile == 100) h264_profile_.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
      else h264_profile_.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN;
      h264_profile_.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_INTERLACED_INTERLEAVED_LINES_BIT_KHR;
      h264_parser_.reset();
      h264_parser_.parseExtradata(options.extradata);
      syncH264ParserState();
      for (uint32_t i = 0; i < 32; ++i) {
        if (!h264_sps_valid_[i]) continue;
        h264_profile_.stdProfileIdc = (StdVideoH264ProfileIdc)h264_sps_[i].profile_idc;
        break;
      }
    } else if (codec_id_ == OM_CODEC_H265) {
      h265_profile_ = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR};
      if (options.format.profile == 2 || bit_depth_ == 10) {
        h265_profile_.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN_10;
        bit_depth_ = 10;
      } else {
        h265_profile_.stdProfileIdc = STD_VIDEO_H265_PROFILE_IDC_MAIN;
      }
      h265_parser_.reset();
      h265_parser_.parseExtradata(options.extradata);
      has_h265_sps_ = h265_parser_.hasSps();
      has_h265_pps_ = h265_parser_.hasPps();
    } else if (codec_id_ == OM_CODEC_AV1) {
      av1_profile_ = {VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PROFILE_INFO_KHR};
      if (options.format.profile == 1) av1_profile_.stdProfile = STD_VIDEO_AV1_PROFILE_HIGH;
      else if (options.format.profile == 2) av1_profile_.stdProfile = STD_VIDEO_AV1_PROFILE_PROFESSIONAL;
      else av1_profile_.stdProfile = STD_VIDEO_AV1_PROFILE_MAIN;
      av1_profile_.filmGrainSupport = VK_TRUE;
      av1_parser_.reset();
      if (!options.extradata.empty()) {
        av1_parser_.parse(options.extradata);
        if (av1_parser_.sequenceHeader().valid) {
          has_av1_seq_ = true;
          const auto& seq = av1_parser_.sequenceHeader();
          bit_depth_ = seq.color_config.bit_depth;
          if (seq.max_frame_width > 0 && width_ == 0) width_ = seq.max_frame_width;
          if (seq.max_frame_height > 0 && height_ == 0) height_ = seq.max_frame_height;
        }
      }
    } else if (codec_id_ == OM_CODEC_VP9) {
      vp9_parser_.reset();
      vp9_profile_ = {VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_PROFILE_INFO_KHR};
      if (options.format.profile == 1) vp9_profile_.stdProfile = STD_VIDEO_VP9_PROFILE_1;
      else if (options.format.profile == 2) {
        vp9_profile_.stdProfile = STD_VIDEO_VP9_PROFILE_2;
        bit_depth_ = 10;
      } else if (options.format.profile == 3) {
        vp9_profile_.stdProfile = STD_VIDEO_VP9_PROFILE_3;
        bit_depth_ = 10;
      } else {
        vp9_profile_.stdProfile = STD_VIDEO_VP9_PROFILE_0;
      }
    } else {
      return OM_CODEC_NOT_SUPPORTED;
    }

    if (width_ > 0 && height_ > 0) {
      auto err = initSession(width_, height_, bit_depth_);
      if (err != OM_SUCCESS) return err;
      if (codec_id_ == OM_CODEC_H264 && has_h264_sps_ && has_h264_pps_) {
        updateSessionParametersH264();
        if (!session_params_) return OM_CODEC_HWACCEL_FAILED;
      } else if (codec_id_ == OM_CODEC_AV1 && has_av1_seq_) {
        updateSessionParametersAV1();
      }
    }

    initialized_ = true;
    return OM_SUCCESS;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    if (!initialized_) return Err(OM_COMMON_NOT_INITIALIZED);
    if (packet.bytes.empty()) return Ok(std::vector<Frame>{});

    if (codec_id_ == OM_CODEC_H264) {
      // HDR10 static metadata only exists in the bitstream for H.26x; pick it
      // out before the packet disappears into the hardware decoder.
      hdr_sei::parseAnnexB(packet.bytes, false, output_format_.mastering_display,
                           output_format_.content_light_level);
      auto parsed_frames = h264_parser_.parse(packet.bytes);
      syncH264ParserState();
      applyVuiColor();

      std::vector<Frame> frames;
      for (const auto& parsed : parsed_frames) {
        if (parsed.slice_offsets.empty()) continue;
        if (parsed.bitstream.empty() || parsed.bitstream.size() > BITSTREAM_SIZE) return Err(OM_CODEC_HWACCEL_FAILED);
        if (!video_session_) {
          auto err = initSession(width_, height_, bit_depth_);
          if (err != OM_SUCCESS) return Err(err);
        }
        if (parsed.parameter_sets_changed || !session_params_) {
          updateSessionParametersH264();
          if (!session_params_) return Err(OM_CODEC_HWACCEL_FAILED);
        }

        if (parsed.is_intra) {
          for (auto& dpb : dpb_slots_) dpb.is_reference = false;
          reference_usage_.clear();
          next_ref_ = 0;
          next_slot_ = 0;
        }

        uint32_t current_idx = next_slot_;
        VulkanDPBEntry* slot = &dpb_slots_[current_idx];
        recordDecodeH264(slot, current_idx, parsed.nal, parsed.slice, parsed.bitstream, parsed.poc, parsed.is_reference, parsed.slice_offsets);

        slot->poc = parsed.poc;
        slot->frame_num = (uint32_t)parsed.slice.frame_num;
        slot->is_reference = parsed.is_reference;
        if (parsed.is_reference && dpb_slot_count_ > 1) {
          if (next_ref_ >= reference_usage_.size()) reference_usage_.resize(next_ref_ + 1);
          reference_usage_[next_ref_] = static_cast<uint8_t>(current_idx);
          next_ref_ = (next_ref_ + 1) % max_active_refs_;
          next_slot_ = (next_slot_ + 1) % dpb_slot_count_;
        }

        frames.push_back(createFrame(slot, packet.pts, packet.dts));
      }
      return Ok(std::move(frames));
    } else if (codec_id_ == OM_CODEC_H265) {
      // HDR10 static metadata only exists in the bitstream for H.26x; pick it
      // out before the packet disappears into the hardware decoder.
      hdr_sei::parseAnnexB(packet.bytes, true, output_format_.mastering_display,
                           output_format_.content_light_level);
      auto parsed_frames = h265_parser_.parse(packet.bytes);
      has_h265_sps_ = h265_parser_.hasSps();
      has_h265_pps_ = h265_parser_.hasPps();
      applyVuiColor();

      std::vector<Frame> frames;
      for (const auto& parsed : parsed_frames) {
        if (parsed.slice_offsets.empty()) continue;
        if (parsed.bitstream.empty() || parsed.bitstream.size() > BITSTREAM_SIZE) return Err(OM_CODEC_HWACCEL_FAILED);
        if (!video_session_) {
          auto err = initSession(width_, height_, bit_depth_);
          if (err != OM_SUCCESS) return Err(err);
        }
        if (parsed.parameter_sets_changed || !session_params_) {
          updateSessionParametersH265();
          if (!session_params_) return Err(OM_CODEC_HWACCEL_FAILED);
        }

        if (parsed.is_irap) {
          for (auto& dpb : dpb_slots_) dpb.is_reference = false;
          reference_usage_.clear();
          next_ref_ = 0;
          next_slot_ = 0;
        }

        uint32_t current_idx = next_slot_;
        VulkanDPBEntry* slot = &dpb_slots_[current_idx];
        recordDecodeH265(slot, current_idx, parsed);

        slot->poc = parsed.poc;
        slot->is_reference = parsed.is_reference;
        if (parsed.is_reference && dpb_slot_count_ > 1) {
          if (next_ref_ >= reference_usage_.size()) reference_usage_.resize(next_ref_ + 1);
          reference_usage_[next_ref_] = static_cast<uint8_t>(current_idx);
          next_ref_ = (next_ref_ + 1) % max_active_refs_;
          next_slot_ = (next_slot_ + 1) % dpb_slot_count_;
        }

        frames.push_back(createFrame(slot, packet.pts, packet.dts));
      }
      return Ok(std::move(frames));
    } else if (codec_id_ == OM_CODEC_AV1) {
      auto parsed_frames = av1_parser_.parse(packet.bytes);
      const auto& seq = av1_parser_.sequenceHeader();
      if (seq.valid) {
        if (!has_av1_seq_ || seq.color_config.bit_depth != bit_depth_ || !video_session_) {
          has_av1_seq_ = true;
          bit_depth_ = seq.color_config.bit_depth;
          uint32_t new_w = seq.max_frame_width > 0 ? seq.max_frame_width : width_;
          uint32_t new_h = seq.max_frame_height > 0 ? seq.max_frame_height : height_;
          initSession(new_w, new_h, bit_depth_);
          updateSessionParametersAV1();
        } else if (!session_params_) {
          updateSessionParametersAV1();
        }
        output_format_.color_primaries = mapColorPrimaries(seq.color_config.color_primaries);
        output_format_.transfer_char = mapTransferCharacteristic(seq.color_config.transfer_characteristics);
        output_format_.color_space = mapMatrixCoefficients(seq.color_config.matrix_coefficients);
        output_format_.color_range = seq.color_config.color_range ? OM_COLOR_RANGE_FULL : OM_COLOR_RANGE_LIMITED;
      }
      const auto& hdr = av1_parser_.hdrMetadata();
      if (hdr.has_mdcv) {
        // OMMasteringDisplayMetadata is defined in the H.26x SEI convention:
        // chromaticity in 0.00002 units, luminance in 0.0001 units, primaries
        // ordered green-blue-red. AV1 codes the same information as 0.16 fixed
        // point with 24.8 / 18.14 luminance and red-green-blue order, so copying
        // it across verbatim reported a 1000 nit master as 25.6 nits.
        auto av1_chroma = [](uint16_t v) -> uint16_t {
          return static_cast<uint16_t>((static_cast<uint32_t>(v) * 50000u + 32768u) / 65536u);
        };
        const uint32_t rgb_for_sei_index[3] = {1, 2, 0}; // green, blue, red
        for (uint32_t i = 0; i < 3; ++i) {
          const uint32_t c = rgb_for_sei_index[i];
          mastering_display_.display_primaries[i][0] = av1_chroma(hdr.primary_chromaticity_x[c]);
          mastering_display_.display_primaries[i][1] = av1_chroma(hdr.primary_chromaticity_y[c]);
        }
        mastering_display_.white_point[0] = av1_chroma(hdr.white_point_chromaticity_x);
        mastering_display_.white_point[1] = av1_chroma(hdr.white_point_chromaticity_y);
        mastering_display_.max_display_mastering_luminance =
            static_cast<uint32_t>((static_cast<uint64_t>(hdr.luminance_max) * 10000u + 128u) / 256u);
        mastering_display_.min_display_mastering_luminance =
            static_cast<uint32_t>((static_cast<uint64_t>(hdr.luminance_min) * 10000u + 8192u) / 16384u);
        mastering_display_.has_value = true;
        output_format_.mastering_display = mastering_display_;
      }
      if (hdr.has_cll) {
        content_light_level_.max_content_light_level = hdr.max_cll;
        content_light_level_.max_pic_average_light_level = hdr.max_fall;
        content_light_level_.has_value = true;
        output_format_.content_light_level = content_light_level_;
      }

      std::vector<Frame> frames;
      for (const auto& parsed : parsed_frames) {
        if (parsed.header.show_existing_frame) {
          uint8_t show_idx = parsed.header.frame_to_show_map_idx;
          if (show_idx < 8 && av1_refs_[show_idx].valid) {
            int32_t slot_idx = av1_refs_[show_idx].dpb_slot;
            if (slot_idx >= 0 && slot_idx < (int32_t)dpb_slot_count_) {
              frames.push_back(createFrame(&dpb_slots_[slot_idx], packet.pts, packet.dts));
            }
          }
        } else if (parsed.has_frame) {
          if (parsed.bitstream.empty() || parsed.bitstream.size() > BITSTREAM_SIZE) return Err(OM_CODEC_HWACCEL_FAILED);
          if (!video_session_) {
            uint32_t fw = parsed.header.frame_width > 0 ? parsed.header.frame_width : width_;
            uint32_t fh = parsed.header.frame_height > 0 ? parsed.header.frame_height : height_;
            auto err = initSession(fw, fh, bit_depth_);
            if (err != OM_SUCCESS) return Err(err);
            if (has_av1_seq_) updateSessionParametersAV1();
          }
          if (has_av1_seq_ && !session_params_) {
            updateSessionParametersAV1();
          }
          if (!session_params_) return Err(OM_CODEC_HWACCEL_FAILED);

          if (parsed.header.frame_width > 0) output_format_.width = parsed.header.frame_width;
          if (parsed.header.frame_height > 0) output_format_.height = parsed.header.frame_height;

          uint32_t current_idx = next_slot_;
          std::array<bool, MAX_DPB_SLOTS + 1> in_use{};
          for (int i = 0; i < 8; ++i) {
            if (av1_refs_[i].valid && av1_refs_[i].dpb_slot >= 0 && av1_refs_[i].dpb_slot < static_cast<int32_t>(dpb_slot_count_)) {
              in_use[av1_refs_[i].dpb_slot] = true;
            }
          }
          for (uint32_t i = 0; i < dpb_slot_count_; ++i) {
            uint32_t candidate = (next_slot_ + i) % dpb_slot_count_;
            if (!in_use[candidate]) {
              current_idx = candidate;
              break;
            }
          }
          next_slot_ = (current_idx + 1) % dpb_slot_count_;

          VulkanDPBEntry* slot = &dpb_slots_[current_idx];
          recordDecodeAV1(slot, current_idx, parsed);

          if (parsed.header.show_frame) {
            frames.push_back(createFrame(slot, packet.pts, packet.dts));
          }
        }
      }
      return Ok(std::move(frames));
    } else if (codec_id_ == OM_CODEC_VP9) {
      auto parsed_frames = vp9_parser_.parse(packet.bytes);
      std::vector<Frame> frames;
      for (const auto& parsed : parsed_frames) {
        if (!parsed.header.valid) continue;
        if (parsed.header.bit_depth != bit_depth_ ||
            parsed.header.subsampling_x != vp9_subsampling_x_ ||
            parsed.header.subsampling_y != vp9_subsampling_y_ || !video_session_) {
          bit_depth_ = parsed.header.bit_depth;
          vp9_subsampling_x_ = parsed.header.subsampling_x;
          vp9_subsampling_y_ = parsed.header.subsampling_y;
          uint32_t fw = parsed.header.frame_width > 0 ? parsed.header.frame_width : width_;
          uint32_t fh = parsed.header.frame_height > 0 ? parsed.header.frame_height : height_;
          auto err = initSession(fw, fh, bit_depth_);
          if (err != OM_SUCCESS) return Err(err);
        }
        auto [prim, cs] = mapVP9ColorSpace(parsed.header.color_space);
        if (prim != OM_PRIMARIES_UNKNOWN) output_format_.color_primaries = prim;
        if (cs != OM_COLOR_SPACE_UNKNOWN) output_format_.color_space = cs;
        output_format_.color_range = parsed.header.color_range ? OM_COLOR_RANGE_FULL : OM_COLOR_RANGE_LIMITED;
        if (output_format_.color_primaries == OM_PRIMARIES_BT2020 && output_format_.transfer_char == OM_TRANSFER_UNKNOWN) {
          if (parsed.header.bit_depth >= 10) output_format_.transfer_char = OM_TRANSFER_PQ;
        }
        output_format_.mastering_display = mastering_display_;
        output_format_.content_light_level = content_light_level_;

        if (parsed.header.show_existing_frame) {
          uint8_t show_idx = parsed.header.frame_to_show_map_idx;
          if (show_idx < 8 && vp9_refs_[show_idx].valid) {
            int32_t slot_idx = vp9_refs_[show_idx].dpb_slot;
            if (slot_idx >= 0 && slot_idx < (int32_t)dpb_slot_count_) {
              frames.push_back(createFrame(&dpb_slots_[slot_idx], packet.pts, packet.dts));
            }
          }
        } else {
          if (parsed.bitstream.empty() || parsed.bitstream.size() > BITSTREAM_SIZE) return Err(OM_CODEC_HWACCEL_FAILED);
          if (parsed.header.frame_width > 0) output_format_.width = parsed.header.frame_width;
          if (parsed.header.frame_height > 0) output_format_.height = parsed.header.frame_height;

          uint32_t current_idx = next_slot_;
          std::array<bool, MAX_DPB_SLOTS + 1> in_use{};
          for (int i = 0; i < 8; ++i) {
            if (vp9_refs_[i].valid && vp9_refs_[i].dpb_slot >= 0 && vp9_refs_[i].dpb_slot < static_cast<int32_t>(dpb_slot_count_)) {
              in_use[vp9_refs_[i].dpb_slot] = true;
            }
          }
          for (uint32_t i = 0; i < dpb_slot_count_; ++i) {
            uint32_t candidate = (next_slot_ + i) % dpb_slot_count_;
            if (!in_use[candidate]) {
              current_idx = candidate;
              break;
            }
          }
          next_slot_ = (current_idx + 1) % dpb_slot_count_;

          VulkanDPBEntry* slot = &dpb_slots_[current_idx];
          recordDecodeVP9(slot, current_idx, parsed);

          if (parsed.header.show_frame) {
            frames.push_back(createFrame(slot, packet.pts, packet.dts));
          }
        }
      }
      return Ok(std::move(frames));
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

  void flush() override {
    first_decode_ = true;
    dpb_slot_active_.fill(false);
    reference_usage_.clear();
    next_ref_ = 0;
    next_slot_ = 0;
    for (auto& dpb : dpb_slots_) dpb.is_reference = false;
    for (int i = 0; i < 8; ++i) {
      av1_refs_[i] = {};
      vp9_refs_[i] = {};
    }
    h264_parser_.reset();
    h265_parser_.reset();
    vp9_parser_.reset();
    av1_parser_.reset();
  }

private:

  void applyVuiColor() {
    auto apply = [&](bool colour_present, int primaries, int transfer, int matrix,
                     bool signal_present, bool full_range) {
      if (colour_present) {
        output_format_.color_primaries = color_codes::primariesFromCode(primaries);
        output_format_.transfer_char = color_codes::transferFromCode(transfer);
        output_format_.color_space = color_codes::colorSpaceFromMatrix(matrix);
      }
      if (signal_present) {
        output_format_.color_range = full_range ? OM_COLOR_RANGE_FULL : OM_COLOR_RANGE_LIMITED;
      }
    };

    if (codec_id_ == OM_CODEC_H264 && has_h264_sps_) {
      for (uint32_t i = 0; i < 32; ++i) {
        if (!h264_sps_valid_[i]) continue;
        const auto& sps = h264_sps_[i];
        if (!sps.vui_parameters_present_flag) break;
        apply(sps.vui.colour_description_present_flag != 0, sps.vui.colour_primaries,
              sps.vui.transfer_characteristics, sps.vui.matrix_coefficients,
              sps.vui.video_signal_type_present_flag != 0,
              sps.vui.video_full_range_flag != 0);
        break;
      }
    } else if (codec_id_ == OM_CODEC_H265 && h265_parser_.hasSps()) {
      for (int i = 0; i < 16; ++i) {
        const auto& sps = h265_parser_.sps(i);
        if (!sps.valid) continue;
        if (!sps.vui_parameters_present_flag) break;
        apply(sps.vui.colour_description_present_flag, sps.vui.colour_primaries,
              sps.vui.transfer_characteristics, sps.vui.matrix_coeffs,
              sps.vui.video_signal_type_present_flag,
              sps.vui.video_full_range_flag);
        break;
      }
    }
  }

  // 4:2:0 -> (1,1), 4:2:2 -> (1,0), 4:4:4 -> (0,0), monochrome -> no chroma.
  void resolveChromaFormat() {
    subsampling_x_ = 1;
    subsampling_y_ = 1;
    mono_chrome_ = false;

    auto fromChromaFormatIdc = [&](uint32_t idc) {
      switch (idc) {
        case 0: mono_chrome_ = true; subsampling_x_ = 1; subsampling_y_ = 1; break;
        case 2: subsampling_x_ = 1; subsampling_y_ = 0; break;  // 4:2:2
        case 3: subsampling_x_ = 0; subsampling_y_ = 0; break;  // 4:4:4
        default: subsampling_x_ = 1; subsampling_y_ = 1; break; // 4:2:0
      }
    };

    if (codec_id_ == OM_CODEC_H264 && has_h264_sps_) {
      for (uint32_t i = 0; i < 32; ++i) {
        if (!h264_sps_valid_[i]) continue;
        fromChromaFormatIdc(static_cast<uint32_t>(h264_sps_[i].chroma_format_idc));
        break;
      }
    } else if (codec_id_ == OM_CODEC_H265 && h265_parser_.hasSps()) {
      for (int i = 0; i < 16; ++i) {
        const auto& sps = h265_parser_.sps(i);
        if (!sps.valid) continue;
        fromChromaFormatIdc(static_cast<uint32_t>(sps.chroma_format_idc));
        break;
      }
    } else if (codec_id_ == OM_CODEC_AV1) {
      const auto& cc = av1_parser_.sequenceHeader().color_config;
      if (av1_parser_.sequenceHeader().valid) {
        mono_chrome_ = cc.mono_chrome;
        subsampling_x_ = cc.subsampling_x;
        subsampling_y_ = cc.subsampling_y;
      }
    } else if (codec_id_ == OM_CODEC_VP9) {
      subsampling_x_ = vp9_subsampling_x_;
      subsampling_y_ = vp9_subsampling_y_;
    }
  }

  auto chromaSubsamplingFlag() const -> VkVideoChromaSubsamplingFlagBitsKHR {
    if (mono_chrome_) return VK_VIDEO_CHROMA_SUBSAMPLING_MONOCHROME_BIT_KHR;
    if (subsampling_x_ == 0 && subsampling_y_ == 0) return VK_VIDEO_CHROMA_SUBSAMPLING_444_BIT_KHR;
    if (subsampling_x_ == 1 && subsampling_y_ == 0) return VK_VIDEO_CHROMA_SUBSAMPLING_422_BIT_KHR;
    return VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
  }

  auto pickOutputPixelFormat() const -> OMPixelFormat {
    if (mono_chrome_) return bit_depth_ > 8 ? OM_FORMAT_GRAY16 : OM_FORMAT_GRAY8;
    if (subsampling_x_ == 0 && subsampling_y_ == 0)
      return bit_depth_ > 8 ? OM_FORMAT_YUV444P16 : OM_FORMAT_NV24;
    if (subsampling_x_ == 1 && subsampling_y_ == 0)
      return bit_depth_ > 8 ? OM_FORMAT_YUV422P16 : OM_FORMAT_NV16;
    if (bit_depth_ == 12) return OM_FORMAT_P012;
    if (bit_depth_ == 10) return OM_FORMAT_P010;
    return OM_FORMAT_NV12;
  }

  // Asks the driver which formats it can actually produce for this profile and
  // usage instead of assuming a 4:2:0 layout. Returns VK_FORMAT_UNDEFINED when
  // the combination is unsupported, which is how an unsupported chroma format
  // surfaces as a clean decoder-open failure.
  auto pickVideoFormat(VkImageUsageFlags usage, const VkVideoProfileListInfoKHR& profiles) const -> VkFormat {
    VkPhysicalDeviceVideoFormatInfoKHR info = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR};
    info.pNext = const_cast<VkVideoProfileListInfoKHR*>(&profiles);
    info.imageUsage = usage;

    uint32_t count = 0;
    if (VK(vkGetPhysicalDeviceVideoFormatPropertiesKHR)(hw_context_->vk_physical_device, &info, &count, nullptr) != VK_SUCCESS || count == 0)
      return VK_FORMAT_UNDEFINED;
    std::vector<VkVideoFormatPropertiesKHR> props(count, {VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR});
    if (VK(vkGetPhysicalDeviceVideoFormatPropertiesKHR)(hw_context_->vk_physical_device, &info, &count, props.data()) != VK_SUCCESS)
      return VK_FORMAT_UNDEFINED;

    // Prefer the layout that matches the stream; otherwise take whatever the
    // driver offers first, which is always a usable decode target.
    const VkFormat preferred = preferredVideoFormat();
    for (const auto& prop : props)
      if (prop.format == preferred) return prop.format;
    return props[0].format;
  }

  auto preferredVideoFormat() const -> VkFormat {
    if (mono_chrome_) {
      if (bit_depth_ == 10) return VK_FORMAT_R10X6_UNORM_PACK16;
      if (bit_depth_ == 12) return VK_FORMAT_R12X4_UNORM_PACK16;
      return VK_FORMAT_R8_UNORM;
    }
    if (subsampling_x_ == 0 && subsampling_y_ == 0) {
      if (bit_depth_ == 10) return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16;
      if (bit_depth_ == 12) return VK_FORMAT_G12X4_B12X4R12X4_2PLANE_444_UNORM_3PACK16;
      return VK_FORMAT_G8_B8R8_2PLANE_444_UNORM;
    }
    if (subsampling_x_ == 1 && subsampling_y_ == 0) {
      if (bit_depth_ == 10) return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16;
      if (bit_depth_ == 12) return VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16;
      return VK_FORMAT_G8_B8R8_2PLANE_422_UNORM;
    }
    if (bit_depth_ == 10) return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
    if (bit_depth_ == 12) return VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16;
    return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
  }

  void syncH264ParserState() {
    const auto& state = h264_parser_.state();
    std::copy(std::begin(state.sps), std::end(state.sps), std::begin(h264_sps_));
    std::copy(std::begin(state.pps), std::end(state.pps), std::begin(h264_pps_));
    std::copy(std::begin(state.sps_valid), std::end(state.sps_valid), std::begin(h264_sps_valid_));
    std::copy(std::begin(state.pps_valid), std::end(state.pps_valid), std::begin(h264_pps_valid_));
    has_h264_sps_ = state.has_sps;
    has_h264_pps_ = state.has_pps;
  }

  void updateSessionParametersH265() {
    if (!video_session_ || !has_h265_sps_ || !has_h265_pps_) return;
    if (session_params_) {
      VK(vkDestroyVideoSessionParametersKHR)(hw_context_->vk_device, session_params_, hw_context_->allocator);
      session_params_ = VK_NULL_HANDLE;
    }

    std::vector<StdVideoH265VideoParameterSet> vpss;
    for (int i = 0; i < 16; ++i) {
      const auto& d = h265_parser_.vps(i);
      if (!d.valid) continue;
      StdVideoH265VideoParameterSet v = {};
      v.vps_video_parameter_set_id = (uint8_t)d.id;
      v.vps_max_sub_layers_minus1 = (uint8_t)d.max_sub_layers_minus1;
      v.flags.vps_temporal_id_nesting_flag = d.temporal_id_nesting_flag;
      vpss.push_back(v);
    }

    std::vector<StdVideoH265SequenceParameterSet> spss;
    std::vector<StdVideoH265SequenceParameterSetVui> vuis(16);
    std::vector<StdVideoH265ShortTermRefPicSet> sps_strps(16 * 64);

    for (int i = 0; i < 16; ++i) {
      const auto& d = h265_parser_.sps(i);
      if (!d.valid) continue;
      StdVideoH265SequenceParameterSet s = {};
      s.sps_video_parameter_set_id = (uint8_t)d.vps_id;
      s.sps_max_sub_layers_minus1 = (uint8_t)d.max_sub_layers_minus1;
      s.sps_seq_parameter_set_id = (uint8_t)d.id;
      s.chroma_format_idc = (StdVideoH265ChromaFormatIdc)d.chroma_format_idc;
      s.pic_width_in_luma_samples = (uint32_t)d.pic_width_in_luma_samples;
      s.pic_height_in_luma_samples = (uint32_t)d.pic_height_in_luma_samples;
      s.bit_depth_luma_minus8 = (uint8_t)d.bit_depth_luma_minus8;
      s.bit_depth_chroma_minus8 = (uint8_t)d.bit_depth_chroma_minus8;
      s.log2_max_pic_order_cnt_lsb_minus4 = (uint8_t)d.log2_max_pic_order_cnt_lsb_minus4;
      s.log2_min_luma_coding_block_size_minus3 = (uint8_t)d.log2_min_luma_coding_block_size_minus3;
      s.log2_diff_max_min_luma_coding_block_size = (uint8_t)d.log2_diff_max_min_luma_coding_block_size;
      s.log2_min_luma_transform_block_size_minus2 = (uint8_t)d.log2_min_luma_transform_block_size_minus2;
      s.log2_diff_max_min_luma_transform_block_size = (uint8_t)d.log2_diff_max_min_luma_transform_block_size;
      s.max_transform_hierarchy_depth_inter = (uint8_t)d.max_transform_hierarchy_depth_inter;
      s.max_transform_hierarchy_depth_intra = (uint8_t)d.max_transform_hierarchy_depth_intra;
      s.num_short_term_ref_pic_sets = (uint8_t)d.num_short_term_ref_pic_sets;
      s.num_long_term_ref_pics_sps = (uint8_t)d.num_long_term_ref_pics_sps;
      s.flags.conformance_window_flag = d.conformance_window_flag;
      s.flags.separate_colour_plane_flag = d.separate_colour_plane_flag;
      s.flags.sps_sub_layer_ordering_info_present_flag = d.sps_sub_layer_ordering_info_present_flag;
      s.flags.scaling_list_enabled_flag = d.scaling_list_enabled_flag;
      s.flags.amp_enabled_flag = d.amp_enabled_flag;
      s.flags.sample_adaptive_offset_enabled_flag = d.sample_adaptive_offset_enabled_flag;
      s.flags.pcm_enabled_flag = d.pcm_enabled_flag;
      s.flags.long_term_ref_pics_present_flag = d.long_term_ref_pics_present_flag;
      s.flags.sps_temporal_mvp_enabled_flag = d.sps_temporal_mvp_enabled_flag;
      s.flags.strong_intra_smoothing_enabled_flag = d.strong_intra_smoothing_enabled_flag;
      s.flags.vui_parameters_present_flag = d.vui_parameters_present_flag;

      if (d.num_short_term_ref_pic_sets > 0) {
        s.pShortTermRefPicSet = &sps_strps[i * 64];
        for (int j = 0; j < d.num_short_term_ref_pic_sets; ++j) {
          auto& strps = sps_strps[i * 64 + j];
          const auto& src = d.st_ref_pic_set[j];
          strps.flags.inter_ref_pic_set_prediction_flag = src.inter_ref_pic_set_prediction_flag;
          strps.flags.delta_rps_sign = src.delta_rps_sign;
          strps.delta_idx_minus1 = src.delta_idx_minus1;
          strps.abs_delta_rps_minus1 = src.abs_delta_rps_minus1;
          for (int k = 0; k < 16; ++k) {
            if (src.used_by_curr_pic_flag[k]) strps.used_by_curr_pic_flag |= (1 << k);
            if (src.use_delta_flag[k]) strps.use_delta_flag |= (1 << k);
          }
          for (int k = 0; k < src.num_negative_pics; ++k) {
            strps.delta_poc_s0_minus1[k] = -src.delta_poc_s0[k] - 1;
            if (src.used_by_curr_pic_s0_flag[k]) strps.used_by_curr_pic_s0_flag |= (1 << k);
          }
          for (int k = 0; k < src.num_positive_pics; ++k) {
            strps.delta_poc_s1_minus1[k] = src.delta_poc_s1[k] - 1;
            if (src.used_by_curr_pic_s1_flag[k]) strps.used_by_curr_pic_s1_flag |= (1 << k);
          }
          strps.num_negative_pics = src.num_negative_pics;
          strps.num_positive_pics = src.num_positive_pics;
        }
      }

      if (d.vui_parameters_present_flag) {
        auto& vui = vuis[i];
        s.pSequenceParameterSetVui = &vui;
        vui.flags.aspect_ratio_info_present_flag = d.vui.aspect_ratio_info_present_flag;
        vui.flags.overscan_info_present_flag = d.vui.overscan_info_present_flag;
        vui.flags.overscan_appropriate_flag = d.vui.overscan_appropriate_flag;
        vui.flags.video_signal_type_present_flag = d.vui.video_signal_type_present_flag;
        vui.flags.video_full_range_flag = d.vui.video_full_range_flag;
        vui.flags.colour_description_present_flag = d.vui.colour_description_present_flag;
        vui.flags.chroma_loc_info_present_flag = d.vui.chroma_loc_info_present_flag;
        vui.flags.neutral_chroma_indication_flag = d.vui.neutral_chroma_indication_flag;
        vui.flags.field_seq_flag = d.vui.field_seq_flag;
        vui.flags.frame_field_info_present_flag = d.vui.frame_field_info_present_flag;
        vui.flags.default_display_window_flag = d.vui.default_display_window_flag;
        vui.flags.vui_timing_info_present_flag = d.vui.vui_timing_info_present_flag;
        vui.flags.vui_poc_proportional_to_timing_flag = d.vui.vui_poc_proportional_to_timing_flag;
        vui.flags.vui_hrd_parameters_present_flag = d.vui.vui_hrd_parameters_present_flag;
        vui.flags.bitstream_restriction_flag = d.vui.bitstream_restriction_flag;
        vui.flags.tiles_fixed_structure_flag = d.vui.tiles_fixed_structure_flag;
        vui.flags.motion_vectors_over_pic_boundaries_flag = d.vui.motion_vectors_over_pic_boundaries_flag;
        vui.flags.restricted_ref_pic_lists_flag = d.vui.restricted_ref_pic_lists_flag;
        vui.aspect_ratio_idc = (StdVideoH265AspectRatioIdc)d.vui.aspect_ratio_idc;
        vui.sar_width = (uint16_t)d.vui.sar_width;
        vui.sar_height = (uint16_t)d.vui.sar_height;
        vui.video_format = (uint8_t)d.vui.video_format;
        vui.colour_primaries = (uint8_t)d.vui.colour_primaries;
        vui.transfer_characteristics = (uint8_t)d.vui.transfer_characteristics;
        vui.matrix_coeffs = (uint8_t)d.vui.matrix_coeffs;
        vui.chroma_sample_loc_type_top_field = (uint8_t)d.vui.chroma_sample_loc_type_top_field;
        vui.chroma_sample_loc_type_bottom_field = (uint8_t)d.vui.chroma_sample_loc_type_bottom_field;
        vui.def_disp_win_left_offset = (uint16_t)d.vui.def_disp_win_left_offset;
        vui.def_disp_win_right_offset = (uint16_t)d.vui.def_disp_win_right_offset;
        vui.def_disp_win_top_offset = (uint16_t)d.vui.def_disp_win_top_offset;
        vui.def_disp_win_bottom_offset = (uint16_t)d.vui.def_disp_win_bottom_offset;
        vui.vui_num_units_in_tick = d.vui.vui_num_units_in_tick;
        vui.vui_time_scale = d.vui.vui_time_scale;
        vui.vui_num_ticks_poc_diff_one_minus1 = (uint32_t)d.vui.vui_num_ticks_poc_diff_one_minus1;
        vui.min_spatial_segmentation_idc = (uint16_t)d.vui.min_spatial_segmentation_idc;
        vui.max_bytes_per_pic_denom = (uint8_t)d.vui.max_bytes_per_pic_denom;
        vui.max_bits_per_min_cu_denom = (uint8_t)d.vui.max_bits_per_min_cu_denom;
        vui.log2_max_mv_length_horizontal = (uint8_t)d.vui.log2_max_mv_length_horizontal;
        vui.log2_max_mv_length_vertical = (uint8_t)d.vui.log2_max_mv_length_vertical;
      }

      spss.push_back(s);
    }

    std::vector<StdVideoH265PictureParameterSet> ppss;
    for (int i = 0; i < 64; ++i) {
      const auto& d = h265_parser_.pps(i);
      if (!d.valid) continue;
      StdVideoH265PictureParameterSet p = {};
      p.pps_pic_parameter_set_id = (uint8_t)d.id;
      p.pps_seq_parameter_set_id = (uint8_t)d.sps_id;
      p.num_extra_slice_header_bits = (uint8_t)d.num_extra_slice_header_bits;
      p.num_ref_idx_l0_default_active_minus1 = (uint8_t)d.num_ref_idx_l0_default_active_minus1;
      p.num_ref_idx_l1_default_active_minus1 = (uint8_t)d.num_ref_idx_l1_default_active_minus1;
      p.init_qp_minus26 = (int8_t)d.init_qp_minus26;
      p.diff_cu_qp_delta_depth = (uint8_t)d.diff_cu_qp_delta_depth;
      p.pps_cb_qp_offset = (int8_t)d.pps_cb_qp_offset;
      p.pps_cr_qp_offset = (int8_t)d.pps_cr_qp_offset;
      p.num_tile_columns_minus1 = (uint8_t)d.num_tile_columns_minus1;
      p.num_tile_rows_minus1 = (uint8_t)d.num_tile_rows_minus1;
      p.log2_parallel_merge_level_minus2 = (uint8_t)d.log2_parallel_merge_level_minus2;
      p.flags.dependent_slice_segments_enabled_flag = d.dependent_slice_segments_enabled_flag;
      p.flags.output_flag_present_flag = d.output_flag_present_flag;
      p.flags.sign_data_hiding_enabled_flag = d.sign_data_hiding_enabled_flag;
      p.flags.cabac_init_present_flag = d.cabac_init_present_flag;
      p.flags.constrained_intra_pred_flag = d.constrained_intra_pred_flag;
      p.flags.transform_skip_enabled_flag = d.transform_skip_enabled_flag;
      p.flags.cu_qp_delta_enabled_flag = d.cu_qp_delta_enabled_flag;
      p.flags.pps_slice_chroma_qp_offsets_present_flag = d.pps_slice_chroma_qp_offsets_present_flag;
      p.flags.weighted_pred_flag = d.weighted_pred_flag;
      p.flags.weighted_bipred_flag = d.weighted_bipred_flag;
      p.flags.transquant_bypass_enabled_flag = d.transquant_bypass_enabled_flag;
      p.flags.tiles_enabled_flag = d.tiles_enabled_flag;
      p.flags.entropy_coding_sync_enabled_flag = d.entropy_coding_sync_enabled_flag;
      p.flags.uniform_spacing_flag = d.uniform_spacing_flag;
      p.flags.loop_filter_across_tiles_enabled_flag = d.loop_filter_across_tiles_enabled_flag;
      p.flags.pps_loop_filter_across_slices_enabled_flag = d.pps_loop_filter_across_slices_enabled_flag;
      p.flags.deblocking_filter_control_present_flag = d.deblocking_filter_control_present_flag;
      p.flags.deblocking_filter_override_enabled_flag = d.deblocking_filter_override_enabled_flag;
      p.flags.pps_deblocking_filter_disabled_flag = d.pps_deblocking_filter_disabled_flag;
      p.flags.pps_scaling_list_data_present_flag = d.pps_scaling_list_data_present_flag;
      p.flags.lists_modification_present_flag = d.lists_modification_present_flag;
      p.flags.slice_segment_header_extension_present_flag = d.slice_segment_header_extension_present_flag;
      ppss.push_back(p);
    }

    if (spss.empty() || ppss.empty()) return;

    VkVideoDecodeH265SessionParametersAddInfoKHR add = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR};
    add.stdVPSCount = (uint32_t)vpss.size();
    add.pStdVPSs = vpss.data();
    add.stdSPSCount = (uint32_t)spss.size();
    add.pStdSPSs = spss.data();
    add.stdPPSCount = (uint32_t)ppss.size();
    add.pStdPPSs = ppss.data();

    VkVideoDecodeH265SessionParametersCreateInfoKHR h265 = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR};
    h265.maxStdVPSCount = std::max(1u, (uint32_t)vpss.size());
    h265.maxStdSPSCount = (uint32_t)spss.size();
    h265.maxStdPPSCount = (uint32_t)ppss.size();
    h265.pParametersAddInfo = &add;
    VkVideoSessionParametersCreateInfoKHR info = {VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR};
    info.pNext = &h265;
    info.videoSession = video_session_;
    if (VK(vkCreateVideoSessionParametersKHR)(hw_context_->vk_device, &info, hw_context_->allocator, &session_params_) != VK_SUCCESS) {
      session_params_ = VK_NULL_HANDLE;
    }
  }

  void recordDecodeH265(VulkanDPBEntry* slot, uint32_t slot_idx, const video_parser::H265ParsedFrame& parsed) {
    size_t aligned_size = alignUp(parsed.bitstream.size(), static_cast<size_t>(min_bitstream_alignment_));
    std::memcpy(bitstream_ptr_, parsed.bitstream.data(), parsed.bitstream.size());
    VkCommandBuffer cb = command_buffers_[0];
    VK(vkResetCommandBuffer)(cb, 0);
    VkCommandBufferBeginInfo begin_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
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

    std::array<VkVideoReferenceSlotInfoKHR, MAX_DPB_SLOTS + 1> slot_infos;
    std::array<VkVideoPictureResourceInfoKHR, MAX_DPB_SLOTS + 1> slot_pics;
    std::array<VkVideoDecodeH265DpbSlotInfoKHR, MAX_DPB_SLOTS + 1> slot_h265;
    std::array<StdVideoDecodeH265ReferenceInfo, MAX_DPB_SLOTS + 1> slot_stds;

    std::memset(slot_infos.data(), 0, sizeof(slot_infos));
    std::memset(slot_pics.data(), 0, sizeof(slot_pics));
    std::memset(slot_h265.data(), 0, sizeof(slot_h265));
    std::memset(slot_stds.data(), 0, sizeof(slot_stds));

    for (uint32_t i = 0; i < dpb_slot_count_; ++i) {
      slot_stds[i].PicOrderCntVal = dpb_slots_[i].poc;
      slot_stds[i].flags.unused_for_reference = dpb_slots_[i].is_reference ? 0 : 1;

      slot_h265[i].sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_DPB_SLOT_INFO_KHR;
      slot_h265[i].pStdReferenceInfo = &slot_stds[i];

      slot_pics[i].sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
      slot_pics[i].codedExtent = {padded_width_, padded_height_};
      slot_pics[i].baseArrayLayer = i;
      slot_pics[i].imageViewBinding = dpb_image_view_;

      slot_infos[i].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
      slot_infos[i].pNext = &slot_h265[i];
      slot_infos[i].slotIndex = (int32_t)i;
      slot_infos[i].pPictureResource = &slot_pics[i];
    }

    const auto& sh = parsed.slice_headers.empty() ? video_parser::H265SliceHeader{} : parsed.slice_headers[0];
    const auto& st_ref = sh.st_ref_pic_set;

    std::vector<int32_t> poc_st_curr_before;
    std::vector<int32_t> poc_st_curr_after;
    for (int i = 0; i < st_ref.num_negative_pics; ++i) {
      if (st_ref.used_by_curr_pic_s0_flag[i]) poc_st_curr_before.push_back(parsed.poc + st_ref.delta_poc_s0[i]);
    }
    for (int i = 0; i < st_ref.num_positive_pics; ++i) {
      if (st_ref.used_by_curr_pic_s1_flag[i]) poc_st_curr_after.push_back(parsed.poc + st_ref.delta_poc_s1[i]);
    }

    std::array<uint8_t, 8> st_curr_before; st_curr_before.fill(0xFF);
    std::array<uint8_t, 8> st_curr_after; st_curr_after.fill(0xFF);
    std::array<uint8_t, 8> lt_curr; lt_curr.fill(0xFF);

    std::array<VkVideoReferenceSlotInfoKHR, MAX_DPB_SLOTS + 1> begin_slots;
    std::array<VkVideoReferenceSlotInfoKHR, MAX_DPB_SLOTS + 1> ref_slots;
    std::memset(begin_slots.data(), 0, sizeof(begin_slots));
    std::memset(ref_slots.data(), 0, sizeof(ref_slots));

    uint32_t ref_count = 0;
    for (uint8_t ref_slot : reference_usage_) {
      if (ref_slot >= dpb_slot_count_ || ref_slot == slot_idx || !dpb_slots_[ref_slot].is_reference) continue;

      bool already_added = false;
      for (uint32_t i = 0; i < ref_count; ++i) if (ref_slots[i].slotIndex == (int32_t)ref_slot) { already_added = true; break; }
      if (already_added) continue;

      ref_slots[ref_count] = slot_infos[ref_slot];
      begin_slots[ref_count] = slot_infos[ref_slot];

      int32_t ref_poc = dpb_slots_[ref_slot].poc;
      auto it_b = std::find(poc_st_curr_before.begin(), poc_st_curr_before.end(), ref_poc);
      if (it_b != poc_st_curr_before.end() && std::distance(poc_st_curr_before.begin(), it_b) < 8)
        st_curr_before[std::distance(poc_st_curr_before.begin(), it_b)] = (uint8_t)ref_count;

      auto it_a = std::find(poc_st_curr_after.begin(), poc_st_curr_after.end(), ref_poc);
      if (it_a != poc_st_curr_after.end() && std::distance(poc_st_curr_after.begin(), it_a) < 8)
        st_curr_after[std::distance(poc_st_curr_after.begin(), it_a)] = (uint8_t)ref_count;

      ++ref_count;
    }
    begin_slots[ref_count] = slot_infos[slot_idx];
    if (!dpb_slot_active_[slot_idx]) begin_slots[ref_count].slotIndex = -1;

    StdVideoDecodeH265PictureInfo std_pic;
    std::memset(&std_pic, 0, sizeof(std_pic));
    std_pic.flags.IrapPicFlag = parsed.is_irap;
    std_pic.flags.IdrPicFlag = (parsed.nal_unit_type == 19 || parsed.nal_unit_type == 20) ? 1 : 0;
    std_pic.flags.IsReference = parsed.is_reference ? 1 : 0;
    std_pic.flags.short_term_ref_pic_set_sps_flag = sh.short_term_ref_pic_set_sps_flag;
    if (sh.pps_id >= 0 && sh.pps_id < 64 && h265_parser_.pps(sh.pps_id).valid) {
      std_pic.pps_pic_parameter_set_id = (uint8_t)sh.pps_id;
      int sps_id = h265_parser_.pps(sh.pps_id).sps_id;
      if (sps_id >= 0 && sps_id < 16 && h265_parser_.sps(sps_id).valid) {
        std_pic.pps_seq_parameter_set_id = (uint8_t)sps_id;
        std_pic.sps_video_parameter_set_id = (uint8_t)h265_parser_.sps(sps_id).vps_id;
      }
    }
    std_pic.PicOrderCntVal = parsed.poc;
    for (size_t i = 0; i < 8; ++i) {
      std_pic.RefPicSetStCurrBefore[i] = st_curr_before[i];
      std_pic.RefPicSetStCurrAfter[i] = st_curr_after[i];
      std_pic.RefPicSetLtCurr[i] = lt_curr[i];
    }

    VkVideoDecodeH265PictureInfoKHR h265_pic = {VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PICTURE_INFO_KHR};
    h265_pic.pStdPictureInfo = &std_pic;
    h265_pic.sliceSegmentCount = (uint32_t)parsed.slice_offsets.size();
    h265_pic.pSliceSegmentOffsets = parsed.slice_offsets.data();

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
    decode.pNext = &h265_pic;
    decode.srcBuffer = bitstream_buffer_;
    decode.srcBufferOffset = 0;
    decode.srcBufferRange = aligned_size;
    decode.dstPictureResource = dst;
    decode.pSetupReferenceSlot = &slot_infos[slot_idx];
    decode.referenceSlotCount = ref_count;
    decode.pReferenceSlots = ref_count == 0 ? nullptr : ref_slots.data();
    VK(vkCmdDecodeVideoKHR)(cb, &decode);    VkVideoEndCodingInfoKHR end = {VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
    VK(vkCmdEndVideoCodingKHR)(cb, &end);
    VK(vkEndCommandBuffer)(cb);
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cb};
    VK(vkQueueSubmit)(hw_context_->video_decode_queue, 1, &submit, decode_fence_);
    VK(vkWaitForFences)(hw_context_->vk_device, 1, &decode_fence_, VK_TRUE, UINT64_MAX);
    VK(vkResetFences)(hw_context_->vk_device, 1, &decode_fence_);
    // Only a picture that is kept as a reference activates its DPB slot;
    // a non-reference picture leaves the slot inactive, so binding it by
    // index on a later frame is invalid.
    dpb_slot_active_[slot_idx] = parsed.is_reference;
  }

  uint32_t findMemoryType(uint32_t filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties m;
    VK(vkGetPhysicalDeviceMemoryProperties)(hw_context_->vk_physical_device, &m);
    for (uint32_t i = 0; i < m.memoryTypeCount; i++) if ((filter & (1 << i)) && (m.memoryTypes[i].propertyFlags & props) == props) return i;
    return 0;
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
    for(auto& s : begin_slots) s.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
    std::array<VkVideoReferenceSlotInfoKHR, MAX_DPB_SLOTS + 1> ref_slots{};
    for(auto& s : ref_slots) s.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;

    uint32_t ref_count = 0;
    for (uint8_t ref_slot : reference_usage_) {
      if (ref_slot >= dpb_slot_count_ || ref_slot == slot_idx || !dpb_slots_[ref_slot].is_reference) continue;
      ref_slots[ref_count] = slot_infos[ref_slot];
      begin_slots[ref_count] = slot_infos[ref_slot];
      ++ref_count;
    }
    begin_slots[ref_count] = slot_infos[slot_idx];
    if (!dpb_slot_active_[slot_idx]) begin_slots[ref_count].slotIndex = -1;

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
    // Only a picture that is kept as a reference activates its DPB slot;
    // a non-reference picture leaves the slot inactive, so binding it by
    // index on a later frame is invalid.
    dpb_slot_active_[slot_idx] = is_reference;
  }

  void recordDecodeH265(VulkanDPBEntry* slot, uint32_t slot_idx, const Packet& packet, const std::vector<uint32_t>& slice_offsets) { }

  void updateSessionParametersAV1() {
    if (!video_session_) return;
    const auto& seq = av1_parser_.sequenceHeader();
    if (!seq.valid) return;

    StdVideoAV1ColorConfig color_cfg = {};
    color_cfg.flags.mono_chrome = seq.color_config.mono_chrome;
    color_cfg.flags.color_range = seq.color_config.color_range;
    color_cfg.flags.separate_uv_delta_q = seq.color_config.separate_uv_delta_q;
    color_cfg.flags.color_description_present_flag = seq.color_config.color_description_present_flag;
    color_cfg.BitDepth = seq.color_config.bit_depth;
    color_cfg.subsampling_x = seq.color_config.subsampling_x;
    color_cfg.subsampling_y = seq.color_config.subsampling_y;
    color_cfg.color_primaries = static_cast<StdVideoAV1ColorPrimaries>(seq.color_config.color_primaries);
    color_cfg.transfer_characteristics = static_cast<StdVideoAV1TransferCharacteristics>(seq.color_config.transfer_characteristics);
    color_cfg.matrix_coefficients = static_cast<StdVideoAV1MatrixCoefficients>(seq.color_config.matrix_coefficients);
    color_cfg.chroma_sample_position = static_cast<StdVideoAV1ChromaSamplePosition>(seq.color_config.chroma_sample_position);

    StdVideoAV1TimingInfo timing_info = {};
    timing_info.flags.equal_picture_interval = seq.timing_info.equal_picture_interval;
    timing_info.num_units_in_display_tick = seq.timing_info.num_units_in_display_tick;
    timing_info.time_scale = seq.timing_info.time_scale;
    timing_info.num_ticks_per_picture_minus_1 = seq.timing_info.num_ticks_per_picture > 0 ? (seq.timing_info.num_ticks_per_picture - 1) : 0;

    StdVideoAV1SequenceHeader std_seq = {};
    std_seq.flags.still_picture = seq.still_picture;
    std_seq.flags.reduced_still_picture_header = seq.reduced_still_picture_header;
    std_seq.flags.use_128x128_superblock = seq.use_128x128_superblock;
    std_seq.flags.enable_filter_intra = seq.enable_filter_intra;
    std_seq.flags.enable_intra_edge_filter = seq.enable_intra_edge_filter;
    std_seq.flags.enable_interintra_compound = seq.enable_interintra_compound;
    std_seq.flags.enable_masked_compound = seq.enable_masked_compound;
    std_seq.flags.enable_warped_motion = seq.enable_warped_motion;
    std_seq.flags.enable_dual_filter = seq.enable_dual_filter;
    std_seq.flags.enable_order_hint = seq.enable_order_hint;
    std_seq.flags.enable_jnt_comp = seq.enable_jnt_comp;
    std_seq.flags.enable_ref_frame_mvs = seq.enable_ref_frame_mvs;
    std_seq.flags.frame_id_numbers_present_flag = seq.frame_id_numbers_present_flag;
    std_seq.flags.enable_superres = seq.enable_superres;
    std_seq.flags.enable_cdef = seq.enable_cdef;
    std_seq.flags.enable_restoration = seq.enable_restoration;
    std_seq.flags.film_grain_params_present = seq.film_grain_params_present;
    std_seq.flags.timing_info_present_flag = seq.timing_info.present;
    std_seq.flags.initial_display_delay_present_flag = seq.initial_display_delay_present_flag;
    std_seq.seq_profile = static_cast<StdVideoAV1Profile>(seq.seq_profile);
    std_seq.frame_width_bits_minus_1 = seq.frame_width_bits > 0 ? (seq.frame_width_bits - 1) : 0;
    std_seq.frame_height_bits_minus_1 = seq.frame_height_bits > 0 ? (seq.frame_height_bits - 1) : 0;
    std_seq.max_frame_width_minus_1 = seq.max_frame_width > 0 ? static_cast<uint16_t>(seq.max_frame_width - 1) : (width_ > 0 ? static_cast<uint16_t>(width_ - 1) : 0);
    std_seq.max_frame_height_minus_1 = seq.max_frame_height > 0 ? static_cast<uint16_t>(seq.max_frame_height - 1) : (height_ > 0 ? static_cast<uint16_t>(height_ - 1) : 0);
    std_seq.delta_frame_id_length_minus_2 = seq.delta_frame_id_length > 1 ? (seq.delta_frame_id_length - 2) : 0;
    std_seq.additional_frame_id_length_minus_1 = seq.additional_frame_id_length > 0 ? (seq.additional_frame_id_length - 1) : 0;
    std_seq.order_hint_bits_minus_1 = seq.order_hint_bits > 0 ? (seq.order_hint_bits - 1) : 0;
    std_seq.seq_force_integer_mv = seq.seq_force_integer_mv;
    std_seq.seq_force_screen_content_tools = seq.seq_force_screen_content_tools;
    std_seq.pColorConfig = &color_cfg;
    std_seq.pTimingInfo = seq.timing_info.present ? &timing_info : nullptr;

    if (session_params_) {
      VK(vkDestroyVideoSessionParametersKHR)(hw_context_->vk_device, session_params_, hw_context_->allocator);
      session_params_ = VK_NULL_HANDLE;
    }

    VkVideoDecodeAV1SessionParametersCreateInfoKHR av1_params = {VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_SESSION_PARAMETERS_CREATE_INFO_KHR};
    av1_params.pStdSequenceHeader = &std_seq;
    VkVideoSessionParametersCreateInfoKHR info = {VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR};
    info.pNext = &av1_params;
    info.videoSession = video_session_;
    if (VK(vkCreateVideoSessionParametersKHR)(hw_context_->vk_device, &info, hw_context_->allocator, &session_params_) != VK_SUCCESS) {
      session_params_ = VK_NULL_HANDLE;
    }
  }

  void recordDecodeAV1(VulkanDPBEntry* slot, uint32_t slot_idx, const video_parser::AV1ParsedFrame& parsed) {
    size_t aligned_size = alignUp(parsed.bitstream.size(), static_cast<size_t>(min_bitstream_alignment_));
    std::memcpy(bitstream_ptr_, parsed.bitstream.data(), parsed.bitstream.size());
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
    std::array<VkVideoDecodeAV1DpbSlotInfoKHR, MAX_DPB_SLOTS + 1> slot_av1{};
    std::array<StdVideoDecodeAV1ReferenceInfo, MAX_DPB_SLOTS + 1> slot_stds{};

    for (uint32_t i = 0; i < dpb_slot_count_; ++i) {
      slot_stds[i] = {};
      slot_stds[i].frame_type = static_cast<uint8_t>(dpb_slots_[i].frame_num);
      slot_stds[i].OrderHint = static_cast<uint8_t>(dpb_slots_[i].poc);
      slot_av1[i] = {VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_DPB_SLOT_INFO_KHR};
      slot_av1[i].pStdReferenceInfo = &slot_stds[i];
      slot_pics[i] = {VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
      slot_pics[i].codedExtent = {padded_width_, padded_height_};
      slot_pics[i].baseArrayLayer = i;
      slot_pics[i].imageViewBinding = dpb_image_view_;
      slot_infos[i] = {VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
      slot_infos[i].pNext = &slot_av1[i];
      slot_infos[i].slotIndex = static_cast<int32_t>(i);
      slot_infos[i].pPictureResource = &slot_pics[i];
    }

    StdVideoDecodeAV1ReferenceInfo setup_ref_info = {};
    setup_ref_info.flags.disable_frame_end_update_cdf = parsed.header.disable_frame_end_update_cdf;
    setup_ref_info.flags.segmentation_enabled = parsed.header.segmentation.enabled;
    setup_ref_info.frame_type = parsed.header.frame_type;
    setup_ref_info.OrderHint = static_cast<uint8_t>(parsed.header.order_hint);
    for (uint32_t i = 0; i < 8; ++i) {
      setup_ref_info.SavedOrderHints[i] = static_cast<uint8_t>(parsed.header.order_hints[i]);
    }
    VkVideoDecodeAV1DpbSlotInfoKHR setup_av1 = {VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_DPB_SLOT_INFO_KHR};
    setup_av1.pStdReferenceInfo = &setup_ref_info;

    VkVideoReferenceSlotInfoKHR setup_slot = slot_infos[slot_idx];
    setup_slot.pNext = &setup_av1;

    int32_t ref_name_slots[VK_MAX_VIDEO_AV1_REFERENCES_PER_FRAME_KHR];
    for (int i = 0; i < VK_MAX_VIDEO_AV1_REFERENCES_PER_FRAME_KHR; ++i) {
      ref_name_slots[i] = -1;
    }

    bool is_inter = (parsed.header.frame_type != video_parser::AV1_KEY_FRAME &&
                     parsed.header.frame_type != video_parser::AV1_INTRA_ONLY_FRAME &&
                     !parsed.header.frame_is_intra);

    if (is_inter) {
      for (int i = 0; i < VK_MAX_VIDEO_AV1_REFERENCES_PER_FRAME_KHR; ++i) {
        uint8_t ref_map_idx = parsed.header.ref_frame_idx[i];
        if (ref_map_idx < 8 && av1_refs_[ref_map_idx].valid) {
          int32_t s = av1_refs_[ref_map_idx].dpb_slot;
          if (s >= 0 && s < static_cast<int32_t>(dpb_slot_count_)) {
            ref_name_slots[i] = s;
            slot_stds[s].frame_type = av1_refs_[ref_map_idx].frame_type;
            slot_stds[s].OrderHint = static_cast<uint8_t>(av1_refs_[ref_map_idx].order_hint);
            slot_stds[s].flags.disable_frame_end_update_cdf = av1_refs_[ref_map_idx].disable_frame_end_update_cdf;
            slot_stds[s].flags.segmentation_enabled = av1_refs_[ref_map_idx].segmentation_enabled;
            for (uint32_t j = 0; j < 8; ++j) {
              slot_stds[s].SavedOrderHints[j] = static_cast<uint8_t>(av1_refs_[ref_map_idx].saved_order_hints[j]);
            }
          }
        }
      }
    }

    std::array<VkVideoReferenceSlotInfoKHR, MAX_DPB_SLOTS + 1> begin_slots{};
    for (auto& s : begin_slots) s.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
    std::array<VkVideoReferenceSlotInfoKHR, MAX_DPB_SLOTS + 1> ref_slots{};
    for (auto& s : ref_slots) s.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;

    uint32_t ref_count = 0;
    std::array<bool, MAX_DPB_SLOTS> added{};
    for (int i = 0; i < VK_MAX_VIDEO_AV1_REFERENCES_PER_FRAME_KHR; ++i) {
      int32_t s = ref_name_slots[i];
      if (s >= 0 && s < static_cast<int32_t>(dpb_slot_count_) && !added[s]) {
        added[s] = true;
        ref_slots[ref_count] = slot_infos[s];
        begin_slots[ref_count] = slot_infos[s];
        ++ref_count;
      }
    }

    begin_slots[ref_count] = setup_slot;
    if (!dpb_slot_active_[slot_idx]) begin_slots[ref_count].slotIndex = -1;


    StdVideoDecodeAV1PictureInfo std_pic = {};
    std_pic.flags.error_resilient_mode = parsed.header.error_resilient_mode;
    std_pic.flags.disable_cdf_update = parsed.header.disable_cdf_update;
    std_pic.flags.use_superres = parsed.header.use_superres;
    // Use the coded flag: with superres FrameWidth != UpscaledWidth even when
    // the syntax element is 0, so comparing sizes gives the wrong answer.
    std_pic.flags.render_and_frame_size_different = parsed.header.render_and_frame_size_different;
    std_pic.flags.allow_screen_content_tools = parsed.header.allow_screen_content_tools;
    std_pic.flags.is_filter_switchable = (parsed.header.interpolation_filter == video_parser::AV1_SWITCHABLE);
    std_pic.flags.force_integer_mv = parsed.header.force_integer_mv;
    std_pic.flags.frame_size_override_flag = parsed.header.frame_size_override_flag;
    std_pic.flags.allow_intrabc = parsed.header.allow_intrabc;
    std_pic.flags.frame_refs_short_signaling = parsed.header.frame_refs_short_signaling;
    std_pic.flags.allow_high_precision_mv = parsed.header.allow_high_precision_mv;
    std_pic.flags.is_motion_mode_switchable = parsed.header.is_motion_mode_switchable;
    std_pic.flags.use_ref_frame_mvs = parsed.header.use_ref_frame_mvs;
    std_pic.flags.disable_frame_end_update_cdf = parsed.header.disable_frame_end_update_cdf;
    std_pic.flags.allow_warped_motion = parsed.header.allow_warped_motion;
    std_pic.flags.reduced_tx_set = parsed.header.reduced_tx_set;
    std_pic.flags.reference_select = parsed.header.reference_select;
    std_pic.flags.skip_mode_present = parsed.header.skip_mode_present;
    std_pic.flags.delta_q_present = parsed.header.delta_q_present;
    std_pic.flags.delta_lf_present = parsed.header.loop_filter.delta_lf_present;
    std_pic.flags.delta_lf_multi = parsed.header.loop_filter.delta_lf_multi;
    std_pic.flags.segmentation_enabled = parsed.header.segmentation.enabled;
    std_pic.flags.segmentation_update_map = parsed.header.segmentation.update_map;
    std_pic.flags.segmentation_temporal_update = parsed.header.segmentation.temporal_update;
    std_pic.flags.segmentation_update_data = parsed.header.segmentation.update_data;
    std_pic.flags.UsesLr = parsed.header.lr.uses_lr;
    std_pic.flags.usesChromaLr = (parsed.header.lr.frame_restoration_type[1] != 0 || parsed.header.lr.frame_restoration_type[2] != 0);
    std_pic.flags.apply_grain = parsed.header.film_grain.apply_grain;

    std_pic.frame_type = static_cast<StdVideoAV1FrameType>(parsed.header.frame_type);
    std_pic.current_frame_id = parsed.header.current_frame_id;
    std_pic.OrderHint = static_cast<uint8_t>(parsed.header.order_hint);
    std_pic.primary_ref_frame = parsed.header.primary_ref_frame;
    std_pic.refresh_frame_flags = parsed.header.refresh_frame_flags;
    std_pic.interpolation_filter = static_cast<StdVideoAV1InterpolationFilter>(parsed.header.interpolation_filter);
    std_pic.TxMode = static_cast<StdVideoAV1TxMode>(parsed.header.tx_mode);
    std_pic.delta_q_res = parsed.header.delta_q_res;
    std_pic.delta_lf_res = parsed.header.loop_filter.delta_lf_res;
    std_pic.SkipModeFrame[0] = parsed.header.skip_mode_frame[0];
    std_pic.SkipModeFrame[1] = parsed.header.skip_mode_frame[1];
    // coded_denom is the coded field (SuperresDenom - 9), not the denominator.
    std_pic.coded_denom = parsed.header.coded_denom;
    for (uint32_t i = 0; i < STD_VIDEO_AV1_NUM_REF_FRAMES; ++i) {
      std_pic.OrderHints[i] = static_cast<uint8_t>(parsed.header.order_hints[i]);
    }

    uint32_t t_cols = parsed.header.tile_info.tile_cols > 0 ? parsed.header.tile_info.tile_cols : 1;
    uint32_t t_rows = parsed.header.tile_info.tile_rows > 0 ? parsed.header.tile_info.tile_rows : 1;
    uint32_t sb_shift = av1_parser_.sequenceHeader().use_128x128_superblock ? 5 : 4;
    uint32_t sb_size = 1u << sb_shift;

    uint16_t col_starts[video_parser::AV1_MAX_TILE_COLS + 1] = {};
    uint16_t row_starts[video_parser::AV1_MAX_TILE_ROWS + 1] = {};
    uint16_t width_in_sbs_minus_1[video_parser::AV1_MAX_TILE_COLS] = {};
    uint16_t height_in_sbs_minus_1[video_parser::AV1_MAX_TILE_ROWS] = {};

    for (uint32_t i = 0; i <= t_cols && i <= video_parser::AV1_MAX_TILE_COLS; ++i) {
      col_starts[i] = static_cast<uint16_t>(parsed.header.tile_info.mi_col_starts[i]);
    }
    for (uint32_t i = 0; i <= t_rows && i <= video_parser::AV1_MAX_TILE_ROWS; ++i) {
      row_starts[i] = static_cast<uint16_t>(parsed.header.tile_info.mi_row_starts[i]);
    }
    if (col_starts[t_cols] == 0 && parsed.header.mi_cols > 0) {
      col_starts[t_cols] = static_cast<uint16_t>(parsed.header.mi_cols);
    }
    if (row_starts[t_rows] == 0 && parsed.header.mi_rows > 0) {
      row_starts[t_rows] = static_cast<uint16_t>(parsed.header.mi_rows);
    }
    for (uint32_t i = 0; i < t_cols && i < video_parser::AV1_MAX_TILE_COLS; ++i) {
      uint32_t diff = col_starts[i + 1] > col_starts[i] ? (col_starts[i + 1] - col_starts[i]) : sb_size;
      uint32_t sbs = (diff + sb_size - 1) >> sb_shift;
      width_in_sbs_minus_1[i] = static_cast<uint16_t>(sbs > 0 ? (sbs - 1) : 0);
    }
    for (uint32_t i = 0; i < t_rows && i < video_parser::AV1_MAX_TILE_ROWS; ++i) {
      uint32_t diff = row_starts[i + 1] > row_starts[i] ? (row_starts[i + 1] - row_starts[i]) : sb_size;
      uint32_t sbs = (diff + sb_size - 1) >> sb_shift;
      height_in_sbs_minus_1[i] = static_cast<uint16_t>(sbs > 0 ? (sbs - 1) : 0);
    }

    StdVideoAV1TileInfo tile_info = {};
    tile_info.flags.uniform_tile_spacing_flag = parsed.header.tile_info.uniform_tile_spacing_flag;
    tile_info.TileCols = static_cast<uint8_t>(t_cols);
    tile_info.TileRows = static_cast<uint8_t>(t_rows);
    tile_info.context_update_tile_id = static_cast<uint16_t>(parsed.header.tile_info.context_update_tile_id);
    tile_info.tile_size_bytes_minus_1 = parsed.header.tile_info.tile_size_bytes > 0 ? (parsed.header.tile_info.tile_size_bytes - 1) : 0;
    tile_info.pMiColStarts = col_starts;
    tile_info.pMiRowStarts = row_starts;
    tile_info.pWidthInSbsMinus1 = width_in_sbs_minus_1;
    tile_info.pHeightInSbsMinus1 = height_in_sbs_minus_1;
    std_pic.pTileInfo = &tile_info;

    StdVideoAV1Quantization quant = {};
    quant.flags.using_qmatrix = parsed.header.quantization.using_qmatrix;
    quant.flags.diff_uv_delta = (parsed.header.quantization.delta_q_u_dc != parsed.header.quantization.delta_q_v_dc ||
                                 parsed.header.quantization.delta_q_u_ac != parsed.header.quantization.delta_q_v_ac);
    quant.base_q_idx = parsed.header.quantization.base_q_idx;
    quant.DeltaQYDc = static_cast<int8_t>(parsed.header.quantization.delta_q_y_dc);
    quant.DeltaQUDc = static_cast<int8_t>(parsed.header.quantization.delta_q_u_dc);
    quant.DeltaQUAc = static_cast<int8_t>(parsed.header.quantization.delta_q_u_ac);
    quant.DeltaQVDc = static_cast<int8_t>(parsed.header.quantization.delta_q_v_dc);
    quant.DeltaQVAc = static_cast<int8_t>(parsed.header.quantization.delta_q_v_ac);
    quant.qm_y = parsed.header.quantization.qm_y;
    quant.qm_u = parsed.header.quantization.qm_u;
    quant.qm_v = parsed.header.quantization.qm_v;
    std_pic.pQuantization = &quant;

    StdVideoAV1Segmentation seg = {};
    for (uint32_t s = 0; s < STD_VIDEO_AV1_MAX_SEGMENTS; ++s) {
      for (uint32_t f = 0; f < STD_VIDEO_AV1_SEG_LVL_MAX; ++f) {
        if (parsed.header.segmentation.feature_enabled[s][f]) seg.FeatureEnabled[s] |= (1 << f);
        seg.FeatureData[s][f] = parsed.header.segmentation.feature_data[s][f];
      }
    }
    std_pic.pSegmentation = &seg;

    StdVideoAV1LoopFilter lf = {};
    lf.flags.loop_filter_delta_enabled = parsed.header.loop_filter.delta_enabled;
    lf.flags.loop_filter_delta_update = parsed.header.loop_filter.delta_update;
    lf.loop_filter_sharpness = parsed.header.loop_filter.sharpness;
    for (uint32_t i = 0; i < STD_VIDEO_AV1_MAX_LOOP_FILTER_STRENGTHS; ++i) {
      lf.loop_filter_level[i] = parsed.header.loop_filter.level[i];
    }
    for (uint32_t i = 0; i < STD_VIDEO_AV1_TOTAL_REFS_PER_FRAME; ++i) {
      lf.loop_filter_ref_deltas[i] = parsed.header.loop_filter.ref_deltas[i];
    }
    for (uint32_t i = 0; i < STD_VIDEO_AV1_LOOP_FILTER_ADJUSTMENTS; ++i) {
      lf.loop_filter_mode_deltas[i] = parsed.header.loop_filter.mode_deltas[i];
    }
    std_pic.pLoopFilter = &lf;

    StdVideoAV1CDEF cdef = {};
    cdef.cdef_damping_minus_3 = parsed.header.cdef.damping >= 3 ? (parsed.header.cdef.damping - 3) : 0;
    cdef.cdef_bits = parsed.header.cdef.bits;
    for (uint32_t i = 0; i < STD_VIDEO_AV1_MAX_CDEF_FILTER_STRENGTHS; ++i) {
      cdef.cdef_y_pri_strength[i] = parsed.header.cdef.y_pri_strength[i];
      cdef.cdef_y_sec_strength[i] = parsed.header.cdef.y_sec_strength[i];
      cdef.cdef_uv_pri_strength[i] = parsed.header.cdef.uv_pri_strength[i];
      cdef.cdef_uv_sec_strength[i] = parsed.header.cdef.uv_sec_strength[i];
    }
    std_pic.pCDEF = &cdef;

    StdVideoAV1LoopRestoration lr = {};
    for (uint32_t p = 0; p < STD_VIDEO_AV1_MAX_NUM_PLANES; ++p) {
      lr.FrameRestorationType[p] = static_cast<StdVideoAV1FrameRestorationType>(parsed.header.lr.frame_restoration_type[p]);
      lr.LoopRestorationSize[p] = parsed.header.lr.loop_restoration_size[p];
    }
    std_pic.pLoopRestoration = &lr;

    StdVideoAV1GlobalMotion gm = {};
    for (uint32_t r = 0; r < STD_VIDEO_AV1_NUM_REF_FRAMES; ++r) {
      gm.GmType[r] = parsed.header.global_motion.type[r];
      for (uint32_t p = 0; p < STD_VIDEO_AV1_GLOBAL_MOTION_PARAMS; ++p) {
        gm.gm_params[r][p] = parsed.header.global_motion.params[r][p];
      }
    }
    std_pic.pGlobalMotion = &gm;

    StdVideoAV1FilmGrain fg = {};
    if (parsed.header.film_grain.apply_grain) {
      fg.flags.chroma_scaling_from_luma = parsed.header.film_grain.chroma_scaling_from_luma;
      fg.flags.overlap_flag = parsed.header.film_grain.overlap_flag;
      fg.flags.clip_to_restricted_range = parsed.header.film_grain.clip_to_restricted_range;
      fg.flags.update_grain = parsed.header.film_grain.update_grain;
      fg.grain_scaling_minus_8 = parsed.header.film_grain.grain_scaling >= 8 ? (parsed.header.film_grain.grain_scaling - 8) : 0;
      fg.ar_coeff_lag = parsed.header.film_grain.ar_coeff_lag;
      fg.ar_coeff_shift_minus_6 = parsed.header.film_grain.ar_coeff_shift >= 6 ? (parsed.header.film_grain.ar_coeff_shift - 6) : 0;
      fg.grain_scale_shift = parsed.header.film_grain.grain_scale_shift;
      fg.grain_seed = parsed.header.film_grain.grain_seed;
      fg.film_grain_params_ref_idx = parsed.header.film_grain.film_grain_params_ref_idx;
      fg.num_y_points = parsed.header.film_grain.num_y_points;
      for (uint32_t i = 0; i < STD_VIDEO_AV1_MAX_NUM_Y_POINTS; ++i) {
        fg.point_y_value[i] = parsed.header.film_grain.point_y_value[i];
        fg.point_y_scaling[i] = parsed.header.film_grain.point_y_scaling[i];
      }
      fg.num_cb_points = parsed.header.film_grain.num_cb_points;
      for (uint32_t i = 0; i < STD_VIDEO_AV1_MAX_NUM_CB_POINTS; ++i) {
        fg.point_cb_value[i] = parsed.header.film_grain.point_cb_value[i];
        fg.point_cb_scaling[i] = parsed.header.film_grain.point_cb_scaling[i];
      }
      fg.num_cr_points = parsed.header.film_grain.num_cr_points;
      for (uint32_t i = 0; i < STD_VIDEO_AV1_MAX_NUM_CR_POINTS; ++i) {
        fg.point_cr_value[i] = parsed.header.film_grain.point_cr_value[i];
        fg.point_cr_scaling[i] = parsed.header.film_grain.point_cr_scaling[i];
      }
      for (uint32_t i = 0; i < STD_VIDEO_AV1_MAX_NUM_POS_LUMA; ++i) {
        fg.ar_coeffs_y_plus_128[i] = static_cast<int8_t>(parsed.header.film_grain.ar_coeffs_y[i] + 128);
      }
      for (uint32_t i = 0; i < STD_VIDEO_AV1_MAX_NUM_POS_CHROMA; ++i) {
        fg.ar_coeffs_cb_plus_128[i] = static_cast<int8_t>(parsed.header.film_grain.ar_coeffs_cb[i] + 128);
        fg.ar_coeffs_cr_plus_128[i] = static_cast<int8_t>(parsed.header.film_grain.ar_coeffs_cr[i] + 128);
      }
      fg.cb_mult = parsed.header.film_grain.cb_mult;
      fg.cb_luma_mult = parsed.header.film_grain.cb_luma_mult;
      fg.cb_offset = parsed.header.film_grain.cb_offset;
      fg.cr_mult = parsed.header.film_grain.cr_mult;
      fg.cr_luma_mult = parsed.header.film_grain.cr_luma_mult;
      fg.cr_offset = parsed.header.film_grain.cr_offset;
      std_pic.pFilmGrain = &fg;
    }

    uint32_t frame_hdr_offset = 0;
    for (const auto& obu : parsed.obus) {
      if (obu.type == video_parser::AV1_OBU_FRAME_HEADER || obu.type == video_parser::AV1_OBU_FRAME) {
        frame_hdr_offset = static_cast<uint32_t>(obu.offset);
        break;
      }
    }
    if (frame_hdr_offset > aligned_size) frame_hdr_offset = 0;

    std::vector<uint32_t> tile_offsets;
    std::vector<uint32_t> tile_sizes;
    uint32_t num_tiles = t_cols * t_rows;

    for (const auto& obu : parsed.obus) {
      if (obu.type != video_parser::AV1_OBU_FRAME && obu.type != video_parser::AV1_OBU_TILE_GROUP) continue;

      size_t tg_offset = (obu.type == video_parser::AV1_OBU_FRAME) ? parsed.tile_group_offset : obu.payload_offset;
      size_t tg_size = (obu.type == video_parser::AV1_OBU_FRAME) ? parsed.tile_group_size : obu.payload_size;
      if (tg_offset >= parsed.bitstream.size() || tg_offset + tg_size > parsed.bitstream.size() || tg_size == 0) continue;

      BitReader reader(std::span<const uint8_t>(parsed.bitstream.data() + tg_offset, tg_size));
      uint32_t tg_start = 0;
      uint32_t tg_end = num_tiles > 0 ? (num_tiles - 1) : 0;
      if (num_tiles > 1) {
        bool tile_start_and_end_present_flag = reader.readFlag();
        if (tile_start_and_end_present_flag && obu.type != video_parser::AV1_OBU_FRAME) {
          uint32_t tile_bits = parsed.header.tile_info.tile_cols_log2 + parsed.header.tile_info.tile_rows_log2;
          tg_start = reader.readBits(tile_bits);
          tg_end = reader.readBits(tile_bits);
        }
      }
      reader.alignToByte();
      size_t curr_pos = tg_offset + reader.bytePosition();
      size_t tg_end_pos = tg_offset + tg_size;
      uint32_t tile_size_bytes = parsed.header.tile_info.tile_size_bytes > 0 ? parsed.header.tile_info.tile_size_bytes : 1;

      for (uint32_t t = tg_start; t <= tg_end && curr_pos < tg_end_pos; ++t) {
        bool last_tile = (t == tg_end);
        uint32_t sz = 0;
        if (last_tile) {
          sz = static_cast<uint32_t>(tg_end_pos - curr_pos);
          tile_offsets.push_back(static_cast<uint32_t>(curr_pos));
          tile_sizes.push_back(sz);
          curr_pos = tg_end_pos;
        } else {
          if (curr_pos + tile_size_bytes <= tg_end_pos) {
            uint32_t sz_minus_1 = 0;
            for (uint32_t b = 0; b < tile_size_bytes; ++b) {
              sz_minus_1 |= static_cast<uint32_t>(parsed.bitstream[curr_pos + b]) << (8 * b);
            }
            curr_pos += tile_size_bytes;
            sz = sz_minus_1 + 1;
            if (curr_pos + sz <= tg_end_pos) {
              tile_offsets.push_back(static_cast<uint32_t>(curr_pos));
              tile_sizes.push_back(sz);
              curr_pos += sz;
            }
          }
        }
      }
    }

    if (tile_offsets.empty()) {
      uint32_t tile_offset = static_cast<uint32_t>(parsed.tile_group_offset > 0 ? parsed.tile_group_offset : frame_hdr_offset);
      if (tile_offset > aligned_size) tile_offset = 0;
      uint32_t max_avail = static_cast<uint32_t>(aligned_size - tile_offset);
      uint32_t tile_size = static_cast<uint32_t>(parsed.tile_group_size > 0 ? parsed.tile_group_size : max_avail);
      if (tile_size > max_avail) tile_size = max_avail;
      tile_offsets.push_back(tile_offset);
      tile_sizes.push_back(tile_size);
    }

    VkVideoDecodeAV1PictureInfoKHR av1_pic = {VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PICTURE_INFO_KHR};
    av1_pic.pStdPictureInfo = &std_pic;
    for (int i = 0; i < VK_MAX_VIDEO_AV1_REFERENCES_PER_FRAME_KHR; ++i) {
      av1_pic.referenceNameSlotIndices[i] = ref_name_slots[i];
    }
    av1_pic.frameHeaderOffset = frame_hdr_offset;
    av1_pic.tileCount = static_cast<uint32_t>(tile_offsets.size());
    av1_pic.pTileOffsets = tile_offsets.data();
    av1_pic.pTileSizes = tile_sizes.data();

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
    decode.pNext = &av1_pic;
    decode.srcBuffer = bitstream_buffer_;
    decode.srcBufferOffset = 0;
    decode.srcBufferRange = aligned_size;
    decode.dstPictureResource = dst;
    decode.pSetupReferenceSlot = &setup_slot;
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
    slot->poc = static_cast<int32_t>(parsed.header.order_hint);
    slot->frame_num = static_cast<uint32_t>(parsed.header.frame_type);
    uint8_t refresh = parsed.header.refresh_frame_flags;
    if (parsed.header.frame_type == video_parser::AV1_KEY_FRAME) refresh = 0xFF;
    slot->is_reference = (refresh != 0);
    // A decode operation only activates its DPB slot when the picture actually
    // refreshes a reference. Output-only frames (refresh_frame_flags == 0)
    // leave the slot inactive, so it must not be bound by index next time.
    dpb_slot_active_[slot_idx] = (refresh != 0);
    for (uint32_t i = 0; i < 8; ++i) {
      if ((refresh >> i) & 1) {
        av1_refs_[i].valid = true;
        av1_refs_[i].dpb_slot = slot_idx;
        av1_refs_[i].order_hint = parsed.header.order_hint;
        av1_refs_[i].frame_type = parsed.header.frame_type;
        av1_refs_[i].disable_frame_end_update_cdf = parsed.header.disable_frame_end_update_cdf;
        av1_refs_[i].segmentation_enabled = parsed.header.segmentation.enabled;
        for (uint32_t j = 0; j < 8; ++j) {
          av1_refs_[i].saved_order_hints[j] = parsed.header.order_hints[j];
        }
      }
    }
  }

  void recordDecodeVP9(VulkanDPBEntry* slot, uint32_t slot_idx, const video_parser::VP9ParsedFrame& parsed) {
    size_t aligned_size = alignUp(parsed.bitstream.size(), static_cast<size_t>(min_bitstream_alignment_));
    std::memcpy(bitstream_ptr_, parsed.bitstream.data(), parsed.bitstream.size());
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

    for (uint32_t i = 0; i < dpb_slot_count_; ++i) {
      slot_pics[i] = {VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
      slot_pics[i].codedExtent = {padded_width_, padded_height_};
      slot_pics[i].baseArrayLayer = i;
      slot_pics[i].imageViewBinding = dpb_image_view_;
      slot_infos[i] = {VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
      slot_infos[i].slotIndex = static_cast<int32_t>(i);
      slot_infos[i].pPictureResource = &slot_pics[i];
    }

    VkVideoReferenceSlotInfoKHR setup_slot = slot_infos[slot_idx];

    int32_t ref_name_slots[VK_MAX_VIDEO_VP9_REFERENCES_PER_FRAME_KHR] = {-1, -1, -1};
    bool is_inter = (parsed.header.frame_type != video_parser::VP9_KEY_FRAME && !parsed.header.intra_only);
    if (is_inter) {
      for (int i = 0; i < static_cast<int>(VK_MAX_VIDEO_VP9_REFERENCES_PER_FRAME_KHR); ++i) {
        uint8_t ref_idx = parsed.header.ref_frame_idx[i];
        if (ref_idx < 8 && vp9_refs_[ref_idx].valid) {
          int32_t s = vp9_refs_[ref_idx].dpb_slot;
          if (s >= 0 && s < static_cast<int32_t>(dpb_slot_count_)) {
            ref_name_slots[i] = s;
          }
        }
      }
    }

    std::array<VkVideoReferenceSlotInfoKHR, MAX_DPB_SLOTS + 1> begin_slots{};
    for (auto& s : begin_slots) s.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
    std::array<VkVideoReferenceSlotInfoKHR, MAX_DPB_SLOTS + 1> ref_slots{};
    for (auto& s : ref_slots) s.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;

    uint32_t ref_count = 0;
    std::array<bool, MAX_DPB_SLOTS> added{};
    for (int i = 0; i < static_cast<int>(VK_MAX_VIDEO_VP9_REFERENCES_PER_FRAME_KHR); ++i) {
      int32_t s = ref_name_slots[i];
      if (s >= 0 && s < static_cast<int32_t>(dpb_slot_count_) && !added[s]) {
        added[s] = true;
        ref_slots[ref_count] = slot_infos[s];
        begin_slots[ref_count] = slot_infos[s];
        ++ref_count;
      }
    }

    begin_slots[ref_count] = setup_slot;
    if (!dpb_slot_active_[slot_idx]) begin_slots[ref_count].slotIndex = -1;

    StdVideoDecodeVP9PictureInfo std_pic = {};
    std_pic.flags.error_resilient_mode = parsed.header.error_resilient_mode;
    std_pic.flags.intra_only = parsed.header.intra_only;
    std_pic.flags.allow_high_precision_mv = parsed.header.allow_high_precision_mv;
    std_pic.flags.refresh_frame_context = parsed.header.refresh_frame_context;
    std_pic.flags.frame_parallel_decoding_mode = parsed.header.frame_parallel_decoding_mode;
    std_pic.flags.segmentation_enabled = parsed.header.segmentation.enabled;
    std_pic.flags.show_frame = parsed.header.show_frame;
    std_pic.flags.UsePrevFrameMvs = (parsed.header.frame_type != video_parser::VP9_KEY_FRAME &&
                                     !parsed.header.intra_only &&
                                     !parsed.header.error_resilient_mode);

    std_pic.profile = static_cast<StdVideoVP9Profile>(parsed.header.profile);
    std_pic.frame_type = static_cast<StdVideoVP9FrameType>(parsed.header.frame_type);
    std_pic.frame_context_idx = parsed.header.frame_context_idx;
    std_pic.reset_frame_context = parsed.header.reset_frame_context;
    std_pic.refresh_frame_flags = parsed.header.refresh_frame_flags;
    std_pic.ref_frame_sign_bias_mask = (parsed.header.ref_frame_sign_bias[1] ? 2 : 0) |
                                       (parsed.header.ref_frame_sign_bias[2] ? 4 : 0) |
                                       (parsed.header.ref_frame_sign_bias[3] ? 8 : 0);
    std_pic.interpolation_filter = static_cast<StdVideoVP9InterpolationFilter>(parsed.header.interpolation_filter);
    std_pic.base_q_idx = parsed.header.quantization.base_q_idx;
    std_pic.delta_q_y_dc = parsed.header.quantization.delta_q_y_dc;
    std_pic.delta_q_uv_dc = parsed.header.quantization.delta_q_uv_dc;
    std_pic.delta_q_uv_ac = parsed.header.quantization.delta_q_uv_ac;
    std_pic.tile_cols_log2 = parsed.header.tile_cols_log2;
    std_pic.tile_rows_log2 = parsed.header.tile_rows_log2;

    StdVideoVP9ColorConfig color_cfg = {};
    color_cfg.flags.color_range = parsed.header.color_range;
    color_cfg.BitDepth = parsed.header.bit_depth;
    color_cfg.subsampling_x = parsed.header.subsampling_x;
    color_cfg.subsampling_y = parsed.header.subsampling_y;
    color_cfg.color_space = static_cast<StdVideoVP9ColorSpace>(parsed.header.color_space);
    std_pic.pColorConfig = &color_cfg;

    StdVideoVP9LoopFilter lf = {};
    lf.flags.loop_filter_delta_enabled = parsed.header.loop_filter.delta_enabled;
    lf.flags.loop_filter_delta_update = parsed.header.loop_filter.delta_update;
    lf.loop_filter_level = parsed.header.loop_filter.level;
    lf.loop_filter_sharpness = parsed.header.loop_filter.sharpness;
    for (uint32_t j = 0; j < STD_VIDEO_VP9_MAX_REF_FRAMES; ++j) {
      if (parsed.header.loop_filter.update_ref_delta[j]) lf.update_ref_delta |= (1 << j);
      lf.loop_filter_ref_deltas[j] = parsed.header.loop_filter.ref_deltas[j];
    }
    for (uint32_t j = 0; j < STD_VIDEO_VP9_LOOP_FILTER_ADJUSTMENTS; ++j) {
      if (parsed.header.loop_filter.update_mode_delta[j]) lf.update_mode_delta |= (1 << j);
      lf.loop_filter_mode_deltas[j] = parsed.header.loop_filter.mode_deltas[j];
    }
    std_pic.pLoopFilter = &lf;

    StdVideoVP9Segmentation seg = {};
    seg.flags.segmentation_update_map = parsed.header.segmentation.update_map;
    seg.flags.segmentation_temporal_update = parsed.header.segmentation.temporal_update;
    seg.flags.segmentation_update_data = parsed.header.segmentation.update_data;
    seg.flags.segmentation_abs_or_delta_update = parsed.header.segmentation.abs_or_delta_update;
    for (uint32_t j = 0; j < STD_VIDEO_VP9_MAX_SEGMENTATION_TREE_PROBS; ++j) {
      seg.segmentation_tree_probs[j] = parsed.header.segmentation.tree_probs[j];
    }
    for (uint32_t j = 0; j < STD_VIDEO_VP9_MAX_SEGMENTATION_PRED_PROB; ++j) {
      seg.segmentation_pred_prob[j] = parsed.header.segmentation.pred_probs[j];
    }
    for (uint32_t s = 0; s < STD_VIDEO_VP9_MAX_SEGMENTS; ++s) {
      for (uint32_t f = 0; f < STD_VIDEO_VP9_SEG_LVL_MAX; ++f) {
        if (parsed.header.segmentation.feature_enabled[s][f]) seg.FeatureEnabled[s] |= (1 << f);
        seg.FeatureData[s][f] = parsed.header.segmentation.feature_data[s][f];
      }
    }
    std_pic.pSegmentation = &seg;

    VkVideoDecodeVP9PictureInfoKHR vp9_pic = {VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_PICTURE_INFO_KHR};
    vp9_pic.pStdPictureInfo = &std_pic;
    for (int i = 0; i < static_cast<int>(VK_MAX_VIDEO_VP9_REFERENCES_PER_FRAME_KHR); ++i) {
      vp9_pic.referenceNameSlotIndices[i] = ref_name_slots[i];
    }
    vp9_pic.uncompressedHeaderOffset = 0;
    vp9_pic.compressedHeaderOffset = std::min<uint32_t>(parsed.header.uncompressed_header_size, static_cast<uint32_t>(aligned_size));
    vp9_pic.tilesOffset = std::min<uint32_t>(parsed.header.uncompressed_header_size + parsed.header.header_size_in_bytes, static_cast<uint32_t>(aligned_size));

    VkVideoPictureResourceInfoKHR dst = {VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
    dst.codedExtent = {padded_width_, padded_height_};
    dst.baseArrayLayer = coincide_supported_ ? slot_idx : 0;
    dst.imageViewBinding = coincide_supported_ ? dpb_image_view_ : output_view_;

    VkVideoBeginCodingInfoKHR begin = {VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
    begin.videoSession = video_session_;
    begin.videoSessionParameters = VK_NULL_HANDLE;
    begin.referenceSlotCount = ref_count + 1;
    begin.pReferenceSlots = begin_slots.data();
    VK(vkCmdBeginVideoCodingKHR)(cb, &begin);
    if (first_decode_) {
      VkVideoCodingControlInfoKHR ctrl = {VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR, nullptr, VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR};
      VK(vkCmdControlVideoCodingKHR)(cb, &ctrl);
      first_decode_ = false;
    }
    VkVideoDecodeInfoKHR decode = {VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR};
    decode.pNext = &vp9_pic;
    decode.srcBuffer = bitstream_buffer_;
    decode.srcBufferOffset = 0;
    decode.srcBufferRange = aligned_size;
    decode.dstPictureResource = dst;
    decode.pSetupReferenceSlot = &setup_slot;
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
    uint8_t refresh = parsed.header.refresh_frame_flags;
    if (parsed.header.frame_type == video_parser::VP9_KEY_FRAME) refresh = 0xFF;
    slot->is_reference = (refresh != 0);
    // Same rule as AV1: no refresh means the slot is never activated.
    dpb_slot_active_[slot_idx] = (refresh != 0);
    for (uint32_t i = 0; i < 8; ++i) {
      if ((refresh >> i) & 1) {
        vp9_refs_[i].valid = true;
        vp9_refs_[i].dpb_slot = slot_idx;
      }
    }
  }

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
    command_pool_ = VK_NULL_HANDLE; decode_fence_ = VK_NULL_HANDLE; bitstream_buffer_ = VK_NULL_HANDLE; bitstream_memory_ = VK_NULL_HANDLE; bitstream_ptr_ = nullptr;
    video_session_ = VK_NULL_HANDLE; session_params_ = VK_NULL_HANDLE; dpb_image_view_ = VK_NULL_HANDLE; dpb_image_ = VK_NULL_HANDLE;
    dpb_memory_ = VK_NULL_HANDLE; output_view_ = VK_NULL_HANDLE; output_image_ = VK_NULL_HANDLE; output_memory_ = VK_NULL_HANDLE;
    for (int i = 0; i < 8; ++i) {
      av1_refs_[i] = {};
      vp9_refs_[i] = {};
    }
    has_av1_seq_ = false;
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
};
const CodecDescriptor CODEC_VULKAN_H265 = {
    .codec_id = OM_CODEC_H265,
    .type = OM_MEDIA_VIDEO,
    .name = "vulkan_h265",
    .long_name = "Vulkan H.265/HEVC Codec",
    .vendor = "Vulkan",
    .flags = HARDWARE,
    .decoder_factory = [] { return std::make_unique<VulkanDecoder>(); },
};
const CodecDescriptor CODEC_VULKAN_AV1 = {
    .codec_id = OM_CODEC_AV1,
    .type = OM_MEDIA_VIDEO,
    .name = "vulkan_av1",
    .long_name = "Vulkan AV1 Codec",
    .vendor = "Vulkan",
    .flags = HARDWARE,
    .decoder_factory = [] { return std::make_unique<VulkanDecoder>(); },
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

}
