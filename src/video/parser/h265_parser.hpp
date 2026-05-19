#pragma once

#include "start_code.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace openmedia::video_parser {

struct H265ParsedFrame {
  std::vector<uint8_t> bitstream;
  std::vector<uint32_t> slice_offsets;
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

private:
  struct NalUnit {
    size_t start = 0;
    size_t header = 0;
    size_t end = 0;
  };

  struct Sps {
    bool valid = false;
    int id = -1;
    int chroma_format_idc = 1;
    int log2_max_pic_order_cnt_lsb_minus4 = 4;
  };

  struct Pps {
    bool valid = false;
    int id = -1;
    int sps_id = -1;
    bool dependent_slice_segments_enabled_flag = false;
    bool output_flag_present_flag = false;
    int num_extra_slice_header_bits = 0;
  };

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
