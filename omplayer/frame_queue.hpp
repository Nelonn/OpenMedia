#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>
#include <vector>

// VideoFrame - carries one decoded YUV420p frame plus its PTS (in the
// stream's native timebase units).
struct VideoFrame {
  std::vector<uint8_t> y_plane;
  std::vector<uint8_t> u_plane;
  std::vector<uint8_t> v_plane;
  int y_stride = 0;
  int u_stride = 0;
  int v_stride = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  int64_t pts = 0;      // native timebase units
  double pts_sec = 0.0; // pre-computed seconds (set by decoder)
};

// FrameQueue - bounded MPSC/SPSC queue with flush support.
//
// The worker thread pushes; the render thread pops.
// flush() unblocks any waiting push/pop and drains the queue.
class FrameQueue {
public:
  explicit FrameQueue(size_t capacity = 8)
      : capacity_(capacity) {}

  // Try to push one frame. Returns false if full or flushing.
  // Caller is responsible for back-pressure (sleep + retry loop).
  auto tryPush(VideoFrame frame) -> bool {
    std::lock_guard lock(mutex_);
    if (flushing_ || queue_.size() >= capacity_) return false;
    queue_.push(std::move(frame));
    return true;
  }

  // Peek + conditionally pop in one atomic operation.
  // `decision` receives pts_sec of the front frame; return true to consume.
  // Returns nullopt if the queue is empty or flushing.
  //
  // Usage:
  //   queue.peekPop([&](double pts_sec) {
  //       return pts_sec <= master_sec + kFutureThresh;
  //   });
  template<typename Fn>
  auto peekPop(Fn&& decision) -> std::optional<VideoFrame> {
    std::lock_guard lock(mutex_);
    if (flushing_ || queue_.empty()) return std::nullopt;
    if (!decision(queue_.front().pts_sec)) return std::nullopt;
    VideoFrame vf = std::move(queue_.front());
    queue_.pop();
    return vf;
  }

  // Unconditional non-blocking pop.
  auto tryPop() -> std::optional<VideoFrame> {
    std::lock_guard lock(mutex_);
    if (flushing_ || queue_.empty()) return std::nullopt;
    VideoFrame vf = std::move(queue_.front());
    queue_.pop();
    return vf;
  }

  // Peek at front pts_sec without removing. Only safe to read pts_sec
  // (POD double); do not hold a pointer/ref across unlock.
  auto frontPtsSec() const -> std::optional<double> {
    std::lock_guard lock(mutex_);
    if (queue_.empty()) return std::nullopt;
    return queue_.front().pts_sec;
  }

  void flush() {
    std::lock_guard lock(mutex_);
    flushing_ = true;
    while (!queue_.empty()) queue_.pop();
  }

  void resetFlush() {
    std::lock_guard lock(mutex_);
    flushing_ = false;
  }

  auto size() const -> size_t {
    std::lock_guard lock(mutex_);
    return queue_.size();
  }

  auto capacity() const -> size_t { return capacity_; }

  auto empty() const -> bool {
    std::lock_guard lock(mutex_);
    return queue_.empty();
  }

  auto isFlushing() const -> bool {
    std::lock_guard lock(mutex_);
    return flushing_;
  }

private:
  std::queue<VideoFrame> queue_;
  mutable std::mutex mutex_;
  size_t capacity_;
  bool flushing_ = false;
};
