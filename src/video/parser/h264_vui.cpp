#include "h264_internal.hpp"

namespace h264 {

static void read_hrd(BitReader& br, HRDParameters& hrd) {
  hrd.cpb_cnt_minus1 = static_cast<int>(br.readUE());
  hrd.bit_rate_scale = static_cast<int>(br.readBits(4));
  hrd.cpb_size_scale = static_cast<int>(br.readBits(4));
  const int count = hrd.cpb_cnt_minus1 < 31 ? hrd.cpb_cnt_minus1 + 1 : 32;
  for (int i = 0; i < count; ++i) {
    hrd.bit_rate_value_minus1[i] = static_cast<int>(br.readUE());
    hrd.cpb_size_value_minus1[i] = static_cast<int>(br.readUE());
    hrd.cbr_flag[i] = static_cast<int>(br.readBit());
  }
  hrd.initial_cpb_removal_delay_length_minus1 = static_cast<int>(br.readBits(5));
  hrd.cpb_removal_delay_length_minus1 = static_cast<int>(br.readBits(5));
  hrd.dpb_output_delay_length_minus1 = static_cast<int>(br.readBits(5));
  hrd.time_offset_length = static_cast<int>(br.readBits(5));
}

void h264ReadVui(BitReader& br, SPS& sps) {
  auto& vui = sps.vui;
  vui.aspect_ratio_info_present_flag = static_cast<int>(br.readBit());
  if (vui.aspect_ratio_info_present_flag) {
    vui.aspect_ratio_idc = static_cast<int>(br.readBits(8));
    if (vui.aspect_ratio_idc == 255) {
      vui.sar_width = static_cast<int>(br.readBits(16));
      vui.sar_height = static_cast<int>(br.readBits(16));
    }
  }
  vui.overscan_info_present_flag = static_cast<int>(br.readBit());
  if (vui.overscan_info_present_flag) vui.overscan_appropriate_flag = static_cast<int>(br.readBit());
  vui.video_signal_type_present_flag = static_cast<int>(br.readBit());
  if (vui.video_signal_type_present_flag) {
    vui.video_format = static_cast<int>(br.readBits(3));
    vui.video_full_range_flag = static_cast<int>(br.readBit());
    vui.colour_description_present_flag = static_cast<int>(br.readBit());
    if (vui.colour_description_present_flag) {
      vui.colour_primaries = static_cast<int>(br.readBits(8));
      vui.transfer_characteristics = static_cast<int>(br.readBits(8));
      vui.matrix_coefficients = static_cast<int>(br.readBits(8));
    }
  }
  vui.chroma_loc_info_present_flag = static_cast<int>(br.readBit());
  if (vui.chroma_loc_info_present_flag) {
    vui.chroma_sample_loc_type_top_field = static_cast<int>(br.readUE());
    vui.chroma_sample_loc_type_bottom_field = static_cast<int>(br.readUE());
  }
  vui.timing_info_present_flag = static_cast<int>(br.readBit());
  if (vui.timing_info_present_flag) {
    vui.num_units_in_tick = static_cast<int>(br.readBits(32));
    vui.time_scale = static_cast<int>(br.readBits(32));
    vui.fixed_frame_rate_flag = static_cast<int>(br.readBit());
  }
  vui.nal_hrd_parameters_present_flag = static_cast<int>(br.readBit());
  if (vui.nal_hrd_parameters_present_flag) read_hrd(br, sps.hrd);
  vui.vcl_hrd_parameters_present_flag = static_cast<int>(br.readBit());
  if (vui.vcl_hrd_parameters_present_flag) read_hrd(br, sps.hrd);
  if (vui.nal_hrd_parameters_present_flag || vui.vcl_hrd_parameters_present_flag) vui.low_delay_hrd_flag = static_cast<int>(br.readBit());
  vui.pic_struct_present_flag = static_cast<int>(br.readBit());
  vui.bitstream_restriction_flag = static_cast<int>(br.readBit());
  if (vui.bitstream_restriction_flag) {
    vui.motion_vectors_over_pic_boundaries_flag = static_cast<int>(br.readBit());
    vui.max_bytes_per_pic_denom = static_cast<int>(br.readUE());
    vui.max_bits_per_mb_denom = static_cast<int>(br.readUE());
    vui.log2_max_mv_length_horizontal = static_cast<int>(br.readUE());
    vui.log2_max_mv_length_vertical = static_cast<int>(br.readUE());
    vui.num_reorder_frames = static_cast<int>(br.readUE());
    vui.max_dec_frame_buffering = static_cast<int>(br.readUE());
  }
}

} // namespace h264
