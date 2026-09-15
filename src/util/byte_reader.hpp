#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <util/io_util.hpp>

namespace openmedia {

// Bounds-checked byte parser. Errors are sticky: an out-of-range read returns 0
// (or an empty span), moves the cursor to the end and clears ok().
class ByteReader {
  std::span<const uint8_t> data_;
  size_t pos_ = 0;
  bool ok_ = true;

public:
  ByteReader() = default;
  explicit ByteReader(std::span<const uint8_t> data) noexcept : data_(data) {}

  auto u8() noexcept -> uint8_t { return canRead(1) ? data_[pos_++] : fail<uint8_t>(); }

  auto u16be() noexcept -> uint16_t { return read<uint16_t, 2>(load_u16_be); }
  auto u24be() noexcept -> uint32_t { return read<uint32_t, 3>(load_u24_be); }
  auto u32be() noexcept -> uint32_t { return read<uint32_t, 4>(load_u32_be); }
  auto u64be() noexcept -> uint64_t { return read<uint64_t, 8>(load_u64_be); }

  auto u16le() noexcept -> uint16_t { return read<uint16_t, 2>(load_u16_le); }
  auto u24le() noexcept -> uint32_t { return read<uint32_t, 3>(load_u24_le); }
  auto u32le() noexcept -> uint32_t { return read<uint32_t, 4>(load_u32_le); }
  auto u64le() noexcept -> uint64_t { return read<uint64_t, 8>(load_u64_le); }

  auto i8() noexcept -> int8_t { return static_cast<int8_t>(u8()); }
  auto i16be() noexcept -> int16_t { return static_cast<int16_t>(u16be()); }
  auto i32be() noexcept -> int32_t { return static_cast<int32_t>(u32be()); }
  auto i64be() noexcept -> int64_t { return static_cast<int64_t>(u64be()); }
  auto i16le() noexcept -> int16_t { return static_cast<int16_t>(u16le()); }
  auto i32le() noexcept -> int32_t { return static_cast<int32_t>(u32le()); }
  auto i64le() noexcept -> int64_t { return static_cast<int64_t>(u64le()); }

  // Zero-copy view of the next `n` bytes; empty on overflow.
  auto bytes(size_t n) noexcept -> std::span<const uint8_t> {
    if (!canRead(n)) return fail<std::span<const uint8_t>>();
    const auto out = data_.subspan(pos_, n);
    pos_ += n;
    return out;
  }

  auto str(size_t n) noexcept -> std::string_view {
    const auto b = bytes(n);
    return {reinterpret_cast<const char*>(b.data()), b.size()};
  }

  // Reader over the next `n` bytes (e.g. a nested box); advances past them.
  auto sub(size_t n) noexcept -> ByteReader {
    ByteReader r(bytes(n));
    r.ok_ = ok_;
    return r;
  }

  auto skip(size_t n) noexcept -> bool {
    if (!canRead(n)) return fail<bool>();
    pos_ += n;
    return true;
  }

  auto seek(size_t offset) noexcept -> bool {
    if (offset > data_.size()) return fail<bool>();
    pos_ = offset;
    return true;
  }

  auto canRead(size_t n) const noexcept -> bool { return n <= remaining(); }
  auto ok() const noexcept -> bool { return ok_; }
  auto empty() const noexcept -> bool { return remaining() == 0; }
  auto tell() const noexcept -> size_t { return pos_; }
  auto remaining() const noexcept -> size_t { return data_.size() - pos_; }
  auto size() const noexcept -> size_t { return data_.size(); }
  auto cur() const noexcept -> const uint8_t* { return data_.data() + pos_; }
  auto data() const noexcept -> std::span<const uint8_t> { return data_; }

private:
  template<class T, size_t N, class Loader>
  auto read(Loader load) noexcept -> T {
    if (!canRead(N)) return fail<T>();
    const T v = load(data_.data() + pos_);
    pos_ += N;
    return v;
  }

  template<class T>
  auto fail() noexcept -> T {
    ok_ = false;
    pos_ = data_.size();
    return T {};
  }
};

} // namespace openmedia
