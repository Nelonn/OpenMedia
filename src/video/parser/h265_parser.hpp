#pragma once

#include "start_code.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace openmedia::video_parser {

enum NalUnitType {
  NAL_TRAIL_N = 0,
  NAL_TRAIL_R = 1,
  NAL_TSA_N = 2,
  NAL_TSA_R = 3,
  NAL_STSA_N = 4,
  NAL_STSA_R = 5,
  NAL_RADL_N = 6,
  NAL_RADL_R = 7,
  NAL_RASL_N = 8,
  NAL_RASL_R = 9,
  NAL_BLA_W_LP = 16,
  NAL_BLA_W_RADL = 17,
  NAL_BLA_N_LP = 18,
  NAL_IDR_W_RADL = 19,
  NAL_IDR_N_LP = 20,
  NAL_CRA_NUT = 21,
  NAL_VPS = 32,
  NAL_SPS = 33,
  NAL_PPS = 34,
  NAL_AUD = 35,
  NAL_EOS = 36,
  NAL_EOB = 37,
  NAL_FD = 38,
  NAL_PREFIX_SEI = 39,
  NAL_SUFFIX_SEI = 40,
};

struct H265SliceHeader {
  int pps_id = 0;
  int slice_segment_address = 0;
  bool dependent_slice_segment_flag = false;
  int slice_type = 0;
  int colour_plane_id = 0;
  int slice_pic_order_cnt_lsb = 0;
  bool slice_temporal_mvp_enabled_flag = false;
  bool slice_sao_luma_flag = false;
  bool slice_sao_chroma_flag = false;
  bool mvd_l1_zero_flag = false;
  bool cabac_init_flag = false;
  bool collocated_from_l0_flag = false;
  int collocated_ref_idx = 0;
  int num_ref_idx_l0_active_minus1 = 0;
  int num_ref_idx_l1_active_minus1 = 0;
  int slice_qp_delta = 0;
  int slice_cb_qp_offset = 0;
  int slice_cr_qp_offset = 0;
  int slice_beta_offset_div2 = 0;
  int slice_tc_offset_div2 = 0;
  bool slice_loop_filter_across_slices_enabled_flag = false;
  int five_minus_max_num_merge_cand = 0;
  uint32_t header_bit_size = 0;
};

struct H265ParsedFrame {
  std::vector<uint8_t> bitstream;
  std::vector<uint32_t> slice_offsets;
  std::vector<H265SliceHeader> slice_headers;
  int nal_unit_type = 0;
  int poc = 0;
  bool is_irap = false;
  bool is_reference = false;
  bool parameter_sets_changed = false;
};

class H265AccessUnitParser {
public:
  void reset();
  void parseExtradata(std::span<const uint8_t> extradata);
  auto parse(std::span<const uint8_t> packet, bool end_of_packet = true) -> std::vector<H265ParsedFrame>;

  auto hasVps() const noexcept -> bool { return has_vps_; }
  auto hasSps() const noexcept -> bool { return has_sps_; }
  auto hasPps() const noexcept -> bool { return has_pps_; }

  struct Vps {
    bool valid = false;
    int id = -1;
    int max_layers_minus1 = 0;
    int max_sub_layers_minus1 = 0;
    bool temporal_id_nesting_flag = false;
  };

  struct Sps {
    bool valid = false;
    int id = -1;
    int vps_id = 0;
    int max_sub_layers_minus1 = 0;
    bool temporal_id_nesting_flag = false;
    int chroma_format_idc = 1;
    bool separate_colour_plane_flag = false;
    int pic_width_in_luma_samples = 0;
    int pic_height_in_luma_samples = 0;
    bool conformance_window_flag = false;
    int conf_win_left_offset = 0;
    int conf_win_right_offset = 0;
    int conf_win_top_offset = 0;
    int conf_win_bottom_offset = 0;
    int bit_depth_luma_minus8 = 0;
    int bit_depth_chroma_minus8 = 0;
    int log2_max_pic_order_cnt_lsb_minus4 = 4;
    bool sps_sub_layer_ordering_info_present_flag = false;
    int sps_max_dec_pic_buffering_minus1[8] = {};
    int sps_max_num_reorder_pics[8] = {};
    int sps_max_latency_increase_plus1[8] = {};
    int log2_min_luma_coding_block_size_minus3 = 0;
    int log2_diff_max_min_luma_coding_block_size = 0;
    int log2_min_luma_transform_block_size_minus2 = 0;
    int log2_diff_max_min_luma_transform_block_size = 0;
    int max_transform_hierarchy_depth_inter = 0;
    int max_transform_hierarchy_depth_intra = 0;
    bool scaling_list_enabled_flag = false;
    bool sps_scaling_list_data_present_flag = false;
    bool amp_enabled_flag = false;
    bool sample_adaptive_offset_enabled_flag = false;
    bool pcm_enabled_flag = false;
    int pcm_sample_bit_depth_luma_minus1 = 0;
    int pcm_sample_bit_depth_chroma_minus1 = 0;
    int log2_min_pcm_luma_coding_block_size_minus3 = 0;
    int log2_diff_max_min_pcm_luma_coding_block_size = 0;
    bool pcm_loop_filter_disabled_flag = false;
    int num_short_term_ref_pic_sets = 0;
    bool long_term_ref_pics_present_flag = false;
    int num_long_term_ref_pics_sps = 0;
    bool sps_temporal_mvp_enabled_flag = false;
    bool strong_intra_smoothing_enabled_flag = false;
    bool vui_parameters_present_flag = false;
  };

  struct Pps {
    bool valid = false;
    int id = -1;
    int sps_id = -1;
    bool dependent_slice_segments_enabled_flag = false;
    bool output_flag_present_flag = false;
    int num_extra_slice_header_bits = 0;
    bool sign_data_hiding_enabled_flag = false;
    bool cabac_init_present_flag = false;
    int num_ref_idx_l0_default_active_minus1 = 0;
    int num_ref_idx_l1_default_active_minus1 = 0;
    int init_qp_minus26 = 0;
    bool constrained_intra_pred_flag = false;
    bool transform_skip_enabled_flag = false;
    bool cu_qp_delta_enabled_flag = false;
    int diff_cu_qp_delta_depth = 0;
    int pps_cb_qp_offset = 0;
    int pps_cr_qp_offset = 0;
    bool pps_slice_chroma_qp_offsets_present_flag = false;
    bool weighted_pred_flag = false;
    bool weighted_bipred_flag = false;
    bool transquant_bypass_enabled_flag = false;
    bool tiles_enabled_flag = false;
    bool entropy_coding_sync_enabled_flag = false;
    int num_tile_columns_minus1 = 0;
    int num_tile_rows_minus1 = 0;
    bool uniform_spacing_flag = false;
    int column_width_minus1[20] = {};
    int row_height_minus1[20] = {};
    bool loop_filter_across_tiles_enabled_flag = false;
    bool pps_loop_filter_across_slices_enabled_flag = false;
    bool deblocking_filter_control_present_flag = false;
    bool deblocking_filter_override_enabled_flag = false;
    bool pps_deblocking_filter_disabled_flag = false;
    int pps_beta_offset_div2 = 0;
    int pps_tc_offset_div2 = 0;
    bool pps_scaling_list_data_present_flag = false;
    bool lists_modification_present_flag = false;
    int log2_parallel_merge_level_minus2 = 0;
    bool slice_segment_header_extension_present_flag = false;
    bool pps_extension_present_flag = false;
  };

  auto vps(int id) const noexcept -> const Vps& { return vps_[id & 0xf]; }
  auto sps(int id) const noexcept -> const Sps& { return sps_[id & 0xf]; }
  auto pps(int id) const noexcept -> const Pps& { return pps_[id & 0x3f]; }

private:
  struct NalUnit {
    size_t start = 0;
    size_t header = 0;
    size_t end = 0;
  };

  Vps vps_[16] = {};
  Sps sps_[16] = {};
  Pps pps_[64] = {};
  StartCodeScanner scanner_;
  H265ParsedFrame current_ = {};
  bool current_has_vcl_ = false;
  bool current_parameter_sets_changed_ = false;
  bool has_vps_ = false;
  bool has_sps_ = false;
  bool has_pps_ = false;
  uint8_t nal_length_size_ = 0;
  int previous_poc_ = 0;
  int previous_nal_type_ = -1;
  bool have_previous_slice_ = false;
  int nal_unit_type_ = 0;
  int slice_pic_order_cnt_lsb_ = 0;
  bool first_slice_segment_in_pic_flag_ = false;

  auto normalizePacket(std::span<const uint8_t> packet) const -> std::vector<uint8_t>;
  auto findNalUnits(std::span<const uint8_t> packet) -> std::vector<NalUnit>;
  auto parseNal(std::span<const uint8_t> nal_data) -> bool;
  auto startsNewAccessUnit(int nal_type) const -> bool;
  auto finishCurrentFrame() -> H265ParsedFrame;
};

} // namespace openmedia::video_parser
