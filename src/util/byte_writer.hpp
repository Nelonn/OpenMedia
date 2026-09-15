#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>
#include <util/io_util.hpp>
#include <vector>

namespace openmedia {

class VectorSink {
  std::vector<uint8_t>* vec_;

public:
  VectorSink(std::vector<uint8_t>& vec) noexcept : vec_(&vec) {}

  auto grow(size_t n) -> uint8_t* {
    const size_t old = vec_->size();
    vec_->resize(old + n);
    return vec_->data() + old;
  }

  auto size() const noexcept -> size_t { return vec_->size(); }
  auto data() noexcept -> uint8_t* { return vec_->data(); }
};

// Writes into a caller-provided fixed buffer. Overflow is a programming error
// (the caller sizes the buffer), so it is only checked in debug builds.
class SpanSink {
  std::span<uint8_t> buf_;
  size_t pos_ = 0;

public:
  SpanSink(std::span<uint8_t> buf) noexcept : buf_(buf) {}

  auto grow(size_t n) noexcept -> uint8_t* {
    assert(n <= buf_.size() - pos_);
    uint8_t* p = buf_.data() + pos_;
    pos_ += n;
    return p;
  }

  auto size() const noexcept -> size_t { return pos_; }
  auto data() noexcept -> uint8_t* { return buf_.data(); }
};

template<class Sink>
class BasicByteWriter {
  Sink sink_;

public:
  template<class Target>
    requires std::constructible_from<Sink, Target&&>
  explicit BasicByteWriter(Target&& target) noexcept : sink_(std::forward<Target>(target)) {}

  auto u8(uint8_t v) -> BasicByteWriter& { *sink_.grow(1) = v; return *this; }

  auto u16be(uint16_t v) -> BasicByteWriter& { store_u16_be(sink_.grow(2), v); return *this; }
  auto u24be(uint32_t v) -> BasicByteWriter& { store_u24_be(sink_.grow(3), v); return *this; }
  auto u32be(uint32_t v) -> BasicByteWriter& { store_u32_be(sink_.grow(4), v); return *this; }
  auto u64be(uint64_t v) -> BasicByteWriter& { store_u64_be(sink_.grow(8), v); return *this; }

  auto u16le(uint16_t v) -> BasicByteWriter& { store_u16_le(sink_.grow(2), v); return *this; }
  auto u24le(uint32_t v) -> BasicByteWriter& { store_u24_le(sink_.grow(3), v); return *this; }
  auto u32le(uint32_t v) -> BasicByteWriter& { store_u32_le(sink_.grow(4), v); return *this; }
  auto u64le(uint64_t v) -> BasicByteWriter& { store_u64_le(sink_.grow(8), v); return *this; }

  auto bytes(std::span<const uint8_t> data) -> BasicByteWriter& {
    if (!data.empty()) std::memcpy(sink_.grow(data.size()), data.data(), data.size());
    return *this;
  }

  auto str(std::string_view s) -> BasicByteWriter& {
    return bytes({reinterpret_cast<const uint8_t*>(s.data()), s.size()});
  }

  auto zeros(size_t n) -> BasicByteWriter& {
    if (n != 0) std::memset(sink_.grow(n), 0, n);
    return *this;
  }

  // Reserves `n` zeroed bytes to be patched later via at(); returns their offset.
  auto reserve(size_t n) -> size_t {
    const size_t offset = sink_.size();
    zeros(n);
    return offset;
  }

  // Pointer to an already written byte. Invalidated by further writes into a
  // vector sink, so patch right away.
  auto at(size_t offset) noexcept -> uint8_t* {
    assert(offset <= sink_.size());
    return sink_.data() + offset;
  }

  auto size() const noexcept -> size_t { return sink_.size(); }
};

using ByteWriter = BasicByteWriter<VectorSink>;
using SpanWriter = BasicByteWriter<SpanSink>;

} // namespace openmedia
