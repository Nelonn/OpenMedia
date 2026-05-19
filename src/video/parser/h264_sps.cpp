#include "h264_internal.hpp"

namespace h264 {

static auto isHighProfile(int profile) -> bool {
  switch (profile) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118: case 128: case 138: case 139: case 134: case 135:
      return true;
    default:
      return false;
  }
}

auto read_sps(SPS& sps, Bitstream& bs) -> bool {
  if (!bs.valid() || bs.remaining() == 0) return false;
  sps = {};
  h264FillDefaultScaling(sps);
  auto rbsp = openmedia::nalToRbsp({bs.current(), bs.remaining()});
  if (rbsp.empty()) return false;
  BitReader br(rbsp);

  sps.profile_idc = static_cast<int>(br.readBits(8));
  sps.constraint_set0_flag = static_cast<int>(br.readBit());
  sps.constraint_set1_flag = static_cast<int>(br.readBit());
  sps.constraint_set2_flag = static_cast<int>(br.readBit());
  sps.constraint_set3_flag = static_cast<int>(br.readBit());
  sps.constraint_set4_flag = static_cast<int>(br.readBit());
  sps.constraint_set5_flag = static_cast<int>(br.readBit());
  sps.reserved_zero_2bits = static_cast<int>(br.readBits(2));
  sps.level_idc = static_cast<int>(br.readBits(8));
  sps.seq_parameter_set_id = static_cast<int>(br.readUE());
  if (sps.seq_parameter_set_id < 0 || sps.seq_parameter_set_id >= 32) return false;

  if (isHighProfile(sps.profile_idc)) {
    sps.chroma_format_idc = static_cast<int>(br.readUE());
    if (sps.chroma_format_idc == 3) sps.separate_colour_plane_flag = static_cast<int>(br.readBit());
    sps.bit_depth_luma_minus8 = static_cast<int>(br.readUE());
    sps.bit_depth_chroma_minus8 = static_cast<int>(br.readUE());
    sps.qpprime_y_zero_transform_bypass_flag = static_cast<int>(br.readBit());
    sps.seq_scaling_matrix_present_flag = static_cast<int>(br.readBit());
    if (sps.seq_scaling_matrix_present_flag) {
      const int count = sps.chroma_format_idc != 3 ? 8 : 12;
      for (int i = 0; i < count; ++i) {
        sps.seq_scaling_list_present_flag[i] = static_cast<int>(br.readBit());
        if (!sps.seq_scaling_list_present_flag[i]) continue;
        if (i < 6) h264ReadScalingList(br, sps.ScalingList4x4[i], 16, sps.UseDefaultScalingMatrix4x4Flag[i]);
        else h264ReadScalingList(br, sps.ScalingList8x8[i - 6], 64, sps.UseDefaultScalingMatrix8x8Flag[i - 6]);
      }
    }
  }

  sps.log2_max_frame_num_minus4 = static_cast<int>(br.readUE());
  sps.pic_order_cnt_type = static_cast<int>(br.readUE());
  if (sps.pic_order_cnt_type == 0) {
    sps.log2_max_pic_order_cnt_lsb_minus4 = static_cast<int>(br.readUE());
  } else if (sps.pic_order_cnt_type == 1) {
    sps.delta_pic_order_always_zero_flag = static_cast<int>(br.readBit());
    sps.offset_for_non_ref_pic = br.readSE();
    sps.offset_for_top_to_bottom_field = br.readSE();
    sps.num_ref_frames_in_pic_order_cnt_cycle = static_cast<int>(br.readUE());
    const int count = sps.num_ref_frames_in_pic_order_cnt_cycle < 256 ? sps.num_ref_frames_in_pic_order_cnt_cycle : 256;
    for (int i = 0; i < count; ++i) sps.offset_for_ref_frame[i] = br.readSE();
  }
  sps.num_ref_frames = static_cast<int>(br.readUE());
  sps.gaps_in_frame_num_value_allowed_flag = static_cast<int>(br.readBit());
  sps.pic_width_in_mbs_minus1 = static_cast<int>(br.readUE());
  sps.pic_height_in_map_units_minus1 = static_cast<int>(br.readUE());
  sps.frame_mbs_only_flag = static_cast<int>(br.readBit());
  if (!sps.frame_mbs_only_flag) sps.mb_adaptive_frame_field_flag = static_cast<int>(br.readBit());
  sps.direct_8x8_inference_flag = static_cast<int>(br.readBit());
  sps.frame_cropping_flag = static_cast<int>(br.readBit());
  if (sps.frame_cropping_flag) {
    sps.frame_crop_left_offset = static_cast<int>(br.readUE());
    sps.frame_crop_right_offset = static_cast<int>(br.readUE());
    sps.frame_crop_top_offset = static_cast<int>(br.readUE());
    sps.frame_crop_bottom_offset = static_cast<int>(br.readUE());
  }
  sps.vui_parameters_present_flag = static_cast<int>(br.readBit());
  if (sps.vui_parameters_present_flag) h264ReadVui(br, sps);
  bs.finish();
  return br.ok();
}

} // namespace h264
