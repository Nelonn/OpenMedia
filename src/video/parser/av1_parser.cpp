#include "av1_parser.hpp"

namespace openmedia::video_parser {

static constexpr uint8_t OBU_SEQUENCE_HEADER = 1;
static constexpr uint8_t OBU_FRAME_HEADER = 3;
static constexpr uint8_t OBU_TILE_GROUP = 4;
static constexpr uint8_t OBU_FRAME = 6;
static constexpr uint8_t OBU_TEMPORAL_DELIMITER = 2;

auto AV1ObuParser::parse(std::span<const uint8_t> packet) -> std::vector<AV1ParsedFrame> {
  std::vector<AV1ParsedFrame> frames;
  AV1ParsedFrame current;
  size_t offset = 0;

  while (offset < packet.size()) {
    const size_t obu_start = offset;
    const uint8_t header = packet[offset++];
    const uint8_t type = (header >> 3u) & 0x0fu;
    const bool has_extension = ((header >> 2u) & 1u) != 0;
    const bool has_size_field = ((header >> 1u) & 1u) != 0;
    if (has_extension) {
      if (offset >= packet.size()) break;
      ++offset;
    }

    size_t size_len = 0;
    const uint64_t payload_size = has_size_field ? readLeb128(packet.subspan(offset), size_len) : packet.size() - offset;
    offset += size_len;
    if (payload_size > packet.size() - offset) break;

    if ((type == OBU_TEMPORAL_DELIMITER || type == OBU_FRAME_HEADER || type == OBU_FRAME) && (current.has_frame || current.has_frame_header)) {
      frames.push_back(std::move(current));
      current = {};
    }

    const size_t payload_offset = offset;
    const size_t obu_size = payload_offset + static_cast<size_t>(payload_size) - obu_start;
    const size_t output_offset = current.bitstream.size();
    current.bitstream.insert(current.bitstream.end(), packet.begin() + static_cast<ptrdiff_t>(obu_start), packet.begin() + static_cast<ptrdiff_t>(obu_start + obu_size));
    current.obus.push_back({type, output_offset, obu_size, output_offset + (payload_offset - obu_start), static_cast<size_t>(payload_size)});
    current.has_sequence_header = current.has_sequence_header || type == OBU_SEQUENCE_HEADER;
    current.has_frame_header = current.has_frame_header || type == OBU_FRAME_HEADER;
    current.has_frame = current.has_frame || type == OBU_FRAME || type == OBU_TILE_GROUP;

    offset += static_cast<size_t>(payload_size);
  }

  if (!current.bitstream.empty()) frames.push_back(std::move(current));
  return frames;
}

auto AV1ObuParser::readLeb128(std::span<const uint8_t> data, size_t& bytes_read) -> uint64_t {
  uint64_t value = 0;
  bytes_read = 0;
  for (size_t i = 0; i < data.size() && i < 8; ++i) {
    value |= static_cast<uint64_t>(data[i] & 0x7fu) << (i * 7u);
    ++bytes_read;
    if ((data[i] & 0x80u) == 0) break;
  }
  return value;
}

} // namespace openmedia::video_parser
