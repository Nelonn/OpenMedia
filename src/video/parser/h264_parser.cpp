#include "h264_parser.hpp"

#include <algorithm>
#include <cstring>

namespace openmedia::video_parser {

static auto isAnnexB(std::span<const uint8_t> data) noexcept -> bool {
  return data.size() >= 3 && data[0] == 0 && data[1] == 0 &&
         (data[2] == 1 || (data.size() >= 4 && data[2] == 0 && data[3] == 1));
}

static auto readNalSize(std::span<const uint8_t> input, size_t offset, uint8_t nal_length_size) noexcept -> uint32_t {
  uint32_t size = 0;
  for (uint8_t i = 0; i < nal_length_size; ++i) size = (size << 8u) | input[offset + i];
  return size;
}

void H264AccessUnitParser::reset() {
  state_ = {};
  current_ = {};
  current_has_vcl_ = false;
  current_parameter_sets_changed_ = false;
  previous_slice_ = {};
  previous_nal_ = {};
  have_previous_slice_ = false;
  scanner_.reset();
}

void H264AccessUnitParser::resetPoc() {
  state_.prev_pic_order_cnt_lsb = 0;
  state_.prev_pic_order_cnt_msb = 0;
  state_.prev_frame_num = 0;
  state_.prev_frame_num_offset = 0;
}

void H264AccessUnitParser::parseExtradata(std::span<const uint8_t> extradata) {
  if (extradata.empty()) return;
  if (extradata.size() >= 7 && extradata[0] == 1) {
    state_.nal_length_size = static_cast<uint8_t>((extradata[4] & 0x03u) + 1);
    size_t offset = 5;
    const uint8_t sps_count = extradata[offset++] & 0x1fu;
    for (uint8_t i = 0; i < sps_count && offset + 2 <= extradata.size(); ++i) {
      const size_t size = (static_cast<size_t>(extradata[offset]) << 8u) | extradata[offset + 1];
      offset += 2;
      if (offset + size > extradata.size()) return;
      h264::NALHeader nal = {};
      h264::SliceHeader slice = {};
      if (parseNal(extradata.subspan(offset, size), nal, slice)) storeParameterSet(extradata.subspan(offset, size), nal);
      offset += size;
    }
    if (offset >= extradata.size()) return;
    const uint8_t pps_count = extradata[offset++];
    for (uint8_t i = 0; i < pps_count && offset + 2 <= extradata.size(); ++i) {
      const size_t size = (static_cast<size_t>(extradata[offset]) << 8u) | extradata[offset + 1];
      offset += 2;
      if (offset + size > extradata.size()) return;
      h264::NALHeader nal = {};
      h264::SliceHeader slice = {};
      if (parseNal(extradata.subspan(offset, size), nal, slice)) storeParameterSet(extradata.subspan(offset, size), nal);
      offset += size;
    }
    return;
  }

  auto packet = normalizePacket(extradata);
  auto nals = findNalUnits(packet);
  for (const auto& unit : nals) {
    h264::NALHeader nal = {};
    h264::SliceHeader slice = {};
    const auto nal_data = std::span<const uint8_t>(packet.data() + unit.header, unit.end - unit.header);
    if (parseNal(nal_data, nal, slice)) storeParameterSet(nal_data, nal);
  }
}

auto H264AccessUnitParser::parse(std::span<const uint8_t> packet, bool end_of_packet) -> std::vector<H264ParsedFrame> {
  std::vector<H264ParsedFrame> frames;
  if (packet.empty()) return frames;

  auto normalized = normalizePacket(packet);
  auto nals = findNalUnits(normalized);
  for (const auto& unit : nals) {
    auto nal_data = std::span<const uint8_t>(normalized.data() + unit.header, unit.end - unit.header);
    h264::NALHeader nal = {};
    h264::SliceHeader slice = {};
    if (!parseNal(nal_data, nal, slice)) continue;

    const bool is_vcl = nal.type == h264::NAL_UNIT_TYPE_CODED_SLICE_IDR || nal.type == h264::NAL_UNIT_TYPE_CODED_SLICE_NON_IDR;
    if (startsNewAccessUnit(nal, slice, is_vcl) && current_has_vcl_) {
      frames.push_back(finishCurrentFrame());
    }

    const uint32_t output_offset = static_cast<uint32_t>(current_.bitstream.size());
    current_.bitstream.insert(current_.bitstream.end(), normalized.begin() + static_cast<ptrdiff_t>(unit.start), normalized.begin() + static_cast<ptrdiff_t>(unit.end));

    if (storeParameterSet(nal_data, nal)) current_parameter_sets_changed_ = true;
    if (is_vcl && state_.has_sps && state_.has_pps) {
      current_.slice_offsets.push_back(output_offset);
      current_.nal = nal;
      current_.slice = slice;
      current_has_vcl_ = true;
      previous_slice_ = slice;
      previous_nal_ = nal;
      have_previous_slice_ = true;
    }
  }

  if (end_of_packet && current_has_vcl_) frames.push_back(finishCurrentFrame());
  return frames;
}

auto H264AccessUnitParser::normalizePacket(std::span<const uint8_t> packet) const -> std::vector<uint8_t> {
  if (packet.empty()) return {};
  if (isAnnexB(packet) || state_.nal_length_size == 0) return {packet.begin(), packet.end()};

  std::vector<uint8_t> out;
  out.reserve(packet.size() + 16);
  size_t offset = 0;
  while (offset + state_.nal_length_size <= packet.size()) {
    const uint32_t nal_size = readNalSize(packet, offset, state_.nal_length_size);
    offset += state_.nal_length_size;
    if (nal_size == 0 || offset + nal_size > packet.size()) break;
    out.insert(out.end(), {0, 0, 1});
    out.insert(out.end(), packet.begin() + static_cast<ptrdiff_t>(offset), packet.begin() + static_cast<ptrdiff_t>(offset + nal_size));
    offset += nal_size;
  }
  if (out.empty()) return {packet.begin(), packet.end()};
  return out;
}

auto H264AccessUnitParser::findNalUnits(std::span<const uint8_t> packet) -> std::vector<NalUnit> {
  std::vector<size_t> starts;
  scanner_.reset();
  size_t offset = 0;
  while (offset < packet.size()) {
    bool found = false;
    const size_t used = scanner_.next(packet.data() + offset, packet.size() - offset, found);
    offset += used;
    if (found && offset >= 3) starts.push_back(offset - 3);
  }

  std::vector<NalUnit> nals;
  for (size_t i = 0; i < starts.size(); ++i) {
    const size_t start = starts[i];
    const size_t header = start + 3;
    const size_t end = (i + 1 < starts.size()) ? starts[i + 1] : packet.size();
    if (header < end) nals.push_back({start, header, end});
  }
  return nals;
}

auto H264AccessUnitParser::parseNal(std::span<const uint8_t> nal_data, h264::NALHeader& nal, h264::SliceHeader& slice) -> bool {
  if (nal_data.empty()) return false;
  h264::Bitstream bs;
  bs.init(nal_data.data(), nal_data.size());
  if (!h264::read_nal_header(nal, bs)) return false;
  if ((nal.type == h264::NAL_UNIT_TYPE_CODED_SLICE_IDR || nal.type == h264::NAL_UNIT_TYPE_CODED_SLICE_NON_IDR) && state_.has_sps && state_.has_pps) {
    return h264::read_slice_header(slice, nal, state_.pps, state_.sps, bs);
  }
  return true;
}

auto H264AccessUnitParser::storeParameterSet(std::span<const uint8_t> nal_data, const h264::NALHeader& nal) -> bool {
  h264::Bitstream bs;
  bs.init(nal_data.data(), nal_data.size());
  h264::NALHeader ignored = {};
  if (!h264::read_nal_header(ignored, bs)) return false;
  if (nal.type == h264::NAL_UNIT_TYPE_SPS) {
    h264::SPS parsed = {};
    h264::read_sps(parsed, bs);
    if (parsed.seq_parameter_set_id >= 0 && parsed.seq_parameter_set_id < 32) {
      state_.sps[parsed.seq_parameter_set_id] = parsed;
      state_.sps_valid[parsed.seq_parameter_set_id] = true;
      state_.has_sps = true;
      return true;
    }
  } else if (nal.type == h264::NAL_UNIT_TYPE_PPS) {
    h264::PPS parsed = {};
    h264::read_pps(parsed, bs);
    if (parsed.pic_parameter_set_id >= 0 && parsed.pic_parameter_set_id < 256) {
      state_.pps[parsed.pic_parameter_set_id] = parsed;
      state_.pps_valid[parsed.pic_parameter_set_id] = true;
      state_.has_pps = true;
      return true;
    }
  }
  return false;
}

auto H264AccessUnitParser::startsNewAccessUnit(const h264::NALHeader& nal, const h264::SliceHeader& slice, bool is_vcl) const -> bool {
  if (!current_has_vcl_) return false;
  if (nal.type == h264::NAL_UNIT_TYPE_AUD || nal.type == h264::NAL_UNIT_TYPE_SPS || nal.type == h264::NAL_UNIT_TYPE_PPS) return true;
  if (!is_vcl || !have_previous_slice_) return false;
  if ((previous_nal_.idc == 0) != (nal.idc == 0)) return true;
  if ((previous_nal_.type == h264::NAL_UNIT_TYPE_CODED_SLICE_IDR) != (nal.type == h264::NAL_UNIT_TYPE_CODED_SLICE_IDR)) return true;
  if (previous_slice_.pic_parameter_set_id != slice.pic_parameter_set_id) return true;
  if (previous_slice_.frame_num != slice.frame_num) return true;
  if (previous_slice_.field_pic_flag != slice.field_pic_flag) return true;
  if (previous_slice_.bottom_field_flag != slice.bottom_field_flag) return true;
  if (previous_slice_.pic_order_cnt_lsb != slice.pic_order_cnt_lsb) return true;
  if (previous_slice_.delta_pic_order_cnt_bottom != slice.delta_pic_order_cnt_bottom) return true;
  if (previous_slice_.delta_pic_order_cnt[0] != slice.delta_pic_order_cnt[0]) return true;
  if (previous_slice_.delta_pic_order_cnt[1] != slice.delta_pic_order_cnt[1]) return true;
  if (previous_nal_.type == h264::NAL_UNIT_TYPE_CODED_SLICE_IDR && previous_slice_.idr_pic_id != slice.idr_pic_id) return true;
  return false;
}

auto H264AccessUnitParser::computePoc(const h264::SliceHeader& slice) -> int32_t {
  if (slice.pic_parameter_set_id < 0 || slice.pic_parameter_set_id >= 256 || !state_.pps_valid[slice.pic_parameter_set_id]) return slice.pic_order_cnt_lsb;
  const auto& pps = state_.pps[slice.pic_parameter_set_id];
  if (pps.seq_parameter_set_id < 0 || pps.seq_parameter_set_id >= 32 || !state_.sps_valid[pps.seq_parameter_set_id]) return slice.pic_order_cnt_lsb;
  const auto& sps = state_.sps[pps.seq_parameter_set_id];

  const bool is_idr = current_.nal.type == h264::NAL_UNIT_TYPE_CODED_SLICE_IDR;
  int32_t poc = 0;

  if (sps.pic_order_cnt_type == 0) {
    if (is_idr) {
      state_.prev_pic_order_cnt_msb = 0;
      state_.prev_pic_order_cnt_lsb = 0;
    }
    const int max_pic_order_cnt_lsb = 1 << (sps.log2_max_pic_order_cnt_lsb_minus4 + 4);
    int pic_order_cnt_msb = 0;
    if (slice.pic_order_cnt_lsb < state_.prev_pic_order_cnt_lsb &&
        (state_.prev_pic_order_cnt_lsb - slice.pic_order_cnt_lsb) >= max_pic_order_cnt_lsb / 2) {
      pic_order_cnt_msb = state_.prev_pic_order_cnt_msb + max_pic_order_cnt_lsb;
    } else if (slice.pic_order_cnt_lsb > state_.prev_pic_order_cnt_lsb &&
               (slice.pic_order_cnt_lsb - state_.prev_pic_order_cnt_lsb) > max_pic_order_cnt_lsb / 2) {
      pic_order_cnt_msb = state_.prev_pic_order_cnt_msb - max_pic_order_cnt_lsb;
    } else {
      pic_order_cnt_msb = state_.prev_pic_order_cnt_msb;
    }
    const int top_foc = pic_order_cnt_msb + slice.pic_order_cnt_lsb;
    const int bottom_foc = top_foc + slice.delta_pic_order_cnt_bottom;
    poc = slice.mmco5 ? 0 : std::min(top_foc, bottom_foc);
    if (current_.nal.idc != 0) {
      if (slice.mmco5) {
        state_.prev_pic_order_cnt_msb = 0;
        state_.prev_pic_order_cnt_lsb = top_foc;
      } else {
        state_.prev_pic_order_cnt_lsb = slice.pic_order_cnt_lsb;
        state_.prev_pic_order_cnt_msb = pic_order_cnt_msb;
      }
    }
  } else if (sps.pic_order_cnt_type == 1) {
    const int max_frame_num = 1 << (sps.log2_max_frame_num_minus4 + 4);
    int frame_num_offset = 0;
    if (is_idr) frame_num_offset = 0;
    else if (state_.prev_frame_num > slice.frame_num) frame_num_offset = state_.prev_frame_num_offset + max_frame_num;
    else frame_num_offset = state_.prev_frame_num_offset;

    int abs_frame_num = 0;
    if (sps.num_ref_frames_in_pic_order_cnt_cycle > 0) abs_frame_num = frame_num_offset + slice.frame_num;
    if (current_.nal.idc == 0 && abs_frame_num > 0) abs_frame_num--;

    int expected_poc = 0;
    if (abs_frame_num > 0) {
      const int cycle_cnt = (abs_frame_num - 1) / sps.num_ref_frames_in_pic_order_cnt_cycle;
      const int frame_num_in_cycle = (abs_frame_num - 1) % sps.num_ref_frames_in_pic_order_cnt_cycle;
      int expected_delta_per_cycle = 0;
      for (int i = 0; i < sps.num_ref_frames_in_pic_order_cnt_cycle; ++i) expected_delta_per_cycle += sps.offset_for_ref_frame[i];
      expected_poc = cycle_cnt * expected_delta_per_cycle;
      for (int i = 0; i <= frame_num_in_cycle; ++i) expected_poc += sps.offset_for_ref_frame[i];
    }
    if (current_.nal.idc == 0) expected_poc += sps.offset_for_non_ref_pic;

    const int top_foc = expected_poc + slice.delta_pic_order_cnt[0];
    const int bottom_foc = top_foc + sps.offset_for_top_to_bottom_field + slice.delta_pic_order_cnt[1];
    poc = slice.mmco5 ? 0 : std::min(top_foc, bottom_foc);

    state_.prev_frame_num = slice.mmco5 ? 0 : slice.frame_num;
    state_.prev_frame_num_offset = slice.mmco5 ? 0 : frame_num_offset;
  } else if (sps.pic_order_cnt_type == 2) {
    const int max_frame_num = 1 << (sps.log2_max_frame_num_minus4 + 4);
    int frame_num_offset = 0;
    if (is_idr) frame_num_offset = 0;
    else if (state_.prev_frame_num > slice.frame_num) frame_num_offset = state_.prev_frame_num_offset + max_frame_num;
    else frame_num_offset = state_.prev_frame_num_offset;

    const int abs_frame_num = frame_num_offset + slice.frame_num;
    if (is_idr) poc = 0;
    else if (current_.nal.idc == 0) poc = 2 * abs_frame_num - 1;
    else poc = 2 * abs_frame_num;
    if (slice.mmco5) poc = 0;

    state_.prev_frame_num = slice.mmco5 ? 0 : slice.frame_num;
    state_.prev_frame_num_offset = slice.mmco5 ? 0 : frame_num_offset;
  }

  return poc;
}

auto H264AccessUnitParser::finishCurrentFrame() -> H264ParsedFrame {
  current_.is_intra = current_.nal.type == h264::NAL_UNIT_TYPE_CODED_SLICE_IDR;
  current_.is_reference = current_.nal.idc != h264::NAL_REF_IDC_PRIORITY_DISPOSABLE;
  if (current_.is_intra) resetPoc();
  current_.poc = computePoc(current_.slice);
  current_.parameter_sets_changed = current_parameter_sets_changed_;

  H264ParsedFrame out = std::move(current_);
  current_ = {};
  current_has_vcl_ = false;
  current_parameter_sets_changed_ = false;
  have_previous_slice_ = false;
  previous_slice_ = {};
  previous_nal_ = {};
  return out;
}

} // namespace openmedia::video_parser
