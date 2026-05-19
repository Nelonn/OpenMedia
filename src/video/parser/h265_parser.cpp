#include "h265_parser.hpp"

#include <util/bit_reader.hpp>

#include <cstring>
#include <algorithm>

namespace openmedia::video_parser {

static auto isAnnexB(std::span<const uint8_t> data) noexcept -> bool {
  return data.size() >= 3 && data[0] == 0 && data[1] == 0 &&
         (data[2] == 1 || (data.size() >= 4 && data[2] == 0 && data[3] == 1));
}

static auto isVclNal(int type) noexcept -> bool {
  return type >= 0 && type <= 31;
}

static auto isIrapNal(int type) noexcept -> bool {
  return type >= 16 && type <= 23;
}

static auto isIdrNal(int type) noexcept -> bool {
  return type == 19 || type == 20;
}

static auto readNalSize(std::span<const uint8_t> input, size_t offset, uint8_t nal_length_size) noexcept -> uint32_t {
  uint32_t size = 0;
  for (uint8_t i = 0; i < nal_length_size; ++i) size = (size << 8u) | input[offset + i];
  return size;
}

static auto ceilLog2(uint32_t value) noexcept -> int {
  int bits = 0;
  if (value > 0) --value;
  while (value > 0) {
    ++bits;
    value >>= 1u;
  }
  return bits;
}

static void skipProfileTierLevel(openmedia::BitReader& br, int max_sub_layers_minus1) {
  br.skipBits(2 + 1 + 5 + 32 + 4 + 44 + 8);
  bool sub_layer_profile_present[8] = {};
  bool sub_layer_level_present[8] = {};
  for (int i = 0; i < max_sub_layers_minus1; ++i) {
    sub_layer_profile_present[i] = br.readBit() != 0;
    sub_layer_level_present[i] = br.readBit() != 0;
  }
  if (max_sub_layers_minus1 > 0) {
    for (int i = max_sub_layers_minus1; i < 8; ++i) br.skipBits(2);
  }
  for (int i = 0; i < max_sub_layers_minus1; ++i) {
    if (sub_layer_profile_present[i]) br.skipBits(2 + 1 + 5 + 32 + 4 + 44);
    if (sub_layer_level_present[i]) br.skipBits(8);
  }
}

void H265AccessUnitParser::reset() {
  std::memset(vps_, 0, sizeof(vps_));
  std::memset(sps_, 0, sizeof(sps_));
  std::memset(pps_, 0, sizeof(pps_));
  scanner_.reset();
  current_ = {};
  current_has_vcl_ = false;
  current_parameter_sets_changed_ = false;
  has_vps_ = false;
  has_sps_ = false;
  has_pps_ = false;
  nal_length_size_ = 0;
  previous_poc_ = 0;
  previous_nal_type_ = -1;
  have_previous_slice_ = false;
  nal_unit_type_ = 0;
  slice_pic_order_cnt_lsb_ = 0;
  first_slice_segment_in_pic_flag_ = false;
}

void H265AccessUnitParser::parseExtradata(std::span<const uint8_t> extradata) {
  if (extradata.empty()) return;
  if (extradata.size() >= 23 && extradata[0] == 1) {
    nal_length_size_ = static_cast<uint8_t>((extradata[21] & 0x03u) + 1);
    size_t offset = 23;
    if (offset > extradata.size()) return;
    const uint8_t array_count = extradata[22];
    for (uint8_t array = 0; array < array_count && offset + 3 <= extradata.size(); ++array) {
      ++offset;
      const uint16_t nal_count = static_cast<uint16_t>((extradata[offset] << 8u) | extradata[offset + 1]);
      offset += 2;
      for (uint16_t i = 0; i < nal_count && offset + 2 <= extradata.size(); ++i) {
        const size_t size = (static_cast<size_t>(extradata[offset]) << 8u) | extradata[offset + 1];
        offset += 2;
        if (offset + size > extradata.size()) return;
        parseNal(extradata.subspan(offset, size));
        offset += size;
      }
    }
    return;
  }

  auto normalized = normalizePacket(extradata);
  for (const auto& unit : findNalUnits(normalized)) {
    parseNal({normalized.data() + unit.header, unit.end - unit.header});
  }
}

auto H265AccessUnitParser::parse(std::span<const uint8_t> packet, bool end_of_packet) -> std::vector<H265ParsedFrame> {
  std::vector<H265ParsedFrame> frames;
  if (packet.empty()) return frames;

  auto normalized = normalizePacket(packet);
  for (const auto& unit : findNalUnits(normalized)) {
    const auto nal_data = std::span<const uint8_t>(normalized.data() + unit.header, unit.end - unit.header);
    const size_t slice_header_count = current_.slice_headers.size();
    if (!parseNal(nal_data)) continue;

    const int nal_type = nal_unit_type_;
    const bool is_vcl = isVclNal(nal_type);
    H265SliceHeader pending_slice_header = {};
    const bool added_slice_header = is_vcl && current_.slice_headers.size() > slice_header_count;
    if (startsNewAccessUnit(nal_type) && current_has_vcl_) {
      if (added_slice_header) {
        pending_slice_header = current_.slice_headers.back();
        current_.slice_headers.pop_back();
      }
      frames.push_back(finishCurrentFrame());
      if (added_slice_header) current_.slice_headers.push_back(pending_slice_header);
    }

    const uint32_t output_offset = static_cast<uint32_t>(current_.bitstream.size());
    current_.bitstream.insert(current_.bitstream.end(), normalized.begin() + static_cast<ptrdiff_t>(unit.start), normalized.begin() + static_cast<ptrdiff_t>(unit.end));

    if (nal_type == 32) {
      has_vps_ = true;
      current_parameter_sets_changed_ = true;
    } else if (nal_type == 33) {
      has_sps_ = true;
      current_parameter_sets_changed_ = true;
    } else if (nal_type == 34) {
      has_pps_ = true;
      current_parameter_sets_changed_ = true;
    } else if (is_vcl && has_sps_ && has_pps_) {
      current_.slice_offsets.push_back(output_offset);
      current_.slice_nalus.emplace_back(nal_data.begin(), nal_data.end());
      current_.nal_unit_type = nal_type;
      current_.poc = slice_pic_order_cnt_lsb_;
      current_.is_irap = isIrapNal(nal_type);
      current_.is_reference = nal_type != 0 && nal_type != 2 && nal_type != 4 && nal_type != 6 && nal_type != 8;
      current_has_vcl_ = true;
      previous_poc_ = current_.poc;
      previous_nal_type_ = nal_type;
      have_previous_slice_ = true;
    }
  }

  if (end_of_packet && current_has_vcl_) frames.push_back(finishCurrentFrame());
  return frames;
}

auto H265AccessUnitParser::normalizePacket(std::span<const uint8_t> packet) const -> std::vector<uint8_t> {
  if (packet.empty()) return {};
  if (isAnnexB(packet) || nal_length_size_ == 0) return {packet.begin(), packet.end()};

  std::vector<uint8_t> out;
  out.reserve(packet.size() + 16);
  size_t offset = 0;
  while (offset + nal_length_size_ <= packet.size()) {
    const uint32_t nal_size = readNalSize(packet, offset, nal_length_size_);
    offset += nal_length_size_;
    if (nal_size == 0 || offset + nal_size > packet.size()) break;
    out.insert(out.end(), {0, 0, 1});
    out.insert(out.end(), packet.begin() + static_cast<ptrdiff_t>(offset), packet.begin() + static_cast<ptrdiff_t>(offset + nal_size));
    offset += nal_size;
  }
  if (out.empty()) return {packet.begin(), packet.end()};
  return out;
}

auto H265AccessUnitParser::findNalUnits(std::span<const uint8_t> packet) -> std::vector<NalUnit> {
  struct StartCode { size_t start; size_t size; };
  std::vector<StartCode> starts;
  for (size_t i = 0; i + 3 <= packet.size();) {
    if (packet[i] == 0 && packet[i + 1] == 0 && packet[i + 2] == 1) {
      starts.push_back({i, 3});
      i += 3;
      continue;
    }
    if (i + 4 <= packet.size() && packet[i] == 0 && packet[i + 1] == 0 && packet[i + 2] == 0 && packet[i + 3] == 1) {
      starts.push_back({i, 4});
      i += 4;
      continue;
    }
    ++i;
  }

  std::vector<NalUnit> nals;
  for (size_t i = 0; i < starts.size(); ++i) {
    const size_t start = starts[i].start;
    const size_t header = start + starts[i].size;
    const size_t end = (i + 1 < starts.size()) ? starts[i + 1].start : packet.size();
    if (header + 2 <= end) nals.push_back({start, header, end});
  }
  return nals;
}

auto H265AccessUnitParser::parseScalingListData(openmedia::BitReader& br, H265ScalingListData& sl) -> bool {
  for (int size_id = 0; size_id < 4; ++size_id) {
    for (int matrix_id = 0; matrix_id < (size_id == 3 ? 2 : 6); ++matrix_id) {
      if (!br.readBit()) {
        int pred_matrix_id_delta = static_cast<int>(br.readUE());
        int pred_matrix_id = matrix_id - (pred_matrix_id_delta + 1);
        if (pred_matrix_id < 0) {
          int coef_count = std::min(64, 1 << (4 + (size_id << 1)));
          for (int i = 0; i < coef_count; ++i) {
            if (size_id == 0) sl.scaling_list_4x4[matrix_id][i] = 16;
            else if (size_id == 1) sl.scaling_list_8x8[matrix_id][i] = 16;
            else if (size_id == 2) sl.scaling_list_16x16[matrix_id][i] = 16;
            else if (size_id == 3) sl.scaling_list_32x32[matrix_id][i] = 16;
          }
          if (size_id == 2) sl.scaling_list_dc_coef_16x16[matrix_id] = 16;
          else if (size_id == 3) sl.scaling_list_dc_coef_32x32[matrix_id] = 16;
        } else {
          if (size_id == 0) std::memcpy(sl.scaling_list_4x4[matrix_id], sl.scaling_list_4x4[pred_matrix_id], 16);
          else if (size_id == 1) std::memcpy(sl.scaling_list_8x8[matrix_id], sl.scaling_list_8x8[pred_matrix_id], 64);
          else if (size_id == 2) {
            std::memcpy(sl.scaling_list_16x16[matrix_id], sl.scaling_list_16x16[pred_matrix_id], 64);
            sl.scaling_list_dc_coef_16x16[matrix_id] = sl.scaling_list_dc_coef_16x16[pred_matrix_id];
          } else if (size_id == 3) {
            std::memcpy(sl.scaling_list_32x32[matrix_id], sl.scaling_list_32x32[pred_matrix_id], 64);
            sl.scaling_list_dc_coef_32x32[matrix_id] = sl.scaling_list_dc_coef_32x32[pred_matrix_id];
          }
        }
      } else {
        int next_coef = 8;
        int coef_count = std::min(64, 1 << (4 + (size_id << 1)));
        if (size_id > 1) {
          int dc_coef = br.readSE() + 8;
          if (size_id == 2) sl.scaling_list_dc_coef_16x16[matrix_id] = static_cast<uint8_t>(dc_coef);
          else if (size_id == 3) sl.scaling_list_dc_coef_32x32[matrix_id] = static_cast<uint8_t>(dc_coef);
          next_coef = dc_coef;
        }
        for (int i = 0; i < coef_count; ++i) {
          int delta_coef = br.readSE();
          next_coef = (next_coef + delta_coef + 256) % 256;
          if (size_id == 0) sl.scaling_list_4x4[matrix_id][i] = static_cast<uint8_t>(next_coef);
          else if (size_id == 1) sl.scaling_list_8x8[matrix_id][i] = static_cast<uint8_t>(next_coef);
          else if (size_id == 2) sl.scaling_list_16x16[matrix_id][i] = static_cast<uint8_t>(next_coef);
          else if (size_id == 3) sl.scaling_list_32x32[matrix_id][i] = static_cast<uint8_t>(next_coef);
        }
      }
    }
  }
  return br.ok();
}

auto H265AccessUnitParser::parsePredWeightTable(BitReader& br, const Sps& sps, H265SliceHeader& sh) -> bool {
  sh.pred_weight_table.luma_log2_weight_denom = static_cast<int>(br.readUE());
  if (sps.chroma_format_idc != 0) {
    sh.pred_weight_table.delta_chroma_log2_weight_denom = br.readSE();
  }
  bool luma_weight_l0_flag[15] = {};
  bool chroma_weight_l0_flag[15] = {};
  for (int i = 0; i <= sh.num_ref_idx_l0_active_minus1; ++i) {
    luma_weight_l0_flag[i] = br.readBit() != 0;
  }
  if (sps.chroma_format_idc != 0) {
    for (int i = 0; i <= sh.num_ref_idx_l0_active_minus1; ++i) {
      chroma_weight_l0_flag[i] = br.readBit() != 0;
    }
  }
  for (int i = 0; i <= sh.num_ref_idx_l0_active_minus1; ++i) {
    if (luma_weight_l0_flag[i]) {
      sh.pred_weight_table.delta_luma_weight_l0[i] = br.readSE();
      sh.pred_weight_table.luma_offset_l0[i] = br.readSE();
    }
    if (chroma_weight_l0_flag[i]) {
      for (int j = 0; j < 2; ++j) {
        sh.pred_weight_table.delta_chroma_weight_l0[i][j] = br.readSE();
        sh.pred_weight_table.delta_chroma_offset_l0[i][j] = br.readSE();
      }
    }
  }
  if (sh.slice_type == 0 /* B */) {
    bool luma_weight_l1_flag[15] = {};
    bool chroma_weight_l1_flag[15] = {};
    for (int i = 0; i <= sh.num_ref_idx_l1_active_minus1; ++i) {
      luma_weight_l1_flag[i] = br.readBit() != 0;
    }
    if (sps.chroma_format_idc != 0) {
      for (int i = 0; i <= sh.num_ref_idx_l1_active_minus1; ++i) {
        chroma_weight_l1_flag[i] = br.readBit() != 0;
      }
    }
    for (int i = 0; i <= sh.num_ref_idx_l1_active_minus1; ++i) {
      if (luma_weight_l1_flag[i]) {
        sh.pred_weight_table.delta_luma_weight_l1[i] = br.readSE();
        sh.pred_weight_table.luma_offset_l1[i] = br.readSE();
      }
      if (chroma_weight_l1_flag[i]) {
        for (int j = 0; j < 2; ++j) {
          sh.pred_weight_table.delta_chroma_weight_l1[i][j] = br.readSE();
          sh.pred_weight_table.delta_chroma_offset_l1[i][j] = br.readSE();
        }
      }
    }
  }
  return br.ok();
}

auto H265AccessUnitParser::parseStRefPicSet(openmedia::BitReader& br, H265StRefPicSet& st, int idx, int num_sets, const H265StRefPicSet* sets) -> bool {
  if (idx != 0) {
    st.inter_ref_pic_set_prediction_flag = br.readBit() != 0;
  }

  if (st.inter_ref_pic_set_prediction_flag) {
    int delta_idx_minus1 = 0;
    if (idx == num_sets) {
      delta_idx_minus1 = static_cast<int>(br.readUE());
    }
    int ref_idx = idx - (delta_idx_minus1 + 1);
    if (ref_idx < 0 || ref_idx >= idx) return false;

    const H265StRefPicSet& ref = sets[ref_idx];
    st.delta_rps_sign = static_cast<int>(br.readBit());
    st.abs_delta_rps_minus1 = static_cast<int>(br.readUE());
    int delta_rps = (1 - 2 * st.delta_rps_sign) * (st.abs_delta_rps_minus1 + 1);

    int num_delta_pocs = ref.num_delta_pocs;
    for (int j = 0; j <= num_delta_pocs; ++j) {
      st.used_by_curr_pic_flag[j] = br.readBit() != 0;
      if (!st.used_by_curr_pic_flag[j]) {
        st.use_delta_flag[j] = br.readBit() != 0;
      } else {
        st.use_delta_flag[j] = true;
      }
    }

    // Derive current RPS from reference RPS (8.3.2)
    int i = 0;
    for (int j = ref.num_positive_pics - 1; j >= 0; --j) {
      int d_poc = ref.delta_poc_s1[j] + delta_rps;
      if (d_poc < 0 && st.use_delta_flag[ref.num_negative_pics + j]) {
        st.delta_poc_s0[i] = d_poc;
        st.used_by_curr_pic_s0_flag[i++] = st.used_by_curr_pic_flag[ref.num_negative_pics + j];
      }
    }
    if (delta_rps < 0 && st.use_delta_flag[num_delta_pocs]) {
      st.delta_poc_s0[i] = delta_rps;
      st.used_by_curr_pic_s0_flag[i++] = st.used_by_curr_pic_flag[num_delta_pocs];
    }
    for (int j = 0; j < ref.num_negative_pics; ++j) {
      int d_poc = ref.delta_poc_s0[j] + delta_rps;
      if (d_poc < 0 && st.use_delta_flag[j]) {
        st.delta_poc_s0[i] = d_poc;
        st.used_by_curr_pic_s0_flag[i++] = st.used_by_curr_pic_flag[j];
      }
    }
    st.num_negative_pics = i;

    i = 0;
    for (int j = ref.num_negative_pics - 1; j >= 0; --j) {
      int d_poc = ref.delta_poc_s0[j] + delta_rps;
      if (d_poc > 0 && st.use_delta_flag[j]) {
        st.delta_poc_s1[i] = d_poc;
        st.used_by_curr_pic_s1_flag[i++] = st.used_by_curr_pic_flag[j];
      }
    }
    if (delta_rps > 0 && st.use_delta_flag[num_delta_pocs]) {
      st.delta_poc_s1[i] = delta_rps;
      st.used_by_curr_pic_s1_flag[i++] = st.used_by_curr_pic_flag[num_delta_pocs];
    }
    for (int j = 0; j < ref.num_positive_pics; ++j) {
      int d_poc = ref.delta_poc_s1[j] + delta_rps;
      if (d_poc > 0 && st.use_delta_flag[ref.num_negative_pics + j]) {
        st.delta_poc_s1[i] = d_poc;
        st.used_by_curr_pic_s1_flag[i++] = st.used_by_curr_pic_flag[ref.num_negative_pics + j];
      }
    }
    st.num_positive_pics = i;
    st.num_delta_pocs = st.num_negative_pics + st.num_positive_pics;
  } else {
    st.num_negative_pics = static_cast<int>(br.readUE());
    st.num_positive_pics = static_cast<int>(br.readUE());
    int prev = 0;
    for (int i = 0; i < st.num_negative_pics; ++i) {
      int delta_poc_s0_minus1 = static_cast<int>(br.readUE());
      prev -= (delta_poc_s0_minus1 + 1);
      st.delta_poc_s0[i] = prev;
      st.used_by_curr_pic_s0_flag[i] = br.readBit() != 0;
    }
    prev = 0;
    for (int i = 0; i < st.num_positive_pics; ++i) {
      int delta_poc_s1_minus1 = static_cast<int>(br.readUE());
      prev += (delta_poc_s1_minus1 + 1);
      st.delta_poc_s1[i] = prev;
      st.used_by_curr_pic_s1_flag[i] = br.readBit() != 0;
    }
    st.num_delta_pocs = st.num_negative_pics + st.num_positive_pics;
  }
  return br.ok();
}

auto H265AccessUnitParser::skipHrdParameters(openmedia::BitReader& br, bool common_inf_present_flag, int max_num_sub_layers_minus1) -> bool {
  bool nal_hrd_parameters_present_flag = false;
  bool vcl_hrd_parameters_present_flag = false;
  bool sub_pic_hrd_params_present_flag = false;
  if (common_inf_present_flag) {
    nal_hrd_parameters_present_flag = br.readBit() != 0;
    vcl_hrd_parameters_present_flag = br.readBit() != 0;
    if (nal_hrd_parameters_present_flag || vcl_hrd_parameters_present_flag) {
      sub_pic_hrd_params_present_flag = br.readBit() != 0;
      if (sub_pic_hrd_params_present_flag) {
        br.skipBits(8 + 5 + 1 + 5);
      }
      br.skipBits(4 + 4);
      if (sub_pic_hrd_params_present_flag) {
        br.skipBits(4);
      }
      br.skipBits(5 + 5 + 5);
    }
  }
  for (int i = 0; i <= max_num_sub_layers_minus1; ++i) {
    bool fixed_pic_rate_general_flag = br.readBit() != 0;
    bool fixed_pic_rate_within_cvs_flag = false;
    if (!fixed_pic_rate_general_flag) {
      fixed_pic_rate_within_cvs_flag = br.readBit() != 0;
    } else {
      fixed_pic_rate_within_cvs_flag = true;
    }
    bool low_delay_hrd_flag = false;
    if (fixed_pic_rate_within_cvs_flag) {
      br.readUE();
    } else {
      low_delay_hrd_flag = br.readBit() != 0;
    }
    int cpb_cnt_minus1 = 0;
    if (!low_delay_hrd_flag) {
      cpb_cnt_minus1 = static_cast<int>(br.readUE());
    }
    if (nal_hrd_parameters_present_flag) {
      for (int j = 0; j <= cpb_cnt_minus1; ++j) {
        br.readUE(); br.readUE();
        if (sub_pic_hrd_params_present_flag) {
          br.readUE(); br.readUE();
        }
        br.readBit();
      }
    }
    if (vcl_hrd_parameters_present_flag) {
      for (int j = 0; j <= cpb_cnt_minus1; ++j) {
        br.readUE(); br.readUE();
        if (sub_pic_hrd_params_present_flag) {
          br.readUE(); br.readUE();
        }
        br.readBit();
      }
    }
  }
  return br.ok();
}

auto H265AccessUnitParser::parseVui(openmedia::BitReader& br, Sps& sps) -> bool {
  auto& vui = sps.vui;
  vui.aspect_ratio_info_present_flag = br.readBit() != 0;
  if (vui.aspect_ratio_info_present_flag) {
    vui.aspect_ratio_idc = static_cast<int>(br.readBits(8));
    if (vui.aspect_ratio_idc == 255) {
      vui.sar_width = static_cast<int>(br.readBits(16));
      vui.sar_height = static_cast<int>(br.readBits(16));
    }
  }
  vui.overscan_info_present_flag = br.readBit() != 0;
  if (vui.overscan_info_present_flag) vui.overscan_appropriate_flag = br.readBit() != 0;
  vui.video_signal_type_present_flag = br.readBit() != 0;
  if (vui.video_signal_type_present_flag) {
    vui.video_format = static_cast<int>(br.readBits(3));
    vui.video_full_range_flag = br.readBit() != 0;
    vui.colour_description_present_flag = br.readBit() != 0;
    if (vui.colour_description_present_flag) {
      vui.colour_primaries = static_cast<int>(br.readBits(8));
      vui.transfer_characteristics = static_cast<int>(br.readBits(8));
      vui.matrix_coeffs = static_cast<int>(br.readBits(8));
    }
  }
  vui.chroma_loc_info_present_flag = br.readBit() != 0;
  if (vui.chroma_loc_info_present_flag) {
    vui.chroma_sample_loc_type_top_field = static_cast<int>(br.readUE());
    vui.chroma_sample_loc_type_bottom_field = static_cast<int>(br.readUE());
  }
  vui.neutral_chroma_indication_flag = br.readBit() != 0;
  vui.field_seq_flag = br.readBit() != 0;
  vui.frame_field_info_present_flag = br.readBit() != 0;
  vui.default_display_window_flag = br.readBit() != 0;
  if (vui.default_display_window_flag) {
    vui.def_disp_win_left_offset = static_cast<int>(br.readUE());
    vui.def_disp_win_right_offset = static_cast<int>(br.readUE());
    vui.def_disp_win_top_offset = static_cast<int>(br.readUE());
    vui.def_disp_win_bottom_offset = static_cast<int>(br.readUE());
  }
  vui.vui_timing_info_present_flag = br.readBit() != 0;
  if (vui.vui_timing_info_present_flag) {
    vui.vui_num_units_in_tick = br.readBits(32);
    vui.vui_time_scale = br.readBits(32);
    vui.vui_poc_proportional_to_timing_flag = br.readBit() != 0;
    if (vui.vui_poc_proportional_to_timing_flag) vui.vui_num_ticks_poc_diff_one_minus1 = static_cast<int>(br.readUE());
    vui.vui_hrd_parameters_present_flag = br.readBit() != 0;
    if (vui.vui_hrd_parameters_present_flag) skipHrdParameters(br, true, sps.max_sub_layers_minus1);
  }
  vui.bitstream_restriction_flag = br.readBit() != 0;
  if (vui.bitstream_restriction_flag) {
    vui.tiles_fixed_structure_flag = br.readBit() != 0;
    vui.motion_vectors_over_pic_boundaries_flag = br.readBit() != 0;
    vui.restricted_ref_pic_lists_flag = br.readBit() != 0;
    vui.min_spatial_segmentation_idc = static_cast<int>(br.readUE());
    vui.max_bytes_per_pic_denom = static_cast<int>(br.readUE());
    vui.max_bits_per_min_cu_denom = static_cast<int>(br.readUE());
    vui.log2_max_mv_length_horizontal = static_cast<int>(br.readUE());
    vui.log2_max_mv_length_vertical = static_cast<int>(br.readUE());
  }
  return br.ok();
}

auto H265AccessUnitParser::parseNal(std::span<const uint8_t> nal_data) -> bool {
  if (nal_data.size() < 2) return false;
  if ((nal_data[0] & 0x80u) != 0 || (nal_data[1] & 0x07u) == 0) return false;
  nal_unit_type_ = (nal_data[0] >> 1u) & 0x3fu;
  first_slice_segment_in_pic_flag_ = false;
  slice_pic_order_cnt_lsb_ = 0;

  auto rbsp = openmedia::nalToRbsp(nal_data.subspan(2));
  openmedia::BitReader br(rbsp);

  if (nal_unit_type_ == 32) {
    int vps_id = static_cast<int>(br.readBits(4));
    if (vps_id >= 16) return false;
    auto& vps = vps_[vps_id];
    vps = {};
    vps.valid = true;
    vps.id = vps_id;
    br.readBit();
    vps.max_layers_minus1 = static_cast<int>(br.readBits(6));
    vps.max_sub_layers_minus1 = static_cast<int>(br.readBits(3));
    vps.temporal_id_nesting_flag = br.readBit() != 0;
    return br.ok();
  }

  if (nal_unit_type_ == 33) {
    int vps_id = static_cast<int>(br.readBits(4));
    int max_sub_layers_minus1 = static_cast<int>(br.readBits(3));
    bool temporal_id_nesting_flag = br.readBit() != 0;
    skipProfileTierLevel(br, max_sub_layers_minus1);
    int sps_id = static_cast<int>(br.readUE());
    if (sps_id >= 16) return false;
    auto& sps = sps_[sps_id];
    sps = {};
    sps.valid = true;
    sps.id = sps_id;
    sps.vps_id = vps_id;
    sps.max_sub_layers_minus1 = max_sub_layers_minus1;
    sps.temporal_id_nesting_flag = temporal_id_nesting_flag;
    sps.chroma_format_idc = static_cast<int>(br.readUE());
    if (sps.chroma_format_idc == 3) sps.separate_colour_plane_flag = br.readBit() != 0;
    sps.pic_width_in_luma_samples = static_cast<int>(br.readUE());
    sps.pic_height_in_luma_samples = static_cast<int>(br.readUE());
    sps.conformance_window_flag = br.readBit() != 0;
    if (sps.conformance_window_flag) {
      sps.conf_win_left_offset = static_cast<int>(br.readUE());
      sps.conf_win_right_offset = static_cast<int>(br.readUE());
      sps.conf_win_top_offset = static_cast<int>(br.readUE());
      sps.conf_win_bottom_offset = static_cast<int>(br.readUE());
    }
    sps.bit_depth_luma_minus8 = static_cast<int>(br.readUE());
    sps.bit_depth_chroma_minus8 = static_cast<int>(br.readUE());
    sps.log2_max_pic_order_cnt_lsb_minus4 = static_cast<int>(br.readUE());
    sps.sps_sub_layer_ordering_info_present_flag = br.readBit() != 0;
    for (int i = (sps.sps_sub_layer_ordering_info_present_flag ? 0 : max_sub_layers_minus1); i <= max_sub_layers_minus1; ++i) {
      sps.sps_max_dec_pic_buffering_minus1[i] = static_cast<int>(br.readUE());
      sps.sps_max_num_reorder_pics[i] = static_cast<int>(br.readUE());
      sps.sps_max_latency_increase_plus1[i] = static_cast<int>(br.readUE());
    }
    sps.log2_min_luma_coding_block_size_minus3 = static_cast<int>(br.readUE());
    sps.log2_diff_max_min_luma_coding_block_size = static_cast<int>(br.readUE());
    sps.log2_min_luma_transform_block_size_minus2 = static_cast<int>(br.readUE());
    sps.log2_diff_max_min_luma_transform_block_size = static_cast<int>(br.readUE());
    sps.max_transform_hierarchy_depth_inter = static_cast<int>(br.readUE());
    sps.max_transform_hierarchy_depth_intra = static_cast<int>(br.readUE());
    sps.scaling_list_enabled_flag = br.readBit() != 0;
    if (sps.scaling_list_enabled_flag) {
      sps.sps_scaling_list_data_present_flag = br.readBit() != 0;
      if (sps.sps_scaling_list_data_present_flag) parseScalingListData(br, sps.scaling_list_data);
    }
    sps.amp_enabled_flag = br.readBit() != 0;
    sps.sample_adaptive_offset_enabled_flag = br.readBit() != 0;
    sps.pcm_enabled_flag = br.readBit() != 0;
    if (sps.pcm_enabled_flag) {
      sps.pcm_sample_bit_depth_luma_minus1 = static_cast<int>(br.readBits(4));
      sps.pcm_sample_bit_depth_chroma_minus1 = static_cast<int>(br.readBits(4));
      sps.log2_min_pcm_luma_coding_block_size_minus3 = static_cast<int>(br.readUE());
      sps.log2_diff_max_min_pcm_luma_coding_block_size = static_cast<int>(br.readUE());
      sps.pcm_loop_filter_disabled_flag = br.readBit() != 0;
    }
    sps.num_short_term_ref_pic_sets = static_cast<int>(br.readUE());
    for (int i = 0; i < sps.num_short_term_ref_pic_sets; ++i) {
      parseStRefPicSet(br, sps.st_ref_pic_set[i], i, sps.num_short_term_ref_pic_sets, sps.st_ref_pic_set);
    }
    sps.long_term_ref_pics_present_flag = br.readBit() != 0;
    if (sps.long_term_ref_pics_present_flag) {
      sps.num_long_term_ref_pics_sps = static_cast<int>(br.readUE());
      for (int i = 0; i < sps.num_long_term_ref_pics_sps; ++i) {
        br.readBits(static_cast<uint32_t>(sps.log2_max_pic_order_cnt_lsb_minus4 + 4));
        br.readBit();
      }
    }
    sps.sps_temporal_mvp_enabled_flag = br.readBit() != 0;
    sps.strong_intra_smoothing_enabled_flag = br.readBit() != 0;
    sps.vui_parameters_present_flag = br.readBit() != 0;
    if (sps.vui_parameters_present_flag) parseVui(br, sps);
    return br.ok();
  }

  if (nal_unit_type_ == 34) {
    int pps_id = static_cast<int>(br.readUE());
    int sps_id = static_cast<int>(br.readUE());
    if (pps_id >= 64 || sps_id >= 16) return false;
    auto& pps = pps_[pps_id];
    pps = {};
    pps.valid = true;
    pps.id = pps_id;
    pps.sps_id = sps_id;
    pps.dependent_slice_segments_enabled_flag = br.readBit() != 0;
    pps.output_flag_present_flag = br.readBit() != 0;
    pps.num_extra_slice_header_bits = static_cast<int>(br.readBits(3));
    pps.sign_data_hiding_enabled_flag = br.readBit() != 0;
    pps.cabac_init_present_flag = br.readBit() != 0;
    pps.num_ref_idx_l0_default_active_minus1 = static_cast<int>(br.readUE());
    pps.num_ref_idx_l1_default_active_minus1 = static_cast<int>(br.readUE());
    pps.init_qp_minus26 = br.readSE();
    pps.constrained_intra_pred_flag = br.readBit() != 0;
    pps.transform_skip_enabled_flag = br.readBit() != 0;
    pps.cu_qp_delta_enabled_flag = br.readBit() != 0;
    if (pps.cu_qp_delta_enabled_flag) pps.diff_cu_qp_delta_depth = static_cast<int>(br.readUE());
    pps.pps_cb_qp_offset = br.readSE();
    pps.pps_cr_qp_offset = br.readSE();
    pps.pps_slice_chroma_qp_offsets_present_flag = br.readBit() != 0;
    pps.weighted_pred_flag = br.readBit() != 0;
    pps.weighted_bipred_flag = br.readBit() != 0;
    pps.transquant_bypass_enabled_flag = br.readBit() != 0;
    pps.tiles_enabled_flag = br.readBit() != 0;
    pps.entropy_coding_sync_enabled_flag = br.readBit() != 0;
    if (pps.tiles_enabled_flag) {
      pps.num_tile_columns_minus1 = static_cast<int>(br.readUE());
      pps.num_tile_rows_minus1 = static_cast<int>(br.readUE());
      pps.uniform_spacing_flag = br.readBit() != 0;
      if (!pps.uniform_spacing_flag) {
        for (int i = 0; i < pps.num_tile_columns_minus1; ++i) pps.column_width_minus1[i] = static_cast<int>(br.readUE());
        for (int i = 0; i < pps.num_tile_rows_minus1; ++i) pps.row_height_minus1[i] = static_cast<int>(br.readUE());
      }
      pps.loop_filter_across_tiles_enabled_flag = br.readBit() != 0;
    }
    pps.pps_loop_filter_across_slices_enabled_flag = br.readBit() != 0;
    pps.deblocking_filter_control_present_flag = br.readBit() != 0;
    if (pps.deblocking_filter_control_present_flag) {
      pps.deblocking_filter_override_enabled_flag = br.readBit() != 0;
      pps.pps_deblocking_filter_disabled_flag = br.readBit() != 0;
      if (!pps.pps_deblocking_filter_disabled_flag) {
        pps.pps_beta_offset_div2 = br.readSE();
        pps.pps_tc_offset_div2 = br.readSE();
      }
    }
    pps.pps_scaling_list_data_present_flag = br.readBit() != 0;
    if (pps.pps_scaling_list_data_present_flag) parseScalingListData(br, pps.scaling_list_data);
    pps.lists_modification_present_flag = br.readBit() != 0;
    pps.log2_parallel_merge_level_minus2 = static_cast<int>(br.readUE());
    pps.slice_segment_header_extension_present_flag = br.readBit() != 0;
    pps.pps_extension_present_flag = br.readBit() != 0;
    return br.ok();
  }

  if (!isVclNal(nal_unit_type_)) return true;

  H265SliceHeader sh = {};
  sh.dependent_slice_segment_flag = false;
  sh.slice_segment_address = 0;

  first_slice_segment_in_pic_flag_ = br.readBit() != 0;
  if (isIrapNal(nal_unit_type_)) br.readBit();
  const int pps_id = static_cast<int>(br.readUE());
  sh.pps_id = pps_id;
  if (pps_id < 0 || pps_id >= 64 || !pps_[pps_id].valid) return br.ok();
  const auto& pps = pps_[pps_id];
  const auto& sps = sps_[pps.sps_id];

  if (!first_slice_segment_in_pic_flag_) {
    if (pps.dependent_slice_segments_enabled_flag) {
      sh.dependent_slice_segment_flag = br.readBit() != 0;
    }
    int ctb_log2_size_y = sps.log2_min_luma_coding_block_size_minus3 + 3 + sps.log2_diff_max_min_luma_coding_block_size;
    int ctb_size_y = 1 << ctb_log2_size_y;
    int pic_width_in_ctbs_y = (sps.pic_width_in_luma_samples + ctb_size_y - 1) / ctb_size_y;
    int pic_height_in_ctbs_y = (sps.pic_height_in_luma_samples + ctb_size_y - 1) / ctb_size_y;
    int pic_size_in_ctbs_y = pic_width_in_ctbs_y * pic_height_in_ctbs_y;
    int addr_bits = ceilLog2(pic_size_in_ctbs_y);
    if (addr_bits > 0) sh.slice_segment_address = static_cast<int>(br.readBits(static_cast<uint32_t>(addr_bits)));
  }

  if (!sh.dependent_slice_segment_flag) {
    for (int i = 0; i < pps.num_extra_slice_header_bits; ++i) br.readBit();
    sh.slice_type = static_cast<int>(br.readUE());
    if (pps.output_flag_present_flag) br.readBit();
    if (sps.separate_colour_plane_flag) sh.colour_plane_id = static_cast<int>(br.readBits(2));
    if (!isIdrNal(nal_unit_type_)) {
      sh.slice_pic_order_cnt_lsb = static_cast<int>(br.readBits(static_cast<uint32_t>(sps.log2_max_pic_order_cnt_lsb_minus4 + 4)));
      slice_pic_order_cnt_lsb_ = sh.slice_pic_order_cnt_lsb;
      sh.short_term_ref_pic_set_sps_flag = br.readBit() != 0;
      if (!sh.short_term_ref_pic_set_sps_flag) {
        size_t start_pos = br.bitPosition();
        parseStRefPicSet(br, sh.st_ref_pic_set, sps.num_short_term_ref_pic_sets, sps.num_short_term_ref_pic_sets, sps.st_ref_pic_set);
        sh.st_rps_bits = static_cast<int>(br.bitPosition() - start_pos);
      } else if (sps.num_short_term_ref_pic_sets > 1) {
        sh.short_term_ref_pic_set_idx = static_cast<int>(br.readBits(static_cast<uint32_t>(ceilLog2(sps.num_short_term_ref_pic_sets))));
      }
      if (sps.long_term_ref_pics_present_flag) {
        int num_long_term_sps = 0;
        if (sps.num_long_term_ref_pics_sps > 0) num_long_term_sps = static_cast<int>(br.readUE());
        int num_long_term_pics = static_cast<int>(br.readUE());
        for (int i = 0; i < num_long_term_sps + num_long_term_pics; ++i) {
          if (i < num_long_term_sps) {
            if (sps.num_long_term_ref_pics_sps > 1) br.readBits(static_cast<uint32_t>(ceilLog2(sps.num_long_term_ref_pics_sps)));
          } else {
            br.readBits(static_cast<uint32_t>(sps.log2_max_pic_order_cnt_lsb_minus4 + 4));
            br.readBit();
          }
          if (br.readBit()) br.readUE();
        }
      }
      const auto& st = sh.short_term_ref_pic_set_sps_flag ? sps.st_ref_pic_set[sh.short_term_ref_pic_set_idx] : sh.st_ref_pic_set;
      for (int i = 0; i < st.num_negative_pics; ++i) {
        if (st.used_by_curr_pic_s0_flag[i]) ++sh.num_pic_total_curr;
      }
      for (int i = 0; i < st.num_positive_pics; ++i) {
        if (st.used_by_curr_pic_s1_flag[i]) ++sh.num_pic_total_curr;
      }
      if (sps.sps_temporal_mvp_enabled_flag) sh.slice_temporal_mvp_enabled_flag = br.readBit() != 0;
    }
    if (sps.sample_adaptive_offset_enabled_flag) {
      sh.slice_sao_luma_flag = br.readBit() != 0;
      sh.slice_sao_chroma_flag = br.readBit() != 0;
    }
    if (sh.slice_type == 0 /* B */ || sh.slice_type == 1 /* P */) {
      bool num_ref_idx_active_override_flag = br.readBit() != 0;
      if (num_ref_idx_active_override_flag) {
        sh.num_ref_idx_l0_active_minus1 = static_cast<int>(br.readUE());
        if (sh.slice_type == 0 /* B */) sh.num_ref_idx_l1_active_minus1 = static_cast<int>(br.readUE());
      } else {
        sh.num_ref_idx_l0_active_minus1 = pps.num_ref_idx_l0_default_active_minus1;
        sh.num_ref_idx_l1_active_minus1 = pps.num_ref_idx_l1_default_active_minus1;
      }
      if (pps.lists_modification_present_flag && sh.num_pic_total_curr > 1) {
        // skip ref_pic_lists_modification
        if (br.readBit()) { // ref_pic_list_modification_flag_l0
            for (int i = 0; i <= sh.num_ref_idx_l0_active_minus1; ++i) br.readBits(static_cast<uint32_t>(ceilLog2(sh.num_pic_total_curr))); // list_entry_l0
        }
        if (sh.slice_type == 0 /* B */) {
            if (br.readBit()) { // ref_pic_list_modification_flag_l1
                for (int i = 0; i <= sh.num_ref_idx_l1_active_minus1; ++i) br.readBits(static_cast<uint32_t>(ceilLog2(sh.num_pic_total_curr))); // list_entry_l1
            }
        }
      }
      if (sh.slice_type == 0 /* B */) sh.mvd_l1_zero_flag = br.readBit() != 0;
      if (pps.cabac_init_present_flag) sh.cabac_init_flag = br.readBit() != 0;
      if (sh.slice_temporal_mvp_enabled_flag) {
        sh.collocated_from_l0_flag = true;
        if (sh.slice_type == 0 /* B */) sh.collocated_from_l0_flag = br.readBit() != 0;
        if ((sh.collocated_from_l0_flag && sh.num_ref_idx_l0_active_minus1 > 0) ||
            (!sh.collocated_from_l0_flag && sh.num_ref_idx_l1_active_minus1 > 0)) {
          sh.collocated_ref_idx = static_cast<int>(br.readUE());
        }
      }
      if ((pps.weighted_pred_flag && sh.slice_type == 1 /* P */) ||
          (pps.weighted_bipred_flag && sh.slice_type == 0 /* B */)) {
        parsePredWeightTable(br, sps, sh);
      }
      sh.five_minus_max_num_merge_cand = static_cast<int>(br.readUE());
    }
    sh.slice_qp_delta = br.readSE();
    if (pps.pps_slice_chroma_qp_offsets_present_flag) {
      sh.slice_cb_qp_offset = br.readSE();
      sh.slice_cr_qp_offset = br.readSE();
    }
    if (pps.deblocking_filter_control_present_flag) {
      bool deblocking_filter_override_flag = false;
      if (pps.deblocking_filter_override_enabled_flag) deblocking_filter_override_flag = br.readBit() != 0;
      if (deblocking_filter_override_flag) {
        bool slice_deblocking_filter_disabled_flag = br.readBit() != 0;
        if (!slice_deblocking_filter_disabled_flag) {
          sh.slice_beta_offset_div2 = br.readSE();
          sh.slice_tc_offset_div2 = br.readSE();
        }
      } else {
        sh.slice_beta_offset_div2 = pps.pps_beta_offset_div2;
        sh.slice_tc_offset_div2 = pps.pps_tc_offset_div2;
      }
    }
    if (pps.pps_loop_filter_across_slices_enabled_flag &&
        (sh.slice_sao_luma_flag || sh.slice_sao_chroma_flag || !pps.pps_deblocking_filter_disabled_flag)) {
      sh.slice_loop_filter_across_slices_enabled_flag = br.readBit() != 0;
    }
  }

  sh.header_bit_size = static_cast<uint32_t>(br.bitPosition());
  current_.slice_headers.push_back(sh);
  return br.ok();
}

auto H265AccessUnitParser::startsNewAccessUnit(int nal_type) const -> bool {
  if (!current_has_vcl_) return false;
  if (nal_type == 35 || nal_type == 32 || nal_type == 33 || nal_type == 34) return true;
  if (!isVclNal(nal_type) || !have_previous_slice_) return false;
  if (first_slice_segment_in_pic_flag_) return true;
  if (slice_pic_order_cnt_lsb_ != previous_poc_) return true;
  if (isIrapNal(nal_type) != isIrapNal(previous_nal_type_)) return true;
  return false;
}

auto H265AccessUnitParser::finishCurrentFrame() -> H265ParsedFrame {
  current_.parameter_sets_changed = current_parameter_sets_changed_;
  H265ParsedFrame out = std::move(current_);
  current_ = {};
  current_has_vcl_ = false;
  current_parameter_sets_changed_ = false;
  previous_poc_ = 0;
  previous_nal_type_ = -1;
  have_previous_slice_ = false;
  return out;
}

} // namespace openmedia::video_parser
