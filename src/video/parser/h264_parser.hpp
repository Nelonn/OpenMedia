#pragma once

#include "start_code.hpp"

#include <cstdint>
#include <span>
#include <vector>

#include "h264_types.hpp"

namespace openmedia::video_parser {

struct H264ParsedFrame {
  std::vector<uint8_t> bitstream;
  std::vector<uint32_t> slice_offsets;
  h264::NALHeader nal = {};
  h264::SliceHeader slice = {};
  int32_t poc = 0;
  bool is_intra = false;
  bool is_reference = false;
  bool parameter_sets_changed = false;
};

struct H264ParserState {
  h264::SPS sps[32] = {};
  h264::PPS pps[256] = {};
  bool sps_valid[32] = {};
  bool pps_valid[256] = {};
  bool has_sps = false;
  bool has_pps = false;
  uint8_t nal_length_size = 0;
  int prev_pic_order_cnt_lsb = 0;
  int prev_pic_order_cnt_msb = 0;
  int prev_frame_num = 0;
  int prev_frame_num_offset = 0;
  bool have_prev_poc = false;
};

class H264AccessUnitParser {
public:
  void reset();
  void resetPoc();
  void parseExtradata(std::span<const uint8_t> extradata);

  auto parse(std::span<const uint8_t> packet, bool end_of_packet = true) -> std::vector<H264ParsedFrame>;

  auto state() const noexcept -> const H264ParserState& { return state_; }
  auto state() noexcept -> H264ParserState& { return state_; }

private:
  struct NalUnit {
    size_t start = 0;
    size_t header = 0;
    size_t end = 0;
  };

  H264ParserState state_ = {};
  StartCodeScanner scanner_;
  H264ParsedFrame current_ = {};
  bool current_has_vcl_ = false;
  bool current_parameter_sets_changed_ = false;
  h264::SliceHeader previous_slice_ = {};
  h264::NALHeader previous_nal_ = {};
  bool have_previous_slice_ = false;

  auto normalizePacket(std::span<const uint8_t> packet) const -> std::vector<uint8_t>;
  auto findNalUnits(std::span<const uint8_t> packet) -> std::vector<NalUnit>;
  auto parseNal(std::span<const uint8_t> nal_data, h264::NALHeader& nal, h264::SliceHeader& slice) -> bool;
  auto storeParameterSet(std::span<const uint8_t> nal_data, const h264::NALHeader& nal) -> bool;
  auto startsNewAccessUnit(const h264::NALHeader& nal, const h264::SliceHeader& slice, bool is_vcl) const -> bool;
  auto computePoc(const h264::SliceHeader& slice) -> int32_t;
  auto finishCurrentFrame() -> H264ParsedFrame;
};

} // namespace openmedia::video_parser
