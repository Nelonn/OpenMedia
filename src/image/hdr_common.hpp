#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace openmedia {
namespace hdr {

struct Header {
  uint32_t width = 0;
  uint32_t height = 0;
  size_t data_offset = 0; // first byte of scanline data
  bool is_xyze = false; // FORMAT=32-bit_rle_xyze
  bool flip_vertical = false; // "+Y" resolution (bottom-to-top)
  bool flip_horizontal = false; // "-X" resolution (right-to-left)
};

inline auto lineEnd(const uint8_t* data, size_t size, size_t start) -> size_t {
  for (size_t i = start; i < size; i++) {
    if (data[i] == '\n') return i + 1;
  }
  return size;
}

inline auto parseNumber(std::string_view s, size_t& i, uint32_t& out) -> bool {
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
    i++;
  }
  const size_t start = i;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    i++;
  }
  if (start == i) return false;
  uint64_t v = 0;
  for (size_t k = start; k < i; k++) {
    v = v * 10 + static_cast<uint32_t>(s[k] - '0');
  }
  out = static_cast<uint32_t>(v);
  return true;
}

// Parses the text header of a Radiance HDR file up to and including the
// resolution line. Returns false if the magic or the resolution line is bad.
inline auto parseHeader(const uint8_t* data, size_t size, Header& out) -> bool {
  if (size < 9) return false;
  if (data[0] != '#' || data[1] != '?') return false;

  size_t pos = 0;

  // 1. Magic line.
  pos = lineEnd(data, size, 0);
  if (pos == 0 || pos >= size) return false;

  // 2. Header lines until a blank line.
  while (pos < size) {
    const size_t line_start = pos;
    pos = lineEnd(data, size, pos);
    std::string_view line(reinterpret_cast<const char*>(data + line_start),
                          pos - line_start - (data[pos - 1] == '\n' ? 1 : 0));
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty()) break; // blank line terminates the header

    if (line.starts_with("FORMAT=")) {
      out.is_xyze = line.find("xyze") != std::string_view::npos;
    }
  }
  if (pos >= size) return false;

  // 3. Resolution line.
  const size_t res_start = pos;
  const size_t res_end = lineEnd(data, size, pos);
  pos = res_end;
  if (res_end == res_start) return false;

  std::string_view res(reinterpret_cast<const char*>(data + res_start),
                       res_end - res_start - (data[res_end - 1] == '\n' ? 1 : 0));
  if (!res.empty() && res.back() == '\r') res.remove_suffix(1);

  bool have_w = false, have_h = false;
  size_t i = 0;
  while (i < res.size()) {
    while (i < res.size() && (res[i] == ' ' || res[i] == '\t')) i++;
    if (i >= res.size()) break;
    const size_t tok_start = i;
    while (i < res.size() && res[i] != ' ' && res[i] != '\t') i++;
    std::string_view tok = res.substr(tok_start, i - tok_start);

    if (tok.size() == 2 && (tok[1] == 'Y' || tok[1] == 'y')) {
      const bool neg = tok[0] == '-';
      if (!parseNumber(res, i, out.height)) return false;
      out.flip_vertical = !neg; // "+Y" rows are bottom-to-top
      have_h = true;
    } else if (tok.size() == 2 && (tok[1] == 'X' || tok[1] == 'x')) {
      const bool neg = tok[0] == '-';
      if (!parseNumber(res, i, out.width)) return false;
      out.flip_horizontal = neg; // "-X" rows run right-to-left
      have_w = true;
    } else if (tok.size() == 1 && (tok[0] == 'X' || tok[0] == 'x')) {
      if (!parseNumber(res, i, out.width)) return false;
      have_w = true;
    } else if (tok.size() == 1 && (tok[0] == 'Y' || tok[0] == 'y')) {
      if (!parseNumber(res, i, out.height)) return false;
      have_h = true;
    }
  }

  if (!have_w || !have_h || out.width == 0 || out.height == 0) return false;
  out.data_offset = pos;
  return true;
}

} // namespace hdr
} // namespace openmedia
