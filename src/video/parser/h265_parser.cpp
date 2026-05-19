#include "h265_parser.hpp"

#include <util/bit_reader.hpp>

#include <cstring>

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
  --value;
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
    if (!parseNal(nal_data)) continue;

    const int nal_type = nal_unit_type_;
    const bool is_vcl = isVclNal(nal_type);
    if (startsNewAccessUnit(nal_type) && current_has_vcl_) frames.push_back(finishCurrentFrame());

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
  std::vector<size_t> starts;
  scanner_.reset();
  size_t offset = 0;
  while (offset < packet.size()) {
    bool found = false;
    offset += scanner_.next(packet.data() + offset, packet.size() - offset, found);
    if (found && offset >= 3) starts.push_back(offset - 3);
  }

  std::vector<NalUnit> nals;
  for (size_t i = 0; i < starts.size(); ++i) {
    const size_t start = starts[i];
    const size_t header = start + 3;
    const size_t end = (i + 1 < starts.size()) ? starts[i + 1] : packet.size();
    if (header + 2 <= end) nals.push_back({start, header, end});
  }
  return nals;
}

auto H265AccessUnitParser::parseNal(std::span<const uint8_t> nal_data) -> bool {
  if (nal_data.size() < 2) return false;
  if ((nal_data[0] & 0x80u) != 0 || (nal_data[1] & 0x07u) == 0) return false;
  nal_unit_type_ = (nal_data[0] >> 1u) & 0x3fu;
  first_slice_segment_in_pic_flag_ = false;
  slice_pic_order_cnt_lsb_ = 0;

  auto rbsp = openmedia::nalToRbsp(nal_data.subspan(2));
  openmedia::BitReader br(rbsp);
  if (nal_unit_type_ == 33) {
    br.readBits(4);
    const int max_sub_layers_minus1 = static_cast<int>(br.readBits(3));
    br.readBit();
    skipProfileTierLevel(br, max_sub_layers_minus1);
    const int sps_id = static_cast<int>(br.readUE());
    if (sps_id < 0 || sps_id >= 16) return false;
    auto& sps = sps_[sps_id];
    sps = {};
    sps.valid = true;
    sps.id = sps_id;
    sps.chroma_format_idc = static_cast<int>(br.readUE());
    if (sps.chroma_format_idc == 3) br.readBit();
    br.readUE();
    br.readUE();
    if (br.readBit()) {
      br.readUE();
      br.readUE();
      br.readUE();
      br.readUE();
    }
    br.readUE();
    br.readUE();
    sps.log2_max_pic_order_cnt_lsb_minus4 = static_cast<int>(br.readUE());
    return br.ok();
  }
  if (nal_unit_type_ == 34) {
    const int pps_id = static_cast<int>(br.readUE());
    const int sps_id = static_cast<int>(br.readUE());
    if (pps_id < 0 || pps_id >= 64 || sps_id < 0 || sps_id >= 16) return false;
    auto& pps = pps_[pps_id];
    pps = {};
    pps.valid = true;
    pps.id = pps_id;
    pps.sps_id = sps_id;
    pps.dependent_slice_segments_enabled_flag = br.readBit() != 0;
    pps.output_flag_present_flag = br.readBit() != 0;
    pps.num_extra_slice_header_bits = static_cast<int>(br.readBits(3));
    return br.ok();
  }
  if (!isVclNal(nal_unit_type_)) return true;

  first_slice_segment_in_pic_flag_ = br.readBit() != 0;
  if (isIrapNal(nal_unit_type_)) br.readBit();
  const int pps_id = static_cast<int>(br.readUE());
  if (pps_id < 0 || pps_id >= 64 || !pps_[pps_id].valid) return br.ok();
  const auto& pps = pps_[pps_id];
  bool dependent_slice_segment_flag = false;
  if (!first_slice_segment_in_pic_flag_ && pps.dependent_slice_segments_enabled_flag) {
    dependent_slice_segment_flag = br.readBit() != 0;
  }
  if (!first_slice_segment_in_pic_flag_) br.skipBits(ceilLog2(1));
  if (!dependent_slice_segment_flag) {
    br.skipBits(static_cast<uint32_t>(pps.num_extra_slice_header_bits));
    br.readUE();
    if (pps.output_flag_present_flag) br.readBit();
    if (pps.sps_id >= 0 && pps.sps_id < 16 && sps_[pps.sps_id].valid && sps_[pps.sps_id].chroma_format_idc == 3) br.readBits(2);
    if (!isIdrNal(nal_unit_type_) && pps.sps_id >= 0 && pps.sps_id < 16 && sps_[pps.sps_id].valid) {
      slice_pic_order_cnt_lsb_ = static_cast<int>(br.readBits(static_cast<uint32_t>(sps_[pps.sps_id].log2_max_pic_order_cnt_lsb_minus4 + 4)));
    }
  }
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
