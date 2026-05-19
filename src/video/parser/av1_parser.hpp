#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace openmedia::video_parser {

struct AV1Obu {
  uint8_t type = 0;
  size_t offset = 0;
  size_t size = 0;
  size_t payload_offset = 0;
  size_t payload_size = 0;
};

struct AV1ParsedFrame {
  std::vector<uint8_t> bitstream;
  std::vector<AV1Obu> obus;
  bool has_sequence_header = false;
  bool has_frame_header = false;
  bool has_frame = false;
};

class AV1ObuParser {
public:
  auto parse(std::span<const uint8_t> packet) -> std::vector<AV1ParsedFrame>;

private:
  static auto readLeb128(std::span<const uint8_t> data, size_t& bytes_read) -> uint64_t;
};

} // namespace openmedia::video_parser
