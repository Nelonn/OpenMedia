#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <util/io_util.hpp>
#include <vector>

namespace openmedia {

auto nalToRbsp(std::span<const uint8_t> nal) -> std::vector<uint8_t>;

// Bounds-checked MSB-first bit parser. Each read is O(1) via a 64-bit window.
// Errors are sticky, same as ByteReader.
class BitReader {
  std::span<const uint8_t> data_;
  size_t bit_offset_ = 0;
  bool ok_ = true;

public:
  BitReader() = default;
  explicit BitReader(std::span<const uint8_t> data) noexcept { reset(data); }

  void reset(std::span<const uint8_t> data) noexcept {
    data_ = data;
    bit_offset_ = 0;
    ok_ = true;
  }

  // Swaps the underlying buffer but keeps the position, e.g. after the
  // storage behind the span has grown and reallocated.
  void rebind(std::span<const uint8_t> data) noexcept { data_ = data; }

  auto ok() const noexcept -> bool { return ok_; }
  auto empty() const noexcept -> bool { return bitsLeft() == 0; }
  auto byteAligned() const noexcept -> bool { return (bit_offset_ & 7u) == 0; }
  auto bitPosition() const noexcept -> size_t { return bit_offset_; }
  auto bytePosition() const noexcept -> size_t { return bit_offset_ >> 3u; }

  auto bitsLeft() const noexcept -> size_t {
    const size_t total = data_.size() * 8u;
    return bit_offset_ < total ? total - bit_offset_ : 0;
  }

  // Next `count` bits (<= 32) without consuming them; missing bits read as 0.
  auto peekBits(uint32_t count) const noexcept -> uint32_t {
    assert(count <= 32);
    if (count == 0) return 0;
    return static_cast<uint32_t>((window() << (bit_offset_ & 7u)) >> (64u - count));
  }

  auto peekBit() const noexcept -> uint32_t { return peekBits(1); }

  auto readBit() noexcept -> uint32_t { return readBits(1); }

  auto readFlag() noexcept -> bool { return readBits(1) != 0; }

  auto readBits(uint32_t count) noexcept -> uint32_t {
    if (count > 32 || bitsLeft() < count) return fail();
    const uint32_t v = peekBits(count);
    bit_offset_ += count;
    return v;
  }

  auto readBits64(uint32_t count) noexcept -> uint64_t {
    if (count > 64 || bitsLeft() < count) return fail();
    if (count <= 32) return readBits(count);
    const uint64_t hi = readBits(count - 32);
    return (hi << 32) | readBits(32);
  }

  // Two's complement field of `count` bits (1..32), sign-extended.
  auto readSigned(uint32_t count) noexcept -> int32_t {
    if (count == 0 || count > 32) return static_cast<int32_t>(fail());
    const uint32_t v = readBits(count);
    const uint32_t shift = 32u - count;
    return static_cast<int32_t>(v << shift) >> shift;
  }

  // Unsigned Exp-Golomb, ue(v). Codes longer than 32 bits are rejected.
  auto readUE() noexcept -> uint32_t {
    const uint32_t leading_zeros = static_cast<uint32_t>(std::countl_zero(peekBits(32)));
    if (leading_zeros >= 32) return fail();
    skipBits(leading_zeros);
    const uint32_t code = readBits(leading_zeros + 1); // top bit is always set, 0 only on failure
    return code != 0 ? code - 1u : 0;
  }

  // Signed Exp-Golomb, se(v): 0, 1, -1, 2, -2, ...
  auto readSE() noexcept -> int32_t {
    const uint64_t code = readUE();
    const auto magnitude = static_cast<int64_t>((code + 1) >> 1);
    return static_cast<int32_t>((code & 1u) != 0 ? magnitude : -magnitude);
  }

  void skipBits(size_t count) noexcept {
    if (bitsLeft() < count) {
      fail();
      return;
    }
    bit_offset_ += count;
  }

  void alignToByte() noexcept {
    skipBits((8u - (bit_offset_ & 7u)) & 7u);
  }

  // H.264/H.265 more_rbsp_data(): true while there is data before the
  // rbsp_stop_one_bit (the last set bit of the payload).
  auto moreRbspData() const noexcept -> bool {
    size_t last = data_.size();
    while (last > 0 && data_[last - 1] == 0) --last;
    if (last == 0) return false;
    const size_t stop_bit = (last - 1) * 8u + 7u - static_cast<size_t>(std::countr_zero(data_[last - 1]));
    return bit_offset_ < stop_bit;
  }

private:
  // Up to 64 bits starting at the current byte, zero-padded past the end.
  auto window() const noexcept -> uint64_t {
    const size_t byte = bit_offset_ >> 3u;
    if (byte + 8 <= data_.size()) [[likely]] {
      return load_u64_be(data_.data() + byte);
    }
    uint8_t tail[8] = {};
    if (byte < data_.size()) std::memcpy(tail, data_.data() + byte, data_.size() - byte);
    return load_u64_be(tail);
  }

  auto fail() noexcept -> uint32_t {
    ok_ = false;
    bit_offset_ = data_.size() * 8u;
    return 0;
  }
};

} // namespace openmedia
