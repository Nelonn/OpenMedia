#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>
#include <vector>

// ---------------------------------------------------------------------------
// VideoFrame
// ---------------------------------------------------------------------------
struct VideoFrame {
    std::vector<uint8_t> y_plane;
    std::vector<uint8_t> u_plane;
    std::vector<uint8_t> v_plane;
    int      y_stride = 0;
    int      u_stride = 0;
    int      v_stride = 0;
    uint32_t width    = 0;
    uint32_t height   = 0;
    int64_t  pts      = 0;      // native timebase units
    double   pts_sec  = 0.0;    // pre-computed seconds (set by decoder)
};

// ---------------------------------------------------------------------------
// FrameQueue
//
// ffplay-style bounded queue used between the decoder thread (producer) and
// the render thread (consumer).
//
// Key design differences from the original spin-loop version:
//
//   1. blockingPush() — producer BLOCKS on a condition variable when the queue
//      is full rather than spinning with sleep(2 ms).  It wakes immediately
//      when a slot is freed OR when abort() is called.
//
//   2. blockingPop() — consumer BLOCKS when the queue is empty, waking on new
//      data or abort().
//
//   3. abort() — sets an abort flag, broadcasts on both CVs, and drains the
//      queue.  Any thread currently sleeping inside blockingPush/blockingPop
//      returns immediately with false / nullopt.
//
//   4. reset() — clears the abort flag so the queue can be reused after a
//      seek without reconstructing the object.
//
// The original tryPush / tryPop / peekPop helpers are kept for callers that
// still want non-blocking behaviour.
// ---------------------------------------------------------------------------
class FrameQueue {
public:
    explicit FrameQueue(size_t capacity = 8) : capacity_(capacity) {}

    // -----------------------------------------------------------------------
    // Blocking interface (ffplay-style)
    // -----------------------------------------------------------------------

    // Block until there is room and push, or until abort().
    // Returns true on success, false if aborted.
    auto blockingPush(VideoFrame frame) -> bool {
        std::unique_lock lock(mutex_);
        not_full_cv_.wait(lock, [&] {
            return aborted_ || queue_.size() < capacity_;
        });
        if (aborted_) return false;
        queue_.push(std::move(frame));
        not_empty_cv_.notify_one();
        return true;
    }

    // Block until a frame is available and pop it, or until abort().
    // Returns nullopt if aborted.
    auto blockingPop() -> std::optional<VideoFrame> {
        std::unique_lock lock(mutex_);
        not_empty_cv_.wait(lock, [&] {
            return aborted_ || !queue_.empty();
        });
        if (aborted_ && queue_.empty()) return std::nullopt;
        VideoFrame vf = std::move(queue_.front());
        queue_.pop();
        not_full_cv_.notify_one();
        return vf;
    }

    // Peek-then-conditionally-pop in one lock acquisition.
    // `decision` receives pts_sec of the front frame; return true to consume.
    // Returns nullopt when empty, aborted, or decision() returns false.
    template<typename Fn>
    auto peekPop(Fn&& decision) -> std::optional<VideoFrame> {
        std::lock_guard lock(mutex_);
        if (aborted_ || queue_.empty()) return std::nullopt;
        if (!decision(queue_.front().pts_sec)) return std::nullopt;
        VideoFrame vf = std::move(queue_.front());
        queue_.pop();
        not_full_cv_.notify_one();
        return vf;
    }

    // -----------------------------------------------------------------------
    // Non-blocking interface (kept for compatibility)
    // -----------------------------------------------------------------------

    auto tryPush(VideoFrame frame) -> bool {
        std::lock_guard lock(mutex_);
        if (aborted_ || queue_.size() >= capacity_) return false;
        queue_.push(std::move(frame));
        not_empty_cv_.notify_one();
        return true;
    }

    auto tryPop() -> std::optional<VideoFrame> {
        std::lock_guard lock(mutex_);
        if (aborted_ || queue_.empty()) return std::nullopt;
        VideoFrame vf = std::move(queue_.front());
        queue_.pop();
        not_full_cv_.notify_one();
        return vf;
    }

    auto frontPtsSec() const -> std::optional<double> {
        std::lock_guard lock(mutex_);
        if (queue_.empty()) return std::nullopt;
        return queue_.front().pts_sec;
    }

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    // Abort all blocked callers and drain the queue.
    // Maps to ffplay's packet_queue_abort().
    void abort() {
        std::lock_guard lock(mutex_);
        aborted_ = true;
        while (!queue_.empty()) queue_.pop();
        not_empty_cv_.notify_all();
        not_full_cv_.notify_all();
    }

    // Clear the abort flag and make the queue ready for reuse (post-seek).
    // Maps to ffplay's packet_queue_start().
    void reset() {
        std::lock_guard lock(mutex_);
        aborted_ = false;
        while (!queue_.empty()) queue_.pop();
    }

    // Legacy aliases used by the existing MediaPlayer code.
    void flush()      { abort(); }
    void resetFlush() { reset(); }

    auto isFlushing() const -> bool {
        std::lock_guard lock(mutex_);
        return aborted_;
    }

    // -----------------------------------------------------------------------
    // Queries
    // -----------------------------------------------------------------------

    auto size() const -> size_t {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

    auto capacity() const -> size_t { return capacity_; }

    auto empty() const -> bool {
        std::lock_guard lock(mutex_);
        return queue_.empty();
    }

private:
    std::queue<VideoFrame>  queue_;
    mutable std::mutex      mutex_;
    std::condition_variable not_full_cv_;
    std::condition_variable not_empty_cv_;
    size_t                  capacity_;
    bool                    aborted_ = false;
};
