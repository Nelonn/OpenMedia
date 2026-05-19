#include "vp9_parser.hpp"

namespace openmedia::video_parser {

auto VP9FrameParser::parse(std::span<const uint8_t> packet) -> std::vector<VP9ParsedFrame> {
  std::vector<VP9ParsedFrame> frames;
  if (packet.empty()) return frames;

  const uint8_t marker = packet.back();
  if ((marker & 0xe0u) == 0xc0u) {
    const size_t length_size = ((marker >> 3u) & 0x03u) + 1u;
    const size_t frame_count = (marker & 0x07u) + 1u;
    const size_t index_size = 2 + length_size * frame_count;
    if (packet.size() >= index_size && packet[packet.size() - index_size] == marker) {
      size_t pos = packet.size() - index_size + 1;
      size_t frame_offset = 0;
      for (size_t i = 0; i < frame_count; ++i) {
        size_t frame_size = 0;
        for (size_t j = 0; j < length_size; ++j) frame_size |= static_cast<size_t>(packet[pos++]) << (j * 8u);
        if (frame_size == 0 || frame_offset + frame_size > packet.size() - index_size) break;
        auto frame = packet.subspan(frame_offset, frame_size);
        frames.push_back({frame, {}, isKeyFrame(frame.data(), frame.size()), parseProfile(frame.data(), frame.size())});
        frame_offset += frame_size;
      }
      if (!frames.empty()) return frames;
    }
  }

  frames.push_back({packet, {}, isKeyFrame(packet.data(), packet.size()), parseProfile(packet.data(), packet.size())});
  return frames;
}

auto VP9FrameParser::parseProfile(const uint8_t* data, size_t size) -> uint8_t {
  if (size == 0) return 0;
  const uint8_t profile_low = (data[0] >> 4u) & 1u;
  const uint8_t profile_high = (data[0] >> 5u) & 1u;
  uint8_t profile = static_cast<uint8_t>(profile_low | (profile_high << 1u));
  if (profile == 3 && size > 0) profile = static_cast<uint8_t>(profile + ((data[0] >> 6u) & 1u));
  return profile;
}

auto VP9FrameParser::isKeyFrame(const uint8_t* data, size_t size) -> bool {
  if (size == 0) return false;
  return ((data[0] >> 2u) & 0x1u) == 0;
}

} // namespace openmedia::video_parser
