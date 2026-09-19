#include "h264_internal.hpp"

#include <cstdint>

namespace h264 {

// 7.4.5: memory_management_control_operation and the ref pic list modification
// lists are self-terminating, so they need an explicit cap to stay finite on a
// corrupt stream; MAX_REF_PIC_LIST_MODIFICATIONS in h264_types.hpp is that cap.

enum class SliceParse {
  ok,
  invalid,   // a constraint was violated; unescaping more data would not help
  truncated, // ran out of bits, the caller may unescape a longer prefix
};

static auto ceilLog2(uint32_t value) -> int {
  int bits = 0;
  while (bits < 32 && (1u << bits) < value) ++bits;
  return bits;
}

static auto isIntraSlice(int slice_type) -> bool {
  const int normalized = slice_type % 5;
  return normalized == 2 || normalized == 4;
}

static auto read_ref_pic_list_modification(BitReader& br, SliceHeader& sh) -> bool {
  const int normalized = sh.slice_type % 5;
  for (int list = 0; list < 2; ++list) {
    sh.ref_pic_list_modification_flag[list] = 0;
    sh.num_ref_pic_list_modifications[list] = 0;
    if (list == 0 && (normalized == 2 || normalized == 4)) continue;
    if (list == 1 && normalized != 1) continue;
    if (!br.readBit()) continue;
    sh.ref_pic_list_modification_flag[list] = 1;
    int count = 0;
    for (;;) {
      const uint32_t idc = br.readUE();
      if (idc == 3) break;
      if (idc > 3) return false;
      if (count >= MAX_REF_PIC_LIST_MODIFICATIONS) return false;
      auto& modification = sh.ref_pic_list_modifications[list][count];
      modification = {};
      modification.modification_of_pic_nums_idc = static_cast<int>(idc);
      const uint32_t value = br.readUE();
      if (idc == 2) {
        modification.long_term_pic_num = static_cast<int>(value);
      } else {
        // abs_diff_pic_num_minus1 is at most MaxPicNum - 1, which is below 2^17.
        if (value > (1u << 17)) return false;
        modification.abs_diff_pic_num_minus1 = static_cast<int>(value);
      }
      if (!br.ok()) return false;
      sh.num_ref_pic_list_modifications[list] = ++count;
    }
  }
  return true;
}

static void set_pred_weight_defaults(SliceHeader& sh) {
  const int luma_default = 1 << sh.pwt.luma_log2_weight_denom;
  const int chroma_default = 1 << sh.pwt.chroma_log2_weight_denom;
  for (int i = 0; i < 32; ++i) {
    sh.pwt.luma_weight_l0[i] = luma_default;
    sh.pwt.luma_weight_l1[i] = luma_default;
    for (int j = 0; j < 2; ++j) {
      sh.pwt.chroma_weight_l0[i][j] = chroma_default;
      sh.pwt.chroma_weight_l1[i][j] = chroma_default;
    }
  }
}

static auto inWeightRange(int value) -> bool { return value >= -128 && value <= 127; }

// `count` is num_ref_idx_lX_active_minus1 + 1, already bounded to 32 by the
// caller, which is what keeps these writes inside the fixed-size arrays.
static auto read_weighting_factors(BitReader& br, int count, int chroma_array_type,
                                   int* weight_flag, int* weight, int* offset,
                                   int* chroma_flag, int (*chroma_weight)[2], int (*chroma_offset)[2]) -> bool {
  for (int i = 0; i < count; ++i) {
    weight_flag[i] = static_cast<int>(br.readBit());
    if (weight_flag[i]) {
      weight[i] = br.readSE();
      offset[i] = br.readSE();
      if (!inWeightRange(weight[i]) || !inWeightRange(offset[i])) return false;
    }
    if (chroma_array_type != 0) {
      chroma_flag[i] = static_cast<int>(br.readBit());
      if (chroma_flag[i]) {
        for (int j = 0; j < 2; ++j) {
          chroma_weight[i][j] = br.readSE();
          chroma_offset[i][j] = br.readSE();
          if (!inWeightRange(chroma_weight[i][j]) || !inWeightRange(chroma_offset[i][j])) return false;
        }
      }
    }
    if (!br.ok()) return false;
  }
  return true;
}

static auto read_pred_weight_table(BitReader& br, const SPS& sps, SliceHeader& sh) -> bool {
  const int chroma_array_type = chromaArrayType(sps);
  sh.pwt.luma_log2_weight_denom = static_cast<int>(br.readUE());
  if (sh.pwt.luma_log2_weight_denom < 0 || sh.pwt.luma_log2_weight_denom > 7) return false;
  if (chroma_array_type != 0) {
    sh.pwt.chroma_log2_weight_denom = static_cast<int>(br.readUE());
    if (sh.pwt.chroma_log2_weight_denom < 0 || sh.pwt.chroma_log2_weight_denom > 7) return false;
  }
  set_pred_weight_defaults(sh);
  if (!read_weighting_factors(br, sh.num_ref_idx_l0_active_minus1 + 1, chroma_array_type,
                              sh.pwt.luma_weight_l0_flag, sh.pwt.luma_weight_l0, sh.pwt.luma_offset_l0,
                              sh.pwt.chroma_weight_l0_flag, sh.pwt.chroma_weight_l0, sh.pwt.chroma_offset_l0)) {
    return false;
  }
  if (sh.slice_type % 5 != 1) return true;
  return read_weighting_factors(br, sh.num_ref_idx_l1_active_minus1 + 1, chroma_array_type,
                                sh.pwt.luma_weight_l1_flag, sh.pwt.luma_weight_l1, sh.pwt.luma_offset_l1,
                                sh.pwt.chroma_weight_l1_flag, sh.pwt.chroma_weight_l1, sh.pwt.chroma_offset_l1);
}

static auto read_dec_ref_pic_marking(BitReader& br, const NALHeader& nal, SliceHeader& sh) -> bool {
  sh.mmco5 = 0;
  sh.no_output_of_prior_pics_flag = 0;
  sh.long_term_reference_flag = 0;
  sh.adaptive_ref_pic_marking_mode_flag = 0;
  sh.num_ref_pic_markings = 0;

  if (nal.type == NAL_UNIT_TYPE_CODED_SLICE_IDR) {
    sh.no_output_of_prior_pics_flag = br.readBit() ? 1 : 0;
    sh.long_term_reference_flag = br.readBit() ? 1 : 0;
    return br.ok();
  }

  sh.adaptive_ref_pic_marking_mode_flag = br.readBit() ? 1 : 0;
  if (!sh.adaptive_ref_pic_marking_mode_flag) return br.ok();

  for (int i = 0; i < MAX_REF_PIC_MARKINGS; ++i) {
    const uint32_t op = br.readUE();
    if (op == 0) return br.ok();
    if (op > 6) return false;

    auto& marking = sh.ref_pic_markings[sh.num_ref_pic_markings];
    marking = {};
    marking.operation = static_cast<int>(op);
    if (op == 1 || op == 3) marking.difference_of_pic_nums_minus1 = static_cast<int>(br.readUE());
    if (op == 2) marking.long_term_pic_num = static_cast<int>(br.readUE());
    if (op == 3 || op == 6) marking.long_term_frame_idx = static_cast<int>(br.readUE());
    if (op == 4) marking.max_long_term_frame_idx_plus1 = static_cast<int>(br.readUE());
    if (op == 5) sh.mmco5 = 1;
    if (!br.ok()) return false;
    ++sh.num_ref_pic_markings;
  }
  return false; // ran past the cap without reaching the terminating 0
}

static auto read_slice_header_rbsp(SliceHeader& slice, const NALHeader& nal, const PPS pps_table[256],
                                   const SPS sps_table[32], BitReader& br) -> SliceParse {
  const auto fail = [&br]() { return br.ok() ? SliceParse::invalid : SliceParse::truncated; };

  slice.first_mb_in_slice = static_cast<int>(br.readUE());
  slice.slice_type = static_cast<int>(br.readUE());
  if (slice.slice_type < 0 || slice.slice_type > 9) return fail();
  slice.pic_parameter_set_id = static_cast<int>(br.readUE());
  if (slice.pic_parameter_set_id < 0 || slice.pic_parameter_set_id >= 256) return fail();
  const PPS& pps = pps_table[slice.pic_parameter_set_id];
  if (pps.pic_parameter_set_id != slice.pic_parameter_set_id) return fail();
  if (pps.seq_parameter_set_id < 0 || pps.seq_parameter_set_id >= 32) return fail();
  const SPS& sps = sps_table[pps.seq_parameter_set_id];
  if (sps.seq_parameter_set_id != pps.seq_parameter_set_id) return fail();

  // 7.4.3: first_mb_in_slice must address a macroblock of this picture. Checked
  // against the frame-sized value so field pictures are not rejected; that is
  // still tight enough to keep the value usable as a decode surface offset.
  const int64_t pic_size_in_mbs = static_cast<int64_t>(sps.pic_width_in_mbs_minus1 + 1) *
                                  static_cast<int64_t>(sps.pic_height_in_map_units_minus1 + 1) *
                                  (2 - sps.frame_mbs_only_flag);
  if (slice.first_mb_in_slice < 0 || slice.first_mb_in_slice >= pic_size_in_mbs) return fail();

  if (sps.separate_colour_plane_flag) slice.colour_plane_id = static_cast<int>(br.readBits(2));
  slice.frame_num = static_cast<int>(br.readBits(static_cast<uint32_t>(sps.log2_max_frame_num_minus4 + 4)));
  if (!sps.frame_mbs_only_flag) {
    slice.field_pic_flag = static_cast<int>(br.readBit());
    if (slice.field_pic_flag) slice.bottom_field_flag = static_cast<int>(br.readBit());
  }
  if (nal.type == NAL_UNIT_TYPE_CODED_SLICE_IDR) {
    slice.idr_pic_id = static_cast<int>(br.readUE());
    if (slice.idr_pic_id < 0 || slice.idr_pic_id > 65535) return fail();
  }
  if (sps.pic_order_cnt_type == 0) {
    slice.pic_order_cnt_lsb = static_cast<int>(br.readBits(static_cast<uint32_t>(sps.log2_max_pic_order_cnt_lsb_minus4 + 4)));
    if (pps.pic_order_present_flag && !slice.field_pic_flag) slice.delta_pic_order_cnt_bottom = br.readSE();
  } else if (sps.pic_order_cnt_type == 1 && !sps.delta_pic_order_always_zero_flag) {
    slice.delta_pic_order_cnt[0] = br.readSE();
    if (pps.pic_order_present_flag && !slice.field_pic_flag) slice.delta_pic_order_cnt[1] = br.readSE();
  }
  if (pps.redundant_pic_cnt_present_flag) {
    slice.redundant_pic_cnt = static_cast<int>(br.readUE());
    if (slice.redundant_pic_cnt < 0 || slice.redundant_pic_cnt > 127) return fail();
  }

  const int normalized = slice.slice_type % 5;
  slice.num_ref_idx_l0_active_minus1 = pps.num_ref_idx_l0_active_minus1;
  slice.num_ref_idx_l1_active_minus1 = pps.num_ref_idx_l1_active_minus1;
  set_pred_weight_defaults(slice);
  if (normalized == 1) slice.direct_spatial_mv_pred_flag = static_cast<int>(br.readBit());
  if (normalized == 0 || normalized == 1 || normalized == 3) {
    slice.num_ref_idx_active_override_flag = static_cast<int>(br.readBit());
    if (slice.num_ref_idx_active_override_flag) {
      slice.num_ref_idx_l0_active_minus1 = static_cast<int>(br.readUE());
      if (normalized == 1) slice.num_ref_idx_l1_active_minus1 = static_cast<int>(br.readUE());
    }
  }
  // Keeps the pred_weight_table loops inside the fixed-size weight arrays.
  const int max_ref_idx = slice.field_pic_flag ? 31 : 15;
  if (slice.num_ref_idx_l0_active_minus1 < 0 || slice.num_ref_idx_l0_active_minus1 > max_ref_idx) return fail();
  if (slice.num_ref_idx_l1_active_minus1 < 0 || slice.num_ref_idx_l1_active_minus1 > max_ref_idx) return fail();

  if (!read_ref_pic_list_modification(br, slice)) return fail();
  if ((pps.weighted_pred_flag && (normalized == 0 || normalized == 3)) || (pps.weighted_bipred_idc == 1 && normalized == 1)) {
    if (!read_pred_weight_table(br, sps, slice)) return fail();
  }
  if (nal.idc != NAL_REF_IDC_PRIORITY_DISPOSABLE) {
    if (!read_dec_ref_pic_marking(br, nal, slice)) return fail();
  }
  if (pps.entropy_coding_mode_flag && !isIntraSlice(slice.slice_type)) {
    slice.cabac_init_idc = static_cast<int>(br.readUE());
    if (slice.cabac_init_idc < 0 || slice.cabac_init_idc > 2) return fail();
  }
  slice.slice_qp_delta = br.readSE();
  {
    // SliceQPY = 26 + pic_init_qp_minus26 + slice_qp_delta, in [-QpBdOffsetY, 51].
    const int qp_bd_offset_y = 6 * sps.bit_depth_luma_minus8;
    const int base_qp = 26 + pps.pic_init_qp_minus26;
    if (slice.slice_qp_delta < -qp_bd_offset_y - base_qp || slice.slice_qp_delta > 51 - base_qp) return fail();
  }
  if (normalized == 3 || normalized == 4) {
    if (normalized == 3) slice.sp_for_switch_flag = static_cast<int>(br.readBit());
    slice.slice_qs_delta = br.readSE();
    const int base_qs = 26 + pps.pic_init_qs_minus26;
    if (slice.slice_qs_delta < -base_qs || slice.slice_qs_delta > 51 - base_qs) return fail();
  }
  if (pps.deblocking_filter_control_present_flag) {
    slice.disable_deblocking_filter_idc = static_cast<int>(br.readUE());
    if (slice.disable_deblocking_filter_idc < 0 || slice.disable_deblocking_filter_idc > 2) return fail();
    if (slice.disable_deblocking_filter_idc != 1) {
      slice.slice_alpha_c0_offset_div2 = br.readSE();
      slice.slice_beta_offset_div2 = br.readSE();
      if (slice.slice_alpha_c0_offset_div2 < -6 || slice.slice_alpha_c0_offset_div2 > 6) return fail();
      if (slice.slice_beta_offset_div2 < -6 || slice.slice_beta_offset_div2 > 6) return fail();
    }
  }
  if (pps.num_slice_groups_minus1 > 0 && pps.slice_group_map_type >= 3 && pps.slice_group_map_type <= 5) {
    // Without this, the header bit size -- and every slice data offset derived
    // from it -- would be short by len(slice_group_change_cycle).
    const int64_t pic_size_in_map_units = static_cast<int64_t>(sps.pic_width_in_mbs_minus1 + 1) *
                                          static_cast<int64_t>(sps.pic_height_in_map_units_minus1 + 1);
    const int64_t change_rate = static_cast<int64_t>(pps.slice_group_change_rate_minus1) + 1;
    const int bits = ceilLog2(static_cast<uint32_t>(pic_size_in_map_units / change_rate + 1));
    if (bits > 0) slice.slice_group_change_cycle = static_cast<int>(br.readBits(static_cast<uint32_t>(bits)));
  }
  if (!br.ok()) return SliceParse::truncated;
  slice.header_bit_size = static_cast<int>(br.bitPosition());
  return SliceParse::ok;
}

auto read_slice_header(SliceHeader& slice, const NALHeader& nal, const PPS pps_table[256],
                       const SPS sps_table[32], Bitstream& bs, openmedia::RbspBuffer& scratch) -> bool {
  if (!bs.valid() || bs.remaining() == 0) return false;
  const std::span<const uint8_t> nal_body{bs.current(), bs.remaining()};
  bs.finish();

  // Unescape a prefix instead of the whole NAL: the header is a few hundred
  // bytes at most, while the slice payload behind it can be megabytes. Only a
  // header that genuinely runs past the prefix pays for a second pass.
  for (size_t limit = 256;; limit *= 8) {
    const bool complete = scratch.convert(nal_body, limit);
    if (scratch.rbsp().empty()) return false;
    slice = {};
    BitReader br(scratch.rbsp());
    const SliceParse result = read_slice_header_rbsp(slice, nal, pps_table, sps_table, br);
    if (result == SliceParse::ok) return true;
    if (result == SliceParse::invalid || complete) return false;
  }
}

} // namespace h264
