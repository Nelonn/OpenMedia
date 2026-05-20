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

void BitReader::reset(std::span<const uint8_t> data) noexcept {
  data_ = data;
  bit_offset_ = 0;
  ok_ = true;
}

auto BitReader::bitsLeft() const noexcept -> size_t {
  const size_t total = data_.size() * 8u;
  return bit_offset_ < total ? total - bit_offset_ : 0;
}

auto BitReader::bitAt(size_t offset) const -> uint32_t {
  const uint8_t byte = data_[offset >> 3u];
  return (byte >> (7u - (offset & 7u))) & 1u;
}

void BitReader::fail() noexcept {
  ok_ = false;
  bit_offset_ = data_.size() * 8u;
}

auto BitReader::peekBit() const -> uint32_t {
  if (bitsLeft() == 0) return 0;
  return bitAt(bit_offset_);
}

auto BitReader::readBit() -> uint32_t {
  if (bitsLeft() == 0) {
    fail();
    return 0;
  }
  const uint32_t bit = bitAt(bit_offset_);
  ++bit_offset_;
  return bit;
}

auto BitReader::readBits(uint32_t count) -> uint32_t {
  if (count > 32 || bitsLeft() < count) {
    fail();
    return 0;
  }
  uint32_t value = 0;
  for (uint32_t i = 0; i < count; ++i) value = (value << 1u) | bitAt(bit_offset_++);
  return value;
}

auto BitReader::readBits64(uint32_t count) -> uint64_t {
  if (count > 64 || bitsLeft() < count) {
    fail();
    return 0;
  }
  uint64_t value = 0;
  for (uint32_t i = 0; i < count; ++i) value = (value << 1u) | bitAt(bit_offset_++);
  return value;
}

void BitReader::skipBits(size_t count) {
  if (bitsLeft() < count) {
    fail();
    return;
  }
  bit_offset_ += count;
}

void BitReader::alignToByte() {
  const size_t remainder = bit_offset_ & 7u;
  if (remainder != 0) skipBits(8u - remainder);
}

auto BitReader::readUE() -> uint32_t {
  uint32_t leading_zero_bits = 0;
  while (bitsLeft() > 0 && peekBit() == 0) {
    skipBits(1);
    ++leading_zero_bits;
    if (leading_zero_bits >= 32) {
      fail();
      return 0;
    }
  }
  if (bitsLeft() == 0) {
    fail();
    return 0;
  }
  skipBits(1);
  if (leading_zero_bits == 0) return 0;
  return ((1u << leading_zero_bits) - 1u) + readBits(leading_zero_bits);
}

auto BitReader::readSE() -> int32_t {
  const uint32_t code_num = readUE();
  if (!ok_) return 0;
  const int32_t value = static_cast<int32_t>((code_num + 1u) >> 1u);
  return (code_num & 1u) != 0 ? value : -value;
}

auto BitReader::moreRbspData() const -> bool {
  if (bitsLeft() == 0) return false;
  if (bitAt(bit_offset_) == 0) return true;
  for (size_t pos = bit_offset_ + 1; pos < data_.size() * 8u; ++pos) {
    if (bitAt(pos) != 0) return true;
  }
  return false;
}

} // namespace openmedia
