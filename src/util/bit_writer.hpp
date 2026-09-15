#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <util/byte_writer.hpp>
#include <util/io_util.hpp>

namespace openmedia {

template<class Sink>
class BasicBitWriter {
  Sink sink_;
  uint64_t acc_ = 0; // low `fill_` bits are pending, higher bits are stale
  uint32_t fill_ = 0; // invariant between calls: fill_ < 32

public:
  template<class Target>
    requires std::constructible_from<Sink, Target&&>
  explicit BasicBitWriter(Target&& target) noexcept : sink_(std::forward<Target>(target)) {}

  // Writes the low `n` bits of `v`, n in [0, 32]. `v` must fit in `n` bits.
  auto bits(uint32_t v, uint32_t n) -> BasicBitWriter& {
    assert(n <= 32);
    assert(n == 32 || (v >> n) == 0);
    acc_ = (acc_ << n) | v;
    fill_ += n;
    if (fill_ >= 32) {
      fill_ -= 32;
      store_u32_be(sink_.grow(4), static_cast<uint32_t>(acc_ >> fill_));
    }
    return *this;
  }

  auto bit(bool v) -> BasicBitWriter& { return bits(v ? 1u : 0u, 1); }

  // n in [0, 64].
  auto bits64(uint64_t v, uint32_t n) -> BasicBitWriter& {
    assert(n <= 64);
    assert(n == 64 || (v >> n) == 0);
    if (n > 32) {
      bits(static_cast<uint32_t>(v >> 32), n - 32);
      return bits(static_cast<uint32_t>(v), 32);
    }
    return bits(static_cast<uint32_t>(v), n);
  }

  // Two's complement value truncated to `n` bits, n in [1, 32].
  auto sbits(int32_t v, uint32_t n) -> BasicBitWriter& {
    assert(n >= 1 && n <= 32);
    assert(n == 32 || (v >= -(int64_t {1} << (n - 1)) && v < (int64_t {1} << (n - 1))));
    const auto mask = static_cast<uint32_t>((uint64_t {1} << n) - 1);
    return bits(static_cast<uint32_t>(v) & mask, n);
  }

  // Unsigned Exp-Golomb, ue(v).
  auto ue(uint32_t v) -> BasicBitWriter& {
    return expGolomb(static_cast<uint64_t>(v) + 1);
  }

  // Signed Exp-Golomb, se(v): 0, 1, -1, 2, -2, ...
  auto se(int32_t v) -> BasicBitWriter& {
    const int64_t w = v;
    return expGolomb(w > 0 ? static_cast<uint64_t>(2 * w) : static_cast<uint64_t>(-2 * w) + 1);
  }

  // Pads with zero bits up to the next byte boundary.
  auto align() -> BasicBitWriter& { return bits(0, (8 - (fill_ & 7)) & 7); }

  // Emits all pending whole bytes. Must be byte-aligned.
  auto flush() -> BasicBitWriter& {
    assert(byteAligned());
    while (fill_ != 0) {
      fill_ -= 8;
      *sink_.grow(1) = static_cast<uint8_t>(acc_ >> fill_);
    }
    return *this;
  }

  // Byte-aligned raw copy.
  auto bytes(std::span<const uint8_t> data) -> BasicBitWriter& {
    flush();
    if (!data.empty()) {
      memcpy(sink_.grow(data.size()), data.data(), data.size());
    }
    return *this;
  }

  auto byteAligned() const noexcept -> bool { return (fill_ & 7) == 0; }
  auto bitPosition() const noexcept -> size_t { return sink_.size() * 8 + fill_; }

private:
  // Writes x (>= 1) as (bit_width(x) - 1) zeros followed by x itself.
  auto expGolomb(uint64_t x) -> BasicBitWriter& {
    const auto len = static_cast<uint32_t>(std::bit_width(x));
    bits(0, len - 1);
    return bits64(x, len);
  }
};

using BitWriter = BasicBitWriter<VectorSink>;
using SpanBitWriter = BasicBitWriter<SpanSink>;

} // namespace openmedia
