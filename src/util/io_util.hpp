#pragma once

#include <openmedia/io.hpp>
#include <vector>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstring>
#include <type_traits>

#if defined(_MSC_VER) && !defined(__clang__)
#include <stdlib.h>
#endif

namespace openmedia {

#if __cpp_lib_byteswap >= 202110L
using std::byteswap;
#else
template<std::integral T>
constexpr auto byteswap(T value) noexcept -> T {
  using U = std::make_unsigned_t<T>;
  const auto v = static_cast<U>(value);
  if (!std::is_constant_evaluated()) {
    switch (sizeof(T)) {
#if defined(_MSC_VER) && !defined(__clang__)
      case 2: return static_cast<T>(_byteswap_ushort(static_cast<uint16_t>(v)));
      case 4: return static_cast<T>(_byteswap_ulong(static_cast<unsigned long>(v)));
      case 8: return static_cast<T>(_byteswap_uint64(static_cast<uint64_t>(v)));
#else
      case 2: return static_cast<T>(__builtin_bswap16(static_cast<uint16_t>(v)));
      case 4: return static_cast<T>(__builtin_bswap32(static_cast<uint32_t>(v)));
      case 8: return static_cast<T>(__builtin_bswap64(static_cast<uint64_t>(v)));
#endif
      default: break;
    }
  }
  U r = 0;
  for (size_t i = 0; i < sizeof(T); ++i) {
    r = static_cast<U>((r << 8) | ((v >> (i * 8)) & 0xFF));
  }
  return static_cast<T>(r);
}
#endif

static constexpr auto magic_u32(uint8_t a, uint8_t b, uint8_t c, uint8_t d) noexcept -> uint32_t {
  if constexpr (std::endian::native == std::endian::little) {
    return (static_cast<uint32_t>(a) << 0) |
           (static_cast<uint32_t>(b) << 8) |
           (static_cast<uint32_t>(c) << 16) |
           (static_cast<uint32_t>(d) << 24);
  } else {
    return (static_cast<uint32_t>(a) << 24) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(c) << 8) |
           (static_cast<uint32_t>(d) << 0);
  }
}

static consteval auto magic_u32(const char str[4]) noexcept -> uint32_t {
  return magic_u32(str[0], str[1], str[2], str[3]);
}

namespace detail {

template<std::unsigned_integral T>
inline auto load(const uint8_t* src, std::endian order) noexcept -> T {
  T v;
  memcpy(&v, src, sizeof(T));
  return order == std::endian::native ? v : byteswap(v);
}

template<std::unsigned_integral T>
inline void store(uint8_t* dst, T v, std::endian order) noexcept {
  if (order != std::endian::native) v = byteswap(v);
  memcpy(dst, &v, sizeof(T));
}

} // namespace detail

// Native byte order, e.g. for comparing against magic_u32().
inline auto load_u32(const void* p) noexcept -> uint32_t {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

inline auto load_u16_be(const uint8_t* p) noexcept -> uint16_t { return detail::load<uint16_t>(p, std::endian::big); }
inline auto load_u32_be(const uint8_t* p) noexcept -> uint32_t { return detail::load<uint32_t>(p, std::endian::big); }
inline auto load_u64_be(const uint8_t* p) noexcept -> uint64_t { return detail::load<uint64_t>(p, std::endian::big); }
inline auto load_u16_le(const uint8_t* p) noexcept -> uint16_t { return detail::load<uint16_t>(p, std::endian::little); }
inline auto load_u32_le(const uint8_t* p) noexcept -> uint32_t { return detail::load<uint32_t>(p, std::endian::little); }
inline auto load_u64_le(const uint8_t* p) noexcept -> uint64_t { return detail::load<uint64_t>(p, std::endian::little); }

inline auto load_u24_be(const uint8_t* p) noexcept -> uint32_t {
  return (static_cast<uint32_t>(p[0]) << 16) | load_u16_be(p + 1);
}

inline auto load_u24_le(const uint8_t* p) noexcept -> uint32_t {
  return load_u16_le(p) | (static_cast<uint32_t>(p[2]) << 16);
}

inline void store_u16_be(uint8_t* p, uint16_t v) noexcept { detail::store(p, v, std::endian::big); }
inline void store_u32_be(uint8_t* p, uint32_t v) noexcept { detail::store(p, v, std::endian::big); }
inline void store_u64_be(uint8_t* p, uint64_t v) noexcept { detail::store(p, v, std::endian::big); }
inline void store_u16_le(uint8_t* p, uint16_t v) noexcept { detail::store(p, v, std::endian::little); }
inline void store_u32_le(uint8_t* p, uint32_t v) noexcept { detail::store(p, v, std::endian::little); }
inline void store_u64_le(uint8_t* p, uint64_t v) noexcept { detail::store(p, v, std::endian::little); }

inline void store_u24_be(uint8_t* p, uint32_t v) noexcept {
  assert(v <= 0xFFFFFF);
  p[0] = static_cast<uint8_t>(v >> 16);
  store_u16_be(p + 1, static_cast<uint16_t>(v));
}

inline void store_u24_le(uint8_t* p, uint32_t v) noexcept {
  assert(v <= 0xFFFFFF);
  store_u16_le(p, static_cast<uint16_t>(v));
  p[2] = static_cast<uint8_t>(v >> 16);
}

static auto read_leb128(const uint8_t* data, size_t size, size_t* len) -> uint32_t {
  uint32_t value = 0;
  for (size_t i = 0; i < 8 && i < size; i++) {
    value |= static_cast<uint32_t>(data[i] & 0x7F) << (i * 7);
    if (!(data[i] & 0x80)) {
      if (len) {
        *len = i + 1;
      }
      return value;
    }
  }
  if (len) {
    *len = 0;
  }
  return 0;
}

static void copyPlane(uint8_t* dst, const uint8_t* src, uint32_t width, uint32_t height, ptrdiff_t stride) {
  for (size_t y = 0; y < height; y++) {
    memcpy(dst, src, width);
    dst += width;
    src += stride;
  }
}

static void copyPlane(uint8_t* dst, ptrdiff_t dst_stride, const uint8_t* src, ptrdiff_t src_stride, uint32_t row_bytes, uint32_t height) {
  for (size_t y = 0; y < height; y++) {
    memcpy(dst, src, row_bytes);
    dst += dst_stride;
    src += src_stride;
  }
}

class RandomRead {
private:
  InputStream* input_ = nullptr;
  std::vector<uint8_t> cache_;
  size_t cache_pos_ = 0;
  size_t cache_size_ = 0;
  int64_t stream_size_ = 0;

  static constexpr size_t DEFAULT_CACHE_SIZE = 8192;

  void invalidateCache() {
    cache_size_ = 0;
  }

  auto loadCache(size_t pos) -> bool {
    if (!input_ || pos >= stream_size_) return false;
    invalidateCache();
    if (!input_->seek(pos, Whence::BEG)) return false;
    cache_pos_ = pos;
    const size_t to_read = std::min(DEFAULT_CACHE_SIZE, static_cast<size_t>(stream_size_ - pos));
    const size_t n = input_->read(std::span<uint8_t>(cache_.data(), to_read));
    cache_size_ = n;
    return n > 0;
  }

  auto isInCache(size_t pos, size_t n) const -> bool {
    return pos >= cache_pos_ && pos + n <= cache_pos_ + cache_size_;
  }

public:
  explicit RandomRead(InputStream* input = nullptr) : input_(input) {
    if (input_) {
      stream_size_ = input_->size();
      cache_.resize(DEFAULT_CACHE_SIZE);
    }
  }

  auto ok() const -> bool { return input_ != nullptr; }

  auto size() const -> size_t { return stream_size_; }

  auto read(size_t pos, void* dst, size_t n) -> bool;

  auto read(size_t pos, std::span<uint8_t> dst) -> bool {
    return read(pos, dst.data(), dst.size());
  }

  auto readBuf(size_t pos, size_t size) -> std::vector<uint8_t> {
    std::vector<uint8_t> buf(size);
    if (!read(pos, buf.data(), size)) {
      buf.clear();
    }
    return buf;
  }
};

} // namespace openmedia
