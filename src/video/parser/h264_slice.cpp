#include "h264_internal.hpp"

namespace h264 {

static auto moreSliceDataGuard(const PPS& pps, const SPS& sps) -> bool {
  return pps.pic_parameter_set_id >= 0 && pps.seq_parameter_set_id >= 0 &&
         pps.seq_parameter_set_id < 32 && sps.seq_parameter_set_id == pps.seq_parameter_set_id;
}

static void skip_ref_pic_list_modification(BitReader& br, int slice_type) {
  const int normalized = slice_type % 5;
  if (normalized == 2 || normalized == 4) return;
  if (br.readBit()) {
    uint32_t idc = 0;
    do {
      idc = br.readUE();
      if (idc == 0 || idc == 1) br.readUE();
      else if (idc == 2) br.readUE();
    } while (idc != 3 && br.ok());
  }
  if (normalized == 1 && br.readBit()) {
    uint32_t idc = 0;
    do {
      idc = br.readUE();
      if (idc == 0 || idc == 1) br.readUE();
      else if (idc == 2) br.readUE();
    } while (idc != 3 && br.ok());
  }
}

static auto isIntraSlice(int slice_type) -> bool {
  const int normalized = slice_type % 5;
  return normalized == 2 || normalized == 4;
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

static void read_pred_weight_table(BitReader& br, const PPS& pps, const SPS& sps, SliceHeader& sh) {
  const int chroma = sps.chroma_format_idc;
  sh.pwt.luma_log2_weight_denom = static_cast<int>(br.readUE());
  if (chroma != 0) sh.pwt.chroma_log2_weight_denom = static_cast<int>(br.readUE());
  set_pred_weight_defaults(sh);
  const int l0 = sh.num_ref_idx_l0_active_minus1 + 1;
  for (int i = 0; i < l0 && br.ok(); ++i) {
    sh.pwt.luma_weight_l0_flag[i] = static_cast<int>(br.readBit());
    if (sh.pwt.luma_weight_l0_flag[i]) {
      sh.pwt.luma_weight_l0[i] = br.readSE();
      sh.pwt.luma_offset_l0[i] = br.readSE();
    }
    if (chroma != 0) {
      sh.pwt.chroma_weight_l0_flag[i] = static_cast<int>(br.readBit());
      if (sh.pwt.chroma_weight_l0_flag[i]) {
        for (int j = 0; j < 2; ++j) {
          sh.pwt.chroma_weight_l0[i][j] = br.readSE();
          sh.pwt.chroma_offset_l0[i][j] = br.readSE();
        }
      }
    }
  }
  const int normalized = sh.slice_type % 5;
  if (normalized != 1) return;
  const int l1 = sh.num_ref_idx_l1_active_minus1 + 1;
  for (int i = 0; i < l1 && br.ok(); ++i) {
    sh.pwt.luma_weight_l1_flag[i] = static_cast<int>(br.readBit());
    if (sh.pwt.luma_weight_l1_flag[i]) {
      sh.pwt.luma_weight_l1[i] = br.readSE();
      sh.pwt.luma_offset_l1[i] = br.readSE();
    }
    if (chroma != 0) {
      sh.pwt.chroma_weight_l1_flag[i] = static_cast<int>(br.readBit());
      if (sh.pwt.chroma_weight_l1_flag[i]) {
        for (int j = 0; j < 2; ++j) {
          sh.pwt.chroma_weight_l1[i][j] = br.readSE();
          sh.pwt.chroma_offset_l1[i][j] = br.readSE();
        }
      }
    }
  }
}

static auto read_dec_ref_pic_marking(BitReader& br, const NALHeader& nal) -> bool {
  if (nal.type == NAL_UNIT_TYPE_CODED_SLICE_IDR) {
    br.readBit();
    br.readBit();
    return false;
  }
  if (!br.readBit()) return false;
  uint32_t op = 0;
  bool mmco5 = false;
  do {
    op = br.readUE();
    if (op == 1 || op == 3) br.readUE();
    if (op == 2) br.readUE();
    if (op == 3 || op == 6) br.readUE();
    if (op == 4) br.readUE();
    if (op == 5) mmco5 = true;
  } while (op != 0 && br.ok());
  return mmco5;
}

auto read_slice_header(SliceHeader& slice, const NALHeader& nal, const PPS pps_table[256], const SPS sps_table[32], Bitstream& bs) -> bool {
  if (!bs.valid() || bs.remaining() == 0) return false;
  slice = {};
  auto rbsp = openmedia::nalToRbsp({bs.current(), bs.remaining()});
  if (rbsp.empty()) return false;
  BitReader br(rbsp);

  slice.first_mb_in_slice = static_cast<int>(br.readUE());
  slice.slice_type = static_cast<int>(br.readUE());
  slice.pic_parameter_set_id = static_cast<int>(br.readUE());
  if (slice.pic_parameter_set_id < 0 || slice.pic_parameter_set_id >= 256) return false;
  const PPS& pps = pps_table[slice.pic_parameter_set_id];
  if (pps.seq_parameter_set_id < 0 || pps.seq_parameter_set_id >= 32) return false;
  const SPS& sps = sps_table[pps.seq_parameter_set_id];
  if (!moreSliceDataGuard(pps, sps)) return false;

  if (sps.separate_colour_plane_flag) slice.colour_plane_id = static_cast<int>(br.readBits(2));
  slice.frame_num = static_cast<int>(br.readBits(sps.log2_max_frame_num_minus4 + 4));
  if (!sps.frame_mbs_only_flag) {
    slice.field_pic_flag = static_cast<int>(br.readBit());
    if (slice.field_pic_flag) slice.bottom_field_flag = static_cast<int>(br.readBit());
  }
  if (nal.type == NAL_UNIT_TYPE_CODED_SLICE_IDR) slice.idr_pic_id = static_cast<int>(br.readUE());
  if (sps.pic_order_cnt_type == 0) {
    slice.pic_order_cnt_lsb = static_cast<int>(br.readBits(sps.log2_max_pic_order_cnt_lsb_minus4 + 4));
    if (pps.pic_order_present_flag && !slice.field_pic_flag) slice.delta_pic_order_cnt_bottom = br.readSE();
  } else if (sps.pic_order_cnt_type == 1 && !sps.delta_pic_order_always_zero_flag) {
    slice.delta_pic_order_cnt[0] = br.readSE();
    if (pps.pic_order_present_flag && !slice.field_pic_flag) slice.delta_pic_order_cnt[1] = br.readSE();
  }
  if (pps.redundant_pic_cnt_present_flag) slice.redundant_pic_cnt = static_cast<int>(br.readUE());

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
  skip_ref_pic_list_modification(br, slice.slice_type);
  if ((pps.weighted_pred_flag && (normalized == 0 || normalized == 3)) || (pps.weighted_bipred_idc == 1 && normalized == 1)) {
    read_pred_weight_table(br, pps, sps, slice);
  }
  if (nal.idc != NAL_REF_IDC_PRIORITY_DISPOSABLE) slice.mmco5 = read_dec_ref_pic_marking(br, nal);
  if (pps.entropy_coding_mode_flag && !isIntraSlice(slice.slice_type)) slice.cabac_init_idc = static_cast<int>(br.readUE());
  slice.slice_qp_delta = br.readSE();
  if (normalized == 3 || normalized == 4) {
    if (normalized == 3) slice.sp_for_switch_flag = static_cast<int>(br.readBit());
    slice.slice_qs_delta = br.readSE();
  }
  if (pps.deblocking_filter_control_present_flag) {
    slice.disable_deblocking_filter_idc = static_cast<int>(br.readUE());
    if (slice.disable_deblocking_filter_idc != 1) {
      slice.slice_alpha_c0_offset_div2 = br.readSE();
      slice.slice_beta_offset_div2 = br.readSE();
    }
  }
  slice.header_bit_size = static_cast<int>(br.bitPosition());
  bs.finish();
  return br.ok();
}

} // namespace h264
