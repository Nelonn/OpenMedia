#include "h264_internal.hpp"

namespace h264 {

static auto readScalingLists(BitReader& br, PPS& pps, const SPS& sps) -> bool {
  for (int i = 0; i < 6; ++i) {
    pps.pic_scaling_list_present_flag[i] = static_cast<int>(br.readBit());
    if (!pps.pic_scaling_list_present_flag[i]) {
      h264ApplyPpsScalingFallback(pps, sps, i);
      continue;
    }
    if (!h264ReadScalingList(br, pps.ScalingList4x4[i], 16, pps.UseDefaultScalingMatrix4x4Flag[i])) return false;
    if (pps.UseDefaultScalingMatrix4x4Flag[i]) h264DefaultScalingList4x4(i, pps.ScalingList4x4);
  }
  if (!pps.transform_8x8_mode_flag) return true;
  const int count_8x8 = sps.chroma_format_idc != 3 ? 2 : 6;
  for (int i = 0; i < count_8x8; ++i) {
    pps.pic_scaling_list_present_flag[6 + i] = static_cast<int>(br.readBit());
    if (!pps.pic_scaling_list_present_flag[6 + i]) {
      h264ApplyPpsScalingFallback(pps, sps, 6 + i);
      continue;
    }
    if (!h264ReadScalingList(br, pps.ScalingList8x8[i], 64, pps.UseDefaultScalingMatrix8x8Flag[i])) return false;
    if (pps.UseDefaultScalingMatrix8x8Flag[i]) h264DefaultScalingList8x8(i, pps.ScalingList8x8);
  }
  return true;
}

auto read_pps(PPS& pps, const SPS sps_table[32], Bitstream& bs, openmedia::RbspBuffer& scratch) -> bool {
  if (!bs.valid() || bs.remaining() == 0) return false;
  pps = {};
  h264FillFlatScaling(pps);
  // PPS NALs are small and more_rbsp_data() needs the rbsp_stop_one_bit, so
  // always unescape the whole unit here.
  scratch.convert({bs.current(), bs.remaining()});
  bs.finish();
  if (scratch.rbsp().empty()) return false;
  BitReader br(scratch.rbsp());

  pps.pic_parameter_set_id = static_cast<int>(br.readUE());
  pps.seq_parameter_set_id = static_cast<int>(br.readUE());
  if (pps.pic_parameter_set_id < 0 || pps.pic_parameter_set_id >= 256) return false;
  if (pps.seq_parameter_set_id < 0 || pps.seq_parameter_set_id >= 32) return false;
  const SPS& sps = sps_table[pps.seq_parameter_set_id];
  if (sps.seq_parameter_set_id != pps.seq_parameter_set_id) return false;

  pps.entropy_coding_mode_flag = static_cast<int>(br.readBit());
  pps.pic_order_present_flag = static_cast<int>(br.readBit());
  pps.num_slice_groups_minus1 = static_cast<int>(br.readUE());
  if (pps.num_slice_groups_minus1 < 0 || pps.num_slice_groups_minus1 > 7) return false;
  if (pps.num_slice_groups_minus1 > 0) {
    pps.slice_group_map_type = static_cast<int>(br.readUE());
    if (pps.slice_group_map_type < 0 || pps.slice_group_map_type > 6) return false;
    if (pps.slice_group_map_type == 0) {
      for (int i = 0; i <= pps.num_slice_groups_minus1; ++i) pps.run_length_minus1[i] = static_cast<int>(br.readUE());
    } else if (pps.slice_group_map_type == 2) {
      for (int i = 0; i < pps.num_slice_groups_minus1; ++i) {
        pps.top_left[i] = static_cast<int>(br.readUE());
        pps.bottom_right[i] = static_cast<int>(br.readUE());
      }
    } else if (pps.slice_group_map_type == 3 || pps.slice_group_map_type == 4 || pps.slice_group_map_type == 5) {
      pps.slice_group_change_direction_flag = static_cast<int>(br.readBit());
      pps.slice_group_change_rate_minus1 = static_cast<int>(br.readUE());
      if (pps.slice_group_change_rate_minus1 < 0) return false;
    } else if (pps.slice_group_map_type == 6) {
      pps.pic_size_in_map_units_minus1 = static_cast<int>(br.readUE());
      if (pps.pic_size_in_map_units_minus1 < 0 || pps.pic_size_in_map_units_minus1 >= 256) return false;
      const int bits = pps.num_slice_groups_minus1 + 1 <= 2 ? 1 : pps.num_slice_groups_minus1 + 1 <= 4 ? 2 : 3;
      for (int i = 0; i <= pps.pic_size_in_map_units_minus1; ++i) pps.slice_group_id[i] = static_cast<int>(br.readBits(bits));
    }
  }
  pps.num_ref_idx_l0_active_minus1 = static_cast<int>(br.readUE());
  pps.num_ref_idx_l1_active_minus1 = static_cast<int>(br.readUE());
  // Bounds the pred_weight_table loops in the slice header.
  if (pps.num_ref_idx_l0_active_minus1 < 0 || pps.num_ref_idx_l0_active_minus1 > 31) return false;
  if (pps.num_ref_idx_l1_active_minus1 < 0 || pps.num_ref_idx_l1_active_minus1 > 31) return false;
  pps.weighted_pred_flag = static_cast<int>(br.readBit());
  pps.weighted_bipred_idc = static_cast<int>(br.readBits(2));
  if (pps.weighted_bipred_idc > 2) return false;
  pps.pic_init_qp_minus26 = br.readSE();
  if (pps.pic_init_qp_minus26 < -26 || pps.pic_init_qp_minus26 > 25) return false;
  pps.pic_init_qs_minus26 = br.readSE();
  if (pps.pic_init_qs_minus26 < -26 || pps.pic_init_qs_minus26 > 25) return false;
  pps.chroma_qp_index_offset = br.readSE();
  if (pps.chroma_qp_index_offset < -12 || pps.chroma_qp_index_offset > 12) return false;
  pps.deblocking_filter_control_present_flag = static_cast<int>(br.readBit());
  pps.constrained_intra_pred_flag = static_cast<int>(br.readBit());
  pps.redundant_pic_cnt_present_flag = static_cast<int>(br.readBit());

  if (br.moreRbspData()) {
    pps._more_rbsp_data_present = 1;
    pps.transform_8x8_mode_flag = static_cast<int>(br.readBit());
    pps.pic_scaling_matrix_present_flag = static_cast<int>(br.readBit());
    if (pps.pic_scaling_matrix_present_flag && !readScalingLists(br, pps, sps)) return false;
    pps.second_chroma_qp_index_offset = br.readSE();
    if (pps.second_chroma_qp_index_offset < -12 || pps.second_chroma_qp_index_offset > 12) return false;
  } else {
    pps.second_chroma_qp_index_offset = pps.chroma_qp_index_offset;
  }
  return br.ok();
}

} // namespace h264
