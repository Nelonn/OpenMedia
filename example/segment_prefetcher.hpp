#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <openmedia/streaming.hpp>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace media {

/**
 * Downloads segments ahead of the demuxer, on its own threads.
 *
 * OpenMedia owns no threads, which is why this lives here rather than in the
 * library: a SegmentedDemuxer never blocks and never fetches anything, it only says
 * what it wants next. The division of labour is then plain:
 *
 *      demux thread                          loader thread(s)
 *   ┌──────────────────────────┐        ┌───────────────────────────┐
 *   │ deliver(demuxer)         │◄───────┤ ready_: {request, bytes}  │
 *   │ request(upcoming(...))   ├───────►│ pending_: requests        │
 *   │ readPacket()             │        │ fetch() on each           │
 *   └──────────────────────────┘        └───────────────────────────┘
 *
 * Only plain data crosses: SegmentRequests out, byte vectors back, through two
 * queues under one mutex. The loader threads never touch the demuxer, so it needs
 * no lock of its own -- and the thread that has a picture to deliver on time is
 * never the thread waiting on a socket.
 *
 * request() and deliver() are called from the demux thread only.
 */
class SegmentPrefetcher {
public:
  explicit SegmentPrefetcher(openmedia::FetchFn fetch, size_t workers = 2)
      : fetch_(std::move(fetch)) {
    for (size_t i = 0; i < std::max<size_t>(workers, 1); ++i) {
      workers_.emplace_back([this](std::stop_token stop) { run(stop); });
    }
  }

  ~SegmentPrefetcher() {
    for (auto& worker : workers_) worker.request_stop();
    // A worker parked on the condition variable has to be woken to see that.
    wake_.notify_all();
  }

  SegmentPrefetcher(const SegmentPrefetcher&) = delete;
  auto operator=(const SegmentPrefetcher&) -> SegmentPrefetcher& = delete;

  /** Queues whatever is not already in flight or already fetched. Plans the
   * demuxer has stopped asking for are simply never queued again. */
  void request(const std::vector<openmedia::SegmentPlan>& plans) {
    if (plans.empty()) return;
    bool added = false;
    {
      const std::lock_guard lock(mutex_);
      for (const openmedia::SegmentPlan& plan : plans) {
        if (known(plan.request)) continue;
        pending_.push_back(plan.request);
        in_flight_.push_back(plan.request);
        added = true;
      }
    }
    if (added) wake_.notify_all();
  }

  /** Hands everything that has arrived to the demuxer, and returns how many.
   * Bytes for a segment the demuxer no longer wants are dropped by it, which is
   * not an error -- a seek can always outrun a download. */
  auto deliver(openmedia::SegmentedDemuxer& demuxer) -> size_t {
    std::deque<Arrived> arrived;
    {
      const std::lock_guard lock(mutex_);
      arrived.swap(ready_);
    }
    for (Arrived& item : arrived) {
      demuxer.appendSegment(item.request, std::move(item.bytes));
    }
    return arrived.size();
  }

  /** Waits until something arrives, or the timeout runs out, or stop is asked for.
   * What the demux thread does when the demuxer says OM_IO_NOT_ENOUGH_DATA and
   * there is nothing else useful to do. */
  void waitForArrival(std::stop_token stop, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    arrived_.wait_for(lock, timeout,
                      [&] { return !ready_.empty() || stop.stop_requested(); });
  }

  /** Forgets everything queued and everything arrived. Called after a seek, when
   * what was being fetched is no longer what is wanted. Downloads already running
   * are allowed to finish and are then thrown away, which costs one segment's
   * bandwidth and saves cancellation machinery nobody would exercise. */
  void reset() {
    const std::lock_guard lock(mutex_);
    pending_.clear();
    ready_.clear();
    in_flight_.clear();
  }

  auto inFlight() const -> size_t {
    const std::lock_guard lock(mutex_);
    return in_flight_.size();
  }

private:
  struct Arrived {
    openmedia::SegmentRequest request;
    std::vector<uint8_t> bytes;
  };

  /** Already queued, being fetched, or fetched and waiting to be handed over. */
  auto known(const openmedia::SegmentRequest& request) const -> bool {
    const auto matches = [&](const openmedia::SegmentRequest& other) {
      return other == request;
    };
    if (std::any_of(in_flight_.begin(), in_flight_.end(), matches)) return true;
    return std::any_of(ready_.begin(), ready_.end(),
                       [&](const Arrived& item) { return matches(item.request); });
  }

  void run(std::stop_token stop) {
    for (;;) {
      openmedia::SegmentRequest request;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [&] { return !pending_.empty() || stop.stop_requested(); });
        if (stop.stop_requested()) return;
        request = std::move(pending_.front());
        pending_.pop_front();
      }

      auto result = fetch_(request);

      const std::lock_guard lock(mutex_);
      const auto still_wanted = std::find_if(
          in_flight_.begin(), in_flight_.end(),
          [&](const openmedia::SegmentRequest& other) { return other == request; });
      // A reset() while this was downloading means nobody is waiting for it.
      if (still_wanted == in_flight_.end()) continue;
      in_flight_.erase(still_wanted);

      if (result.isErr()) {
        // Nothing is retried here: the demuxer will ask for the same segment again
        // on its next upcomingSegments(), and a retry policy belongs where the
        // fetch function is, not in a queue that cannot tell a 404 from a timeout.
        continue;
      }
      ready_.push_back({request, std::move(result).unwrap()});
      arrived_.notify_all();
    }
  }

  openmedia::FetchFn fetch_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;    // pending work for a loader
  std::condition_variable arrived_; // fetched bytes for the demux thread
  std::deque<openmedia::SegmentRequest> pending_;
  std::vector<openmedia::SegmentRequest> in_flight_;
  std::deque<Arrived> ready_;
  std::vector<std::jthread> workers_;
};

} // namespace media
