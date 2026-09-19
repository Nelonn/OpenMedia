#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <vector>

// Byte FIFO of fixed capacity, safe for one producer and one consumer.
class RingBuffer {
public:
  explicit RingBuffer(size_t capacity) : buf_(capacity) {}

  // Returns how many bytes fit; the rest is left to the caller.
  auto write(std::span<const uint8_t> src) -> size_t {
    std::lock_guard lock(mutex_);
    const size_t n = std::min(src.size(), buf_.size() - size_);
    const size_t at = (head_ + size_) % buf_.size();
    const size_t first = std::min(n, buf_.size() - at);
    std::memcpy(buf_.data() + at, src.data(), first);
    std::memcpy(buf_.data(), src.data() + first, n - first);
    size_ += n;
    return n;
  }

  auto read(std::span<uint8_t> dst) -> size_t {
    std::lock_guard lock(mutex_);
    const size_t n = std::min(dst.size(), size_);
    const size_t first = std::min(n, buf_.size() - head_);
    std::memcpy(dst.data(), buf_.data() + head_, first);
    std::memcpy(dst.data() + first, buf_.data(), n - first);
    head_ = (head_ + n) % buf_.size();
    size_ -= n;
    return n;
  }

  void clear() {
    std::lock_guard lock(mutex_);
    head_ = size_ = 0;
  }

  auto size() const -> size_t {
    std::lock_guard lock(mutex_);
    return size_;
  }

  auto fillRatio() const -> double { return static_cast<double>(size()) / static_cast<double>(buf_.size()); }

private:
  std::vector<uint8_t> buf_;
  mutable std::mutex mutex_;
  size_t head_ = 0;
  size_t size_ = 0;
};
