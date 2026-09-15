#include "bit_reader.hpp"

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

} // namespace openmedia
