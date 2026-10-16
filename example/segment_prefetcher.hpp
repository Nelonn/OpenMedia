#pragma once

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
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
 * The segment wanted soonest is the exception, and the reason for `stream_fetch`:
 * that one is handed over in pieces as it downloads rather than when it is whole,
 * so its first picture costs its first few hundred kilobytes instead of all of it.
 * At 4K, where a segment runs to tens of megabytes, that is the difference between
 * playing and waiting. Everything behind it is still fetched whole, because a
 * segment that is not being read yet gains nothing from arriving in pieces.
 *
 * request() and deliver() are called from the demux thread only.
 */
class SegmentPrefetcher {
public:
  explicit SegmentPrefetcher(openmedia::FetchFn fetch,
                             openmedia::StreamFetchFn stream_fetch = {}, size_t workers = 2)
      : fetch_(std::move(fetch)), stream_fetch_(std::move(stream_fetch)) {
    for (size_t i = 0; i < std::max<size_t>(workers, 1); ++i) {
      workers_.emplace_back([this](std::stop_token stop) { run(stop); });
    }
  }

  ~SegmentPrefetcher() {
    for (auto& worker : workers_) worker.request_stop();
    // A worker parked on either condition variable has to be woken to see that.
    wake_.notify_all();
    drained_.notify_all();
  }

  SegmentPrefetcher(const SegmentPrefetcher&) = delete;
  auto operator=(const SegmentPrefetcher&) -> SegmentPrefetcher& = delete;

  /** Queues whatever is not already in flight or already fetched. Plans the
   * demuxer has stopped asking for are simply never queued again.
   *
   * `plans` comes back nearest first, so the head of it is what readPacket() is
   * waiting on and the one worth reading in pieces.
   */
  void request(const std::vector<openmedia::SegmentPlan>& plans) {
    if (plans.empty()) return;
    bool added = false;
    {
      const std::lock_guard lock(mutex_);
      for (size_t i = 0; i < plans.size(); ++i) {
        const openmedia::SegmentPlan& plan = plans[i];
        if (known(plan.request)) continue;
        // An initialization segment is wanted whole -- it is where the track list
        // comes from, and half a track list is not one -- and so is anything behind
        // the segment being read.
        const bool piecewise = i == 0 && !plan.initialization && stream_fetch_ && !piecewise_;
        if (piecewise) piecewise_ = plan.request;
        pending_.push_back({plan.request, piecewise});
        in_flight_.push_back(plan.request);
        added = true;
      }
    }
    if (added) wake_.notify_all();
  }

  /** Hands everything that has arrived to the demuxer, and returns how many pieces
   * and whole segments that was. Bytes for a segment the demuxer no longer wants
   * are dropped by it, which is not an error -- a seek can always outrun a
   * download. */
  auto deliver(openmedia::SegmentedDemuxer& demuxer) -> size_t {
    size_t handed = 0;

    // Pieces first, and strictly in the order they were read: they are parts of one
    // segment, and a part handed over out of turn is a hole in the middle of it.
    for (;;) {
      Piece piece;
      {
        const std::lock_guard lock(mutex_);
        if (pieces_.empty()) break;
        piece = std::move(pieces_.front());
        pieces_.pop_front();
        held_ -= piece.bytes.size();
      }
      drained_.notify_all();

      const OMError error = demuxer.appendSegmentChunk(piece.request, piece.bytes, piece.last);
      if (error == OM_COMMON_OVERFLOW) {
        // The track is holding all it may. Back on the head of the queue it goes,
        // to be offered again once readPacket() has drained some of what is there.
        const std::lock_guard lock(mutex_);
        held_ += piece.bytes.size();
        pieces_.push_front(std::move(piece));
        break;
      }
      ++handed;
    }

    std::deque<Arrived> arrived;
    {
      const std::lock_guard lock(mutex_);
      arrived.swap(ready_);
    }
    for (Arrived& item : arrived) {
      demuxer.appendSegment(item.request, std::move(item.bytes));
      ++handed;
    }
    return handed;
  }

  /** Waits until something arrives, or the timeout runs out, or stop is asked for.
   * What the demux thread does when the demuxer says OM_IO_NOT_ENOUGH_DATA and
   * there is nothing else useful to do. */
  void waitForArrival(std::stop_token stop, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    arrived_.wait_for(lock, timeout, [&] {
      return !ready_.empty() || !pieces_.empty() || stop.stop_requested();
    });
  }

  /** Forgets everything queued and everything arrived. Called after a seek, when
   * what was being fetched is no longer what is wanted. Downloads already running
   * are allowed to finish and are then thrown away, which costs one segment's
   * bandwidth and saves cancellation machinery nobody would exercise. */
  void reset() {
    {
      const std::lock_guard lock(mutex_);
      pending_.clear();
      ready_.clear();
      pieces_.clear();
      in_flight_.clear();
      held_ = 0;
      piecewise_.reset();
    }
    // A worker holding a piece back because the queue was full has to be told the
    // queue is gone, or it sits there until its own download times out.
    drained_.notify_all();
  }

  auto inFlight() const -> size_t {
    const std::lock_guard lock(mutex_);
    return in_flight_.size();
  }

private:
  // How much of a segment being read in pieces is held between the two threads.
  // Enough that the demux thread always has something to get on with, and not so
  // much that the segment ends up in memory twice over -- which would give back
  // exactly what reading it in pieces was for.
  static constexpr size_t PIECE_WATERMARK = 2u << 20;
  static constexpr size_t PIECE_SIZE = 64u << 10;

  struct Job {
    openmedia::SegmentRequest request;
    bool piecewise = false;
  };

  struct Arrived {
    openmedia::SegmentRequest request;
    std::vector<uint8_t> bytes;
  };

  struct Piece {
    openmedia::SegmentRequest request;
    std::vector<uint8_t> bytes;
    bool last = false;
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

  auto wanted(const openmedia::SegmentRequest& request) const -> bool {
    return std::any_of(in_flight_.begin(), in_flight_.end(),
                       [&](const openmedia::SegmentRequest& other) { return other == request; });
  }

  // Drops the request from the in-flight list, and reports whether anything was
  // still waiting for it.
  auto retire(const openmedia::SegmentRequest& request) -> bool {
    const auto found = std::find_if(
        in_flight_.begin(), in_flight_.end(),
        [&](const openmedia::SegmentRequest& other) { return other == request; });
    // A reset() while this was downloading means nobody is waiting for it.
    if (found == in_flight_.end()) return false;
    in_flight_.erase(found);
    return true;
  }

  void run(std::stop_token stop) {
    for (;;) {
      Job job;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [&] { return !pending_.empty() || stop.stop_requested(); });
        if (stop.stop_requested()) return;
        job = std::move(pending_.front());
        pending_.pop_front();
      }

      if (job.piecewise) {
        readInPieces(job.request, stop);
        continue;
      }

      auto result = fetch_(job.request);

      const std::lock_guard lock(mutex_);
      if (!retire(job.request)) continue;
      if (result.isErr()) {
        // Nothing is retried here: the demuxer will ask for the same segment again
        // on its next upcomingSegments(), and a retry policy belongs where the
        // fetch function is, not in a queue that cannot tell a 404 from a timeout.
        continue;
      }
      ready_.push_back({job.request, std::move(result).unwrap()});
      arrived_.notify_all();
    }
  }

  // Reads one segment and hands it over as it comes. A failure part way through
  // still ends the segment: the demuxer reads it as far as it got and moves on,
  // which is what it does with a segment that held nothing for a track.
  void readInPieces(const openmedia::SegmentRequest& request, std::stop_token stop) {
    // Only if this is still the segment being read in pieces: a reset() may have
    // moved on to another one while this download was running, and that one's turn
    // is not this one's to end.
    const auto release = [&] {
      if (piecewise_ && *piecewise_ == request) piecewise_.reset();
    };
    const auto finish = [&](bool send_last) {
      const std::lock_guard lock(mutex_);
      release();
      if (retire(request) && send_last) {
        pieces_.push_back({request, {}, true});
        arrived_.notify_all();
      }
    };

    auto opened = stream_fetch_(request);
    if (opened.isErr()) {
      finish(/*send_last=*/false);
      return;
    }
    const std::unique_ptr<openmedia::InputStream> source = std::move(opened).unwrap();
    if (!source || !source->isValid()) {
      finish(/*send_last=*/false);
      return;
    }

    std::array<uint8_t, PIECE_SIZE> buffer;
    for (;;) {
      const size_t read = source->read(buffer);
      {
        std::unique_lock lock(mutex_);
        if (stop.stop_requested() || !wanted(request)) {
          release();
          return;
        }
        if (read == 0) break; // the end of the segment
        pieces_.push_back({request, {buffer.begin(), buffer.begin() + read}, false});
        held_ += read;
        arrived_.notify_all();

        drained_.wait(lock, [&] {
          return held_ < PIECE_WATERMARK || stop.stop_requested() || !wanted(request);
        });
        if (stop.stop_requested() || !wanted(request)) {
          release();
          return;
        }
      }
    }
    finish(/*send_last=*/true);
  }

  openmedia::FetchFn fetch_;
  openmedia::StreamFetchFn stream_fetch_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;    // pending work for a loader
  std::condition_variable arrived_; // fetched bytes for the demux thread
  std::condition_variable drained_; // room for another piece
  std::deque<Job> pending_;
  std::vector<openmedia::SegmentRequest> in_flight_;
  std::deque<Arrived> ready_;
  std::deque<Piece> pieces_;
  size_t held_ = 0; // bytes sitting in `pieces_`
  // The one segment being read in pieces, if any. One at a time: it is the segment
  // readPacket() is waiting on, and there is only ever one of those.
  std::optional<openmedia::SegmentRequest> piecewise_;
  std::vector<std::jthread> workers_;
};

} // namespace media
