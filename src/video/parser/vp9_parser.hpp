#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace openmedia::video_parser {

struct VP9ParsedFrame {
  std::span<const uint8_t> bitstream;
  std::vector<uint8_t> storage;
  bool key_frame = false;
  uint8_t profile = 0;
};

class VP9FrameParser {
public:
  auto parse(std::span<const uint8_t> packet) -> std::vector<VP9ParsedFrame>;
};

} // namespace openmedia::video_parser
