#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace openmedia {

auto nalToRbsp(std::span<const uint8_t> nal) -> std::vector<uint8_t>;

class BitReader {
  std::span<const uint8_t> data_;
  size_t bit_offset_ = 0;
  bool ok_ = true;

public:
  BitReader() = default;
  explicit BitReader(std::span<const uint8_t> data) { reset(data); }

  void reset(std::span<const uint8_t> data) noexcept;

  auto ok() const noexcept -> bool { return ok_; }
  auto empty() const noexcept -> bool { return bitsLeft() == 0; }
  auto byteAligned() const noexcept -> bool { return (bit_offset_ & 7u) == 0; }
  auto bitPosition() const noexcept -> size_t { return bit_offset_; }
  auto bytePosition() const noexcept -> size_t { return bit_offset_ >> 3u; }
  auto bitsLeft() const noexcept -> size_t;

  auto peekBit() const -> uint32_t;
  auto readBit() -> uint32_t;
  auto readBits(uint32_t count) -> uint32_t;
  auto readBits64(uint32_t count) -> uint64_t;
  auto readUE() -> uint32_t;
  auto readSE() -> int32_t;

  void skipBits(size_t count);
  void alignToByte();

  auto moreRbspData() const -> bool;

private:
  auto bitAt(size_t offset) const -> uint32_t;
  void fail() noexcept;
};

} // namespace openmedia
