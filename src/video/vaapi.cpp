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
#include <util/io_util.hpp>
#include <video/parser/h264_types.hpp>
#include <video/parser/h265_parser.hpp>
#include <vector>
#include "vaapi_loader.hpp"

namespace openmedia {

using sps_t = video_parser::H265AccessUnitParser::Sps;
using pps_t = video_parser::H265AccessUnitParser::Pps;
using slice_segment_header_t = video_parser::H265SliceHeader;

static void fillPictureParamsH264(const h264::SPS& sps, const h264::PPS& pps, const h264::SliceHeader& slice, const h264::NALHeader& nal, VAPictureParameterBufferH264& va_pic_param) {
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
  va_pic_param.seq_fields.bits.MinLumaBiPredSize8x8 = sps.level_idc >= 31;
  va_pic_param.pic_init_qp_minus26 = (int8_t) (pps.pic_init_qp_minus26);
  va_pic_param.pic_init_qs_minus26 = (int8_t) (pps.pic_init_qs_minus26);
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
  va_pic_param.pic_fields.bits.reference_pic_flag = (nal.idc != 0);
  va_pic_param.frame_num = (uint16_t) slice.frame_num;
  va_pic_param.CurrPic.picture_id = VA_INVALID_ID;
  for (int i = 0; i < 16; i++) va_pic_param.ReferenceFrames[i].picture_id = VA_INVALID_ID;
}

static void fillSliceParamsH264(const h264::SliceHeader& slice, const h264::PPS& pps, VASliceParameterBufferH264& va_slice_param) {
  std::memset(&va_slice_param, 0, sizeof(va_slice_param));
  va_slice_param.first_mb_in_slice = (uint16_t) slice.first_mb_in_slice;
  va_slice_param.slice_type = (uint8_t) (slice.slice_type % 5);
  va_slice_param.direct_spatial_mv_pred_flag = (uint8_t) ((slice.slice_type % 5 == 1) ? slice.direct_spatial_mv_pred_flag : 0);
  va_slice_param.num_ref_idx_l0_active_minus1 = (uint8_t) (slice.num_ref_idx_active_override_flag ? slice.num_ref_idx_l0_active_minus1 : pps.num_ref_idx_l0_active_minus1);
  va_slice_param.num_ref_idx_l1_active_minus1 = (uint8_t) (slice.num_ref_idx_active_override_flag ? slice.num_ref_idx_l1_active_minus1 : pps.num_ref_idx_l1_active_minus1);
  va_slice_param.cabac_init_idc = (uint8_t) slice.cabac_init_idc;
  va_slice_param.slice_qp_delta = (int8_t) slice.slice_qp_delta;
  va_slice_param.disable_deblocking_filter_idc = (uint8_t) slice.disable_deblocking_filter_idc;
  va_slice_param.slice_alpha_c0_offset_div2 = (int8_t) slice.slice_alpha_c0_offset_div2;
  va_slice_param.slice_beta_offset_div2 = (int8_t) slice.slice_beta_offset_div2;

  va_slice_param.luma_log2_weight_denom = (uint8_t) slice.pwt.luma_log2_weight_denom;
  va_slice_param.chroma_log2_weight_denom = (uint8_t) slice.pwt.chroma_log2_weight_denom;

  va_slice_param.luma_weight_l0_flag = (uint8_t) (pps.weighted_pred_flag && (slice.slice_type % 5 == 0 || slice.slice_type % 5 == 3));
  if (slice.slice_type % 5 == 1) va_slice_param.luma_weight_l0_flag = (pps.weighted_bipred_idc == 1);

  va_slice_param.chroma_weight_l0_flag = va_slice_param.luma_weight_l0_flag;
  va_slice_param.luma_weight_l1_flag = (slice.slice_type % 5 == 1 && pps.weighted_bipred_idc == 1);
  va_slice_param.chroma_weight_l1_flag = va_slice_param.luma_weight_l1_flag;

  for (int i = 0; i < 32; i++) {
    if (va_slice_param.luma_weight_l0_flag) {
      va_slice_param.luma_weight_l0[i] = (int16_t) slice.pwt.luma_weight_l0[i];
      va_slice_param.luma_offset_l0[i] = (int16_t) slice.pwt.luma_offset_l0[i];
    } else {
      va_slice_param.luma_weight_l0[i] = (int16_t) (1 << slice.pwt.luma_log2_weight_denom);
      va_slice_param.luma_offset_l0[i] = 0;
    }
    if (va_slice_param.luma_weight_l1_flag) {
      va_slice_param.luma_weight_l1[i] = (int16_t) slice.pwt.luma_weight_l1[i];
      va_slice_param.luma_offset_l1[i] = (int16_t) slice.pwt.luma_offset_l1[i];
    } else {
      va_slice_param.luma_weight_l1[i] = (int16_t) (1 << slice.pwt.luma_log2_weight_denom);
      va_slice_param.luma_offset_l1[i] = 0;
    }
    for (int j = 0; j < 2; j++) {
      if (va_slice_param.chroma_weight_l0_flag) {
        va_slice_param.chroma_weight_l0[i][j] = (int16_t) slice.pwt.chroma_weight_l0[i][j];
        va_slice_param.chroma_offset_l0[i][j] = (int16_t) slice.pwt.chroma_offset_l0[i][j];
      } else {
        va_slice_param.chroma_weight_l0[i][j] = (int16_t) (1 << slice.pwt.chroma_log2_weight_denom);
        va_slice_param.chroma_offset_l0[i][j] = 0;
      }
      if (va_slice_param.chroma_weight_l1_flag) {
        va_slice_param.chroma_weight_l1[i][j] = (int16_t) slice.pwt.chroma_weight_l1[i][j];
        va_slice_param.chroma_offset_l1[i][j] = (int16_t) slice.pwt.chroma_offset_l1[i][j];
      } else {
        va_slice_param.chroma_weight_l1[i][j] = (int16_t) (1 << slice.pwt.chroma_log2_weight_denom);
        va_slice_param.chroma_offset_l1[i][j] = 0;
      }
    }
  }
}

static void fillPictureParamsHEVC(const sps_t* sps, const pps_t* pps, const slice_segment_header_t* ssh, int nal_type, VAPictureParameterBufferHEVC& va_pic) {
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
  va_pic.pic_fields.bits.NoPicReorderingFlag = (sps->sps_max_num_reorder_pics[sps->max_sub_layers_minus1] == 0);

  va_pic.bit_depth_luma_minus8 = (uint8_t) sps->bit_depth_luma_minus8;
  va_pic.bit_depth_chroma_minus8 = (uint8_t) sps->bit_depth_chroma_minus8;
  va_pic.pcm_sample_bit_depth_luma_minus1 = (uint8_t) sps->pcm_sample_bit_depth_luma_minus1;
  va_pic.pcm_sample_bit_depth_chroma_minus1 = (uint8_t) sps->pcm_sample_bit_depth_chroma_minus1;
  va_pic.log2_min_luma_coding_block_size_minus3 = (uint8_t) sps->log2_min_luma_coding_block_size_minus3;
  va_pic.log2_diff_max_min_luma_coding_block_size = (uint8_t) sps->log2_diff_max_min_luma_coding_block_size;
  va_pic.log2_min_transform_block_size_minus2 = (uint8_t) sps->log2_min_luma_transform_block_size_minus2;
  va_pic.log2_diff_max_min_transform_block_size = (uint8_t) (sps->log2_diff_max_min_luma_transform_block_size);
  va_pic.log2_min_pcm_luma_coding_block_size_minus3 = (uint8_t) sps->log2_min_pcm_luma_coding_block_size_minus3;
  va_pic.log2_diff_max_min_pcm_luma_coding_block_size = (uint8_t) sps->log2_diff_max_min_pcm_luma_coding_block_size;
  va_pic.max_transform_hierarchy_depth_inter = (uint8_t) sps->max_transform_hierarchy_depth_inter;
  va_pic.max_transform_hierarchy_depth_intra = (uint8_t) sps->max_transform_hierarchy_depth_intra;
  va_pic.sps_max_dec_pic_buffering_minus1 = (uint8_t) sps->sps_max_dec_pic_buffering_minus1[sps->max_sub_layers_minus1];
  va_pic.log2_max_pic_order_cnt_lsb_minus4 = (uint8_t) sps->log2_max_pic_order_cnt_lsb_minus4;
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
  va_pic.st_rps_bits = ssh->short_term_ref_pic_set_sps_flag ? 0 : (uint8_t)ssh->st_rps_bits;

  va_pic.slice_parsing_fields.bits.lists_modification_present_flag = pps->lists_modification_present_flag;
  va_pic.slice_parsing_fields.bits.long_term_ref_pics_present_flag = sps->long_term_ref_pics_present_flag;
  va_pic.slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag = sps->sps_temporal_mvp_enabled_flag;
  va_pic.slice_parsing_fields.bits.cabac_init_present_flag = pps->cabac_init_present_flag;
  va_pic.slice_parsing_fields.bits.output_flag_present_flag = pps->output_flag_present_flag;
  va_pic.slice_parsing_fields.bits.dependent_slice_segments_enabled_flag = pps->dependent_slice_segments_enabled_flag;
  va_pic.slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag = pps->pps_slice_chroma_qp_offsets_present_flag;
  va_pic.slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag = sps->sample_adaptive_offset_enabled_flag;
  va_pic.slice_parsing_fields.bits.deblocking_filter_override_enabled_flag = pps->deblocking_filter_override_enabled_flag;
  va_pic.slice_parsing_fields.bits.pps_disable_deblocking_filter_flag = pps->pps_deblocking_filter_disabled_flag;
  va_pic.slice_parsing_fields.bits.slice_segment_header_extension_present_flag = pps->slice_segment_header_extension_present_flag;
  
  va_pic.slice_parsing_fields.bits.RapPicFlag = (nal_type >= 16 && nal_type <= 21);
  va_pic.slice_parsing_fields.bits.IdrPicFlag = (nal_type == 19 || nal_type == 20);
  va_pic.slice_parsing_fields.bits.IntraPicFlag = (nal_type >= 16 && nal_type <= 21);

  if (pps->tiles_enabled_flag) {
    va_pic.num_tile_columns_minus1 = (uint8_t) pps->num_tile_columns_minus1;
    va_pic.num_tile_rows_minus1 = (uint8_t) pps->num_tile_rows_minus1;
    if (pps->uniform_spacing_flag) {
      int ctb_log2_size_y = sps->log2_min_luma_coding_block_size_minus3 + 3 + sps->log2_diff_max_min_luma_coding_block_size;
      int ctb_size_y = 1 << ctb_log2_size_y;
      int pic_width_in_ctbs_y = (sps->pic_width_in_luma_samples + ctb_size_y - 1) / ctb_size_y;
      int pic_height_in_ctbs_y = (sps->pic_height_in_luma_samples + ctb_size_y - 1) / ctb_size_y;
      
      for (int i = 0; i <= pps->num_tile_columns_minus1; ++i) {
        va_pic.column_width_minus1[i] = (((i + 1) * pic_width_in_ctbs_y) / (pps->num_tile_columns_minus1 + 1)) -
                                       ((i * pic_width_in_ctbs_y) / (pps->num_tile_columns_minus1 + 1)) - 1;
      }
      for (int j = 0; j <= pps->num_tile_rows_minus1; ++j) {
        va_pic.row_height_minus1[j] = (((j + 1) * pic_height_in_ctbs_y) / (pps->num_tile_rows_minus1 + 1)) -
                                     ((j * pic_height_in_ctbs_y) / (pps->num_tile_rows_minus1 + 1)) - 1;
      }
    } else {
      for (int i = 0; i <= pps->num_tile_columns_minus1; ++i) va_pic.column_width_minus1[i] = (uint16_t) pps->column_width_minus1[i];
      for (int i = 0; i <= pps->num_tile_rows_minus1; ++i) va_pic.row_height_minus1[i] = (uint16_t) pps->row_height_minus1[i];
    }
  }

  va_pic.CurrPic.picture_id = VA_INVALID_ID;
  for (int i = 0; i < 15; i++) {
    va_pic.ReferenceFrames[i].picture_id = VA_INVALID_ID;
    va_pic.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
  }
}

static void fillSliceParamsHEVC(const sps_t* sps, const slice_segment_header_t* ssh, VASliceParameterBufferHEVC& va_slice) {
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
  va_slice.LongSliceFlags.fields.slice_loop_filter_across_slices_enabled_flag = ssh->slice_loop_filter_across_slices_enabled_flag;
  
  if (!ssh->slice_temporal_mvp_enabled_flag)
    va_slice.collocated_ref_idx = 0xFF;
  else
    va_slice.collocated_ref_idx = (uint8_t) ssh->collocated_ref_idx;

  va_slice.num_ref_idx_l0_active_minus1 = (uint8_t) ssh->num_ref_idx_l0_active_minus1;
  va_slice.num_ref_idx_l1_active_minus1 = (uint8_t) ssh->num_ref_idx_l1_active_minus1;
  va_slice.slice_qp_delta = (int8_t) ssh->slice_qp_delta;
  va_slice.slice_cb_qp_offset = (int8_t) ssh->slice_cb_qp_offset;
  va_slice.slice_cr_qp_offset = (int8_t) ssh->slice_cr_qp_offset;
  va_slice.slice_beta_offset_div2 = (int8_t) ssh->slice_beta_offset_div2;
  va_slice.slice_tc_offset_div2 = (int8_t) ssh->slice_tc_offset_div2;
  va_slice.five_minus_max_num_merge_cand = (uint8_t) ssh->five_minus_max_num_merge_cand;

  va_slice.luma_log2_weight_denom = (uint8_t) ssh->pred_weight_table.luma_log2_weight_denom;
  va_slice.delta_chroma_log2_weight_denom = (int8_t) ssh->pred_weight_table.delta_chroma_log2_weight_denom;
  
  int chroma_log2_weight_denom = ssh->pred_weight_table.luma_log2_weight_denom + ssh->pred_weight_table.delta_chroma_log2_weight_denom;
  int wp_offset_half_range_c = 1 << (sps->bit_depth_chroma_minus8 + 7);

  for (int i = 0; i < 15; i++) {
    va_slice.delta_luma_weight_l0[i] = (int8_t) ssh->pred_weight_table.delta_luma_weight_l0[i];
    va_slice.luma_offset_l0[i] = (int8_t) ssh->pred_weight_table.luma_offset_l0[i];
    va_slice.delta_luma_weight_l1[i] = (int8_t) ssh->pred_weight_table.delta_luma_weight_l1[i];
    va_slice.luma_offset_l1[i] = (int8_t) ssh->pred_weight_table.luma_offset_l1[i];
    
    for (int j = 0; j < 2; j++) {
      va_slice.delta_chroma_weight_l0[i][j] = (int8_t) ssh->pred_weight_table.delta_chroma_weight_l0[i][j];
      va_slice.delta_chroma_weight_l1[i][j] = (int8_t) ssh->pred_weight_table.delta_chroma_weight_l1[i][j];

      auto calc_chroma_offset = [&](int delta_chroma_offset, int delta_chroma_weight) -> int16_t {
        int weight = (1 << chroma_log2_weight_denom) + delta_chroma_weight;
        int offset = wp_offset_half_range_c + delta_chroma_offset - ((wp_offset_half_range_c * weight) >> chroma_log2_weight_denom);
        return (int16_t) std::clamp(offset, -wp_offset_half_range_c, wp_offset_half_range_c - 1);
      };

      va_slice.ChromaOffsetL0[i][j] = calc_chroma_offset(ssh->pred_weight_table.delta_chroma_offset_l0[i][j], ssh->pred_weight_table.delta_chroma_weight_l0[i][j]);
      va_slice.ChromaOffsetL1[i][j] = calc_chroma_offset(ssh->pred_weight_table.delta_chroma_offset_l1[i][j], ssh->pred_weight_table.delta_chroma_weight_l1[i][j]);
    }
  }
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

struct RBSP {
  std::vector<uint8_t> data;
  std::vector<size_t> src_offsets;
};

static void strip_epb(const uint8_t* src, size_t size, RBSP& rbsp) {
  rbsp.data.clear();
  rbsp.src_offsets.clear();
  rbsp.data.reserve(size);
  rbsp.src_offsets.reserve(size);
  for (size_t i = 0; i < size; ++i) {
    if (i + 2 < size && src[i] == 0 && src[i + 1] == 0 && src[i + 2] == 3) {
      rbsp.data.push_back(0);
      rbsp.src_offsets.push_back(i);
      rbsp.data.push_back(0);
      rbsp.src_offsets.push_back(i + 1);
      i += 2;
    } else {
      rbsp.data.push_back(src[i]);
      rbsp.src_offsets.push_back(i);
    }
  }
}

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
  struct SurfaceSlot {
    VASurfaceID surface;
    std::weak_ptr<VAAPIHardwarePicture> last_pic;
  };
  std::vector<SurfaceSlot> surface_slots_;
  size_t surface_idx_ = 0;
  std::vector<VABufferID> cleanup_buffers;

  // DPB
  std::vector<DPBEntry> dpb_;
  static constexpr size_t MAX_DPB_SIZE = 16;

  // POC derivation state
  int32_t prev_poc_msb_ = 0;
  int32_t prev_poc_lsb_ = 0;

  // H264 state
  h264::SPS h264_sps_table_[32];
  h264::PPS h264_pps_table_[256];
  uint32_t active_sps_id_ = 0xFFFFFFFF;
  uint32_t active_pps_id_ = 0xFFFFFFFF;
  bool has_h264_sps_ = false;
  bool has_h264_pps_ = false;

  // H265 state
  video_parser::H265AccessUnitParser h265_parser_;
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
    VAProfile profile = VAProfileNone;
    const auto& om_profile = options.format.profile;

    int num_profiles = libva.vaMaxNumConfigProfiles(display_);
    std::vector<VAProfile> supported(num_profiles);
    if (libva.vaQueryConfigProfiles(display_, supported.data(), &num_profiles) != VA_STATUS_SUCCESS) return OM_CODEC_HWACCEL_FAILED;
    supported.resize(num_profiles);

    auto has_profile = [&](VAProfile p) {
      return std::find(supported.begin(), supported.end(), p) != supported.end();
    };

    if (codec_id_ == OM_CODEC_H264) {
      if (om_profile == OM_PROFILE_H264_HIGH_10) {
        if (has_profile(VAProfileH264High10)) {
          profile = VAProfileH264High10;
          rt_format = VA_RT_FORMAT_YUV420_10;
        }
      } else {
        // Try exact match first, then upgrade within 8-bit hierarchy
        std::vector<VAProfile> h264_8bit = {VAProfileH264ConstrainedBaseline, VAProfileH264Main, VAProfileH264High};
        int start_idx = 0;
        if (om_profile == OM_PROFILE_H264_MAIN)
          start_idx = 1;
        else if (om_profile == OM_PROFILE_H264_HIGH)
          start_idx = 2;

        for (int i = start_idx; i < (int) h264_8bit.size(); i++) {
          if (has_profile(h264_8bit[i])) {
            profile = h264_8bit[i];
            break;
          }
        }
      }
    } else if (codec_id_ == OM_CODEC_H265) {
      if (om_profile == OM_PROFILE_H265_MAIN_10) {
        if (has_profile(VAProfileHEVCMain10)) {
          profile = VAProfileHEVCMain10;
          rt_format = VA_RT_FORMAT_YUV420_10;
        }
      } else {
        if (has_profile(VAProfileHEVCMain))
          profile = VAProfileHEVCMain;
        else if (has_profile(VAProfileHEVCMain10)) {
          // Some hardware only supports Main10 but can decode 8-bit Main
          profile = VAProfileHEVCMain10;
          rt_format = VA_RT_FORMAT_YUV420_10;
        }
      }
    } else if (codec_id_ == OM_CODEC_VP9) {
      if (om_profile == OM_PROFILE_VP9_2) {
        if (has_profile(VAProfileVP9Profile2)) {
          profile = VAProfileVP9Profile2;
          rt_format = VA_RT_FORMAT_YUV420_10;
        }
      } else {
        if (has_profile(VAProfileVP9Profile0))
          profile = VAProfileVP9Profile0;
        else if (has_profile(VAProfileVP9Profile2)) {
          profile = VAProfileVP9Profile2;
          rt_format = VA_RT_FORMAT_YUV420_10;
        }
      }
    } else if (codec_id_ == OM_CODEC_AV1) {
      if (has_profile(VAProfileAV1Profile0)) profile = VAProfileAV1Profile0;
    }

    if (profile == VAProfileNone) return OM_CODEC_NOT_SUPPORTED;

    // Pad surface dimensions to 64x64 macroblock boundaries (HEVC CTU size)
    uint32_t padded_w = (width_ + 63) & ~63;
    uint32_t padded_h = (height_ + 63) & ~63;

    VAConfigAttrib attrib;
    attrib.type = VAConfigAttribRTFormat;
    attrib.value = rt_format;
    VAStatus status = libva.vaCreateConfig(display_, profile, VAEntrypointVLD, &attrib, 1, &config_);
    if (status != VA_STATUS_SUCCESS) return OM_CODEC_HWACCEL_FAILED;

    std::vector<VASurfaceID> surfaces(64);
    VASurfaceAttrib surf_attribs[1];
    surf_attribs[0].type = VASurfaceAttribPixelFormat;
    surf_attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    surf_attribs[0].value.type = VAGenericValueTypeInteger;
    surf_attribs[0].value.value.i = (rt_format == VA_RT_FORMAT_YUV420_10) ? VA_FOURCC_P010 : VA_FOURCC_NV12;

    status = libva.vaCreateSurfaces(display_, rt_format, padded_w, padded_h, surfaces.data(), (uint32_t) surfaces.size(), surf_attribs, 1);
    if (status != VA_STATUS_SUCCESS) return OM_CODEC_HWACCEL_FAILED;

    surface_slots_.clear();
    for (auto sid : surfaces) surface_slots_.push_back({sid, {}});

    status = libva.vaCreateContext(display_, config_, (int) padded_w, (int) padded_h, VA_PROGRESSIVE, surfaces.data(), (int) surfaces.size(), &context_);
    if (status != VA_STATUS_SUCCESS) return OM_CODEC_HWACCEL_FAILED;

    std::memset(h264_sps_table_, 0, sizeof(h264_sps_table_));
    std::memset(h264_pps_table_, 0, sizeof(h264_pps_table_));

    if (codec_id_ == OM_CODEC_H265) h265_parser_.reset();

    output_format_.width = width_;
    output_format_.height = height_;
    output_format_.format = (rt_format == VA_RT_FORMAT_YUV420_10) ? OM_FORMAT_P010 : OM_FORMAT_NV12;
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

      auto find_start_code = [](const uint8_t* p, size_t sz, size_t& header_size) -> size_t {
        for (size_t i = 0; i + 2 < sz; ++i) {
          if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            header_size = 3;
            size_t off = i;
            while (off > 0 && p[off - 1] == 0) {
              off--;
              header_size++;
            }
            return off;
          }
        }
        return sz;
      };

      size_t pos = 0;
      VASurfaceID surface = VA_INVALID_ID;
      bool picture_started = false;
      h264::SliceHeader first_slice = {};
      int first_nal_ref_idc = 0;
      int first_nal_type = 0;
      size_t first_surface_idx = 0;
      int32_t first_poc = 0;
      int32_t first_poc_msb = 0;
      RBSP rbsp;

      while (pos < size) {
        size_t header_sz = 0;
        size_t start = find_start_code(data + pos, size - pos, header_sz);
        if (start == size - pos) break;
        pos += start + header_sz;

        size_t next_header_sz = 0;
        size_t next = find_start_code(data + pos, size - pos, next_header_sz);
        size_t nal_size = next;
        const uint8_t* nal_ptr = data + pos;
        if (nal_size == 0) continue;

        strip_epb(nal_ptr, nal_size, rbsp);
        h264::Bitstream bs;
        bs.init(rbsp.data.data(), rbsp.data.size());
        h264::NALHeader nal;
        if (!h264::read_nal_header(nal, bs)) {
          pos += next;
          continue;
        }

        if (nal.type == h264::NAL_UNIT_TYPE_SPS) {
          h264::SPS sps;
          h264::read_sps(sps, bs);
          if (sps.seq_parameter_set_id < 32) {
            h264_sps_table_[sps.seq_parameter_set_id] = sps;
            has_h264_sps_ = true;
          }
        } else if (nal.type == h264::NAL_UNIT_TYPE_PPS) {
          h264::PPS pps;
          h264::read_pps(pps, bs);
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
          h264::read_slice_header(slice, nal, h264_pps_table_, h264_sps_table_, bs);

          const auto& pps = h264_pps_table_[slice.pic_parameter_set_id];
          const auto& sps = h264_sps_table_[pps.seq_parameter_set_id];

          if (!picture_started) {
            auto is_surface_busy = [&](const SurfaceSlot& slot) {
              if (!slot.last_pic.expired()) return true;
              for (const auto& e : dpb_)
                if (e.surface == slot.surface) return true;
              return false;
            };
            surface = VA_INVALID_ID;
            for (size_t i = 0; i < surface_slots_.size(); i++) {
              size_t idx = (surface_idx_ + i) % surface_slots_.size();
              if (!is_surface_busy(surface_slots_[idx])) {
                surface = surface_slots_[idx].surface;
                first_surface_idx = idx;
                surface_idx_ = (idx + 1) % surface_slots_.size();
                break;
              }
            }
            if (surface == VA_INVALID_ID) {
              log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "[VAAPI] Surface pool exhausted!");
              pos += next;
              continue;
            }

            first_slice = slice;
            first_nal_ref_idc = (int) nal.idc;
            first_nal_type = (int) nal.type;

            int32_t poc = 0;
            int32_t poc_msb = prev_poc_msb_;
            if (sps.pic_order_cnt_type == 0) {
              int32_t max_poc_lsb = 1 << (sps.log2_max_pic_order_cnt_lsb_minus4 + 4);
              if ((slice.pic_order_cnt_lsb < prev_poc_lsb_) &&
                  ((prev_poc_lsb_ - slice.pic_order_cnt_lsb) >= (max_poc_lsb / 2))) {
                poc_msb = prev_poc_msb_ + max_poc_lsb;
              } else if ((slice.pic_order_cnt_lsb > prev_poc_lsb_) &&
                         ((slice.pic_order_cnt_lsb - prev_poc_lsb_) > (max_poc_lsb / 2))) {
                poc_msb = prev_poc_msb_ - max_poc_lsb;
              }
              poc = poc_msb + slice.pic_order_cnt_lsb;
            } else if (sps.pic_order_cnt_type == 2)
              poc = (int32_t) slice.frame_num * 2;
            else
              poc = (int32_t) packet.pts;

            first_poc = poc;
            first_poc_msb = poc_msb;

            VAPictureParameterBufferH264 pic_param;
            fillPictureParamsH264(sps, pps, slice, nal, pic_param);
            pic_param.CurrPic.picture_id = surface;
            pic_param.CurrPic.frame_idx = slice.frame_num;
            pic_param.CurrPic.TopFieldOrderCnt = poc;
            pic_param.CurrPic.BottomFieldOrderCnt = poc;
            pic_param.CurrPic.flags = (nal.idc != 0 ? VA_PICTURE_H264_SHORT_TERM_REFERENCE : 0) | VA_PICTURE_H264_TOP_FIELD | VA_PICTURE_H264_BOTTOM_FIELD;

            for (size_t i = 0; i < dpb_.size() && i < 16; i++) {
              pic_param.ReferenceFrames[i].picture_id = dpb_[i].surface;
              pic_param.ReferenceFrames[i].TopFieldOrderCnt = dpb_[i].poc;
              pic_param.ReferenceFrames[i].BottomFieldOrderCnt = dpb_[i].poc;
              pic_param.ReferenceFrames[i].frame_idx = dpb_[i].frame_num;
              pic_param.ReferenceFrames[i].flags = (dpb_[i].is_long_term ? VA_PICTURE_H264_LONG_TERM_REFERENCE : VA_PICTURE_H264_SHORT_TERM_REFERENCE) | VA_PICTURE_H264_TOP_FIELD | VA_PICTURE_H264_BOTTOM_FIELD;
            }
            for (size_t i = dpb_.size(); i < 16; i++) {
              pic_param.ReferenceFrames[i].picture_id = VA_INVALID_ID;
              pic_param.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
            }

            VABufferID pic_param_buf, iq_matrix_buf;
            VAStatus status = libva.vaCreateBuffer(display_, context_, VAPictureParameterBufferType, sizeof(pic_param), 1, &pic_param, &pic_param_buf);

            VAIQMatrixBufferH264 iq_matrix = {};
            if (pps.pic_scaling_matrix_present_flag) {
              for (int i = 0; i < 6; i++)
                for (int j = 0; j < 16; j++) iq_matrix.ScalingList4x4[i][j] = (unsigned char) pps.ScalingList4x4[i][j];
              for (int i = 0; i < 2; i++)
                for (int j = 0; j < 64; j++) iq_matrix.ScalingList8x8[i][j] = (unsigned char) pps.ScalingList8x8[i][j];
            } else if (sps.seq_scaling_matrix_present_flag) {
              for (int i = 0; i < 6; i++)
                for (int j = 0; j < 16; j++) iq_matrix.ScalingList4x4[i][j] = (unsigned char) sps.ScalingList4x4[i][j];
              for (int i = 0; i < 2; i++)
                for (int j = 0; j < 64; j++) iq_matrix.ScalingList8x8[i][j] = (unsigned char) sps.ScalingList8x8[i][j];
            } else {
              for (int i = 0; i < 6; i++)
                for (int j = 0; j < 16; j++) iq_matrix.ScalingList4x4[i][j] = 16;
              for (int i = 0; i < 2; i++)
                for (int j = 0; j < 64; j++) iq_matrix.ScalingList8x8[i][j] = 16;
            }
            libva.vaCreateBuffer(display_, context_, VAIQMatrixBufferType, sizeof(iq_matrix), 1, &iq_matrix, &iq_matrix_buf);

            libva.vaBeginPicture(display_, context_, surface);
            VABufferID pic_bufs[] = {pic_param_buf, iq_matrix_buf};
            libva.vaRenderPicture(display_, context_, pic_bufs, 2);
            cleanup_buffers.push_back(pic_param_buf);
            cleanup_buffers.push_back(iq_matrix_buf);
            picture_started = true;
          }

          VASliceParameterBufferH264 slice_param;
          fillSliceParamsH264(slice, pps, slice_param);

          uint8_t slice_type = (uint8_t) (slice.slice_type % 5);
          if (slice_type == 0 /* P */ || slice_type == 3 /* SP */ || slice_type == 1 /* B */) {
            std::vector<DPBEntry> list0, list1;
            std::vector<DPBEntry> st_refs;
            for (const auto& e : dpb_)
              if (!e.is_long_term) st_refs.push_back(e);

            if (slice_type == 0 || slice_type == 3) {
              std::sort(st_refs.begin(), st_refs.end(), [](const DPBEntry& a, const DPBEntry& b) {
                int diff = (int) a.frame_num - (int) b.frame_num;
                if (diff < -30000) return true;
                if (diff > 30000) return false;
                return diff > 0;
              });
              list0 = st_refs;
            } else if (slice_type == 1) {
              std::vector<DPBEntry> past, future;
              for (const auto& e : st_refs) {
                if (e.poc < first_poc)
                  past.push_back(e);
                else
                  future.push_back(e);
              }
              std::sort(past.begin(), past.end(), [](const DPBEntry& a, const DPBEntry& b) { return a.poc > b.poc; });
              std::sort(future.begin(), future.end(), [](const DPBEntry& a, const DPBEntry& b) { return a.poc < b.poc; });
              list0 = past;
              list0.insert(list0.end(), future.begin(), future.end());
              list1 = future;
              list1.insert(list1.end(), past.begin(), past.end());
            }

            int ref_count = slice_param.num_ref_idx_l0_active_minus1 + 1;
            for (int i = 0; i < ref_count && i < (int) list0.size() && i < 32; i++) {
              const auto& entry = list0[i];
              slice_param.RefPicList0[i].picture_id = entry.surface;
              slice_param.RefPicList0[i].TopFieldOrderCnt = entry.poc;
              slice_param.RefPicList0[i].BottomFieldOrderCnt = entry.poc;
              slice_param.RefPicList0[i].frame_idx = entry.frame_num;
              slice_param.RefPicList0[i].flags = (entry.is_long_term ? VA_PICTURE_H264_LONG_TERM_REFERENCE : VA_PICTURE_H264_SHORT_TERM_REFERENCE) | VA_PICTURE_H264_TOP_FIELD | VA_PICTURE_H264_BOTTOM_FIELD;
            }
            if (slice_type == 1 /* B */) {
              int ref_count_l1 = slice_param.num_ref_idx_l1_active_minus1 + 1;
              for (int i = 0; i < ref_count_l1 && i < (int) list1.size() && i < 32; i++) {
                const auto& entry = list1[i];
                slice_param.RefPicList1[i].picture_id = entry.surface;
                slice_param.RefPicList1[i].TopFieldOrderCnt = entry.poc;
                slice_param.RefPicList1[i].BottomFieldOrderCnt = entry.poc;
                slice_param.RefPicList1[i].frame_idx = entry.frame_num;
                slice_param.RefPicList1[i].flags = (entry.is_long_term ? VA_PICTURE_H264_LONG_TERM_REFERENCE : VA_PICTURE_H264_SHORT_TERM_REFERENCE) | VA_PICTURE_H264_TOP_FIELD | VA_PICTURE_H264_BOTTOM_FIELD;
              }
            }
          }

          slice_param.slice_data_size = (uint32_t) nal_size;
          slice_param.slice_data_offset = 0;
          slice_param.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;

          const size_t rbsp_bit_off = 8u + static_cast<size_t>(slice.header_bit_size);
          size_t rbsp_byte_off = rbsp_bit_off / 8u;
          if (rbsp_byte_off >= rbsp.src_offsets.size()) rbsp_byte_off = rbsp.src_offsets.size() - 1;
          size_t orig_byte_off = rbsp.src_offsets[rbsp_byte_off];
          slice_param.slice_data_bit_offset = (uint16_t) (orig_byte_off * 8 + (rbsp_bit_off & 7u));

          VABufferID slice_param_buf, slice_data_buf;
          libva.vaCreateBuffer(display_, context_, VASliceParameterBufferType, sizeof(slice_param), 1, &slice_param, &slice_param_buf);
          libva.vaCreateBuffer(display_, context_, VASliceDataBufferType, (uint32_t) nal_size, 1, (void*) nal_ptr, &slice_data_buf);
          VABufferID slice_bufs[] = {slice_param_buf, slice_data_buf};
          libva.vaRenderPicture(display_, context_, slice_bufs, 2);
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
        if (first_nal_ref_idc != 0) {
          if (sps.pic_order_cnt_type == 0) {
            prev_poc_lsb_ = first_slice.pic_order_cnt_lsb;
            prev_poc_msb_ = first_poc_msb;
          }
          if (first_nal_type == 5) dpb_.clear(); // Type 5 is IDR
          size_t max_refs = std::max((size_t) 1, (size_t) sps.num_ref_frames);
          while (dpb_.size() >= max_refs) dpb_.erase(dpb_.begin());
          dpb_.push_back({surface, first_poc, (uint32_t) first_slice.frame_num, false});
        }

        Frame frame = {};
        frame.pts = packet.pts;
        frame.dts = packet.dts;
        Picture pic;
        pic.format = output_format_.format;
        pic.width = output_format_.width;
        pic.height = output_format_.height;
        auto pic_obj = std::make_shared<VAAPIHardwarePicture>(display_, surface);
        surface_slots_[first_surface_idx].last_pic = pic_obj;
        pic.buffer = pic_obj;
        frame.data = std::move(pic);
        log(OM_CATEGORY_HARDWARE, OM_LEVEL_DEBUG, "[VAAPI] Decoded frame pts={} poc={} dpb_size={} nal_type={}",
            (unsigned long long) packet.pts, first_poc, dpb_.size(), first_nal_ref_idc);
        return Ok(std::vector<Frame> {std::move(frame)});
      }
    } else if (codec_id_ == OM_CODEC_H265) {
      auto frames = h265_parser_.parse(packet.bytes);
      std::vector<Frame> out_frames;
      auto& libva = LibVA::getInstance();

      for (const auto& parsed : frames) {
        if (!h265_parser_.hasSps() || !h265_parser_.hasPps()) continue;
        if (parsed.slice_headers.empty()) continue;

        const auto& first_sh = parsed.slice_headers[0];
        const auto& pps = h265_parser_.pps(first_sh.pps_id);
        const auto& sps = h265_parser_.sps(pps.sps_id);

        auto is_surface_busy = [&](const SurfaceSlot& slot) {
          if (!slot.last_pic.expired()) return true;
          for (const auto& e : dpb_) if (e.surface == slot.surface) return true;
          return false;
        };

        VASurfaceID surface = VA_INVALID_ID;
        size_t surface_idx = 0;
        for (size_t i = 0; i < surface_slots_.size(); i++) {
          size_t idx = (surface_idx_ + i) % surface_slots_.size();
          if (!is_surface_busy(surface_slots_[idx])) {
            surface = surface_slots_[idx].surface;
            surface_idx = idx;
            surface_idx_ = (idx + 1) % surface_slots_.size();
            break;
          }
        }
        if (surface == VA_INVALID_ID) {
          log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "[VAAPI] Surface pool exhausted!");
          break;
        }

        VAPictureParameterBufferHEVC pic_param;
        fillPictureParamsHEVC(&sps, &pps, &first_sh, parsed.nal_unit_type, pic_param);

        pic_param.CurrPic.picture_id = surface;
        pic_param.CurrPic.pic_order_cnt = parsed.poc;
        pic_param.CurrPic.flags = 0;

        // Identify which DPB entries are used by current picture using RPS
        std::vector<int> st_curr_before, st_curr_after;
        const auto& rps = first_sh.short_term_ref_pic_set_sps_flag ? sps.st_ref_pic_set[first_sh.short_term_ref_pic_set_idx] : first_sh.st_ref_pic_set;
        
        for (int i = 0; i < rps.num_negative_pics; i++) {
          if (rps.used_by_curr_pic_s0_flag[i]) st_curr_before.push_back(parsed.poc + rps.delta_poc_s0[i]);
        }
        for (int i = 0; i < rps.num_positive_pics; i++) {
          if (rps.used_by_curr_pic_s1_flag[i]) st_curr_after.push_back(parsed.poc + rps.delta_poc_s1[i]);
        }

        for (size_t i = 0; i < dpb_.size() && i < 15; i++) {
          pic_param.ReferenceFrames[i].picture_id = dpb_[i].surface;
          pic_param.ReferenceFrames[i].pic_order_cnt = dpb_[i].poc;
          pic_param.ReferenceFrames[i].flags = 0;
          
          bool is_active = false;
          for (int poc : st_curr_before) {
            if (dpb_[i].poc == poc) {
              pic_param.ReferenceFrames[i].flags |= VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
              is_active = true;
              break;
            }
          }
          if (!is_active) {
            for (int poc : st_curr_after) {
              if (dpb_[i].poc == poc) {
                pic_param.ReferenceFrames[i].flags |= VA_PICTURE_HEVC_RPS_ST_CURR_AFTER;
                is_active = true;
                break;
              }
            }
          }
          if (!is_active && dpb_[i].is_long_term) {
             // Simplified: we don't track LT usage perfectly here
             pic_param.ReferenceFrames[i].flags |= VA_PICTURE_HEVC_LONG_TERM_REFERENCE;
          }
        }

        VABufferID pic_param_buf;
        libva.vaCreateBuffer(display_, context_, VAPictureParameterBufferType, sizeof(pic_param), 1, &pic_param, &pic_param_buf);
        libva.vaBeginPicture(display_, context_, surface);
        libva.vaRenderPicture(display_, context_, &pic_param_buf, 1);
        std::vector<VABufferID> cleanup_bufs;
        cleanup_bufs.push_back(pic_param_buf);

        if (sps.scaling_list_enabled_flag) {
          VAIQMatrixBufferHEVC iq_matrix = {};
          const auto& sl = pps.pps_scaling_list_data_present_flag ? pps.scaling_list_data : sps.scaling_list_data;
          std::memcpy(iq_matrix.ScalingList4x4, sl.scaling_list_4x4, sizeof(iq_matrix.ScalingList4x4));
          std::memcpy(iq_matrix.ScalingList8x8, sl.scaling_list_8x8, sizeof(iq_matrix.ScalingList8x8));
          std::memcpy(iq_matrix.ScalingList16x16, sl.scaling_list_16x16, sizeof(iq_matrix.ScalingList16x16));
          std::memcpy(iq_matrix.ScalingList32x32, sl.scaling_list_32x32, sizeof(iq_matrix.ScalingList32x32));
          std::memcpy(iq_matrix.ScalingListDC16x16, sl.scaling_list_dc_coef_16x16, sizeof(iq_matrix.ScalingListDC16x16));
          std::memcpy(iq_matrix.ScalingListDC32x32, sl.scaling_list_dc_coef_32x32, sizeof(iq_matrix.ScalingListDC32x32));
          
          VABufferID iq_buf;
          libva.vaCreateBuffer(display_, context_, VAIQMatrixBufferType, sizeof(iq_matrix), 1, &iq_matrix, &iq_buf);
          libva.vaRenderPicture(display_, context_, &iq_buf, 1);
          cleanup_bufs.push_back(iq_buf);
        }

        for (size_t i = 0; i < parsed.slice_offsets.size(); ++i) {
          const auto& sh = parsed.slice_headers[i];
          uint32_t offset = parsed.slice_offsets[i];
          uint32_t next_offset = (i + 1 < parsed.slice_offsets.size()) ? parsed.slice_offsets[i + 1] : (uint32_t)parsed.bitstream.size();
          uint32_t slice_size = next_offset - offset;

          VASliceParameterBufferHEVC slice_param;
          fillSliceParamsHEVC(&sps, &sh, slice_param);
          slice_param.slice_data_size = slice_size;
          slice_param.slice_data_offset = 0;
          slice_param.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
          slice_param.slice_data_byte_offset = (sh.header_bit_size + 7) / 8;

          std::memset(slice_param.RefPicList, 0xFF, sizeof(slice_param.RefPicList));
          if (sh.slice_type != 2 /* I */) {
            int l0_count = 0;
            int l1_count = 0;
            // Populating L0 using ST_CURR_BEFORE POCs
            for (int poc : st_curr_before) {
              for (size_t j = 0; j < dpb_.size() && j < 15; j++) {
                if (dpb_[j].poc == poc) {
                  if (l0_count <= sh.num_ref_idx_l0_active_minus1) slice_param.RefPicList[0][l0_count++] = (uint8_t)j;
                  break;
                }
              }
            }
            // Populating L1 using ST_CURR_AFTER POCs
            for (int poc : st_curr_after) {
              for (size_t j = 0; j < dpb_.size() && j < 15; j++) {
                if (dpb_[j].poc == poc) {
                  if (l1_count <= sh.num_ref_idx_l1_active_minus1) slice_param.RefPicList[1][l1_count++] = (uint8_t)j;
                  break;
                }
              }
            }
            // Fallback: fill remaining slots with any DPB entries if needed
            for (int j = 0; j < (int)dpb_.size() && j < 15; j++) {
              if (l0_count <= sh.num_ref_idx_l0_active_minus1) {
                bool already_in = false;
                for (int k = 0; k < l0_count; k++) if (slice_param.RefPicList[0][k] == j) already_in = true;
                if (!already_in) slice_param.RefPicList[0][l0_count++] = (uint8_t)j;
              }
              if (sh.slice_type == 0 /* B */ && l1_count <= sh.num_ref_idx_l1_active_minus1) {
                bool already_in = false;
                for (int k = 0; k < l1_count; k++) if (slice_param.RefPicList[1][k] == j) already_in = true;
                if (!already_in) slice_param.RefPicList[1][l1_count++] = (uint8_t)j;
              }
            }
          }

          VABufferID slice_param_buf, slice_data_buf;
          libva.vaCreateBuffer(display_, context_, VASliceParameterBufferType, sizeof(slice_param), 1, &slice_param, &slice_param_buf);
          libva.vaCreateBuffer(display_, context_, VASliceDataBufferType, slice_size, 1, (void*)(parsed.bitstream.data() + offset), &slice_data_buf);
          VABufferID bufs[] = {slice_param_buf, slice_data_buf};
          libva.vaRenderPicture(display_, context_, bufs, 2);
          cleanup_bufs.push_back(slice_param_buf);
          cleanup_bufs.push_back(slice_data_buf);
        }

        libva.vaEndPicture(display_, context_);
        libva.vaSyncSurface(display_, surface);
        for (auto buf : cleanup_bufs) libva.vaDestroyBuffer(display_, buf);

        if (parsed.nal_unit_type == 19 || parsed.nal_unit_type == 20) dpb_.clear();
        if (dpb_.size() >= MAX_DPB_SIZE) dpb_.erase(dpb_.begin());
        dpb_.push_back({surface, (int32_t)parsed.poc, 0, false});

        Frame frame = {};
        frame.pts = packet.pts;
        frame.dts = packet.dts;
        Picture pic;
        pic.format = output_format_.format;
        pic.width = output_format_.width;
        pic.height = output_format_.height;
        auto pic_obj = std::make_shared<VAAPIHardwarePicture>(display_, surface);
        surface_slots_[surface_idx].last_pic = pic_obj;
        pic.buffer = pic_obj;
        frame.data = std::move(pic);
        out_frames.push_back(std::move(frame));
      }
      return Ok(std::move(out_frames));
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
          auto is_surface_busy = [&](const SurfaceSlot& slot) {
            if (!slot.last_pic.expired()) return true;
            for (const auto& e : dpb_)
              if (e.surface == slot.surface) return true;
            return false;
          };
          VASurfaceID surface = VA_INVALID_ID;
          size_t selected_slot_idx = 0;
          for (size_t i = 0; i < surface_slots_.size(); i++) {
            size_t idx = (surface_idx_ + i) % surface_slots_.size();
            if (!is_surface_busy(surface_slots_[idx])) {
              surface = surface_slots_[idx].surface;
              selected_slot_idx = idx;
              surface_idx_ = (idx + 1) % surface_slots_.size();
              break;
            }
          }
          if (surface == VA_INVALID_ID) break;

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
          libva.vaDestroyBuffer(display_, pic_param_buf);
          Frame frame = {};
          frame.pts = packet.pts;
          Picture pic;
          pic.format = output_format_.format;
          pic.width = output_format_.width;
          pic.height = output_format_.height;
          auto pic_obj = std::make_shared<VAAPIHardwarePicture>(display_, surface);
          surface_slots_[selected_slot_idx].last_pic = pic_obj;
          pic.buffer = pic_obj;
          frame.data = std::move(pic);
          return Ok(std::vector<Frame> {std::move(frame)});
        }
        if (obu_size > sz) break;
        p += obu_size;
        sz -= obu_size;
      }
    } else if (codec_id_ == OM_CODEC_VP9) {
      auto is_surface_busy = [&](const SurfaceSlot& slot) {
        if (!slot.last_pic.expired()) return true;
        for (const auto& e : dpb_)
          if (e.surface == slot.surface) return true;
        return false;
      };
      VASurfaceID surface = VA_INVALID_ID;
      size_t selected_slot_idx = 0;
      for (size_t i = 0; i < surface_slots_.size(); i++) {
        size_t idx = (surface_idx_ + i) % surface_slots_.size();
        if (!is_surface_busy(surface_slots_[idx])) {
          surface = surface_slots_[idx].surface;
          selected_slot_idx = idx;
          surface_idx_ = (idx + 1) % surface_slots_.size();
          break;
        }
      }
      if (surface != VA_INVALID_ID) {
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
        auto pic_obj = std::make_shared<VAAPIHardwarePicture>(display_, surface);
        surface_slots_[selected_slot_idx].last_pic = pic_obj;
        pic.buffer = pic_obj;
        frame.data = std::move(pic);
        return Ok(std::vector<Frame> {std::move(frame)});
      }
    }
    return Ok(std::vector<Frame> {});
  }

  void flush() override {
    surface_idx_ = 0;
    dpb_.clear();
    prev_poc_msb_ = 0;
    prev_poc_lsb_ = 0;
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
    if (!surface_slots_.empty()) {
      std::vector<VASurfaceID> surfaces;
      for (const auto& slot : surface_slots_) surfaces.push_back(slot.surface);
      libva.vaDestroySurfaces(display_, surfaces.data(), (int) surfaces.size());
      surface_slots_.clear();
    }
    h265_parser_.reset();
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
      surface_pool_.clear();
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
