#pragma once

#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <openmedia/macro.h>
#include <span>
#include <unordered_map>
#include <vector>

namespace openmedia {

class OPENMEDIA_ABI Buffer {
public:
  virtual ~Buffer() = default;
  virtual auto bytes() -> std::span<uint8_t> = 0;
};

// Sized to a size class for reuse, while bytes() exposes exactly what was asked for: callers
// rely on bytes().size() as the payload size.
class OPENMEDIA_ABI PooledBuffer : public Buffer {
  std::unique_ptr<uint8_t[]> storage_;
  size_t capacity_ = 0;
  size_t size_ = 0;

public:
  // Left uninitialised: a recycled block holds stale bytes anyway, and zeroing a 4K
  // picture only to overwrite it costs a pass over 12 MiB.
  PooledBuffer(size_t capacity, size_t size)
      : storage_(std::make_unique_for_overwrite<uint8_t[]>(capacity)), capacity_(capacity), size_(size) {}

  explicit PooledBuffer(size_t size)
      : PooledBuffer(size, size) {}

  auto bytes() -> std::span<uint8_t> override { return {storage_.get(), size_}; }

  auto capacity() const noexcept -> size_t { return capacity_; }

  // Re-target a recycled block at a new logical size. Contents are not cleared.
  auto rebind(size_t size) noexcept -> void { size_ = size; }
};

// Bucketed by size class: payload sizes are effectively unbounded, so exact-size buckets would
// retain one per size ever seen.
//
// A block is cached only once its class has proven recurring demand, more for larger blocks
// (usesRequired), so a one-shot 37 MiB JPEG is never cached while a 4K decode loop is. A class
// untouched for max_idle is dropped. MAX_BUFFERS_PER_CLASS and a byte budget cap the rest.
//
// Sweeps run on get()/release(), so an idle process keeps its cache until clear(); there is no
// reaper thread because joining one from a DLL being unloaded can deadlock the loader.
class OPENMEDIA_ABI BufferPool {
public:
  using Clock = std::chrono::steady_clock;

  static constexpr uint32_t MIN_USES_TO_CACHE = 2;
  static constexpr size_t BYTES_PER_PROVEN_USE = 2ull << 20;
  static constexpr size_t MAX_BUFFERS_PER_CLASS = 8;
  static constexpr size_t DEFAULT_MAX_BYTES = 64ull << 20;
  static constexpr Clock::duration DEFAULT_MAX_IDLE = std::chrono::seconds(2);
  static constexpr Clock::duration SWEEP_INTERVAL = std::chrono::milliseconds(250);
  static constexpr size_t CONTROL_BLOCK_SIZE = 128;
  static constexpr size_t MAX_CONTROL_BLOCKS = 64;

private:
  struct SizeClass {
    std::vector<std::unique_ptr<PooledBuffer>> idle;
    uint32_t uses = 0;
    Clock::time_point last_touch {};
  };

  // Shared with live buffers' deleters, so one outliving the singleton releases safely.
  struct State {
    std::mutex mutex;
    std::unordered_map<size_t, SizeClass> classes;
    size_t cached_bytes = 0;
    size_t max_bytes = DEFAULT_MAX_BYTES;
    Clock::duration max_idle = DEFAULT_MAX_IDLE;
    Clock::time_point last_sweep {};
    std::vector<void*> control_blocks;

    ~State() {
      for (void* block : control_blocks) ::operator delete(block);
    }

    auto takeControlBlock() -> void* {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (!control_blocks.empty()) {
          void* block = control_blocks.back();
          control_blocks.pop_back();
          return block;
        }
      }
      return ::operator new(CONTROL_BLOCK_SIZE);
    }

    auto returnControlBlock(void* block) -> void {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (control_blocks.size() < MAX_CONTROL_BLOCKS) {
          control_blocks.push_back(block);
          return;
        }
      }
      ::operator delete(block);
    }

    // Caller holds the lock.
    auto sweep(Clock::time_point now, bool force) -> void {
      if (!force && now - last_sweep < SWEEP_INTERVAL) {
        return;
      }
      last_sweep = now;
      for (auto it = classes.begin(); it != classes.end();) {
        if (now - it->second.last_touch >= max_idle) {
          cached_bytes -= it->first * it->second.idle.size();
          it = classes.erase(it);
        } else {
          ++it;
        }
      }
    }

    auto release(std::unique_ptr<PooledBuffer> buffer) -> void {
      const size_t capacity = buffer->capacity();

      std::lock_guard<std::mutex> lock(mutex);
      const auto now = Clock::now();
      sweep(now, false);

      auto it = classes.find(capacity);
      if (it == classes.end()) {
        return; // class went cold while this block was in flight
      }

      auto& size_class = it->second;
      size_class.last_touch = now;
      if (size_class.uses < usesRequired(capacity) ||
          size_class.idle.size() >= MAX_BUFFERS_PER_CLASS ||
          cached_bytes + capacity > max_bytes) {
        return; // unproven, or over budget: hand the block back to the allocator
      }

      cached_bytes += capacity;
      size_class.idle.push_back(std::move(buffer));
    }
  };

  // Every buffer handed out needs a shared_ptr control block, and recycling the buffer
  // but not this would still cost one heap allocation per packet.
  template<typename T>
  struct ControlBlockAllocator {
    using value_type = T;

    std::shared_ptr<State> state;

    explicit ControlBlockAllocator(std::shared_ptr<State> owner) noexcept
        : state(std::move(owner)) {}

    template<typename U>
    ControlBlockAllocator(const ControlBlockAllocator<U>& other) noexcept
        : state(other.state) {}

    auto allocate(size_t count) -> T* {
      static_assert(sizeof(T) <= CONTROL_BLOCK_SIZE && alignof(T) <= alignof(std::max_align_t));
      if (count != 1) throw std::bad_alloc();
      return static_cast<T*>(state->takeControlBlock());
    }

    void deallocate(T* block, size_t) noexcept { state->returnControlBlock(block); }

    template<typename U>
    auto operator==(const ControlBlockAllocator<U>& other) const noexcept -> bool {
      return state == other.state;
    }
  };

  std::shared_ptr<State> state_ = std::make_shared<State>();

  static auto usesRequired(size_t capacity) noexcept -> uint32_t {
    const size_t scaled = capacity / BYTES_PER_PROVEN_USE;
    return scaled > MIN_USES_TO_CACHE ? static_cast<uint32_t>(scaled) : MIN_USES_TO_CACHE;
  }

  // Quarter-power-of-two granularity below 1 MiB (<=25% overhead), 64 KiB above: large
  // allocations are whole frames, whose sizes repeat exactly.
  static auto sizeClass(size_t size) noexcept -> size_t {
    constexpr size_t MIN_CLASS = 256;
    constexpr size_t LARGE = size_t {1} << 20;
    constexpr size_t LARGE_GRANULARITY = size_t {64} << 10;

    if (size <= MIN_CLASS) {
      return MIN_CLASS;
    }
    if (size >= LARGE) {
      return (size + LARGE_GRANULARITY - 1) & ~(LARGE_GRANULARITY - 1);
    }
    const size_t step = size_t {1} << (std::bit_width(size) - 3);
    return (size + step - 1) & ~(step - 1);
  }

public:
  static auto getInstance() -> BufferPool&;

  auto get(size_t size) -> std::shared_ptr<Buffer> {
    const size_t capacity = sizeClass(size);

    std::unique_ptr<PooledBuffer> buffer;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      const auto now = Clock::now();
      state_->sweep(now, false);

      auto& size_class = state_->classes[capacity];
      size_class.last_touch = now;
      if (size_class.uses < usesRequired(capacity)) {
        ++size_class.uses;
      }
      if (!size_class.idle.empty()) {
        buffer = std::move(size_class.idle.back());
        size_class.idle.pop_back();
        state_->cached_bytes -= capacity;
      }
    }

    if (buffer) {
      buffer->rebind(size);
    } else {
      buffer = std::make_unique<PooledBuffer>(capacity, size);
    }

    return std::shared_ptr<PooledBuffer>(
        buffer.release(),
        [state = state_](PooledBuffer* b) { state->release(std::unique_ptr<PooledBuffer>(b)); },
        ControlBlockAllocator<PooledBuffer>(state_));
  }

  void trim() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->sweep(Clock::now(), true);
  }

  void clear() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->classes.clear();
    state_->cached_bytes = 0;
  }

  void setMaxIdle(Clock::duration idle) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->max_idle = idle;
    state_->sweep(Clock::now(), true);
  }

  void setMaxBytes(size_t bytes) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->max_bytes = bytes;
    for (auto it = state_->classes.begin();
         it != state_->classes.end() && state_->cached_bytes > bytes; ++it) {
      auto& size_class = it->second;
      while (!size_class.idle.empty() && state_->cached_bytes > bytes) {
        size_class.idle.pop_back();
        state_->cached_bytes -= it->first;
      }
    }
  }

  auto cachedBytes() const -> size_t {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->cached_bytes;
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
