#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace openmedia {

// Xiph lacing: the sizes of the first two of the three Vorbis headers, each a
// run of 255s closed by a byte below 255; the third runs to the end.
inline auto splitVorbisExtradata(std::span<const uint8_t> extradata)
    -> std::optional<std::array<std::span<const uint8_t>, 3>> {
  if (extradata.size() < 3 || extradata[0] != 2) return std::nullopt;

  size_t offset = 1;
  size_t sizes[2] = {0, 0};
  for (size_t& size : sizes) {
    while (offset < extradata.size()) {
      const uint8_t value = extradata[offset++];
      size += value;
      if (value < 255) break;
    }
  }

  if (offset + sizes[0] + sizes[1] > extradata.size()) return std::nullopt;
  return std::array {
      extradata.subspan(offset, sizes[0]),
      extradata.subspan(offset + sizes[0], sizes[1]),
      extradata.subspan(offset + sizes[0] + sizes[1]),
  };
}

inline void appendXiphLacing(std::vector<uint8_t>& out, size_t size) {
  while (size >= 255) {
    out.push_back(255);
    size -= 255;
  }
  out.push_back(static_cast<uint8_t>(size));
}

} // namespace openmedia
