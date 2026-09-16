#include "bit_reader.hpp"

#include <algorithm>

namespace openmedia {

auto nalToRbsp(std::span<const uint8_t> nal) -> std::vector<uint8_t> {
  std::vector<uint8_t> rbsp;
  rbsp.reserve(nal.size());
  int zero_count = 0;
  for (uint8_t byte : nal) {
    if (zero_count >= 2 && byte == 0x03) {
      zero_count = 0;
      continue;
    }
    rbsp.push_back(byte);
    zero_count = byte == 0 ? zero_count + 1 : 0;
  }
  return rbsp;
}

auto RbspBuffer::convert(std::span<const uint8_t> nal, size_t max_bytes) -> bool {
  rbsp_.clear();
  escapes_.clear();
  const size_t count = std::min(max_bytes, nal.size());
  rbsp_.reserve(count);
  int zero_count = 0;
  for (size_t i = 0; i < count; ++i) {
    const uint8_t byte = nal[i];
    if (zero_count >= 2 && byte == 0x03) {
      zero_count = 0;
      escapes_.push_back(static_cast<uint32_t>(rbsp_.size()));
      continue;
    }
    rbsp_.push_back(byte);
    zero_count = byte == 0 ? zero_count + 1 : 0;
  }
  complete_ = count == nal.size();
  return complete_;
}

auto RbspBuffer::sourceOffset(size_t rbsp_index) const noexcept -> size_t {
  // Every emulation_prevention_three_byte dropped at or before `rbsp_index`
  // shifts the matching source byte one further along the NAL.
  const auto end = std::upper_bound(escapes_.begin(), escapes_.end(), static_cast<uint32_t>(rbsp_index));
  return rbsp_index + static_cast<size_t>(end - escapes_.begin());
}

} // namespace openmedia
