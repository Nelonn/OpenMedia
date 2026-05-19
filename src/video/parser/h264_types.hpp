#pragma once

#include <cstddef>
#include <cstdint>

namespace h264 {

enum NAL_REF_IDC {
  NAL_REF_IDC_PRIORITY_DISPOSABLE = 0,
  NAL_REF_IDC_PRIORITY_LOW = 1,
  NAL_REF_IDC_PRIORITY_HIGH = 2,
  NAL_REF_IDC_PRIORITY_HIGHEST = 3,
};

enum NAL_UNIT_TYPE {
  NAL_UNIT_TYPE_UNSPECIFIED = 0,
  NAL_UNIT_TYPE_CODED_SLICE_NON_IDR = 1,
  NAL_UNIT_TYPE_CODED_SLICE_DATA_PARTITION_A = 2,
  NAL_UNIT_TYPE_CODED_SLICE_DATA_PARTITION_B = 3,
  NAL_UNIT_TYPE_CODED_SLICE_DATA_PARTITION_C = 4,
  NAL_UNIT_TYPE_CODED_SLICE_IDR = 5,
  NAL_UNIT_TYPE_SEI = 6,
  NAL_UNIT_TYPE_SPS = 7,
  NAL_UNIT_TYPE_PPS = 8,
  NAL_UNIT_TYPE_AUD = 9,
  NAL_UNIT_TYPE_END_OF_SEQUENCE = 10,
  NAL_UNIT_TYPE_END_OF_STREAM = 11,
  NAL_UNIT_TYPE_FILLER = 12,
  NAL_UNIT_TYPE_SPS_EXT = 13,
  NAL_UNIT_TYPE_PREFIX = 14,
  NAL_UNIT_TYPE_SUBSET_SPS = 15,
  NAL_UNIT_TYPE_CODED_SLICE_AUX = 19,
};

struct NALHeader {
  NAL_REF_IDC idc = NAL_REF_IDC_PRIORITY_DISPOSABLE;
  NAL_UNIT_TYPE type = NAL_UNIT_TYPE_UNSPECIFIED;
  int forbidden_zero_bit = 0;
};

struct HRDParameters {
  int cpb_cnt_minus1 = 0;
  int bit_rate_scale = 0;
  int cpb_size_scale = 0;
  int bit_rate_value_minus1[32] = {};
  int cpb_size_value_minus1[32] = {};
  int cbr_flag[32] = {};
  int initial_cpb_removal_delay_length_minus1 = 0;
  int cpb_removal_delay_length_minus1 = 0;
  int dpb_output_delay_length_minus1 = 0;
  int time_offset_length = 0;
};

struct VUIParameters {
  int aspect_ratio_info_present_flag = 0;
  int aspect_ratio_idc = 0;
  int sar_width = 0;
  int sar_height = 0;
  int overscan_info_present_flag = 0;
  int overscan_appropriate_flag = 0;
  int video_signal_type_present_flag = 0;
  int video_format = 5;
  int video_full_range_flag = 0;
  int colour_description_present_flag = 0;
  int colour_primaries = 2;
  int transfer_characteristics = 2;
  int matrix_coefficients = 2;
  int chroma_loc_info_present_flag = 0;
  int chroma_sample_loc_type_top_field = 0;
  int chroma_sample_loc_type_bottom_field = 0;
  int timing_info_present_flag = 0;
  int num_units_in_tick = 0;
  int time_scale = 0;
  int fixed_frame_rate_flag = 0;
  int nal_hrd_parameters_present_flag = 0;
  int vcl_hrd_parameters_present_flag = 0;
  int low_delay_hrd_flag = 0;
  int pic_struct_present_flag = 0;
  int bitstream_restriction_flag = 0;
  int motion_vectors_over_pic_boundaries_flag = 1;
  int max_bytes_per_pic_denom = 0;
  int max_bits_per_mb_denom = 0;
  int log2_max_mv_length_horizontal = 0;
  int log2_max_mv_length_vertical = 0;
  int num_reorder_frames = 0;
  int max_dec_frame_buffering = 0;
};

struct SPS {
  int profile_idc = 0;
  int constraint_set0_flag = 0;
  int constraint_set1_flag = 0;
  int constraint_set2_flag = 0;
  int constraint_set3_flag = 0;
  int constraint_set4_flag = 0;
  int constraint_set5_flag = 0;
  int reserved_zero_2bits = 0;
  int level_idc = 0;
  int seq_parameter_set_id = -1;
  int chroma_format_idc = 1;
  int separate_colour_plane_flag = 0;
  int bit_depth_luma_minus8 = 0;
  int bit_depth_chroma_minus8 = 0;
  int qpprime_y_zero_transform_bypass_flag = 0;
  int seq_scaling_matrix_present_flag = 0;
  int seq_scaling_list_present_flag[12] = {};
  int ScalingList4x4[6][16] = {};
  int UseDefaultScalingMatrix4x4Flag[6] = {};
  int ScalingList8x8[6][64] = {};
  int UseDefaultScalingMatrix8x8Flag[6] = {};
  int log2_max_frame_num_minus4 = 0;
  int pic_order_cnt_type = 0;
  int log2_max_pic_order_cnt_lsb_minus4 = 0;
  int delta_pic_order_always_zero_flag = 0;
  int offset_for_non_ref_pic = 0;
  int offset_for_top_to_bottom_field = 0;
  int num_ref_frames_in_pic_order_cnt_cycle = 0;
  int offset_for_ref_frame[256] = {};
  int num_ref_frames = 0;
  int gaps_in_frame_num_value_allowed_flag = 0;
  int pic_width_in_mbs_minus1 = 0;
  int pic_height_in_map_units_minus1 = 0;
  int frame_mbs_only_flag = 1;
  int mb_adaptive_frame_field_flag = 0;
  int direct_8x8_inference_flag = 0;
  int frame_cropping_flag = 0;
  int frame_crop_left_offset = 0;
  int frame_crop_right_offset = 0;
  int frame_crop_top_offset = 0;
  int frame_crop_bottom_offset = 0;
  int vui_parameters_present_flag = 0;
  VUIParameters vui = {};
  HRDParameters hrd = {};
};

struct PPS {
  int pic_parameter_set_id = -1;
  int seq_parameter_set_id = -1;
  int entropy_coding_mode_flag = 0;
  int pic_order_present_flag = 0;
  int num_slice_groups_minus1 = 0;
  int slice_group_map_type = 0;
  int run_length_minus1[8] = {};
  int top_left[8] = {};
  int bottom_right[8] = {};
  int slice_group_change_direction_flag = 0;
  int slice_group_change_rate_minus1 = 0;
  int pic_size_in_map_units_minus1 = 0;
  int slice_group_id[256] = {};
  int num_ref_idx_l0_active_minus1 = 0;
  int num_ref_idx_l1_active_minus1 = 0;
  int weighted_pred_flag = 0;
  int weighted_bipred_idc = 0;
  int pic_init_qp_minus26 = 0;
  int pic_init_qs_minus26 = 0;
  int chroma_qp_index_offset = 0;
  int deblocking_filter_control_present_flag = 1;
  int constrained_intra_pred_flag = 0;
  int redundant_pic_cnt_present_flag = 0;
  int _more_rbsp_data_present = 0;
  int transform_8x8_mode_flag = 0;
  int pic_scaling_matrix_present_flag = 0;
  int pic_scaling_list_present_flag[12] = {};
  int ScalingList4x4[6][16] = {};
  int UseDefaultScalingMatrix4x4Flag[6] = {};
  int ScalingList8x8[6][64] = {};
  int UseDefaultScalingMatrix8x8Flag[6] = {};
  int second_chroma_qp_index_offset = 0;
};

struct SliceHeader {
  struct PredWeightTable {
    int luma_log2_weight_denom = 0;
    int chroma_log2_weight_denom = 0;
    int luma_weight_l0_flag[32] = {};
    int luma_weight_l0[32] = {};
    int luma_offset_l0[32] = {};
    int chroma_weight_l0_flag[32] = {};
    int chroma_weight_l0[32][2] = {};
    int chroma_offset_l0[32][2] = {};
    int luma_weight_l1_flag[32] = {};
    int luma_weight_l1[32] = {};
    int luma_offset_l1[32] = {};
    int chroma_weight_l1_flag[32] = {};
    int chroma_weight_l1[32][2] = {};
    int chroma_offset_l1[32][2] = {};
  };

  int first_mb_in_slice = 0;
  int slice_type = 0;
  int pic_parameter_set_id = -1;
  int colour_plane_id = 0;
  int frame_num = 0;
  int field_pic_flag = 0;
  int bottom_field_flag = 0;
  int idr_pic_id = 0;
  int pic_order_cnt_lsb = 0;
  int delta_pic_order_cnt_bottom = 0;
  int delta_pic_order_cnt[2] = {};
  int redundant_pic_cnt = 0;
  int direct_spatial_mv_pred_flag = 0;
  int num_ref_idx_active_override_flag = 0;
  int num_ref_idx_l0_active_minus1 = 0;
  int num_ref_idx_l1_active_minus1 = 0;
  PredWeightTable pwt = {};
  int cabac_init_idc = 0;
  int slice_qp_delta = 0;
  int sp_for_switch_flag = 0;
  int slice_qs_delta = 0;
  int disable_deblocking_filter_idc = 0;
  int slice_alpha_c0_offset_div2 = 0;
  int slice_beta_offset_div2 = 0;
  int header_bit_size = 0;
};

struct Bitstream {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t offset = 0;

  void init(const uint8_t* bytes, size_t byte_count) {
    data = bytes;
    size = bytes || byte_count == 0 ? byte_count : 0;
    offset = 0;
  }
  auto valid() const -> bool { return data != nullptr || size == 0; }
  auto current() const -> const uint8_t* { return data ? data + offset : nullptr; }
  auto byte_offset() const -> size_t { return offset; }
  auto remaining() const -> size_t { return valid() && offset < size ? size - offset : 0; }
  auto consume(size_t bytes) -> bool {
    if (bytes > remaining()) {
      offset = size;
      return false;
    }
    offset += bytes;
    return true;
  }
  void set_offset(size_t new_offset) { offset = new_offset <= size ? new_offset : size; }
  void finish() { offset = size; }
};

auto find_next_nal(Bitstream& bs) -> bool;
auto read_nal_header(NALHeader& nal, Bitstream& bs) -> bool;
auto read_sps(SPS& sps, Bitstream& bs) -> bool;
auto read_pps(PPS& pps, Bitstream& bs) -> bool;
auto read_slice_header(SliceHeader& slice, const NALHeader& nal, const PPS pps_table[256], const SPS sps_table[32], Bitstream& bs) -> bool;

} // namespace h264
