#include "h264_internal.hpp"

namespace h264 {

auto read_pps(PPS& pps, Bitstream& bs) -> bool {
  if (!bs.valid() || bs.remaining() == 0) return false;
  pps = {};
  h264FillDefaultScaling(pps);
  auto rbsp = openmedia::nalToRbsp({bs.current(), bs.remaining()});
  if (rbsp.empty()) return false;
  BitReader br(rbsp);

  pps.pic_parameter_set_id = static_cast<int>(br.readUE());
  pps.seq_parameter_set_id = static_cast<int>(br.readUE());
  if (pps.pic_parameter_set_id < 0 || pps.pic_parameter_set_id >= 256) return false;
  if (pps.seq_parameter_set_id < 0 || pps.seq_parameter_set_id >= 32) return false;
  pps.entropy_coding_mode_flag = static_cast<int>(br.readBit());
  pps.pic_order_present_flag = static_cast<int>(br.readBit());
  pps.num_slice_groups_minus1 = static_cast<int>(br.readUE());
  if (pps.num_slice_groups_minus1 > 0) {
    pps.slice_group_map_type = static_cast<int>(br.readUE());
    if (pps.slice_group_map_type == 0) {
      for (int i = 0; i <= pps.num_slice_groups_minus1 && i < 8; ++i) pps.run_length_minus1[i] = static_cast<int>(br.readUE());
    } else if (pps.slice_group_map_type == 2) {
      for (int i = 0; i < pps.num_slice_groups_minus1 && i < 8; ++i) {
        pps.top_left[i] = static_cast<int>(br.readUE());
        pps.bottom_right[i] = static_cast<int>(br.readUE());
      }
    } else if (pps.slice_group_map_type == 3 || pps.slice_group_map_type == 4 || pps.slice_group_map_type == 5) {
      pps.slice_group_change_direction_flag = static_cast<int>(br.readBit());
      pps.slice_group_change_rate_minus1 = static_cast<int>(br.readUE());
    } else if (pps.slice_group_map_type == 6) {
      pps.pic_size_in_map_units_minus1 = static_cast<int>(br.readUE());
      const int bits = pps.num_slice_groups_minus1 + 1 <= 2 ? 1 : pps.num_slice_groups_minus1 + 1 <= 4 ? 2 : 3;
      for (int i = 0; i <= pps.pic_size_in_map_units_minus1 && i < 256; ++i) pps.slice_group_id[i] = static_cast<int>(br.readBits(bits));
    }
  }
  pps.num_ref_idx_l0_active_minus1 = static_cast<int>(br.readUE());
  pps.num_ref_idx_l1_active_minus1 = static_cast<int>(br.readUE());
  pps.weighted_pred_flag = static_cast<int>(br.readBit());
  pps.weighted_bipred_idc = static_cast<int>(br.readBits(2));
  pps.pic_init_qp_minus26 = br.readSE();
  pps.pic_init_qs_minus26 = br.readSE();
  pps.chroma_qp_index_offset = br.readSE();
  pps.deblocking_filter_control_present_flag = static_cast<int>(br.readBit());
  pps.constrained_intra_pred_flag = static_cast<int>(br.readBit());
  pps.redundant_pic_cnt_present_flag = static_cast<int>(br.readBit());

  if (br.moreRbspData()) {
    pps._more_rbsp_data_present = 1;
    pps.transform_8x8_mode_flag = static_cast<int>(br.readBit());
    pps.pic_scaling_matrix_present_flag = static_cast<int>(br.readBit());
    if (pps.pic_scaling_matrix_present_flag) {
      const int count = 6 + 2 * pps.transform_8x8_mode_flag;
      for (int i = 0; i < count; ++i) {
        pps.pic_scaling_list_present_flag[i] = static_cast<int>(br.readBit());
        if (!pps.pic_scaling_list_present_flag[i]) continue;
        if (i < 6) h264ReadScalingList(br, pps.ScalingList4x4[i], 16, pps.UseDefaultScalingMatrix4x4Flag[i]);
        else h264ReadScalingList(br, pps.ScalingList8x8[i - 6], 64, pps.UseDefaultScalingMatrix8x8Flag[i - 6]);
      }
    }
    pps.second_chroma_qp_index_offset = br.readSE();
  } else {
    pps.second_chroma_qp_index_offset = pps.chroma_qp_index_offset;
  }
  bs.finish();
  return br.ok();
}

} // namespace h264
