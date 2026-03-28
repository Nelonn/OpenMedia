#pragma once

// lock-based, single-producer / single-consumer safe.
class RingBuffer {
public:
  explicit RingBuffer(size_t capacity)
      : buf_(capacity), capacity_(capacity) {}

  // Returns bytes actually written (may be less than len if nearly full).
  auto write(const uint8_t* data, size_t len) -> size_t {
    std::lock_guard lock(mutex_);
    const size_t n = std::min(len, availableWriteUnsafe());
    for (size_t i = 0; i < n; ++i)
      buf_[(write_pos_ + i) % capacity_] = data[i];
    write_pos_ += n;
    return n;
  }

  auto read(uint8_t* dst, size_t len) -> size_t {
    std::lock_guard lock(mutex_);
    const size_t n = std::min(len, sizeUnsafe());
    for (size_t i = 0; i < n; ++i)
      dst[i] = buf_[(read_pos_ + i) % capacity_];
    read_pos_ += n;
    return n;
  }

  void clear() {
    std::lock_guard lock(mutex_);
    read_pos_ = write_pos_;
  }

  auto currentSize() const -> size_t {
    std::lock_guard lock(mutex_);
    return sizeUnsafe();
  }

  auto capacity() const -> size_t { return capacity_; }

  auto availableWrite() const -> size_t {
    std::lock_guard lock(mutex_);
    return availableWriteUnsafe();
  }

  auto fillRatio() const -> double {
    std::lock_guard lock(mutex_);
    return capacity_ > 0
               ? static_cast<double>(sizeUnsafe()) / static_cast<double>(capacity_)
               : 0.0;
  }

private:
  auto sizeUnsafe() const -> size_t { return write_pos_ - read_pos_; }
  auto availableWriteUnsafe() const -> size_t { return capacity_ - sizeUnsafe(); }

  std::vector<uint8_t> buf_;
  size_t capacity_;
  mutable std::mutex mutex_;
  size_t read_pos_ = 0;
  size_t write_pos_ = 0;
};
