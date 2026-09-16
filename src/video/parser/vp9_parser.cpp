#include "vp9_parser.hpp"

namespace openmedia::video_parser {

// VP9 6.2 uncompressed_header(), MSB first from byte 0:
//   frame_marker      f(2)   bits 7..6
//   profile_low_bit   f(1)   bit 5
//   profile_high_bit  f(1)   bit 4
//   reserved_zero     f(1)   bit 3, only when Profile == 3
//   show_existing_frame f(1)
//   frame_type        f(1)   absent when show_existing_frame is set
namespace {

struct FrameFlags {
  uint8_t profile = 0;
  bool key_frame = false;
};

auto readFrameFlags(const uint8_t* data, size_t size) -> FrameFlags {
  FrameFlags flags;
  if (size == 0) return flags;
  const uint8_t byte = data[0];
  const auto bit = [byte](int index) -> uint8_t { return static_cast<uint8_t>((byte >> index) & 1u); };

  // profile_low_bit is read first and carries weight 1.
  flags.profile = static_cast<uint8_t>(bit(5) | (bit(4) << 1u));
  int next_bit = 3;
  if (flags.profile == 3) {
    flags.profile = static_cast<uint8_t>(flags.profile + bit(3));
    --next_bit; // the reserved_zero_bit shifts the rest of the header along
  }

  const bool show_existing_frame = bit(next_bit) != 0;
  // A show_existing_frame header stops before frame_type; it repeats a frame
  // that is already in the reference pool, so it is never a key frame itself.
  if (show_existing_frame) return flags;
  flags.key_frame = bit(next_bit - 1) == 0;
  return flags;
}

} // namespace

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
        const FrameFlags flags = readFrameFlags(frame.data(), frame.size());
        frames.push_back({frame, {}, flags.key_frame, flags.profile});
        frame_offset += frame_size;
      }
      if (!frames.empty()) return frames;
    }
  }

  const FrameFlags flags = readFrameFlags(packet.data(), packet.size());
  frames.push_back({packet, {}, flags.key_frame, flags.profile});
  return frames;
}

} // namespace openmedia::video_parser
