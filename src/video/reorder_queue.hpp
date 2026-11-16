#pragma once

#include <openmedia/frame.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace openmedia {

// Hardware decoders return decode order. Once a stream's stated reorder depth is in hand
// the earliest can no longer be overtaken.
class ReorderQueue {
public:
  // A depth of zero means the stream never reorders. The depth can shrink mid-stream (a new
  // SPS without an IDR); what is held past it goes out now rather than being overtaken.
  auto push(Frame frame, int32_t poc, size_t depth) -> std::vector<Frame> {
    entries_.push_back({poc, std::move(frame)});
    std::vector<Frame> output;
    while (entries_.size() > depth) output.push_back(takeSmallest());
    return output;
  }

  // A picture ordered before one already let out: the stated depth was too small, and it can
  // no longer come out in its place.
  auto isLate(int32_t poc) const -> bool { return has_output_ && poc < last_output_poc_; }

  // Used at a restart, where counts begin again and do not compare across the boundary, and
  // when draining at end of stream.
  auto drain() -> std::vector<Frame> {
    std::stable_sort(entries_.begin(), entries_.end(),
                     [](const Entry& a, const Entry& b) { return a.poc < b.poc; });

    std::vector<Frame> output;
    output.reserve(entries_.size());
    for (auto& entry : entries_) output.push_back(std::move(entry.frame));
    entries_.clear();
    has_output_ = false;
    return output;
  }

  // Drops what is held without emitting it. For a seek or a flush, where the
  // pictures belong to a part of the stream that is no longer being shown.
  void clear() {
    entries_.clear();
    has_output_ = false;
  }

private:
  struct Entry {
    int32_t poc = 0;
    Frame frame;
  };

  // min_element keeps the first of equal counts, so underivable streams keep arrival order.
  auto takeSmallest() -> Frame {
    const auto oldest = std::min_element(entries_.begin(), entries_.end(),
                                         [](const Entry& a, const Entry& b) { return a.poc < b.poc; });
    last_output_poc_ = oldest->poc;
    has_output_ = true;
    Frame frame = std::move(oldest->frame);
    entries_.erase(oldest);
    return frame;
  }

  std::vector<Entry> entries_;
  // Since the last drain or clear, where the counts may start over.
  int32_t last_output_poc_ = 0;
  bool has_output_ = false;
};

} // namespace openmedia
