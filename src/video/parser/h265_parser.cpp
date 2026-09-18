#include "h265_parser.hpp"

#include <util/bit_reader.hpp>

#include <algorithm>
#include <cstring>

namespace openmedia::video_parser {

// Upper bound on bytes held back between parse() calls while waiting for the
// start code that terminates the trailing NAL.
static constexpr size_t MAX_PENDING_BYTES = 8u << 20;

// Entry point offsets are bounded by the tile and CTB row counts; this is a
// generous cap that only exists to keep a corrupt stream finite.
static constexpr int MAX_ENTRY_POINT_OFFSETS = 1 << 16;

// Result of a slice header attempt. `truncated` means the unescaped prefix ran
// out, not that the stream is bad, so the caller may retry with more data.
enum class HevcParse { ok, invalid, truncated };

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

// Sub-layer non-reference pictures are the even VCL types below 16.
static auto isSubLayerNonReference(int type) noexcept -> bool {
  return type < 16 && (type % 2) == 0;
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

static auto skipProfileTierLevel(openmedia::BitReader& br, int max_sub_layers_minus1) -> bool {
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
  return br.ok();
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
  have_previous_slice_ = false;
  nal_unit_type_ = 0;
  slice_pic_order_cnt_lsb_ = 0;
  first_slice_segment_in_pic_flag_ = false;
  temporal_id_ = 0;
  ref_pic_order_cnt_msb_ = 0;
  ref_pic_order_cnt_lsb_ = 0;
  first_picture_ = true;
  pending_.clear();
  work_.clear();
  nal_prefix_size_ = 3;
}

void H265AccessUnitParser::parseExtradata(std::span<const uint8_t> extradata) {
  if (extradata.empty()) return;
  if (extradata.size() >= 23 && extradata[0] == 1) {
    nal_length_size_ = static_cast<uint8_t>((extradata[21] & 0x03u) + 1);
    size_t offset = 23;
    const uint8_t array_count = extradata[22];
    for (uint8_t array = 0; array < array_count && offset + 3 <= extradata.size(); ++array) {
      ++offset;
      const uint16_t nal_count = static_cast<uint16_t>((extradata[offset] << 8u) | extradata[offset + 1]);
      offset += 2;
      for (uint16_t i = 0; i < nal_count && offset + 2 <= extradata.size(); ++i) {
        const size_t size = (static_cast<size_t>(extradata[offset]) << 8u) | extradata[offset + 1];
        offset += 2;
        if (offset + size > extradata.size()) return;
        H265SliceHeader ignored = {};
        bool has_slice = false;
        parseNal(extradata.subspan(offset, size), ignored, has_slice);
        offset += size;
      }
    }
    return;
  }

  std::vector<uint8_t> annex_b;
  appendAnnexB(extradata, annex_b);
  for (const auto& unit : findNalUnits(annex_b)) {
    H265SliceHeader ignored = {};
    bool has_slice = false;
    parseNal({annex_b.data() + unit.header, unit.end - unit.header}, ignored, has_slice);
  }
}

auto H265AccessUnitParser::parse(std::span<const uint8_t> packet, bool end_of_packet) -> std::vector<H265ParsedFrame> {
  std::vector<H265ParsedFrame> frames;
  if (packet.empty() && pending_.empty()) return frames;

  // Carry over the bytes of the NAL that the previous call could not terminate,
  // so that a unit split across packets is parsed once, whole.
  work_.assign(pending_.begin(), pending_.end());
  pending_.clear();
  appendAnnexB(packet, work_);

  auto nals = findNalUnits(work_);
  if (!end_of_packet) {
    const size_t keep_from = nals.empty() ? 0 : nals.back().start;
    if (!nals.empty()) nals.pop_back();
    if (work_.size() - keep_from <= MAX_PENDING_BYTES) {
      pending_.assign(work_.begin() + static_cast<ptrdiff_t>(keep_from), work_.end());
    }
  }

  for (const auto& unit : nals) {
    const auto nal_data = std::span<const uint8_t>(work_.data() + unit.header, unit.end - unit.header);
    nal_prefix_size_ = static_cast<uint32_t>(unit.header - unit.start);

    H265SliceHeader slice = {};
    bool has_slice = false;
    if (!parseNal(nal_data, slice, has_slice)) continue;

    const int nal_type = nal_unit_type_;
    const bool is_vcl = isVclNal(nal_type);
    if (startsNewAccessUnit(nal_type) && current_has_vcl_) frames.push_back(finishCurrentFrame());

    const uint32_t output_offset = static_cast<uint32_t>(current_.bitstream.size());
    current_.bitstream.insert(current_.bitstream.end(), work_.begin() + static_cast<ptrdiff_t>(unit.start), work_.begin() + static_cast<ptrdiff_t>(unit.end));

    if (nal_type == NAL_VPS || nal_type == NAL_SPS || nal_type == NAL_PPS) {
      current_parameter_sets_changed_ = true;
    } else if (is_vcl && has_slice) {
      current_.slice_offsets.push_back(output_offset);
      current_.slice_headers.push_back(slice);
      current_.nal_unit_type = nal_type;
      current_.is_irap = isIrapNal(nal_type);
      current_.is_reference = !isSubLayerNonReference(nal_type);
      if (!current_has_vcl_) {
        // POC is a per-picture value and computePoc advances the prevTid0Pic
        // state, so derive it once, from the first slice of the picture.
        const auto& pps = pps_[slice.pps_id];
        current_.poc = computePoc(sps_[pps.sps_id], slice, nal_type);
      }
      current_has_vcl_ = true;
      have_previous_slice_ = true;
    }
  }

  if (end_of_packet && current_has_vcl_) frames.push_back(finishCurrentFrame());
  return frames;
}

void H265AccessUnitParser::appendAnnexB(std::span<const uint8_t> packet, std::vector<uint8_t>& out) const {
  if (packet.empty()) return;
  if (isAnnexB(packet) || nal_length_size_ == 0) {
    out.insert(out.end(), packet.begin(), packet.end());
    return;
  }

  const size_t out_start = out.size();
  size_t offset = 0;
  while (offset + nal_length_size_ <= packet.size()) {
    const uint32_t nal_size = readNalSize(packet, offset, nal_length_size_);
    offset += nal_length_size_;
    if (nal_size == 0 || offset + nal_size > packet.size()) break;
    out.insert(out.end(), {0, 0, 1});
    out.insert(out.end(), packet.begin() + static_cast<ptrdiff_t>(offset), packet.begin() + static_cast<ptrdiff_t>(offset + nal_size));
    offset += nal_size;
  }
  if (out.size() == out_start) out.insert(out.end(), packet.begin(), packet.end());
}

auto H265AccessUnitParser::findNalUnits(std::span<const uint8_t> packet) -> std::vector<NalUnit> {
  std::vector<size_t> starts;
  // `packet` is one contiguous buffer that already carries whatever was held
  // over from the previous call, so the scanner starts from a clean window.
  // A four byte start code is found as its trailing three bytes; the extra zero
  // stays with the preceding NAL, where it is trailing_zero_8bits.
  scanner_.reset();
  size_t offset = 0;
  while (offset < packet.size()) {
    bool found = false;
    const size_t used = scanner_.next(packet.data() + offset, packet.size() - offset, found);
    offset += used;
    if (found && offset >= 3) starts.push_back(offset - 3);
  }

  std::vector<NalUnit> nals;
  nals.reserve(starts.size());
  for (size_t i = 0; i < starts.size(); ++i) {
    const size_t start = starts[i];
    const size_t header = start + 3;
    const size_t end = (i + 1 < starts.size()) ? starts[i + 1] : packet.size();
    if (header + 2 <= end) nals.push_back({start, header, end});
  }
  return nals;
}

auto H265AccessUnitParser::parseScalingListData(openmedia::BitReader& br, H265ScalingListData& sl) -> bool {
  for (int size_id = 0; size_id < 4; ++size_id) {
    for (int matrix_id = 0; matrix_id < (size_id == 3 ? 2 : 6); ++matrix_id) {
      if (!br.readBit()) {
        const int pred_matrix_id_delta = static_cast<int>(br.readUE());
        if (pred_matrix_id_delta < 0 || pred_matrix_id_delta > matrix_id) return false;
        const int pred_matrix_id = matrix_id - pred_matrix_id_delta;
        const int coef_count = std::min(64, 1 << (4 + (size_id << 1)));
        if (pred_matrix_id_delta == 0) {
          for (int i = 0; i < coef_count; ++i) {
            if (size_id == 0) sl.scaling_list_4x4[matrix_id][i] = 16;
            else if (size_id == 1) sl.scaling_list_8x8[matrix_id][i] = 16;
            else if (size_id == 2) sl.scaling_list_16x16[matrix_id][i] = 16;
            else sl.scaling_list_32x32[matrix_id][i] = 16;
          }
          if (size_id == 2) sl.scaling_list_dc_coef_16x16[matrix_id] = 16;
          else if (size_id == 3) sl.scaling_list_dc_coef_32x32[matrix_id] = 16;
        } else {
          if (size_id == 0) std::memcpy(sl.scaling_list_4x4[matrix_id], sl.scaling_list_4x4[pred_matrix_id], 16);
          else if (size_id == 1) std::memcpy(sl.scaling_list_8x8[matrix_id], sl.scaling_list_8x8[pred_matrix_id], 64);
          else if (size_id == 2) {
            std::memcpy(sl.scaling_list_16x16[matrix_id], sl.scaling_list_16x16[pred_matrix_id], 64);
            sl.scaling_list_dc_coef_16x16[matrix_id] = sl.scaling_list_dc_coef_16x16[pred_matrix_id];
          } else {
            std::memcpy(sl.scaling_list_32x32[matrix_id], sl.scaling_list_32x32[pred_matrix_id], 64);
            sl.scaling_list_dc_coef_32x32[matrix_id] = sl.scaling_list_dc_coef_32x32[pred_matrix_id];
          }
        }
      } else {
        int next_coef = 8;
        const int coef_count = std::min(64, 1 << (4 + (size_id << 1)));
        if (size_id > 1) {
          const int dc_delta = br.readSE();
          if (dc_delta < -7 || dc_delta > 247) return false;
          const int dc_coef = dc_delta + 8;
          if (size_id == 2) sl.scaling_list_dc_coef_16x16[matrix_id] = static_cast<uint8_t>(dc_coef);
          else sl.scaling_list_dc_coef_32x32[matrix_id] = static_cast<uint8_t>(dc_coef);
          next_coef = dc_coef;
        }
        for (int i = 0; i < coef_count; ++i) {
          const int delta_coef = br.readSE();
          if (delta_coef < -128 || delta_coef > 127) return false;
          // & 0xff, not % 256: the C++ remainder of a negative value is
          // negative, which would put out-of-range entries into the matrix
          // handed to the hardware decoder.
          next_coef = (next_coef + delta_coef + 256) & 0xff;
          if (size_id == 0) sl.scaling_list_4x4[matrix_id][i] = static_cast<uint8_t>(next_coef);
          else if (size_id == 1) sl.scaling_list_8x8[matrix_id][i] = static_cast<uint8_t>(next_coef);
          else if (size_id == 2) sl.scaling_list_16x16[matrix_id][i] = static_cast<uint8_t>(next_coef);
          else sl.scaling_list_32x32[matrix_id][i] = static_cast<uint8_t>(next_coef);
        }
      }
      if (!br.ok()) return false;
    }
  }
  return br.ok();
}

auto H265AccessUnitParser::parsePredWeightTable(BitReader& br, const Sps& sps, H265SliceHeader& sh) -> bool {
  // num_ref_idx_lX_active_minus1 has already been bounded to 14 by the caller,
  // which is what keeps these writes inside the fixed-size weight arrays.
  const int l0_count = sh.num_ref_idx_l0_active_minus1 + 1;
  const int l1_count = sh.num_ref_idx_l1_active_minus1 + 1;
  const bool has_chroma = sps.chroma_format_idc != 0 && !sps.separate_colour_plane_flag;

  sh.pred_weight_table.luma_log2_weight_denom = static_cast<int>(br.readUE());
  if (sh.pred_weight_table.luma_log2_weight_denom < 0 || sh.pred_weight_table.luma_log2_weight_denom > 7) return false;
  if (has_chroma) sh.pred_weight_table.delta_chroma_log2_weight_denom = br.readSE();

  bool luma_weight_l0_flag[H265_MAX_REF_IDX_ACTIVE] = {};
  bool chroma_weight_l0_flag[H265_MAX_REF_IDX_ACTIVE] = {};
  for (int i = 0; i < l0_count; ++i) luma_weight_l0_flag[i] = br.readBit() != 0;
  if (has_chroma) {
    for (int i = 0; i < l0_count; ++i) chroma_weight_l0_flag[i] = br.readBit() != 0;
  }
  for (int i = 0; i < l0_count; ++i) {
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
    if (!br.ok()) return false;
  }
  if (sh.slice_type != 0 /* B */) return br.ok();

  bool luma_weight_l1_flag[H265_MAX_REF_IDX_ACTIVE] = {};
  bool chroma_weight_l1_flag[H265_MAX_REF_IDX_ACTIVE] = {};
  for (int i = 0; i < l1_count; ++i) luma_weight_l1_flag[i] = br.readBit() != 0;
  if (has_chroma) {
    for (int i = 0; i < l1_count; ++i) chroma_weight_l1_flag[i] = br.readBit() != 0;
  }
  for (int i = 0; i < l1_count; ++i) {
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
    if (!br.ok()) return false;
  }
  return br.ok();
}

auto H265AccessUnitParser::parseStRefPicSet(openmedia::BitReader& br, H265StRefPicSet& st, int idx, int num_sets, const H265StRefPicSet* sets) -> bool {
  st = {};
  if (idx != 0) {
    st.inter_ref_pic_set_prediction_flag = br.readBit() != 0;
  }

  if (st.inter_ref_pic_set_prediction_flag) {
    int delta_idx_minus1 = 0;
    if (idx == num_sets) {
      delta_idx_minus1 = static_cast<int>(br.readUE());
      if (delta_idx_minus1 < 0 || delta_idx_minus1 >= idx) return false;
    }
    const int ref_idx = idx - (delta_idx_minus1 + 1);
    if (ref_idx < 0 || ref_idx >= idx) return false;

    const H265StRefPicSet& ref = sets[ref_idx];
    st.delta_idx_minus1 = delta_idx_minus1;
    st.num_delta_pocs_of_ref_rps = ref.num_delta_pocs;
    st.delta_rps_sign = static_cast<int>(br.readBit());
    st.abs_delta_rps_minus1 = static_cast<int>(br.readUE());
    if (st.abs_delta_rps_minus1 < 0 || st.abs_delta_rps_minus1 > 32767) return false;
    const int delta_rps = (1 - 2 * st.delta_rps_sign) * (st.abs_delta_rps_minus1 + 1);

    const int num_delta_pocs = ref.num_delta_pocs;
    // The reference set was itself validated, but guard the loop bound anyway
    // since it indexes used_by_curr_pic_flag / use_delta_flag with j <= N.
    if (num_delta_pocs < 0 || num_delta_pocs > H265_MAX_DELTA_POCS) return false;
    for (int j = 0; j <= num_delta_pocs; ++j) {
      st.used_by_curr_pic_flag[j] = br.readBit() != 0;
      st.use_delta_flag[j] = st.used_by_curr_pic_flag[j] ? true : br.readBit() != 0;
    }
    if (!br.ok()) return false;

    // Derive the current RPS from the reference RPS (7.4.8).
    int i = 0;
    const auto push_s0 = [&](int d_poc, bool used) {
      if (i >= H265_MAX_REF_PICS_PER_DIRECTION) return false;
      st.delta_poc_s0[i] = d_poc;
      st.used_by_curr_pic_s0_flag[i] = used;
      ++i;
      return true;
    };
    for (int j = ref.num_positive_pics - 1; j >= 0; --j) {
      const int d_poc = ref.delta_poc_s1[j] + delta_rps;
      if (d_poc < 0 && st.use_delta_flag[ref.num_negative_pics + j]) {
        if (!push_s0(d_poc, st.used_by_curr_pic_flag[ref.num_negative_pics + j])) return false;
      }
    }
    if (delta_rps < 0 && st.use_delta_flag[num_delta_pocs]) {
      if (!push_s0(delta_rps, st.used_by_curr_pic_flag[num_delta_pocs])) return false;
    }
    for (int j = 0; j < ref.num_negative_pics; ++j) {
      const int d_poc = ref.delta_poc_s0[j] + delta_rps;
      if (d_poc < 0 && st.use_delta_flag[j]) {
        if (!push_s0(d_poc, st.used_by_curr_pic_flag[j])) return false;
      }
    }
    st.num_negative_pics = i;

    i = 0;
    const auto push_s1 = [&](int d_poc, bool used) {
      if (i >= H265_MAX_REF_PICS_PER_DIRECTION) return false;
      st.delta_poc_s1[i] = d_poc;
      st.used_by_curr_pic_s1_flag[i] = used;
      ++i;
      return true;
    };
    for (int j = ref.num_negative_pics - 1; j >= 0; --j) {
      const int d_poc = ref.delta_poc_s0[j] + delta_rps;
      if (d_poc > 0 && st.use_delta_flag[j]) {
        if (!push_s1(d_poc, st.used_by_curr_pic_flag[j])) return false;
      }
    }
    if (delta_rps > 0 && st.use_delta_flag[num_delta_pocs]) {
      if (!push_s1(delta_rps, st.used_by_curr_pic_flag[num_delta_pocs])) return false;
    }
    for (int j = 0; j < ref.num_positive_pics; ++j) {
      const int d_poc = ref.delta_poc_s1[j] + delta_rps;
      if (d_poc > 0 && st.use_delta_flag[ref.num_negative_pics + j]) {
        if (!push_s1(d_poc, st.used_by_curr_pic_flag[ref.num_negative_pics + j])) return false;
      }
    }
    st.num_positive_pics = i;
    st.num_delta_pocs = st.num_negative_pics + st.num_positive_pics;
    return br.ok();
  }

  st.num_negative_pics = static_cast<int>(br.readUE());
  st.num_positive_pics = static_cast<int>(br.readUE());
  // Reject rather than clamp: these directly bound the writes below.
  if (st.num_negative_pics < 0 || st.num_negative_pics > H265_MAX_REF_PICS_PER_DIRECTION) return false;
  if (st.num_positive_pics < 0 || st.num_positive_pics > H265_MAX_REF_PICS_PER_DIRECTION) return false;
  int prev = 0;
  for (int i = 0; i < st.num_negative_pics; ++i) {
    const int delta_poc_s0_minus1 = static_cast<int>(br.readUE());
    if (delta_poc_s0_minus1 < 0 || delta_poc_s0_minus1 > 32767) return false;
    prev -= (delta_poc_s0_minus1 + 1);
    st.delta_poc_s0[i] = prev;
    st.used_by_curr_pic_s0_flag[i] = br.readBit() != 0;
  }
  prev = 0;
  for (int i = 0; i < st.num_positive_pics; ++i) {
    const int delta_poc_s1_minus1 = static_cast<int>(br.readUE());
    if (delta_poc_s1_minus1 < 0 || delta_poc_s1_minus1 > 32767) return false;
    prev += (delta_poc_s1_minus1 + 1);
    st.delta_poc_s1[i] = prev;
    st.used_by_curr_pic_s1_flag[i] = br.readBit() != 0;
  }
  st.num_delta_pocs = st.num_negative_pics + st.num_positive_pics;
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
    const bool fixed_pic_rate_general_flag = br.readBit() != 0;
    bool fixed_pic_rate_within_cvs_flag = true;
    if (!fixed_pic_rate_general_flag) fixed_pic_rate_within_cvs_flag = br.readBit() != 0;
    bool low_delay_hrd_flag = false;
    if (fixed_pic_rate_within_cvs_flag) {
      br.readUE();
    } else {
      low_delay_hrd_flag = br.readBit() != 0;
    }
    int cpb_cnt_minus1 = 0;
    if (!low_delay_hrd_flag) {
      cpb_cnt_minus1 = static_cast<int>(br.readUE());
      // Reject rather than clamp so the loops below stay in step with the stream.
      if (cpb_cnt_minus1 < 0 || cpb_cnt_minus1 > 31) return false;
    }
    for (int pass = 0; pass < 2; ++pass) {
      const bool present = pass == 0 ? nal_hrd_parameters_present_flag : vcl_hrd_parameters_present_flag;
      if (!present) continue;
      for (int j = 0; j <= cpb_cnt_minus1; ++j) {
        br.readUE();
        br.readUE();
        if (sub_pic_hrd_params_present_flag) {
          br.readUE();
          br.readUE();
        }
        br.readBit();
      }
    }
    if (!br.ok()) return false;
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
    if (vui.chroma_sample_loc_type_top_field < 0 || vui.chroma_sample_loc_type_top_field > 5) return false;
    if (vui.chroma_sample_loc_type_bottom_field < 0 || vui.chroma_sample_loc_type_bottom_field > 5) return false;
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
    if (vui.vui_hrd_parameters_present_flag && !skipHrdParameters(br, true, sps.max_sub_layers_minus1)) return false;
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

auto H265AccessUnitParser::parseVps(openmedia::BitReader& br) -> bool {
  const int vps_id = static_cast<int>(br.readBits(4));
  if (vps_id < 0 || vps_id >= 16) return false;
  auto& vps = vps_[vps_id];
  vps = {};
  vps.valid = true;
  vps.id = vps_id;
  br.readBit();
  vps.max_layers_minus1 = static_cast<int>(br.readBits(6));
  vps.max_sub_layers_minus1 = static_cast<int>(br.readBits(3));
  vps.temporal_id_nesting_flag = br.readBit() != 0;
  if (!br.ok()) return false;
  has_vps_ = true;
  return true;
}

auto H265AccessUnitParser::parseSps(openmedia::BitReader& br) -> bool {
  const int vps_id = static_cast<int>(br.readBits(4));
  const int max_sub_layers_minus1 = static_cast<int>(br.readBits(3));
  const bool temporal_id_nesting_flag = br.readBit() != 0;
  if (!skipProfileTierLevel(br, max_sub_layers_minus1)) return false;
  const int sps_id = static_cast<int>(br.readUE());
  // readUE returns a 32 bit value, so a lower bound check is what keeps a huge
  // id from wrapping to a negative array index.
  if (sps_id < 0 || sps_id >= 16) return false;

  Sps sps = {};
  sps.valid = true;
  sps.id = sps_id;
  sps.vps_id = vps_id;
  sps.max_sub_layers_minus1 = max_sub_layers_minus1;
  sps.temporal_id_nesting_flag = temporal_id_nesting_flag;
  sps.chroma_format_idc = static_cast<int>(br.readUE());
  if (sps.chroma_format_idc < 0 || sps.chroma_format_idc > 3) return false;
  if (sps.chroma_format_idc == 3) sps.separate_colour_plane_flag = br.readBit() != 0;
  sps.pic_width_in_luma_samples = static_cast<int>(br.readUE());
  sps.pic_height_in_luma_samples = static_cast<int>(br.readUE());
  if (sps.pic_width_in_luma_samples <= 0 || sps.pic_width_in_luma_samples > 65535) return false;
  if (sps.pic_height_in_luma_samples <= 0 || sps.pic_height_in_luma_samples > 65535) return false;
  sps.conformance_window_flag = br.readBit() != 0;
  if (sps.conformance_window_flag) {
    sps.conf_win_left_offset = static_cast<int>(br.readUE());
    sps.conf_win_right_offset = static_cast<int>(br.readUE());
    sps.conf_win_top_offset = static_cast<int>(br.readUE());
    sps.conf_win_bottom_offset = static_cast<int>(br.readUE());
    // The cropping window must leave a non-empty picture; otherwise consumers
    // derive a negative display size from it.
    const int sub_width = sps.chroma_format_idc == 1 || sps.chroma_format_idc == 2 ? 2 : 1;
    const int sub_height = sps.chroma_format_idc == 1 ? 2 : 1;
    if (sps.conf_win_left_offset < 0 || sps.conf_win_right_offset < 0) return false;
    if (sps.conf_win_top_offset < 0 || sps.conf_win_bottom_offset < 0) return false;
    if (static_cast<int64_t>(sps.conf_win_left_offset + sps.conf_win_right_offset) * sub_width >= sps.pic_width_in_luma_samples) return false;
    if (static_cast<int64_t>(sps.conf_win_top_offset + sps.conf_win_bottom_offset) * sub_height >= sps.pic_height_in_luma_samples) return false;
  }
  sps.bit_depth_luma_minus8 = static_cast<int>(br.readUE());
  sps.bit_depth_chroma_minus8 = static_cast<int>(br.readUE());
  if (sps.bit_depth_luma_minus8 < 0 || sps.bit_depth_luma_minus8 > 8) return false;
  if (sps.bit_depth_chroma_minus8 < 0 || sps.bit_depth_chroma_minus8 > 8) return false;
  sps.log2_max_pic_order_cnt_lsb_minus4 = static_cast<int>(br.readUE());
  if (sps.log2_max_pic_order_cnt_lsb_minus4 < 0 || sps.log2_max_pic_order_cnt_lsb_minus4 > 12) return false;
  sps.sps_sub_layer_ordering_info_present_flag = br.readBit() != 0;
  for (int i = (sps.sps_sub_layer_ordering_info_present_flag ? 0 : max_sub_layers_minus1); i <= max_sub_layers_minus1; ++i) {
    sps.sps_max_dec_pic_buffering_minus1[i] = static_cast<int>(br.readUE());
    sps.sps_max_num_reorder_pics[i] = static_cast<int>(br.readUE());
    sps.sps_max_latency_increase_plus1[i] = static_cast<int>(br.readUE());
    if (sps.sps_max_dec_pic_buffering_minus1[i] < 0 || sps.sps_max_dec_pic_buffering_minus1[i] > 15) return false;
  }
  sps.log2_min_luma_coding_block_size_minus3 = static_cast<int>(br.readUE());
  sps.log2_diff_max_min_luma_coding_block_size = static_cast<int>(br.readUE());
  sps.log2_min_luma_transform_block_size_minus2 = static_cast<int>(br.readUE());
  sps.log2_diff_max_min_luma_transform_block_size = static_cast<int>(br.readUE());
  // CtbLog2SizeY is used as a shift when deriving the slice segment address.
  if (sps.log2_min_luma_coding_block_size_minus3 < 0 || sps.log2_min_luma_coding_block_size_minus3 > 3) return false;
  if (sps.log2_diff_max_min_luma_coding_block_size < 0 || sps.log2_diff_max_min_luma_coding_block_size > 3) return false;
  if (sps.log2_min_luma_coding_block_size_minus3 + 3 + sps.log2_diff_max_min_luma_coding_block_size > 6) return false;
  if (sps.log2_min_luma_transform_block_size_minus2 < 0 || sps.log2_min_luma_transform_block_size_minus2 > 3) return false;
  if (sps.log2_diff_max_min_luma_transform_block_size < 0 || sps.log2_diff_max_min_luma_transform_block_size > 3) return false;
  sps.max_transform_hierarchy_depth_inter = static_cast<int>(br.readUE());
  sps.max_transform_hierarchy_depth_intra = static_cast<int>(br.readUE());
  {
    // 7.4.3.2.1: bounded by CtbLog2SizeY - MinTbLog2SizeY.
    const int max_depth = sps.log2_min_luma_coding_block_size_minus3 + 3 + sps.log2_diff_max_min_luma_coding_block_size -
                          (sps.log2_min_luma_transform_block_size_minus2 + 2);
    if (sps.max_transform_hierarchy_depth_inter < 0 || sps.max_transform_hierarchy_depth_inter > max_depth) return false;
    if (sps.max_transform_hierarchy_depth_intra < 0 || sps.max_transform_hierarchy_depth_intra > max_depth) return false;
  }
  sps.scaling_list_enabled_flag = br.readBit() != 0;
  if (sps.scaling_list_enabled_flag) {
    sps.sps_scaling_list_data_present_flag = br.readBit() != 0;
    if (sps.sps_scaling_list_data_present_flag && !parseScalingListData(br, sps.scaling_list_data)) return false;
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
  if (sps.num_short_term_ref_pic_sets < 0 || sps.num_short_term_ref_pic_sets > H265_MAX_SHORT_TERM_REF_PIC_SETS) return false;
  for (int i = 0; i < sps.num_short_term_ref_pic_sets; ++i) {
    if (!parseStRefPicSet(br, sps.st_ref_pic_set[i], i, sps.num_short_term_ref_pic_sets, sps.st_ref_pic_set)) return false;
  }
  sps.long_term_ref_pics_present_flag = br.readBit() != 0;
  if (sps.long_term_ref_pics_present_flag) {
    sps.num_long_term_ref_pics_sps = static_cast<int>(br.readUE());
    if (sps.num_long_term_ref_pics_sps < 0 || sps.num_long_term_ref_pics_sps > H265_MAX_LONG_TERM_REF_PICS_SPS) return false;
    for (int i = 0; i < sps.num_long_term_ref_pics_sps; ++i) {
      sps.lt_ref_pic_poc_lsb_sps[i] = static_cast<int>(br.readBits(static_cast<uint32_t>(sps.log2_max_pic_order_cnt_lsb_minus4 + 4)));
      sps.used_by_curr_pic_lt_sps_flag[i] = br.readBit() != 0;
    }
  }
  sps.sps_temporal_mvp_enabled_flag = br.readBit() != 0;
  sps.strong_intra_smoothing_enabled_flag = br.readBit() != 0;
  sps.vui_parameters_present_flag = br.readBit() != 0;
  if (sps.vui_parameters_present_flag && !parseVui(br, sps)) return false;
  if (!br.ok()) return false;
  sps_[sps_id] = sps;
  has_sps_ = true;
  return true;
}

auto H265AccessUnitParser::parsePps(openmedia::BitReader& br) -> bool {
  const int pps_id = static_cast<int>(br.readUE());
  const int sps_id = static_cast<int>(br.readUE());
  if (pps_id < 0 || pps_id >= 64) return false;
  if (sps_id < 0 || sps_id >= 16) return false;
  if (!sps_[sps_id].valid) return false;
  const Sps& sps = sps_[sps_id];

  Pps pps = {};
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
  // Bounds the pred_weight_table loops in the slice header.
  if (pps.num_ref_idx_l0_default_active_minus1 < 0 || pps.num_ref_idx_l0_default_active_minus1 >= H265_MAX_REF_IDX_ACTIVE) return false;
  if (pps.num_ref_idx_l1_default_active_minus1 < 0 || pps.num_ref_idx_l1_default_active_minus1 >= H265_MAX_REF_IDX_ACTIVE) return false;
  pps.init_qp_minus26 = br.readSE();
  if (pps.init_qp_minus26 < -(26 + 6 * sps.bit_depth_luma_minus8) || pps.init_qp_minus26 > 25) return false;
  pps.constrained_intra_pred_flag = br.readBit() != 0;
  pps.transform_skip_enabled_flag = br.readBit() != 0;
  pps.cu_qp_delta_enabled_flag = br.readBit() != 0;
  if (pps.cu_qp_delta_enabled_flag) {
    pps.diff_cu_qp_delta_depth = static_cast<int>(br.readUE());
    if (pps.diff_cu_qp_delta_depth < 0 || pps.diff_cu_qp_delta_depth > 3) return false;
  }
  pps.pps_cb_qp_offset = br.readSE();
  pps.pps_cr_qp_offset = br.readSE();
  if (pps.pps_cb_qp_offset < -12 || pps.pps_cb_qp_offset > 12) return false;
  if (pps.pps_cr_qp_offset < -12 || pps.pps_cr_qp_offset > 12) return false;
  pps.pps_slice_chroma_qp_offsets_present_flag = br.readBit() != 0;
  pps.weighted_pred_flag = br.readBit() != 0;
  pps.weighted_bipred_flag = br.readBit() != 0;
  pps.transquant_bypass_enabled_flag = br.readBit() != 0;
  pps.tiles_enabled_flag = br.readBit() != 0;
  pps.entropy_coding_sync_enabled_flag = br.readBit() != 0;
  if (pps.tiles_enabled_flag) {
    pps.num_tile_columns_minus1 = static_cast<int>(br.readUE());
    pps.num_tile_rows_minus1 = static_cast<int>(br.readUE());
    // Reject rather than clamp: these bound the writes below.
    if (pps.num_tile_columns_minus1 < 0 || pps.num_tile_columns_minus1 >= H265_MAX_TILE_COLUMNS) return false;
    if (pps.num_tile_rows_minus1 < 0 || pps.num_tile_rows_minus1 >= H265_MAX_TILE_ROWS) return false;
    pps.uniform_spacing_flag = br.readBit() != 0;
    if (!pps.uniform_spacing_flag) {
      // The stream writes down every tile but the last, and a decoder is given all
      // of them.
      const int ctb_log2_size_y = sps.log2_min_luma_coding_block_size_minus3 + 3 +
                                  sps.log2_diff_max_min_luma_coding_block_size;
      const int ctb_size_y = 1 << ctb_log2_size_y;
      int columns_left = (sps.pic_width_in_luma_samples + ctb_size_y - 1) / ctb_size_y - 1;
      for (int i = 0; i < pps.num_tile_columns_minus1; ++i) {
        pps.column_width_minus1[i] = static_cast<int>(br.readUE());
        if (pps.column_width_minus1[i] < 0 || pps.column_width_minus1[i] >= columns_left) return false;
        columns_left -= pps.column_width_minus1[i] + 1;
      }
      pps.column_width_minus1[pps.num_tile_columns_minus1] = columns_left;
      int rows_left = (sps.pic_height_in_luma_samples + ctb_size_y - 1) / ctb_size_y - 1;
      for (int i = 0; i < pps.num_tile_rows_minus1; ++i) {
        pps.row_height_minus1[i] = static_cast<int>(br.readUE());
        if (pps.row_height_minus1[i] < 0 || pps.row_height_minus1[i] >= rows_left) return false;
        rows_left -= pps.row_height_minus1[i] + 1;
      }
      pps.row_height_minus1[pps.num_tile_rows_minus1] = rows_left;
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
      if (pps.pps_beta_offset_div2 < -6 || pps.pps_beta_offset_div2 > 6) return false;
      if (pps.pps_tc_offset_div2 < -6 || pps.pps_tc_offset_div2 > 6) return false;
    }
  }
  pps.pps_scaling_list_data_present_flag = br.readBit() != 0;
  if (pps.pps_scaling_list_data_present_flag && !parseScalingListData(br, pps.scaling_list_data)) return false;
  pps.lists_modification_present_flag = br.readBit() != 0;
  pps.log2_parallel_merge_level_minus2 = static_cast<int>(br.readUE());
  if (pps.log2_parallel_merge_level_minus2 < 0 || pps.log2_parallel_merge_level_minus2 > 4) return false;
  pps.slice_segment_header_extension_present_flag = br.readBit() != 0;
  pps.pps_extension_present_flag = br.readBit() != 0;
  if (!br.ok()) return false;
  pps_[pps_id] = pps;
  has_pps_ = true;
  return true;
}

auto H265AccessUnitParser::parseSliceHeader(openmedia::BitReader& br, H265SliceHeader& sh) -> int {
  const auto fail = [&br]() { return static_cast<int>(br.ok() ? HevcParse::invalid : HevcParse::truncated); };

  first_slice_segment_in_pic_flag_ = br.readBit() != 0;
  if (isIrapNal(nal_unit_type_)) br.readBit(); // no_output_of_prior_pics_flag
  const int pps_id = static_cast<int>(br.readUE());
  if (pps_id < 0 || pps_id >= 64 || !pps_[pps_id].valid) return fail();
  sh.pps_id = pps_id;
  const auto& pps = pps_[pps_id];
  if (pps.sps_id < 0 || pps.sps_id >= 16 || !sps_[pps.sps_id].valid) return fail();
  const auto& sps = sps_[pps.sps_id];

  const int ctb_log2_size_y = sps.log2_min_luma_coding_block_size_minus3 + 3 + sps.log2_diff_max_min_luma_coding_block_size;
  const int ctb_size_y = 1 << ctb_log2_size_y;
  const int pic_width_in_ctbs_y = (sps.pic_width_in_luma_samples + ctb_size_y - 1) / ctb_size_y;
  const int pic_height_in_ctbs_y = (sps.pic_height_in_luma_samples + ctb_size_y - 1) / ctb_size_y;
  const int pic_size_in_ctbs_y = pic_width_in_ctbs_y * pic_height_in_ctbs_y;

  if (!first_slice_segment_in_pic_flag_) {
    if (pps.dependent_slice_segments_enabled_flag) {
      sh.dependent_slice_segment_flag = br.readBit() != 0;
    }
    const int addr_bits = ceilLog2(static_cast<uint32_t>(pic_size_in_ctbs_y));
    if (addr_bits > 0) sh.slice_segment_address = static_cast<int>(br.readBits(static_cast<uint32_t>(addr_bits)));
    if (sh.slice_segment_address < 0 || sh.slice_segment_address >= pic_size_in_ctbs_y) return fail();
  }

  if (!sh.dependent_slice_segment_flag) {
    for (int i = 0; i < pps.num_extra_slice_header_bits; ++i) br.readBit();
    sh.slice_type = static_cast<int>(br.readUE());
    if (sh.slice_type < 0 || sh.slice_type > 2) return fail();
    if (pps.output_flag_present_flag) br.readBit();
    if (sps.separate_colour_plane_flag) sh.colour_plane_id = static_cast<int>(br.readBits(2));
    if (!isIdrNal(nal_unit_type_)) {
      sh.slice_pic_order_cnt_lsb = static_cast<int>(br.readBits(static_cast<uint32_t>(sps.log2_max_pic_order_cnt_lsb_minus4 + 4)));
      slice_pic_order_cnt_lsb_ = sh.slice_pic_order_cnt_lsb;
      sh.short_term_ref_pic_set_sps_flag = br.readBit() != 0;
      if (!sh.short_term_ref_pic_set_sps_flag) {
        const size_t start_pos = br.bitPosition();
        if (!parseStRefPicSet(br, sh.st_ref_pic_set, sps.num_short_term_ref_pic_sets, sps.num_short_term_ref_pic_sets, sps.st_ref_pic_set)) return fail();
        sh.st_rps_bits = static_cast<int>(br.bitPosition() - start_pos);
      } else if (sps.num_short_term_ref_pic_sets > 1) {
        sh.short_term_ref_pic_set_idx = static_cast<int>(br.readBits(static_cast<uint32_t>(ceilLog2(static_cast<uint32_t>(sps.num_short_term_ref_pic_sets)))));
        // ceilLog2 rounds up, so the field can address past the last set.
        if (sh.short_term_ref_pic_set_idx < 0 || sh.short_term_ref_pic_set_idx >= sps.num_short_term_ref_pic_sets) return fail();
      }
      if (sps.long_term_ref_pics_present_flag) {
        int num_long_term_sps = 0;
        if (sps.num_long_term_ref_pics_sps > 0) {
          num_long_term_sps = static_cast<int>(br.readUE());
          if (num_long_term_sps < 0 || num_long_term_sps > sps.num_long_term_ref_pics_sps) return fail();
        }
        const int num_long_term_pics = static_cast<int>(br.readUE());
        if (num_long_term_pics < 0 || num_long_term_pics > H265_MAX_REF_PICS_PER_DIRECTION) return fail();
        sh.num_long_term_refs = num_long_term_sps + num_long_term_pics;
        for (int i = 0; i < sh.num_long_term_refs; ++i) {
          H265LongTermRef& lt = sh.long_term_ref[i];
          if (i < num_long_term_sps) {
            int lt_idx_sps = 0;
            if (sps.num_long_term_ref_pics_sps > 1) {
              lt_idx_sps = static_cast<int>(br.readBits(static_cast<uint32_t>(ceilLog2(static_cast<uint32_t>(sps.num_long_term_ref_pics_sps)))));
              if (lt_idx_sps < 0 || lt_idx_sps >= sps.num_long_term_ref_pics_sps) return fail();
            }
            lt.poc_lsb_lt = sps.lt_ref_pic_poc_lsb_sps[lt_idx_sps];
            lt.used_by_curr_pic_lt_flag = sps.used_by_curr_pic_lt_sps_flag[lt_idx_sps];
          } else {
            lt.poc_lsb_lt = static_cast<int>(br.readBits(static_cast<uint32_t>(sps.log2_max_pic_order_cnt_lsb_minus4 + 4)));
            lt.used_by_curr_pic_lt_flag = br.readBit() != 0;
          }
          if (lt.used_by_curr_pic_lt_flag) ++sh.num_pic_total_curr;
          lt.delta_poc_msb_present_flag = br.readBit() != 0;
          if (lt.delta_poc_msb_present_flag) {
            const int delta = static_cast<int>(br.readUE());
            if (delta < 0 || delta > (1 << 16)) return fail();
            // 7.4.7.1: the field is a delta against the previous entry, except at
            // the first entry of each of the two runs.
            const bool run_start = i == 0 || i == num_long_term_sps;
            lt.delta_poc_msb_cycle_lt = run_start ? delta : delta + sh.long_term_ref[i - 1].delta_poc_msb_cycle_lt;
          }
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
      if (sps.chroma_format_idc != 0 && !sps.separate_colour_plane_flag) sh.slice_sao_chroma_flag = br.readBit() != 0;
    }
    if (sh.slice_type == 0 /* B */ || sh.slice_type == 1 /* P */) {
      const bool num_ref_idx_active_override_flag = br.readBit() != 0;
      if (num_ref_idx_active_override_flag) {
        sh.num_ref_idx_l0_active_minus1 = static_cast<int>(br.readUE());
        if (sh.slice_type == 0 /* B */) sh.num_ref_idx_l1_active_minus1 = static_cast<int>(br.readUE());
      } else {
        sh.num_ref_idx_l0_active_minus1 = pps.num_ref_idx_l0_default_active_minus1;
        sh.num_ref_idx_l1_active_minus1 = pps.num_ref_idx_l1_default_active_minus1;
      }
      // Keeps the pred_weight_table loops inside the fixed-size weight arrays.
      if (sh.num_ref_idx_l0_active_minus1 < 0 || sh.num_ref_idx_l0_active_minus1 >= H265_MAX_REF_IDX_ACTIVE) return fail();
      if (sh.num_ref_idx_l1_active_minus1 < 0 || sh.num_ref_idx_l1_active_minus1 >= H265_MAX_REF_IDX_ACTIVE) return fail();

      if (pps.lists_modification_present_flag && sh.num_pic_total_curr > 1) {
        const auto entry_bits = static_cast<uint32_t>(ceilLog2(static_cast<uint32_t>(sh.num_pic_total_curr)));
        if (br.readBit()) { // ref_pic_list_modification_flag_l0
          for (int i = 0; i <= sh.num_ref_idx_l0_active_minus1; ++i) br.readBits(entry_bits);
        }
        if (sh.slice_type == 0 /* B */ && br.readBit()) { // ref_pic_list_modification_flag_l1
          for (int i = 0; i <= sh.num_ref_idx_l1_active_minus1; ++i) br.readBits(entry_bits);
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
          const int max_collocated = sh.collocated_from_l0_flag ? sh.num_ref_idx_l0_active_minus1 : sh.num_ref_idx_l1_active_minus1;
          if (sh.collocated_ref_idx < 0 || sh.collocated_ref_idx > max_collocated) return fail();
        }
      }
      if ((pps.weighted_pred_flag && sh.slice_type == 1 /* P */) ||
          (pps.weighted_bipred_flag && sh.slice_type == 0 /* B */)) {
        if (!parsePredWeightTable(br, sps, sh)) return fail();
      }
      sh.five_minus_max_num_merge_cand = static_cast<int>(br.readUE());
      if (sh.five_minus_max_num_merge_cand < 0 || sh.five_minus_max_num_merge_cand > 4) return fail();
    }
    sh.slice_qp_delta = br.readSE();
    if (pps.pps_slice_chroma_qp_offsets_present_flag) {
      sh.slice_cb_qp_offset = br.readSE();
      sh.slice_cr_qp_offset = br.readSE();
      if (sh.slice_cb_qp_offset < -12 || sh.slice_cb_qp_offset > 12) return fail();
      if (sh.slice_cr_qp_offset < -12 || sh.slice_cr_qp_offset > 12) return fail();
    }
    bool slice_deblocking_filter_disabled_flag = pps.pps_deblocking_filter_disabled_flag;
    if (pps.deblocking_filter_control_present_flag) {
      bool deblocking_filter_override_flag = false;
      if (pps.deblocking_filter_override_enabled_flag) deblocking_filter_override_flag = br.readBit() != 0;
      if (deblocking_filter_override_flag) {
        slice_deblocking_filter_disabled_flag = br.readBit() != 0;
        if (!slice_deblocking_filter_disabled_flag) {
          sh.slice_beta_offset_div2 = br.readSE();
          sh.slice_tc_offset_div2 = br.readSE();
          if (sh.slice_beta_offset_div2 < -6 || sh.slice_beta_offset_div2 > 6) return fail();
          if (sh.slice_tc_offset_div2 < -6 || sh.slice_tc_offset_div2 > 6) return fail();
        }
      } else {
        sh.slice_beta_offset_div2 = pps.pps_beta_offset_div2;
        sh.slice_tc_offset_div2 = pps.pps_tc_offset_div2;
      }
    }
    if (pps.pps_loop_filter_across_slices_enabled_flag &&
        (sh.slice_sao_luma_flag || sh.slice_sao_chroma_flag || !slice_deblocking_filter_disabled_flag)) {
      sh.slice_loop_filter_across_slices_enabled_flag = br.readBit() != 0;
    }
  }

  // entry_point_offsets and the header extension sit between the slice header
  // proper and byte_alignment(); skipping them would leave header_bit_size, and
  // every slice data offset derived from it, short.
  if (pps.tiles_enabled_flag || pps.entropy_coding_sync_enabled_flag) {
    const int num_entry_point_offsets = static_cast<int>(br.readUE());
    if (num_entry_point_offsets < 0 || num_entry_point_offsets > MAX_ENTRY_POINT_OFFSETS) return fail();
    if (num_entry_point_offsets > 0) {
      const int offset_len_minus1 = static_cast<int>(br.readUE());
      if (offset_len_minus1 < 0 || offset_len_minus1 > 31) return fail();
      for (int i = 0; i < num_entry_point_offsets; ++i) {
        br.skipBits(static_cast<size_t>(offset_len_minus1) + 1);
        if (!br.ok()) return static_cast<int>(HevcParse::truncated);
      }
    }
  }
  if (pps.slice_segment_header_extension_present_flag) {
    const int extension_length = static_cast<int>(br.readUE());
    if (extension_length < 0 || extension_length > 256) return fail();
    br.skipBits(static_cast<size_t>(extension_length) * 8u);
  }

  if (!br.ok()) return static_cast<int>(HevcParse::truncated);
  sh.header_bit_size = static_cast<uint32_t>(br.bitPosition());
  return static_cast<int>(HevcParse::ok);
}

auto H265AccessUnitParser::parseNal(std::span<const uint8_t> nal_data, H265SliceHeader& slice, bool& has_slice) -> bool {
  has_slice = false;
  if (nal_data.size() < 3) return false;
  if ((nal_data[0] & 0x80u) != 0 || (nal_data[1] & 0x07u) == 0) return false;
  nal_unit_type_ = (nal_data[0] >> 1u) & 0x3fu;
  temporal_id_ = static_cast<int>((nal_data[1] & 0x07u) - 1u);
  first_slice_segment_in_pic_flag_ = false;
  slice_pic_order_cnt_lsb_ = 0;

  const auto payload = nal_data.subspan(2);

  if (nal_unit_type_ == NAL_VPS || nal_unit_type_ == NAL_SPS || nal_unit_type_ == NAL_PPS) {
    // Parameter set NALs are small, so unescape them whole.
    rbsp_scratch_.convert(payload);
    openmedia::BitReader br(rbsp_scratch_.rbsp());
    if (nal_unit_type_ == NAL_VPS) return parseVps(br);
    if (nal_unit_type_ == NAL_SPS) return parseSps(br);
    return parsePps(br);
  }

  if (!isVclNal(nal_unit_type_)) return true;
  if (!has_sps_ || !has_pps_) return false;

  // Unescape a prefix instead of the whole NAL: the header is a few hundred
  // bytes at most, while the slice payload behind it can be megabytes. Only a
  // header that genuinely runs past the prefix pays for a second pass.
  for (size_t limit = 256;; limit *= 8) {
    const bool complete = rbsp_scratch_.convert(payload, limit);
    if (rbsp_scratch_.rbsp().empty()) return false;
    slice = {};
    openmedia::BitReader br(rbsp_scratch_.rbsp());
    const auto result = static_cast<HevcParse>(parseSliceHeader(br, slice));
    if (result == HevcParse::ok) break;
    if (result == HevcParse::invalid || complete) return false;
  }

  // byte_alignment() contributes alignment_bit_equal_to_one plus padding, so
  // the payload starts at the next byte boundary strictly after the header.
  const size_t header_rbsp_bytes = (slice.header_bit_size + 8u) / 8u;
  slice.slice_data_byte_offset = nal_prefix_size_ + 2u +
                                 static_cast<uint32_t>(rbsp_scratch_.sourceOffset(header_rbsp_bytes));
  has_slice = true;
  return true;
}

auto H265AccessUnitParser::startsNewAccessUnit(int nal_type) const -> bool {
  if (!current_has_vcl_) return false;
  if (nal_type == NAL_AUD || nal_type == NAL_VPS || nal_type == NAL_SPS || nal_type == NAL_PPS ||
      nal_type == NAL_PREFIX_SEI || nal_type == NAL_EOS || nal_type == NAL_EOB) {
    return true;
  }
  if (!isVclNal(nal_type) || !have_previous_slice_) return false;
  return first_slice_segment_in_pic_flag_;
}

auto H265AccessUnitParser::computePoc(const Sps& sps, const H265SliceHeader& sh, int nal_type) -> int {
  const int max_pic_order_cnt_lsb = 1 << (sps.log2_max_pic_order_cnt_lsb_minus4 + 4);
  const bool irap_pic = isIrapNal(nal_type);
  const bool no_rasl_output_flag = irap_pic && ((nal_type >= NAL_BLA_W_LP && nal_type <= NAL_IDR_N_LP) || first_picture_);

  int pic_order_cnt_msb = 0;
  if (!irap_pic || !no_rasl_output_flag) {
    if (sh.slice_pic_order_cnt_lsb < ref_pic_order_cnt_lsb_ &&
        (ref_pic_order_cnt_lsb_ - sh.slice_pic_order_cnt_lsb) >= max_pic_order_cnt_lsb / 2) {
      pic_order_cnt_msb = ref_pic_order_cnt_msb_ + max_pic_order_cnt_lsb;
    } else if (sh.slice_pic_order_cnt_lsb > ref_pic_order_cnt_lsb_ &&
               (sh.slice_pic_order_cnt_lsb - ref_pic_order_cnt_lsb_) > max_pic_order_cnt_lsb / 2) {
      pic_order_cnt_msb = ref_pic_order_cnt_msb_ - max_pic_order_cnt_lsb;
    } else {
      pic_order_cnt_msb = ref_pic_order_cnt_msb_;
    }
  }

  // 8.3.1. Letting a RASL, RADL or sub-layer non-reference picture through moves
  // the reference the msb is derived against, which changes the outcome for the
  // picture that straddles a wrap of the low bits.
  const bool eligible_for_prev_tid0 =
      temporal_id_ == 0 && !isSubLayerNonReference(nal_type) && (nal_type < NAL_RADL_N || nal_type > NAL_RASL_R);
  if (eligible_for_prev_tid0) {
    ref_pic_order_cnt_lsb_ = sh.slice_pic_order_cnt_lsb;
    ref_pic_order_cnt_msb_ = pic_order_cnt_msb;
  }

  first_picture_ = false;
  return pic_order_cnt_msb + sh.slice_pic_order_cnt_lsb;
}

auto H265AccessUnitParser::finishCurrentFrame() -> H265ParsedFrame {
  current_.parameter_sets_changed = current_parameter_sets_changed_;
  H265ParsedFrame out = std::move(current_);
  current_ = {};
  current_has_vcl_ = false;
  current_parameter_sets_changed_ = false;
  have_previous_slice_ = false;
  return out;
}

} // namespace openmedia::video_parser
