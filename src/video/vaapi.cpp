#include <h264.h>
#include <h265_stream.h>
#include <openmedia/hw_vaapi.h>
#include <va/va.h>
#include <va/va_dec_av1.h>
#include <va/va_dec_hevc.h>
#include <va/va_dec_vp9.h>
#include <va/va_enc_av1.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>
#include <va/va_enc_vp9.h>
#include <algorithm>
#include <codecs.hpp>
#include <cstring>
#include <hw_vaapi_priv.hpp>
#include <memory>
#include <openmedia/log.hpp>
#include <openmedia/video.hpp>
#include <vector>
#include "vaapi_loader.hpp"
#include <util/io_util.hpp>

namespace openmedia {

static void fillPictureParamsH264(const h264::SPS& sps, const h264::PPS& pps, const h264::SliceHeader& slice, VAPictureParameterBufferH264& va_pic_param) {
  std::memset(&va_pic_param, 0, sizeof(va_pic_param));
  va_pic_param.picture_width_in_mbs_minus1 = (uint16_t) sps.pic_width_in_mbs_minus1;
  va_pic_param.picture_height_in_mbs_minus1 = (uint16_t) ((2 - sps.frame_mbs_only_flag) * (sps.pic_height_in_map_units_minus1 + 1) - 1);
  va_pic_param.bit_depth_luma_minus8 = (uint8_t) sps.bit_depth_luma_minus8;
  va_pic_param.bit_depth_chroma_minus8 = (uint8_t) sps.bit_depth_chroma_minus8;
  va_pic_param.num_ref_frames = (uint8_t) sps.num_ref_frames;
  va_pic_param.seq_fields.bits.chroma_format_idc = sps.chroma_format_idc;
  va_pic_param.seq_fields.bits.residual_colour_transform_flag = sps.separate_colour_plane_flag;
  va_pic_param.seq_fields.bits.gaps_in_frame_num_value_allowed_flag = sps.gaps_in_frame_num_value_allowed_flag;
  va_pic_param.seq_fields.bits.frame_mbs_only_flag = sps.frame_mbs_only_flag;
  va_pic_param.seq_fields.bits.mb_adaptive_frame_field_flag = sps.mb_adaptive_frame_field_flag;
  va_pic_param.seq_fields.bits.direct_8x8_inference_flag = sps.direct_8x8_inference_flag;
  va_pic_param.seq_fields.bits.log2_max_frame_num_minus4 = sps.log2_max_frame_num_minus4;
  va_pic_param.seq_fields.bits.pic_order_cnt_type = sps.pic_order_cnt_type;
  va_pic_param.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = sps.log2_max_pic_order_cnt_lsb_minus4;
  va_pic_param.seq_fields.bits.delta_pic_order_always_zero_flag = sps.delta_pic_order_always_zero_flag;
  va_pic_param.pic_init_qp_minus26 = (int8_t) pps.pic_init_qp_minus26;
  va_pic_param.pic_init_qs_minus26 = (int8_t) pps.pic_init_qs_minus26;
  va_pic_param.chroma_qp_index_offset = (int8_t) pps.chroma_qp_index_offset;
  va_pic_param.second_chroma_qp_index_offset = (int8_t) pps.second_chroma_qp_index_offset;
  va_pic_param.pic_fields.bits.entropy_coding_mode_flag = pps.entropy_coding_mode_flag;
  va_pic_param.pic_fields.bits.weighted_pred_flag = pps.weighted_pred_flag;
  va_pic_param.pic_fields.bits.weighted_bipred_idc = pps.weighted_bipred_idc;
  va_pic_param.pic_fields.bits.transform_8x8_mode_flag = pps.transform_8x8_mode_flag;
  va_pic_param.pic_fields.bits.field_pic_flag = slice.field_pic_flag;
  va_pic_param.pic_fields.bits.constrained_intra_pred_flag = pps.constrained_intra_pred_flag;
  va_pic_param.pic_fields.bits.pic_order_present_flag = pps.pic_order_present_flag;
  va_pic_param.pic_fields.bits.deblocking_filter_control_present_flag = pps.deblocking_filter_control_present_flag;
  va_pic_param.pic_fields.bits.redundant_pic_cnt_present_flag = pps.redundant_pic_cnt_present_flag;
  va_pic_param.frame_num = (uint16_t) slice.frame_num;
  va_pic_param.CurrPic.picture_id = VA_INVALID_ID;
  for (int i = 0; i < 16; i++) va_pic_param.ReferenceFrames[i].picture_id = VA_INVALID_ID;
}

static void fillSliceParamsH264(const h264::SliceHeader& slice, VASliceParameterBufferH264& va_slice_param) {
  std::memset(&va_slice_param, 0, sizeof(va_slice_param));
  va_slice_param.first_mb_in_slice = (uint16_t) slice.first_mb_in_slice;
  va_slice_param.slice_type = (uint8_t) (slice.slice_type % 5);
  va_slice_param.direct_spatial_mv_pred_flag = (uint8_t) slice.direct_spatial_mv_pred_flag;
  va_slice_param.num_ref_idx_l0_active_minus1 = (uint8_t) slice.num_ref_idx_l0_active_minus1;
  va_slice_param.num_ref_idx_l1_active_minus1 = (uint8_t) slice.num_ref_idx_l1_active_minus1;
  va_slice_param.cabac_init_idc = (uint8_t) slice.cabac_init_idc;
  va_slice_param.slice_qp_delta = (int8_t) slice.slice_qp_delta;
  va_slice_param.disable_deblocking_filter_idc = (uint8_t) slice.disable_deblocking_filter_idc;
  va_slice_param.slice_alpha_c0_offset_div2 = (int8_t) slice.slice_alpha_c0_offset_div2;
  va_slice_param.slice_beta_offset_div2 = (int8_t) slice.slice_beta_offset_div2;
}

static void fillPictureParamsHEVC(const sps_t* sps, const pps_t* pps, const slice_segment_header_t* ssh, VAPictureParameterBufferHEVC& va_pic) {
  std::memset(&va_pic, 0, sizeof(va_pic));
  va_pic.pic_width_in_luma_samples = (uint16_t) sps->pic_width_in_luma_samples;
  va_pic.pic_height_in_luma_samples = (uint16_t) sps->pic_height_in_luma_samples;
  va_pic.pic_fields.bits.chroma_format_idc = sps->chroma_format_idc;
  va_pic.pic_fields.bits.separate_colour_plane_flag = sps->separate_colour_plane_flag;
  va_pic.pic_fields.bits.pcm_enabled_flag = sps->pcm_enabled_flag;
  va_pic.pic_fields.bits.scaling_list_enabled_flag = sps->scaling_list_enabled_flag;
  va_pic.pic_fields.bits.transform_skip_enabled_flag = pps->transform_skip_enabled_flag;
  va_pic.pic_fields.bits.amp_enabled_flag = sps->amp_enabled_flag;
  va_pic.pic_fields.bits.strong_intra_smoothing_enabled_flag = sps->strong_intra_smoothing_enabled_flag;
  va_pic.pic_fields.bits.sign_data_hiding_enabled_flag = pps->sign_data_hiding_enabled_flag;
  va_pic.pic_fields.bits.constrained_intra_pred_flag = pps->constrained_intra_pred_flag;
  va_pic.pic_fields.bits.cu_qp_delta_enabled_flag = pps->cu_qp_delta_enabled_flag;
  va_pic.pic_fields.bits.weighted_pred_flag = pps->weighted_pred_flag;
  va_pic.pic_fields.bits.weighted_bipred_flag = pps->weighted_bipred_flag;
  va_pic.pic_fields.bits.transquant_bypass_enabled_flag = pps->transquant_bypass_enabled_flag;
  va_pic.pic_fields.bits.tiles_enabled_flag = pps->tiles_enabled_flag;
  va_pic.pic_fields.bits.entropy_coding_sync_enabled_flag = pps->entropy_coding_sync_enabled_flag;
  va_pic.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag = pps->pps_loop_filter_across_slices_enabled_flag;
  va_pic.pic_fields.bits.loop_filter_across_tiles_enabled_flag = pps->loop_filter_across_tiles_enabled_flag;
  va_pic.pic_fields.bits.pcm_loop_filter_disabled_flag = sps->pcm_loop_filter_disabled_flag;
  va_pic.bit_depth_luma_minus8 = (uint8_t) sps->bit_depth_luma_minus8;
  va_pic.bit_depth_chroma_minus8 = (uint8_t) sps->bit_depth_chroma_minus8;
  va_pic.log2_min_luma_coding_block_size_minus3 = (uint8_t) sps->log2_min_luma_coding_block_size_minus3;
  va_pic.log2_diff_max_min_luma_coding_block_size = (uint8_t) sps->log2_diff_max_min_luma_coding_block_size;
  va_pic.log2_min_transform_block_size_minus2 = (uint8_t) sps->log2_min_luma_transform_block_size_minus2;
  va_pic.log2_diff_max_min_transform_block_size = (uint8_t) sps->log2_diff_max_min_luma_transform_block_size;
  va_pic.max_transform_hierarchy_depth_inter = (uint8_t) sps->max_transform_hierarchy_depth_inter;
  va_pic.max_transform_hierarchy_depth_intra = (uint8_t) sps->max_transform_hierarchy_depth_intra;
  va_pic.sps_max_dec_pic_buffering_minus1 = (uint8_t) sps->sps_max_dec_pic_buffering_minus1[sps->sps_max_sub_layers_minus1];
  va_pic.num_short_term_ref_pic_sets = (uint8_t) sps->num_short_term_ref_pic_sets;
  va_pic.num_long_term_ref_pic_sps = (uint8_t) sps->num_long_term_ref_pics_sps;
  va_pic.num_ref_idx_l0_default_active_minus1 = (uint8_t) pps->num_ref_idx_l0_default_active_minus1;
  va_pic.num_ref_idx_l1_default_active_minus1 = (uint8_t) pps->num_ref_idx_l1_default_active_minus1;
  va_pic.init_qp_minus26 = (int8_t) pps->init_qp_minus26;
  va_pic.diff_cu_qp_delta_depth = (uint8_t) pps->diff_cu_qp_delta_depth;
  va_pic.pps_cb_qp_offset = (int8_t) pps->pps_cb_qp_offset;
  va_pic.pps_cr_qp_offset = (int8_t) pps->pps_cr_qp_offset;
  va_pic.pps_beta_offset_div2 = (int8_t) pps->pps_beta_offset_div2;
  va_pic.pps_tc_offset_div2 = (int8_t) pps->pps_tc_offset_div2;
  va_pic.log2_parallel_merge_level_minus2 = (uint8_t) pps->log2_parallel_merge_level_minus2;
  va_pic.num_extra_slice_header_bits = (uint8_t) pps->num_extra_slice_header_bits;
  va_pic.num_tile_columns_minus1 = (uint8_t) pps->num_tile_columns_minus1;
  va_pic.num_tile_rows_minus1 = (uint8_t) pps->num_tile_rows_minus1;
  va_pic.log2_max_pic_order_cnt_lsb_minus4 = (uint8_t) sps->log2_max_pic_order_cnt_lsb_minus4;
  va_pic.CurrPic.picture_id = VA_INVALID_ID;
  for (int i = 0; i < 15; i++) va_pic.ReferenceFrames[i].picture_id = VA_INVALID_ID;
}

static void fillSliceParamsHEVC(const slice_segment_header_t* ssh, VASliceParameterBufferHEVC& va_slice) {
  std::memset(&va_slice, 0, sizeof(va_slice));
  va_slice.slice_segment_address = (uint32_t) ssh->slice_segment_address;
  va_slice.LongSliceFlags.fields.dependent_slice_segment_flag = ssh->dependent_slice_segment_flag;
  va_slice.LongSliceFlags.fields.slice_type = ssh->slice_type;
  va_slice.LongSliceFlags.fields.color_plane_id = ssh->colour_plane_id;
  va_slice.LongSliceFlags.fields.slice_temporal_mvp_enabled_flag = ssh->slice_temporal_mvp_enabled_flag;
  va_slice.LongSliceFlags.fields.slice_sao_luma_flag = ssh->slice_sao_luma_flag;
  va_slice.LongSliceFlags.fields.slice_sao_chroma_flag = ssh->slice_sao_chroma_flag;
  va_slice.LongSliceFlags.fields.mvd_l1_zero_flag = ssh->mvd_l1_zero_flag;
  va_slice.LongSliceFlags.fields.cabac_init_flag = ssh->cabac_init_flag;
  va_slice.LongSliceFlags.fields.collocated_from_l0_flag = ssh->collocated_from_l0_flag;
  va_slice.LongSliceFlags.fields.slice_deblocking_filter_disabled_flag = ssh->slice_deblocking_filter_disabled_flag;
  va_slice.LongSliceFlags.fields.slice_loop_filter_across_slices_enabled_flag = ssh->slice_loop_filter_across_slices_enabled_flag;
  va_slice.collocated_ref_idx = ssh->collocated_ref_idx;
  va_slice.num_ref_idx_l0_active_minus1 = (uint8_t) ssh->num_ref_idx_l0_active_minus1;
  va_slice.num_ref_idx_l1_active_minus1 = (uint8_t) ssh->num_ref_idx_l1_active_minus1;
  va_slice.slice_qp_delta = (int8_t) ssh->slice_qp_delta;
  va_slice.slice_cb_qp_offset = (int8_t) ssh->slice_cb_qp_offset;
  va_slice.slice_cr_qp_offset = (int8_t) ssh->slice_cr_qp_offset;
  va_slice.slice_beta_offset_div2 = (int8_t) ssh->slice_beta_offset_div2;
  va_slice.slice_tc_offset_div2 = (int8_t) ssh->slice_tc_offset_div2;
}

static void retrieveCodedBuffer(VADisplay display, VABufferID coded_buffer, std::vector<uint8_t>& out_data) {
  auto& libva = openmedia::LibVA::getInstance();
  VACodedBufferSegment* segment = nullptr;
  VAStatus status = libva.vaMapBufferCoded(display, coded_buffer, (void**) &segment);
  if (status != VA_STATUS_SUCCESS || !segment) return;
  out_data.clear();
  while (segment) {
    const uint8_t* data = static_cast<const uint8_t*>(segment->buf);
    if (data && segment->size > 0) out_data.insert(out_data.end(), data, data + segment->size);
    segment = static_cast<VACodedBufferSegment*>(segment->next);
  }
  libva.vaUnmapBuffer(display, coded_buffer);
}

struct DPBEntry {
  VASurfaceID surface = VA_INVALID_ID;
  int32_t poc = 0;
  uint32_t frame_num = 0;
  bool is_long_term = false;
};

class VAAPIDecoder final : public Decoder {
  std::unique_ptr<OMVAAPIContext> hw_context_;
  VADisplay display_ = nullptr;
  VAConfigID config_ = VA_INVALID_ID;
  VAContextID context_ = VA_INVALID_ID;
  bool initialized_ = false;
  VideoFormat output_format_ = {};
  OMCodecId codec_id_ = OM_CODEC_NONE;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  std::vector<VASurfaceID> surface_pool_;
  size_t surface_idx_ = 0;
  std::vector<VABufferID> cleanup_buffers;

  // DPB
  std::vector<DPBEntry> dpb_;
  static constexpr size_t MAX_DPB_SIZE = 16;

  // H264 state
  h264::SPS h264_sps_table_[32];
  h264::PPS h264_pps_table_[256];
  uint32_t active_sps_id_ = 0xFFFFFFFF;
  uint32_t active_pps_id_ = 0xFFFFFFFF;
  bool has_h264_sps_ = false;
  bool has_h264_pps_ = false;

  // H265 state
  h265_stream_t* h265_stream_ = nullptr;
  bool has_h265_sps_ = false;
  bool has_h265_pps_ = false;

public:
  VAAPIDecoder() = default;
  ~VAAPIDecoder() override { release(); }

  auto configure(const DecoderOptions& options) -> OMError override {
    release();
    auto& libva = LibVA::getInstance();
    if (!libva.load()) return OM_CODEC_HWACCEL_FAILED;
    codec_id_ = options.format.codec_id;
    width_ = options.format.video.width;
    height_ = options.format.video.height;
    if (width_ == 0 || height_ == 0) return OM_CODEC_INVALID_PARAMS;

    OMVAAPIInit init = {};
    if (options.hw_device.has_value() && options.hw_device->type == HWDeviceType::VAAPI) {
      display_ = static_cast<OMVAAPIContext*>(options.hw_device->context)->display;
    } else {
      hw_context_ = std::unique_ptr<OMVAAPIContext>(HWVAAPIContext_create(init));
      if (!hw_context_) return OM_CODEC_HWACCEL_FAILED;
      display_ = hw_context_->display;
    }

    uint32_t rt_format = VA_RT_FORMAT_YUV420;
    int bit_depth = 8;
    // Default to NV12/P010 based on common hardware support
    if (codec_id_ == OM_CODEC_H265 || codec_id_ == OM_CODEC_VP9 || codec_id_ == OM_CODEC_AV1) {
      // High bit depth is common for these codecs
      // For now, assume 8-bit unless we have a way to detect it from the format
      // In the future, this should be queried from hardware or passed via options
    }

    VAProfile profile = VAProfileNone;

    auto get_supported_profile = [&](const std::vector<VAProfile>& candidates) {
      int num_profiles = libva.vaMaxNumConfigProfiles(display_);
      std::vector<VAProfile> supported(num_profiles);
      if (libva.vaQueryConfigProfiles(display_, supported.data(), &num_profiles) != VA_STATUS_SUCCESS) return VAProfileNone;
      for (auto p : candidates) {
        for (int i = 0; i < num_profiles; i++)
          if (supported[i] == p) return p;
      }
      return VAProfileNone;
    };

    if (codec_id_ == OM_CODEC_H264) {
      profile = get_supported_profile({VAProfileH264High, VAProfileH264Main, VAProfileH264ConstrainedBaseline});
    } else if (codec_id_ == OM_CODEC_H265) {
      profile = get_supported_profile({VAProfileHEVCMain10, VAProfileHEVCMain});
      if (profile == VAProfileHEVCMain10) {
        rt_format = VA_RT_FORMAT_YUV420_10;
        bit_depth = 10;
      }
    } else if (codec_id_ == OM_CODEC_VP9) {
      profile = get_supported_profile({VAProfileVP9Profile2, VAProfileVP9Profile0});
      if (profile == VAProfileVP9Profile2) {
        rt_format = VA_RT_FORMAT_YUV420_10;
        bit_depth = 10;
      }
    } else if (codec_id_ == OM_CODEC_AV1) {
      profile = get_supported_profile({VAProfileAV1Profile0});
    }

    if (profile == VAProfileNone) return OM_CODEC_NOT_SUPPORTED;

    VAConfigAttrib attrib;
    attrib.type = VAConfigAttribRTFormat;
    attrib.value = rt_format;
    VAStatus status = libva.vaCreateConfig(display_, profile, VAEntrypointVLD, &attrib, 1, &config_);
    if (status != VA_STATUS_SUCCESS) return OM_CODEC_HWACCEL_FAILED;
    
    surface_pool_.resize(20);
    VASurfaceAttrib surf_attribs[1];
    surf_attribs[0].type = VASurfaceAttribPixelFormat;
    surf_attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    surf_attribs[0].value.type = VAGenericValueTypeInteger;
    surf_attribs[0].value.value.i = (rt_format == VA_RT_FORMAT_YUV420_10) ? VA_FOURCC_P010 : VA_FOURCC_NV12;

    status = libva.vaCreateSurfaces(display_, rt_format, width_, height_, surface_pool_.data(), (uint32_t) surface_pool_.size(), surf_attribs, 1);
    if (status != VA_STATUS_SUCCESS) return OM_CODEC_HWACCEL_FAILED;
    status = libva.vaCreateContext(display_, config_, (int) width_, (int) height_, VA_PROGRESSIVE, surface_pool_.data(), (int) surface_pool_.size(), &context_);
    if (status != VA_STATUS_SUCCESS) return OM_CODEC_HWACCEL_FAILED;

    std::memset(h264_sps_table_, 0, sizeof(h264_sps_table_));
    std::memset(h264_pps_table_, 0, sizeof(h264_pps_table_));

    if (codec_id_ == OM_CODEC_H265) h265_stream_ = h265_new();

    output_format_.width = width_;
    output_format_.height = height_;
    output_format_.format = OM_FORMAT_NV12;
    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!initialized_) return std::nullopt;
    DecodingInfo info = {};
    info.media_type = OM_MEDIA_VIDEO;
    info.video_format = output_format_;
    return info;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    if (!initialized_) return Err(OM_COMMON_NOT_INITIALIZED);
    if (packet.bytes.empty()) return Ok(std::vector<Frame> {});
    auto& libva = LibVA::getInstance();

    if (codec_id_ == OM_CODEC_H264) {
      const uint8_t* data = packet.bytes.data();
      size_t size = packet.bytes.size();
      
      auto find_start_code = [](const uint8_t* p, size_t sz) -> size_t {
        for (size_t i = 0; i + 2 < sz; ++i) {
          if (p[i] == 0 && p[i+1] == 0 && p[i+2] == 1) return i;
        }
        return sz;
      };

      size_t pos = 0;
      VASurfaceID surface = VA_INVALID_ID;
      bool picture_started = false;
      h264::SliceHeader first_slice = {};

      while (pos < size) {
        size_t start = find_start_code(data + pos, size - pos);
        if (start == size - pos) break;
        pos += start + 3;
        
        size_t next = find_start_code(data + pos, size - pos);
        size_t nal_size = next;
        const uint8_t* nal_ptr = data + pos;
        if (nal_size == 0) continue;

        h264::Bitstream bs;
        bs.init(nal_ptr, nal_size);
        h264::NALHeader nal;
        if (!h264::read_nal_header(&nal, &bs)) {
          pos += next;
          continue;
        }

        if (nal.type == h264::NAL_UNIT_TYPE_SPS) {
          h264::SPS sps;
          h264::read_sps(&sps, &bs);
          if (sps.seq_parameter_set_id < 32) {
              h264_sps_table_[sps.seq_parameter_set_id] = sps;
              has_h264_sps_ = true;
          }
        } else if (nal.type == h264::NAL_UNIT_TYPE_PPS) {
          h264::PPS pps;
          h264::read_pps(&pps, &bs);
          if (pps.pic_parameter_set_id < 256) {
              h264_pps_table_[pps.pic_parameter_set_id] = pps;
              has_h264_pps_ = true;
          }
        } else if (nal.type == h264::NAL_UNIT_TYPE_CODED_SLICE_IDR || nal.type == h264::NAL_UNIT_TYPE_CODED_SLICE_NON_IDR) {
          if (!has_h264_sps_ || !has_h264_pps_) {
            pos += next;
            continue;
          }
          h264::SliceHeader slice;
          h264::read_slice_header(&slice, &nal, h264_pps_table_, h264_sps_table_, &bs);
          
          if (!picture_started) {
            surface = surface_pool_[surface_idx_];
            surface_idx_ = (surface_idx_ + 1) % surface_pool_.size();
            first_slice = slice;
            const auto& sps = h264_sps_table_[h264_pps_table_[slice.pic_parameter_set_id].seq_parameter_set_id];
            const auto& pps = h264_pps_table_[slice.pic_parameter_set_id];

            int32_t poc = (sps.pic_order_cnt_type == 0) ? (int32_t)slice.pic_order_cnt_lsb : (int32_t)packet.pts;

            VAPictureParameterBufferH264 pic_param;
            fillPictureParamsH264(sps, pps, slice, pic_param);
            pic_param.CurrPic.picture_id = surface;
            pic_param.CurrPic.frame_idx = slice.frame_num;
            pic_param.CurrPic.TopFieldOrderCnt = poc;
            pic_param.CurrPic.BottomFieldOrderCnt = poc;
            pic_param.CurrPic.flags = (nal.idc != 0) ? VA_PICTURE_H264_SHORT_TERM_REFERENCE : 0;
            pic_param.pic_fields.bits.reference_pic_flag = (nal.idc != 0);

            for (size_t i = 0; i < dpb_.size() && i < 16; i++) {
              pic_param.ReferenceFrames[i].picture_id = dpb_[i].surface;
              pic_param.ReferenceFrames[i].TopFieldOrderCnt = dpb_[i].poc;
              pic_param.ReferenceFrames[i].BottomFieldOrderCnt = dpb_[i].poc;
              pic_param.ReferenceFrames[i].frame_idx = dpb_[i].frame_num;
              pic_param.ReferenceFrames[i].flags = dpb_[i].is_long_term ? VA_PICTURE_H264_LONG_TERM_REFERENCE : VA_PICTURE_H264_SHORT_TERM_REFERENCE;
            }
            for (size_t i = dpb_.size(); i < 16; i++) {
              pic_param.ReferenceFrames[i].picture_id = VA_INVALID_ID;
              pic_param.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
            }

            VABufferID pic_param_buf, iq_matrix_buf;
            VAStatus status = libva.vaCreateBuffer(display_, context_, VAPictureParameterBufferType, sizeof(pic_param), 1, &pic_param, &pic_param_buf);
            if (status != VA_STATUS_SUCCESS) log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "[VAAPI] vaCreateBuffer PicParam failed: %d", status);
            
            VAIQMatrixBufferH264 iq_matrix = {};
            if (pps.pic_scaling_matrix_present_flag) {
                for (int i=0; i<6; i++) for (int j=0; j<16; j++) iq_matrix.ScalingList4x4[i][j] = (unsigned char)pps.ScalingList4x4[i][j];
                for (int i=0; i<2; i++) for (int j=0; j<64; j++) iq_matrix.ScalingList8x8[i][j] = (unsigned char)pps.ScalingList8x8[i][j];
            } else if (sps.seq_scaling_matrix_present_flag) {
                for (int i=0; i<6; i++) for (int j=0; j<16; j++) iq_matrix.ScalingList4x4[i][j] = (unsigned char)sps.ScalingList4x4[i][j];
                for (int i=0; i<2; i++) for (int j=0; j<64; j++) iq_matrix.ScalingList8x8[i][j] = (unsigned char)sps.ScalingList8x8[i][j];
            } else {
                std::memset(iq_matrix.ScalingList4x4, 16, sizeof(iq_matrix.ScalingList4x4));
                std::memset(iq_matrix.ScalingList8x8, 16, sizeof(iq_matrix.ScalingList8x8));
            }
            status = libva.vaCreateBuffer(display_, context_, VAIQMatrixBufferType, sizeof(iq_matrix), 1, &iq_matrix, &iq_matrix_buf);
            if (status != VA_STATUS_SUCCESS) log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "[VAAPI] vaCreateBuffer IQMatrix failed: %d", status);

            status = libva.vaBeginPicture(display_, context_, surface);
            if (status != VA_STATUS_SUCCESS) log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "[VAAPI] vaBeginPicture failed: %d", status);
            VABufferID pic_bufs[] = {pic_param_buf, iq_matrix_buf};
            status = libva.vaRenderPicture(display_, context_, pic_bufs, 2);
            if (status != VA_STATUS_SUCCESS) log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "[VAAPI] vaRenderPicture PicParams failed: %d", status);
            cleanup_buffers.push_back(pic_param_buf);
            cleanup_buffers.push_back(iq_matrix_buf);
            picture_started = true;
          }

          const auto& pps = h264_pps_table_[slice.pic_parameter_set_id];
          const auto& sps = h264_sps_table_[pps.seq_parameter_set_id];
          VASliceParameterBufferH264 slice_param;
          fillSliceParamsH264(slice, slice_param);
          
          if (slice.num_ref_idx_active_override_flag) {
            slice_param.num_ref_idx_l0_active_minus1 = (uint8_t)slice.num_ref_idx_l0_active_minus1;
            slice_param.num_ref_idx_l1_active_minus1 = (uint8_t)slice.num_ref_idx_l1_active_minus1;
          } else {
            slice_param.num_ref_idx_l0_active_minus1 = (uint8_t)pps.num_ref_idx_l0_active_minus1;
            slice_param.num_ref_idx_l1_active_minus1 = (uint8_t)pps.num_ref_idx_l1_active_minus1;
          }

          slice_param.luma_log2_weight_denom = (uint8_t)slice.pwt.luma_log2_weight_denom;
          slice_param.chroma_log2_weight_denom = (uint8_t)slice.pwt.chroma_log2_weight_denom;
          for (int i=0; i<32; i++) {
              slice_param.luma_weight_l0[i] = (int16_t)slice.pwt.luma_weight_l0[i];
              slice_param.luma_offset_l0[i] = (int16_t)slice.pwt.luma_offset_l0[i];
              slice_param.luma_weight_l1[i] = (int16_t)slice.pwt.luma_weight_l1[i];
              slice_param.luma_offset_l1[i] = (int16_t)slice.pwt.luma_offset_l1[i];
              for (int j=0; j<2; j++) {
                  slice_param.chroma_weight_l0[i][j] = (int16_t)slice.pwt.chroma_weight_l0[i][j];
                  slice_param.chroma_offset_l0[i][j] = (int16_t)slice.pwt.chroma_offset_l0[i][j];
                  slice_param.chroma_weight_l1[i][j] = (int16_t)slice.pwt.chroma_weight_l1[i][j];
                  slice_param.chroma_offset_l1[i][j] = (int16_t)slice.pwt.chroma_offset_l1[i][j];
              }
          }

          for (int i = 0; i < 32; i++) {
            slice_param.RefPicList0[i].picture_id = VA_INVALID_ID;
            slice_param.RefPicList1[i].picture_id = VA_INVALID_ID;
            slice_param.RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
            slice_param.RefPicList1[i].flags = VA_PICTURE_H264_INVALID;
          }

          uint8_t slice_type = (uint8_t)(slice.slice_type % 5);
          /* Let driver handle RefPicList0/1 by leaving them as VA_INVALID_ID if possible, 
             or providing the same list as ReferenceFrames if needed by the driver. */
          for (int i = 0; i < 32; i++) {
            slice_param.RefPicList0[i].picture_id = VA_INVALID_ID;
            slice_param.RefPicList1[i].picture_id = VA_INVALID_ID;
            slice_param.RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
            slice_param.RefPicList1[i].flags = VA_PICTURE_H264_INVALID;
          }

          slice_param.slice_data_size = (uint32_t) nal_size;
          slice_param.slice_data_offset = 0;
          slice_param.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
          slice_param.slice_data_bit_offset = (uint16_t)((bs.p - nal_ptr) * 8 + (8 - bs.bits_left));

          VABufferID slice_param_buf, slice_data_buf;
          VAStatus status = libva.vaCreateBuffer(display_, context_, VASliceParameterBufferType, sizeof(slice_param), 1, &slice_param, &slice_param_buf);
          if (status != VA_STATUS_SUCCESS) log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "[VAAPI] vaCreateBuffer SliceParam failed: %d", status);
          status = libva.vaCreateBuffer(display_, context_, VASliceDataBufferType, (uint32_t) nal_size, 1, (void*) nal_ptr, &slice_data_buf);
          if (status != VA_STATUS_SUCCESS) log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "[VAAPI] vaCreateBuffer SliceData failed: %d", status);
          VABufferID slice_bufs[] = {slice_param_buf, slice_data_buf};
          status = libva.vaRenderPicture(display_, context_, slice_bufs, 2);
          if (status != VA_STATUS_SUCCESS) log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "[VAAPI] vaRenderPicture Slice failed: %d", status);
          cleanup_buffers.push_back(slice_param_buf);
          cleanup_buffers.push_back(slice_data_buf);
        }
        pos += next;
      }

      if (picture_started) {
        libva.vaEndPicture(display_, context_);
        libva.vaSyncSurface(display_, surface);

        for (auto buf : cleanup_buffers) libva.vaDestroyBuffer(display_, buf);
        cleanup_buffers.clear();

        const auto& sps = h264_sps_table_[h264_pps_table_[first_slice.pic_parameter_set_id].seq_parameter_set_id];
        int32_t poc = (sps.pic_order_cnt_type == 0) ? (int32_t)first_slice.pic_order_cnt_lsb : (int32_t)packet.pts;

        if (dpb_.size() >= MAX_DPB_SIZE) dpb_.erase(dpb_.begin());
        dpb_.push_back({surface, poc, (uint32_t) first_slice.frame_num, false});

        Frame frame = {};
        frame.pts = packet.pts;
        frame.dts = packet.dts;
        Picture pic;
        pic.format = output_format_.format;
        pic.width = output_format_.width;
        pic.height = output_format_.height;
        pic.buffer = std::make_shared<VAAPIHardwarePicture>(display_, surface);
        frame.data = std::move(pic);
        return Ok(std::vector<Frame> {std::move(frame)});
      }
    } else if (codec_id_ == OM_CODEC_H265) {
      int nal_start, nal_end;
      uint8_t* p = const_cast<uint8_t*>(packet.bytes.data());
      int sz = static_cast<int>(packet.bytes.size());
      while (find_nal_unit(p, sz, &nal_start, &nal_end) > 0) {
        read_debug_nal_unit(h265_stream_, p + nal_start, nal_end - nal_start);
        if (h265_stream_->nal->nal_unit_type == NAL_UNIT_SPS)
          has_h265_sps_ = true;
        else if (h265_stream_->nal->nal_unit_type == NAL_UNIT_PPS)
          has_h265_pps_ = true;
        else if (h265_stream_->nal->nal_unit_type >= NAL_UNIT_CODED_SLICE_TRAIL_N && h265_stream_->nal->nal_unit_type <= NAL_UNIT_CODED_SLICE_RASL_R) {
          if (!has_h265_sps_ || !has_h265_pps_) continue;
          VASurfaceID surface = surface_pool_[surface_idx_];
          surface_idx_ = (surface_idx_ + 1) % surface_pool_.size();
          VAPictureParameterBufferHEVC pic_param;
          fillPictureParamsHEVC(h265_stream_->sps, h265_stream_->pps, h265_stream_->ssh, pic_param);
          pic_param.CurrPic.picture_id = surface;
          pic_param.CurrPic.pic_order_cnt = (int32_t) packet.pts;
          pic_param.CurrPic.flags = 0;

          for (size_t i = 0; i < dpb_.size() && i < 15; i++) {
            pic_param.ReferenceFrames[i].picture_id = dpb_[i].surface;
            pic_param.ReferenceFrames[i].pic_order_cnt = dpb_[i].poc;
            pic_param.ReferenceFrames[i].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE; // Default to short term
          }
          for (size_t i = dpb_.size(); i < 15; i++) {
            pic_param.ReferenceFrames[i].picture_id = VA_INVALID_ID;
            pic_param.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
          }

          VASliceParameterBufferHEVC slice_param;
          fillSliceParamsHEVC(h265_stream_->ssh, slice_param);
          slice_param.slice_data_size = (uint32_t) (nal_end - nal_start);
          slice_param.slice_data_offset = 0;
          slice_param.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
          VABufferID pic_param_buf, slice_param_buf, slice_data_buf;
          libva.vaCreateBuffer(display_, context_, VAPictureParameterBufferType, sizeof(pic_param), 1, &pic_param, &pic_param_buf);
          libva.vaCreateBuffer(display_, context_, VASliceParameterBufferType, sizeof(slice_param), 1, &slice_param, &slice_param_buf);
          libva.vaCreateBuffer(display_, context_, VASliceDataBufferType, (uint32_t) (nal_end - nal_start), 1, p + nal_start, &slice_data_buf);
          libva.vaBeginPicture(display_, context_, surface);
          VABufferID buffers[] = {pic_param_buf, slice_param_buf, slice_data_buf};
          libva.vaRenderPicture(display_, context_, buffers, 3);
          libva.vaEndPicture(display_, context_);
          libva.vaSyncSurface(display_, surface);

          VAImage debug_img;
          if (libva.vaDeriveImage(display_, surface, &debug_img) == VA_STATUS_SUCCESS) {
              log(OM_CATEGORY_HARDWARE, OM_LEVEL_DEBUG, "[VAAPI] Decoded surface FOURCC: %c%c%c%c", 
                  (char)((debug_img.format.fourcc >> 0) & 0xFF),
                  (char)((debug_img.format.fourcc >> 8) & 0xFF),
                  (char)((debug_img.format.fourcc >> 16) & 0xFF),
                  (char)((debug_img.format.fourcc >> 24) & 0xFF));
              libva.vaDestroyImage(display_, debug_img.image_id);
          }
          libva.vaDestroyBuffer(display_, pic_param_buf);
          libva.vaDestroyBuffer(display_, slice_param_buf);
          libva.vaDestroyBuffer(display_, slice_data_buf);
          if (dpb_.size() >= MAX_DPB_SIZE) dpb_.erase(dpb_.begin());
          dpb_.push_back({surface, (int32_t) packet.pts, 0, false});
          Frame frame = {};
          frame.pts = packet.pts;
          frame.dts = packet.dts;
          Picture pic;
          pic.format = output_format_.format;
          pic.width = output_format_.width;
          pic.height = output_format_.height;
          pic.buffer = std::make_shared<VAAPIHardwarePicture>(display_, surface);
          frame.data = std::move(pic);
          return Ok(std::vector<Frame> {std::move(frame)});
        }
        p += nal_end;
        sz -= nal_end;
      }
    } else if (codec_id_ == OM_CODEC_AV1) {
      const uint8_t* p = packet.bytes.data();
      size_t sz = packet.bytes.size();
      while (sz > 0) {
        uint8_t header = *p;
        bool obu_extension_flag = (header >> 2) & 1;
        bool obu_has_size_field = (header >> 1) & 1;
        uint8_t obu_type = (header >> 3) & 0xF;
        p++;
        sz--;
        if (obu_extension_flag) {
          p++;
          sz--;
        }
        size_t obu_size_len = 0;
        uint32_t obu_size = obu_has_size_field ? read_leb128(p, sz, &obu_size_len) : (uint32_t) sz;
        p += obu_size_len;
        sz -= obu_size_len;
        if (obu_type == 6 /* FRAME */ || obu_type == 3 /* FRAME_HEADER */) {
          VASurfaceID surface = surface_pool_[surface_idx_];
          surface_idx_ = (surface_idx_ + 1) % surface_pool_.size();
          VADecPictureParameterBufferAV1 pic_param = {};
          pic_param.current_frame = surface;
          pic_param.frame_width_minus1 = (uint16_t) (width_ - 1);
          pic_param.frame_height_minus1 = (uint16_t) (height_ - 1);
          VABufferID pic_param_buf;
          libva.vaCreateBuffer(display_, context_, VAPictureParameterBufferType, sizeof(pic_param), 1, &pic_param, &pic_param_buf);
          libva.vaBeginPicture(display_, context_, surface);
          libva.vaRenderPicture(display_, context_, &pic_param_buf, 1);
          libva.vaEndPicture(display_, context_);
          libva.vaSyncSurface(display_, surface);

          VAImage debug_img;
          if (libva.vaDeriveImage(display_, surface, &debug_img) == VA_STATUS_SUCCESS) {
              log(OM_CATEGORY_HARDWARE, OM_LEVEL_DEBUG, "[VAAPI] Decoded surface FOURCC: %c%c%c%c", 
                  (char)((debug_img.format.fourcc >> 0) & 0xFF),
                  (char)((debug_img.format.fourcc >> 8) & 0xFF),
                  (char)((debug_img.format.fourcc >> 16) & 0xFF),
                  (char)((debug_img.format.fourcc >> 24) & 0xFF));
              libva.vaDestroyImage(display_, debug_img.image_id);
          }
          libva.vaDestroyBuffer(display_, pic_param_buf);
          Frame frame = {};
          frame.pts = packet.pts;
          Picture pic;
          pic.format = output_format_.format;
          pic.width = output_format_.width;
          pic.height = output_format_.height;
          pic.buffer = std::make_shared<VAAPIHardwarePicture>(display_, surface);
          frame.data = std::move(pic);
          return Ok(std::vector<Frame> {std::move(frame)});
        }
        if (obu_size > sz) break;
        p += obu_size;
        sz -= obu_size;
      }
    } else if (codec_id_ == OM_CODEC_VP9) {
      VASurfaceID surface = surface_pool_[surface_idx_];
      surface_idx_ = (surface_idx_ + 1) % surface_pool_.size();
      VADecPictureParameterBufferVP9 pic_param = {};
      pic_param.frame_width = (uint16_t) width_;
      pic_param.frame_height = (uint16_t) height_;
      VABufferID pic_param_buf;
      libva.vaCreateBuffer(display_, context_, VAPictureParameterBufferType, sizeof(pic_param), 1, &pic_param, &pic_param_buf);
      libva.vaBeginPicture(display_, context_, surface);
      libva.vaRenderPicture(display_, context_, &pic_param_buf, 1);
      libva.vaEndPicture(display_, context_);
      libva.vaSyncSurface(display_, surface);
      libva.vaDestroyBuffer(display_, pic_param_buf);
      Frame frame = {};
      frame.pts = packet.pts;
      Picture pic;
      pic.format = output_format_.format;
      pic.width = output_format_.width;
      pic.height = output_format_.height;
      pic.buffer = std::make_shared<VAAPIHardwarePicture>(display_, surface);
      frame.data = std::move(pic);
      return Ok(std::vector<Frame> {std::move(frame)});
    }
    return Ok(std::vector<Frame> {});
  }

  void flush() override {
    surface_idx_ = 0;
    dpb_.clear();
  }
private:
  void release() {
    auto& libva = LibVA::getInstance();
    if (!libva.isLoaded()) return;
    if (context_ != VA_INVALID_ID) {
      libva.vaDestroyContext(display_, context_);
      context_ = VA_INVALID_ID;
    }
    if (config_ != VA_INVALID_ID) {
      libva.vaDestroyConfig(display_, config_);
      config_ = VA_INVALID_ID;
    }
    if (!surface_pool_.empty()) {
      libva.vaDestroySurfaces(display_, surface_pool_.data(), (int) surface_pool_.size());
      surface_pool_.clear();
    }
    if (h265_stream_) {
      h265_free(h265_stream_);
      h265_stream_ = nullptr;
    }
    dpb_.clear();
    initialized_ = false;
  }
};

class VAAPIEncoder final : public Encoder {
  std::unique_ptr<OMVAAPIContext> hw_context_;
  VADisplay display_ = nullptr;
  VAConfigID config_ = VA_INVALID_ID;
  VAContextID context_ = VA_INVALID_ID;
  VABufferID coded_buf_ = VA_INVALID_ID;
  bool initialized_ = false;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  std::vector<VASurfaceID> surface_pool_;
  size_t surface_idx_ = 0;
  OMCodecId codec_id_ = OM_CODEC_NONE;
  uint32_t frame_count_ = 0;

public:
  VAAPIEncoder() = default;
  ~VAAPIEncoder() override { release(); }

  auto configure(const EncoderOptions& options) -> OMError override {
    auto& libva = LibVA::getInstance();
    if (!libva.load()) return OM_CODEC_HWACCEL_FAILED;
    codec_id_ = options.format.codec_id;
    width_ = options.format.video.width;
    height_ = options.format.video.height;
    OMVAAPIInit init = {};
    if (options.hw_device.has_value() && options.hw_device->type == HWDeviceType::VAAPI) {
      display_ = static_cast<OMVAAPIContext*>(options.hw_device->context)->display;
    } else {
      hw_context_ = std::unique_ptr<OMVAAPIContext>(HWVAAPIContext_create(init));
      if (!hw_context_) return OM_CODEC_HWACCEL_FAILED;
      display_ = hw_context_->display;
    }
    uint32_t rt_format = VA_RT_FORMAT_YUV420;
    int bit_depth = 8;

    VAProfile profile = VAProfileNone;

    auto get_supported_profile = [&](const std::vector<VAProfile>& candidates) {
      int num_profiles = libva.vaMaxNumConfigProfiles(display_);
      std::vector<VAProfile> supported(num_profiles);
      if (libva.vaQueryConfigProfiles(display_, supported.data(), &num_profiles) != VA_STATUS_SUCCESS) return VAProfileNone;
      for (auto p : candidates) {
        for (int i = 0; i < num_profiles; i++)
          if (supported[i] == p) return p;
      }
      return VAProfileNone;
    };

    if (codec_id_ == OM_CODEC_H264) {
      profile = get_supported_profile({VAProfileH264High, VAProfileH264Main, VAProfileH264ConstrainedBaseline});
    } else if (codec_id_ == OM_CODEC_H265) {
      profile = get_supported_profile({VAProfileHEVCMain10, VAProfileHEVCMain});
      if (profile == VAProfileHEVCMain10) {
        rt_format = VA_RT_FORMAT_YUV420_10;
        bit_depth = 10;
      }
    } else if (codec_id_ == OM_CODEC_VP9) {
      profile = get_supported_profile({VAProfileVP9Profile2, VAProfileVP9Profile0});
      if (profile == VAProfileVP9Profile2) {
        rt_format = VA_RT_FORMAT_YUV420_10;
        bit_depth = 10;
      }
    } else if (codec_id_ == OM_CODEC_AV1) {
      profile = get_supported_profile({VAProfileAV1Profile0});
    }
    if (profile == VAProfileNone) return OM_CODEC_NOT_SUPPORTED;

    VAStatus status = libva.vaCreateConfig(display_, profile, VAEntrypointEncSlice, nullptr, 0, &config_);
    if (status != VA_STATUS_SUCCESS) return OM_CODEC_HWACCEL_FAILED;
    surface_pool_.resize(4);
    status = libva.vaCreateSurfaces(display_, rt_format, width_, height_, surface_pool_.data(), (uint32_t) surface_pool_.size(), nullptr, 0);
    status = libva.vaCreateContext(display_, config_, (int) width_, (int) height_, VA_PROGRESSIVE, surface_pool_.data(), (int) surface_pool_.size(), &context_);
    libva.vaCreateBuffer(display_, context_, VAEncCodedBufferType, width_ * height_ * 3 / 2, 1, nullptr, &coded_buf_);
    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> EncodingInfo override {
    return {};
  }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    if (!initialized_) return Err(OM_COMMON_NOT_INITIALIZED);
    if (!std::holds_alternative<Picture>(frame.data)) return Ok(std::vector<Packet> {});
    const auto& pic = std::get<Picture>(frame.data);
    auto& libva = LibVA::getInstance();
    VASurfaceID surface = surface_pool_[surface_idx_];
    surface_idx_ = (surface_idx_ + 1) % surface_pool_.size();
    if (std::holds_alternative<HostPicture>(pic.buffer)) {
      const auto& host = std::get<HostPicture>(pic.buffer);
      VAImage image;
      libva.vaDeriveImage(display_, surface, &image);
      void* ptr = nullptr;
      libva.vaMapBuffer(display_, image.buf, &ptr);
      if (ptr) {
        std::memcpy(ptr, host.buffer->bytes().data(), std::min((size_t) image.data_size, host.buffer->bytes().size()));
        libva.vaUnmapBuffer(display_, image.buf);
      }
      libva.vaDestroyImage(display_, image.image_id);
    }
    VABufferID seq_param_buf = VA_INVALID_ID, pic_param_buf = VA_INVALID_ID, slice_param_buf = VA_INVALID_ID;
    if (codec_id_ == OM_CODEC_H264) {
      VAEncSequenceParameterBufferH264 s = {};
      s.seq_parameter_set_id = 0;
      s.picture_width_in_mbs = (uint16_t) ((width_ + 15) / 16);
      s.picture_height_in_mbs = (uint16_t) ((height_ + 15) / 16);
      s.bit_depth_luma_minus8 = 0;
      s.bit_depth_chroma_minus8 = 0;
      s.max_num_ref_frames = 1;
      s.seq_fields.bits.frame_mbs_only_flag = 1;
      libva.vaCreateBuffer(display_, context_, VAEncSequenceParameterBufferType, sizeof(s), 1, &s, &seq_param_buf);
      VAEncPictureParameterBufferH264 p = {};
      p.CurrPic.picture_id = surface;
      p.coded_buf = coded_buf_;
      libva.vaCreateBuffer(display_, context_, VAEncPictureParameterBufferType, sizeof(p), 1, &p, &pic_param_buf);
      VAEncSliceParameterBufferH264 sl = {};
      sl.macroblock_address = 0;
      sl.num_macroblocks = ((width_ + 15) / 16) * ((height_ + 15) / 16);
      sl.slice_type = (frame_count_ % 30 == 0) ? 2 : 1;
      libva.vaCreateBuffer(display_, context_, VAEncSliceParameterBufferType, sizeof(sl), 1, &sl, &slice_param_buf);
    } else if (codec_id_ == OM_CODEC_H265) {
      VAEncSequenceParameterBufferHEVC s = {};
      s.pic_width_in_luma_samples = (uint16_t) width_;
      s.pic_height_in_luma_samples = (uint16_t) height_;
      s.log2_min_luma_coding_block_size_minus3 = 0;
      libva.vaCreateBuffer(display_, context_, VAEncSequenceParameterBufferType, sizeof(s), 1, &s, &seq_param_buf);
      VAEncPictureParameterBufferHEVC p = {};
      p.decoded_curr_pic.picture_id = surface;
      p.coded_buf = coded_buf_;
      libva.vaCreateBuffer(display_, context_, VAEncPictureParameterBufferType, sizeof(p), 1, &p, &pic_param_buf);
      VAEncSliceParameterBufferHEVC sl = {};
      sl.slice_type = (frame_count_ % 30 == 0) ? 2 : 1;
      libva.vaCreateBuffer(display_, context_, VAEncSliceParameterBufferType, sizeof(sl), 1, &sl, &slice_param_buf);
    } else if (codec_id_ == OM_CODEC_AV1) {
      VAEncSequenceParameterBufferAV1 s = {};
      s.seq_profile = 0;
      s.seq_level_idx = 0;
      libva.vaCreateBuffer(display_, context_, VAEncSequenceParameterBufferType, sizeof(s), 1, &s, &seq_param_buf);
      VAEncPictureParameterBufferAV1 p = {};
      p.picture_flags.bits.frame_type = (frame_count_ % 30 == 0) ? 0 : 1;
      p.coded_buf = coded_buf_;
      p.order_hint = (uint8_t) (frame_count_ & 0xFF);
      p.frame_width_minus_1 = (uint16_t) (width_ - 1);
      p.frame_height_minus_1 = (uint16_t) (height_ - 1);
      libva.vaCreateBuffer(display_, context_, VAEncPictureParameterBufferType, sizeof(p), 1, &p, &pic_param_buf);
    } else if (codec_id_ == OM_CODEC_VP9) {
      VAEncPictureParameterBufferVP9 p = {};
      p.frame_width_dst = (uint32_t) width_;
      p.frame_height_dst = (uint32_t) height_;
      p.coded_buf = coded_buf_;
      libva.vaCreateBuffer(display_, context_, VAEncPictureParameterBufferType, sizeof(p), 1, &p, &pic_param_buf);
    }
    libva.vaBeginPicture(display_, context_, surface);
    VABufferID bufs[3];
    int num_bufs = 0;
    if (seq_param_buf != VA_INVALID_ID) bufs[num_bufs++] = seq_param_buf;
    if (pic_param_buf != VA_INVALID_ID) bufs[num_bufs++] = pic_param_buf;
    if (slice_param_buf != VA_INVALID_ID) bufs[num_bufs++] = slice_param_buf;
    if (num_bufs > 0) libva.vaRenderPicture(display_, context_, bufs, num_bufs);
    libva.vaEndPicture(display_, context_);
    libva.vaSyncSurface(display_, surface);
    std::vector<uint8_t> coded_data;
    retrieveCodedBuffer(display_, coded_buf_, coded_data);
    if (seq_param_buf != VA_INVALID_ID) libva.vaDestroyBuffer(display_, seq_param_buf);
    if (pic_param_buf != VA_INVALID_ID) libva.vaDestroyBuffer(display_, pic_param_buf);
    if (slice_param_buf != VA_INVALID_ID) libva.vaDestroyBuffer(display_, slice_param_buf);
    Packet pkt;
    pkt.allocate(coded_data.size());
    std::memcpy(pkt.bytes.data(), coded_data.data(), coded_data.size());
    pkt.pts = frame.pts;
    pkt.dts = frame.pts;
    frame_count_++;
    return Ok(std::vector<Packet> {std::move(pkt)});
  }

  auto updateBitrate(const RateControlParams& rc) -> OMError override {
    return OM_SUCCESS;
  }

private:
  void release() {
    auto& libva = LibVA::getInstance();
    if (!libva.isLoaded()) return;
    if (coded_buf_ != VA_INVALID_ID) {
      libva.vaDestroyBuffer(display_, coded_buf_);
    }
    if (context_ != VA_INVALID_ID) {
      libva.vaDestroyContext(display_, context_);
    }
    if (config_ != VA_INVALID_ID) {
      libva.vaDestroyConfig(display_, config_);
    }
    if (!surface_pool_.empty()) {
      libva.vaDestroySurfaces(display_, surface_pool_.data(), (int) surface_pool_.size());
    }
    initialized_ = false;
  }
};

const CodecDescriptor CODEC_VAAPI_H264 = {
    .codec_id = OM_CODEC_H264,
    .type = OM_MEDIA_VIDEO,
    .name = "vaapi_h264",
    .long_name = "VA-API H.264/AVC Codec",
    .vendor = "VA-API",
    .flags = HARDWARE,
    .decoder_factory = [] { return std::make_unique<VAAPIDecoder>(); },
    .encoder_factory = [] { return std::make_unique<VAAPIEncoder>(); },
};

const CodecDescriptor CODEC_VAAPI_H265 = {
    .codec_id = OM_CODEC_H265,
    .type = OM_MEDIA_VIDEO,
    .name = "vaapi_h265",
    .long_name = "VA-API H.265/HEVC Codec",
    .vendor = "VA-API",
    .flags = HARDWARE,
    .decoder_factory = [] { return std::make_unique<VAAPIDecoder>(); },
    .encoder_factory = [] { return std::make_unique<VAAPIEncoder>(); },
};

const CodecDescriptor CODEC_VAAPI_VP9 = {
    .codec_id = OM_CODEC_VP9,
    .type = OM_MEDIA_VIDEO,
    .name = "vaapi_vp9",
    .long_name = "VA-API VP9 Decoder",
    .vendor = "VA-API",
    .flags = HARDWARE,
    .decoder_factory = [] { return std::make_unique<VAAPIDecoder>(); },
    .encoder_factory = [] { return std::make_unique<VAAPIEncoder>(); },
};

const CodecDescriptor CODEC_VAAPI_AV1 = {
    .codec_id = OM_CODEC_AV1,
    .type = OM_MEDIA_VIDEO,
    .name = "vaapi_av1",
    .long_name = "VA-API AV1 Decoder",
    .vendor = "VA-API",
    .flags = HARDWARE,
    .decoder_factory = [] { return std::make_unique<VAAPIDecoder>(); },
    .encoder_factory = [] { return std::make_unique<VAAPIEncoder>(); },
};

} // namespace openmedia
