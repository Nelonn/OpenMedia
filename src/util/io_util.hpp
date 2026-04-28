#pragma once

#include <openmedia/io.hpp>
#include <vector>
#include <bit>
#include <cstring>

namespace openmedia {

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

static auto load_u32(const void* p) -> uint32_t {
  uint32_t val;
  memcpy(&val, p, 4);
  return val;
}

static auto load_u16_be(const uint8_t* p) -> uint32_t {
  return (static_cast<uint32_t>(p[0]) << 8) |
         static_cast<uint32_t>(p[1]);
}

static auto read_u24_be(const uint8_t* p) -> uint32_t {
  return (static_cast<uint32_t>(p[0]) << 16) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]));
}

static auto load_u32_be(const uint8_t* p) -> uint32_t {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) |
         static_cast<uint32_t>(p[3]);
}

static auto load_u64_be(const uint8_t* p) -> uint64_t {
  return (static_cast<uint64_t>(load_u32_be(p)) << 32) | load_u32_be(p + 4);
}

static auto load_u16_le(const uint8_t* p) -> uint16_t {
  return (static_cast<uint32_t>(p[0])) |
         (static_cast<uint32_t>(p[1]) << 8);
}

static auto load_u24_le(const uint8_t* p) -> uint32_t {
  return (static_cast<uint32_t>(p[0])) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16);
}

static auto load_u32_le(const uint8_t* p) -> uint32_t {
  return (static_cast<uint32_t>(p[0])) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

static void copyPlane(uint8_t* dst, const uint8_t* src, uint32_t width, uint32_t height, ptrdiff_t stride) {
  for (size_t y = 0; y < height; y++) {
    memcpy(dst, src, width);
    dst += width;
    src += stride;
  }
}

class MemoryBitReader {
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  size_t byte_ = 0;
  uint8_t bit_ = 0; // [0..7], MSB first

public:
  // Full initialisation: sets data pointer AND resets position.
  void init(const uint8_t* data, size_t size, size_t start_byte = 0) {
    data_ = data;
    size_ = size;
    byte_ = start_byte;
    bit_ = 0;
  }

  void repoint(const uint8_t* data, size_t size) {
    data_ = data;
    size_ = size;
    // byte_ and bit_ are intentionally left unchanged.
  }

  auto eof() const -> bool { return byte_ >= size_; }

  // Current read position in bits from the beginning of the buffer.
  auto bitPos() const -> size_t { return byte_ * 8 + bit_; }

  // Current read position in bytes (rounded down).
  auto bytePos() const -> size_t { return byte_; }

  // Read up to 32 bits, MSB first.  Returns 0 on EOF.
  auto readBits(int n) -> uint32_t {
    uint32_t val = 0;
    for (int i = 0; i < n; i++) {
      if (byte_ >= size_) return val;
      val <<= 1;
      val |= (data_[byte_] >> (7 - bit_)) & 1;
      if (++bit_ == 8) {
        bit_ = 0;
        ++byte_;
      }
    }
    return val;
  }

  // Skip n bits.
  void skipBits(int64_t n) {
    if (n <= 0) return;
    // Fast-path: skip whole bytes when bit-aligned.
    if (bit_ == 0 && n >= 8) {
      int64_t whole = n / 8;
      n %= 8;
      byte_ += static_cast<size_t>(whole);
      if (byte_ > size_) byte_ = size_;
    }
    for (int64_t i = 0; i < n; i++) readBits(1);
  }

  void alignToByte() {
    if (bit_ != 0) {
      bit_ = 0;
      ++byte_;
    }
  }
};

class BufReader {
private:
  const uint8_t* data_;
  size_t size_;
  size_t pos_;
  bool overflow_ = false;

public:
  BufReader(const uint8_t* data, size_t size)
      : data_(data), size_(size), pos_(0) {}

  auto ok() const -> bool {
    return !overflow_;
  }

  auto can_read(size_t n) -> bool {
    overflow_ = true;
    return pos_ + n <= size_;
  }

  auto read_u8() -> uint8_t {
    if (pos_ + 1 > size_) {
      overflow_ = true;
      return 0;
    }
    return data_[pos_++];
  }
  auto read_u16_be() -> uint16_t {
    if (!can_read(2)) return 0;
    uint16_t v = load_u16_be(data_ + pos_);
    pos_ += 2;
    return v;
  }
  auto read_u32_be() -> uint32_t {
    if (!can_read(4)) return 0;
    uint32_t v = load_u32_be(data_ + pos_);
    pos_ += 4;
    return v;
  }
  auto read_u32_le() -> uint32_t {
    if (!can_read(4)) return 0;
    uint32_t v = load_u32_le(data_ + pos_);
    pos_ += 4;
    return v;
  }
  auto read_i32_be() -> int32_t {
    return static_cast<int32_t>(read_u32_be());
  }
  auto read_u64_be() -> uint64_t {
    if (!can_read(8)) return 0;
    uint64_t v = load_u64_be(data_ + pos_);
    pos_ += 8;
    return v;
  }
  auto read_i64_be() -> int64_t {
    return static_cast<int64_t>(read_u64_be());
  }

  auto skip(size_t n) -> bool {
    if (!can_read(n)) {
      pos_ = size_;
      return false;
    }
    pos_ += n;
    return true;
  }
  auto seek(size_t off) -> bool {
    if (off > size_) return false;
    pos_ = off;
    return true;
  }

  auto tell() const -> size_t { return pos_; }
  auto remaining() const -> size_t { return size_ - pos_; }
  auto cur() const -> const uint8_t* { return data_ + pos_; }
  auto base() const -> const uint8_t* { return data_; }
  auto size() const -> size_t { return size_; }

  auto read_bytes(size_t n) -> std::vector<uint8_t> {
    n = std::min(n, remaining());
    std::vector<uint8_t> out(data_ + pos_, data_ + pos_ + n);
    pos_ += n;
    return out;
  }
};

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
