#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <dxva.h>
#endif

#include <video/parser/h265_parser.hpp>
#include "dx_h264.hpp"

namespace openmedia::dx_h265 {

using Sps = video_parser::H265AccessUnitParser::Sps;
using Pps = video_parser::H265AccessUnitParser::Pps;
using SliceHeader = video_parser::H265SliceHeader;
using ParsedFrame = video_parser::H265ParsedFrame;

#ifdef _WIN32
struct SliceData {
  std::vector<uint8_t> bitstream;
  std::vector<DXVA_Slice_HEVC_Short> slices;
};
#endif

static auto isIdr(int nal_type) noexcept -> bool {
  return nal_type == video_parser::NAL_IDR_W_RADL || nal_type == video_parser::NAL_IDR_N_LP;
}

static auto isIrap(int nal_type) noexcept -> bool {
  return nal_type >= video_parser::NAL_BLA_W_LP && nal_type <= video_parser::NAL_CRA_NUT;
}

struct PocState {
  int prev_poc_lsb = 0;
  int prev_poc_msb = 0;
  bool have_prev = false;

  void reset() {
    prev_poc_lsb = 0;
    prev_poc_msb = 0;
    have_prev = false;
  }

  auto compute(const Sps& sps, const ParsedFrame& frame, const SliceHeader& sh) -> int32_t {
    if (isIdr(frame.nal_unit_type)) {
      reset();
      return 0;
    }
    const int max_poc_lsb = 1 << (sps.log2_max_pic_order_cnt_lsb_minus4 + 4);
    int poc_msb = 0;
    if (have_prev) {
      if (sh.slice_pic_order_cnt_lsb < prev_poc_lsb && prev_poc_lsb - sh.slice_pic_order_cnt_lsb >= max_poc_lsb / 2) {
        poc_msb = prev_poc_msb + max_poc_lsb;
      } else if (sh.slice_pic_order_cnt_lsb > prev_poc_lsb && sh.slice_pic_order_cnt_lsb - prev_poc_lsb > max_poc_lsb / 2) {
        poc_msb = prev_poc_msb - max_poc_lsb;
      } else {
        poc_msb = prev_poc_msb;
      }
    }
    if (frame.is_reference) {
      prev_poc_lsb = sh.slice_pic_order_cnt_lsb;
      prev_poc_msb = poc_msb;
      have_prev = true;
    }
    return poc_msb + sh.slice_pic_order_cnt_lsb;
  }
};

#ifdef _WIN32
static auto isStartCode(const std::vector<uint8_t>& data, size_t offset) noexcept -> size_t {
  if (offset + 3 <= data.size() && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 1) return 3;
  if (offset + 4 <= data.size() && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 0 && data[offset + 3] == 1) return 4;
  return 0;
}

static auto findNextStartCode(const std::vector<uint8_t>& data, size_t offset) noexcept -> size_t {
  for (size_t i = offset; i + 3 <= data.size(); ++i) {
    if (isStartCode(data, i) != 0) return i;
  }
  return data.size();
}

static auto buildSliceData(const ParsedFrame& frame) -> SliceData {
  SliceData out;
  out.slices.reserve(frame.slice_offsets.size());
  out.bitstream.reserve(frame.bitstream.size());

  for (const uint32_t offset : frame.slice_offsets) {
    if (offset >= frame.bitstream.size()) continue;
    const size_t start_code_size = isStartCode(frame.bitstream, offset);
    if (start_code_size == 0) continue;

    const size_t end = findNextStartCode(frame.bitstream, offset + start_code_size);
    const uint32_t location = static_cast<uint32_t>(out.bitstream.size());
    const uint32_t bytes = static_cast<uint32_t>(end - offset);
    out.bitstream.insert(out.bitstream.end(), frame.bitstream.begin() + static_cast<ptrdiff_t>(offset), frame.bitstream.begin() + static_cast<ptrdiff_t>(end));

    DXVA_Slice_HEVC_Short slice = {};
    slice.BSNALunitDataLocation = location;
    slice.SliceBytesInBuffer = bytes;
    slice.wBadSliceChopping = 0;
    out.slices.push_back(slice);
  }

  return out;
}

static void fillQMatrix(const Sps& sps, const Pps& pps, DXVA_Qmatrix_HEVC& qmatrix) {
  std::memset(&qmatrix, 16, sizeof(qmatrix));
  if (!sps.scaling_list_enabled_flag) return;

  const auto& sl = pps.pps_scaling_list_data_present_flag ? pps.scaling_list_data : sps.scaling_list_data;
  std::memcpy(qmatrix.ucScalingLists0, sl.scaling_list_4x4, sizeof(qmatrix.ucScalingLists0));
  std::memcpy(qmatrix.ucScalingLists1, sl.scaling_list_8x8, sizeof(qmatrix.ucScalingLists1));
  std::memcpy(qmatrix.ucScalingLists2, sl.scaling_list_16x16, sizeof(qmatrix.ucScalingLists2));
  std::memcpy(qmatrix.ucScalingLists3[0], sl.scaling_list_32x32[0], sizeof(qmatrix.ucScalingLists3[0]));
  std::memcpy(qmatrix.ucScalingLists3[1], sl.scaling_list_32x32[1], sizeof(qmatrix.ucScalingLists3[1]));
  std::memcpy(qmatrix.ucScalingListDCCoefSizeID2, sl.scaling_list_dc_coef_16x16, sizeof(qmatrix.ucScalingListDCCoefSizeID2));
  qmatrix.ucScalingListDCCoefSizeID3[0] = sl.scaling_list_dc_coef_32x32[0];
  qmatrix.ucScalingListDCCoefSizeID3[1] = sl.scaling_list_dc_coef_32x32[1];
}

static auto findRefIndex(uint32_t slot, const DXVA_PicParams_HEVC& pic) -> uint8_t {
  for (uint8_t i = 0; i < 15; ++i) {
    if (pic.RefPicList[i].Index7Bits == slot) return i;
  }
  return 0xff;
}

static void fillPicParams(const Sps& sps,
                          const Pps& pps,
                          const SliceHeader& sh,
                          const ParsedFrame& frame,
                          int32_t poc,
                          uint32_t current_slot,
                          const std::vector<uint8_t>& reference_usage,
                          const std::vector<dx_h264::DpbEntry>& dpb,
                          uint32_t feedback,
                          DXVA_PicParams_HEVC& pic) {
  pic = {};
  const int min_cb_log2_size_y = sps.log2_min_luma_coding_block_size_minus3 + 3;
  pic.PicWidthInMinCbsY = static_cast<USHORT>((sps.pic_width_in_luma_samples + (1 << min_cb_log2_size_y) - 1) >> min_cb_log2_size_y);
  pic.PicHeightInMinCbsY = static_cast<USHORT>((sps.pic_height_in_luma_samples + (1 << min_cb_log2_size_y) - 1) >> min_cb_log2_size_y);
  pic.chroma_format_idc = static_cast<USHORT>(sps.chroma_format_idc);
  pic.separate_colour_plane_flag = static_cast<USHORT>(sps.separate_colour_plane_flag);
  pic.bit_depth_luma_minus8 = static_cast<USHORT>(sps.bit_depth_luma_minus8);
  pic.bit_depth_chroma_minus8 = static_cast<USHORT>(sps.bit_depth_chroma_minus8);
  pic.log2_max_pic_order_cnt_lsb_minus4 = static_cast<USHORT>(sps.log2_max_pic_order_cnt_lsb_minus4);
  pic.NoPicReorderingFlag = 0;
  pic.NoBiPredFlag = 0;

  pic.CurrPic.Index7Bits = static_cast<UCHAR>(current_slot);
  pic.CurrPic.AssociatedFlag = 0;
  pic.CurrPicOrderCntVal = poc;
  for (int i = 0; i < 15; ++i) {
    pic.RefPicList[i].bPicEntry = 0xff;
    pic.PicOrderCntValList[i] = 0;
  }
  for (int i = 0; i < 8; ++i) {
    pic.RefPicSetStCurrBefore[i] = 0xff;
    pic.RefPicSetStCurrAfter[i] = 0xff;
    pic.RefPicSetLtCurr[i] = 0xff;
  }

  uint8_t ref_count = 0;
  for (size_t i = 0; i < reference_usage.size() && ref_count < 15; ++i) {
    const uint32_t ref_slot = reference_usage[i];
    if (ref_slot >= dpb.size() || ref_slot == current_slot || !dpb[ref_slot].is_reference) continue;
    pic.RefPicList[ref_count].Index7Bits = static_cast<UCHAR>(ref_slot);
    pic.RefPicList[ref_count].AssociatedFlag = 0;
    pic.PicOrderCntValList[ref_count] = dpb[ref_slot].poc;
    ++ref_count;
  }

  const auto& st = sh.short_term_ref_pic_set_sps_flag ? sps.st_ref_pic_set[sh.short_term_ref_pic_set_idx] : sh.st_ref_pic_set;
  uint8_t before = 0;
  for (int i = 0; i < st.num_negative_pics && before < 8; ++i) {
    const int target_poc = poc + st.delta_poc_s0[i];
    for (uint32_t slot = 0; slot < dpb.size(); ++slot) {
      if (dpb[slot].is_reference && dpb[slot].poc == target_poc) {
        pic.RefPicSetStCurrBefore[before++] = findRefIndex(slot, pic);
        break;
      }
    }
  }
  uint8_t after = 0;
  for (int i = 0; i < st.num_positive_pics && after < 8; ++i) {
    const int target_poc = poc + st.delta_poc_s1[i];
    for (uint32_t slot = 0; slot < dpb.size(); ++slot) {
      if (dpb[slot].is_reference && dpb[slot].poc == target_poc) {
        pic.RefPicSetStCurrAfter[after++] = findRefIndex(slot, pic);
        break;
      }
    }
  }

  pic.sps_max_dec_pic_buffering_minus1 = static_cast<UCHAR>(sps.sps_max_dec_pic_buffering_minus1[sps.max_sub_layers_minus1]);
  pic.log2_min_luma_coding_block_size_minus3 = static_cast<UCHAR>(sps.log2_min_luma_coding_block_size_minus3);
  pic.log2_diff_max_min_luma_coding_block_size = static_cast<UCHAR>(sps.log2_diff_max_min_luma_coding_block_size);
  pic.log2_min_transform_block_size_minus2 = static_cast<UCHAR>(sps.log2_min_luma_transform_block_size_minus2);
  pic.log2_diff_max_min_transform_block_size = static_cast<UCHAR>(sps.log2_diff_max_min_luma_transform_block_size);
  pic.max_transform_hierarchy_depth_inter = static_cast<UCHAR>(sps.max_transform_hierarchy_depth_inter);
  pic.max_transform_hierarchy_depth_intra = static_cast<UCHAR>(sps.max_transform_hierarchy_depth_intra);
  pic.num_short_term_ref_pic_sets = static_cast<UCHAR>(sps.num_short_term_ref_pic_sets);
  pic.num_long_term_ref_pics_sps = static_cast<UCHAR>(sps.num_long_term_ref_pics_sps);
  pic.scaling_list_enabled_flag = static_cast<UINT>(sps.scaling_list_enabled_flag);
  pic.amp_enabled_flag = static_cast<UINT>(sps.amp_enabled_flag);
  pic.sample_adaptive_offset_enabled_flag = static_cast<UINT>(sps.sample_adaptive_offset_enabled_flag);
  pic.pcm_enabled_flag = static_cast<UINT>(sps.pcm_enabled_flag);
  pic.pcm_sample_bit_depth_luma_minus1 = static_cast<UCHAR>(sps.pcm_sample_bit_depth_luma_minus1);
  pic.pcm_sample_bit_depth_chroma_minus1 = static_cast<UCHAR>(sps.pcm_sample_bit_depth_chroma_minus1);
  pic.log2_min_pcm_luma_coding_block_size_minus3 = static_cast<UCHAR>(sps.log2_min_pcm_luma_coding_block_size_minus3);
  pic.log2_diff_max_min_pcm_luma_coding_block_size = static_cast<UCHAR>(sps.log2_diff_max_min_pcm_luma_coding_block_size);
  pic.pcm_loop_filter_disabled_flag = static_cast<UINT>(sps.pcm_loop_filter_disabled_flag);
  pic.long_term_ref_pics_present_flag = static_cast<UINT>(sps.long_term_ref_pics_present_flag);
  pic.sps_temporal_mvp_enabled_flag = static_cast<UINT>(sps.sps_temporal_mvp_enabled_flag);
  pic.strong_intra_smoothing_enabled_flag = static_cast<UINT>(sps.strong_intra_smoothing_enabled_flag);

  pic.num_ref_idx_l0_default_active_minus1 = static_cast<UCHAR>(pps.num_ref_idx_l0_default_active_minus1);
  pic.num_ref_idx_l1_default_active_minus1 = static_cast<UCHAR>(pps.num_ref_idx_l1_default_active_minus1);
  pic.init_qp_minus26 = static_cast<CHAR>(pps.init_qp_minus26);
  pic.dependent_slice_segments_enabled_flag = static_cast<UINT>(pps.dependent_slice_segments_enabled_flag);
  pic.output_flag_present_flag = static_cast<UINT>(pps.output_flag_present_flag);
  pic.num_extra_slice_header_bits = static_cast<UINT>(pps.num_extra_slice_header_bits);
  pic.sign_data_hiding_enabled_flag = static_cast<UINT>(pps.sign_data_hiding_enabled_flag);
  pic.cabac_init_present_flag = static_cast<UINT>(pps.cabac_init_present_flag);
  pic.constrained_intra_pred_flag = static_cast<UINT>(pps.constrained_intra_pred_flag);
  pic.transform_skip_enabled_flag = static_cast<UINT>(pps.transform_skip_enabled_flag);
  pic.cu_qp_delta_enabled_flag = static_cast<UINT>(pps.cu_qp_delta_enabled_flag);
  pic.pps_slice_chroma_qp_offsets_present_flag = static_cast<UINT>(pps.pps_slice_chroma_qp_offsets_present_flag);
  pic.weighted_pred_flag = static_cast<UINT>(pps.weighted_pred_flag);
  pic.weighted_bipred_flag = static_cast<UINT>(pps.weighted_bipred_flag);
  pic.transquant_bypass_enabled_flag = static_cast<UINT>(pps.transquant_bypass_enabled_flag);
  pic.tiles_enabled_flag = static_cast<UINT>(pps.tiles_enabled_flag);
  pic.entropy_coding_sync_enabled_flag = static_cast<UINT>(pps.entropy_coding_sync_enabled_flag);
  pic.uniform_spacing_flag = static_cast<UINT>(pps.uniform_spacing_flag);
  pic.loop_filter_across_tiles_enabled_flag = static_cast<UINT>(pps.loop_filter_across_tiles_enabled_flag);
  pic.pps_loop_filter_across_slices_enabled_flag = static_cast<UINT>(pps.pps_loop_filter_across_slices_enabled_flag);
  pic.deblocking_filter_override_enabled_flag = static_cast<UINT>(pps.deblocking_filter_override_enabled_flag);
  pic.pps_deblocking_filter_disabled_flag = static_cast<UINT>(pps.pps_deblocking_filter_disabled_flag);
  pic.lists_modification_present_flag = static_cast<UINT>(pps.lists_modification_present_flag);
  pic.slice_segment_header_extension_present_flag = static_cast<UINT>(pps.slice_segment_header_extension_present_flag);
  pic.pps_cb_qp_offset = static_cast<CHAR>(pps.pps_cb_qp_offset);
  pic.pps_cr_qp_offset = static_cast<CHAR>(pps.pps_cr_qp_offset);
  pic.diff_cu_qp_delta_depth = static_cast<UCHAR>(pps.diff_cu_qp_delta_depth);
  pic.pps_beta_offset_div2 = static_cast<CHAR>(pps.pps_beta_offset_div2);
  pic.pps_tc_offset_div2 = static_cast<CHAR>(pps.pps_tc_offset_div2);
  pic.log2_parallel_merge_level_minus2 = static_cast<UCHAR>(pps.log2_parallel_merge_level_minus2);
  pic.num_tile_columns_minus1 = static_cast<UCHAR>(pps.tiles_enabled_flag ? pps.num_tile_columns_minus1 : 0);
  pic.num_tile_rows_minus1 = static_cast<UCHAR>(pps.tiles_enabled_flag ? pps.num_tile_rows_minus1 : 0);
  if (pps.tiles_enabled_flag && !pps.uniform_spacing_flag) {
    for (int i = 0; i <= pps.num_tile_columns_minus1 && i < 20; ++i) pic.column_width_minus1[i] = static_cast<USHORT>(pps.column_width_minus1[i]);
    for (int i = 0; i <= pps.num_tile_rows_minus1 && i < 22; ++i) pic.row_height_minus1[i] = static_cast<USHORT>(pps.row_height_minus1[i]);
  }

  pic.ucNumDeltaPocsOfRefRpsIdx = sh.short_term_ref_pic_set_sps_flag ? 0 : static_cast<UCHAR>(st.num_negative_pics + st.num_positive_pics);
  pic.wNumBitsForShortTermRPSInSlice = sh.short_term_ref_pic_set_sps_flag ? 0 : static_cast<USHORT>(sh.st_rps_bits);
  pic.IrapPicFlag = isIrap(frame.nal_unit_type) ? 1 : 0;
  pic.IdrPicFlag = isIdr(frame.nal_unit_type) ? 1 : 0;
  pic.IntraPicFlag = isIrap(frame.nal_unit_type) ? 1 : 0;
  pic.StatusReportFeedbackNumber = feedback == 0 ? 1 : feedback;
}

#endif

} // namespace openmedia::dx_h265
