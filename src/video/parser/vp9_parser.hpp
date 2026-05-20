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

private:
  static auto parseProfile(const uint8_t* data, size_t size) -> uint8_t;
  static auto isKeyFrame(const uint8_t* data, size_t size) -> bool;
};

} // namespace openmedia::video_parser
