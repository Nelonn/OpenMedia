#pragma once

#include <memory>
#include <mutex>
#include <openmedia/macro.h>
#include <span>
#include <stack>
#include <vector>
#include <unordered_map>

namespace openmedia {

class OPENMEDIA_ABI Buffer {
public:
  virtual ~Buffer() = default;
  virtual auto bytes() -> std::span<uint8_t> = 0;
};

class OPENMEDIA_ABI PooledBuffer : public Buffer {
  std::vector<uint8_t> storage_;
public:
  explicit PooledBuffer(size_t size)
      : storage_(size) {}
  auto bytes() -> std::span<uint8_t> override { return storage_; }
};

class OPENMEDIA_ABI BufferPool {
  std::unordered_map<size_t, std::stack<std::unique_ptr<PooledBuffer>>> pools_;
  std::mutex mutex_;

public:
  static auto getInstance() -> BufferPool& {
    static BufferPool instance;
    return instance;
  }

  auto get(size_t size) -> std::shared_ptr<Buffer> {
    std::lock_guard<std::mutex> lock(mutex_);

    auto& stack = pools_[size];
    if (!stack.empty()) {
      auto buf = std::move(stack.top());
      stack.pop();
      return std::shared_ptr<PooledBuffer>(buf.release(), [size](PooledBuffer* b) {
        BufferPool::getInstance().release(size, b);
      });
    }

    return std::shared_ptr<PooledBuffer>(new PooledBuffer(size), [size](PooledBuffer* b) {
      BufferPool::getInstance().release(size, b);
    });
  }

private:
  void release(size_t size, PooledBuffer* buffer) {
    std::lock_guard<std::mutex> lock(mutex_);
    pools_[size].push(std::unique_ptr<PooledBuffer>(buffer));
  }
};

template<size_t max_planes>
struct PlaneSpan {
  size_t count = 0;
  uint8_t* data[max_planes] = {};
  uint32_t linesize[max_planes] = {};

  constexpr PlaneSpan() = default;
  ~PlaneSpan() = default;

  constexpr auto empty() const noexcept -> bool {
    return count == 0;
  }

  constexpr auto setData(size_t plane_idx, uint8_t* ptr, uint32_t stride) noexcept -> void {
    if (plane_idx < max_planes) {
      data[plane_idx] = ptr;
      linesize[plane_idx] = stride;
      if (plane_idx >= count) {
        count = plane_idx + 1;
      }
    }
  }

  constexpr auto getData(size_t plane_idx) const noexcept -> uint8_t* {
    return (plane_idx < max_planes) ? data[plane_idx] : nullptr;
  }

  constexpr auto getLinesize(size_t plane_idx) const noexcept -> uint32_t {
    return (plane_idx < max_planes) ? linesize[plane_idx] : 0;
  }

  constexpr auto getPlaneCount() const noexcept -> size_t {
    return count;
  }

  constexpr auto getSpan(size_t plane_idx, size_t size) noexcept -> std::span<uint8_t> {
    if (plane_idx >= count || !data[plane_idx]) {
      return {};
    }
    return std::span<uint8_t>(data[plane_idx], size);
  }

  template<typename T>
  constexpr auto getSpan(size_t plane_idx, size_t elements) noexcept -> std::span<T> {
    if (plane_idx >= count || !data[plane_idx]) {
      return {};
    }
    return std::span<T>(reinterpret_cast<T*>(data[plane_idx]), elements);
  }
};

} // namespace openmedia
