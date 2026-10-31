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
  // A depth of zero means the stream never reorders.
  auto push(Frame frame, int32_t poc, size_t depth) -> std::vector<Frame> {
    if (depth == 0) return one(std::move(frame));

    entries_.push_back({poc, std::move(frame)});
    if (entries_.size() <= depth) return {};
    return one(takeSmallest());
  }

  // Used at a restart, where counts begin again and do not compare across the boundary, and
  // when draining at end of stream.
  auto drain() -> std::vector<Frame> {
    std::stable_sort(entries_.begin(), entries_.end(),
                     [](const Entry& a, const Entry& b) { return a.poc < b.poc; });

    std::vector<Frame> output;
    output.reserve(entries_.size());
    for (auto& entry : entries_) output.push_back(std::move(entry.frame));
    entries_.clear();
    return output;
  }

  // Drops what is held without emitting it. For a seek or a flush, where the
  // pictures belong to a part of the stream that is no longer being shown.
  void clear() { entries_.clear(); }

private:
  struct Entry {
    int32_t poc = 0;
    Frame frame;
  };

  static auto one(Frame frame) -> std::vector<Frame> {
    std::vector<Frame> output;
    output.push_back(std::move(frame));
    return output;
  }

  // min_element keeps the first of equal counts, so underivable streams keep arrival order.
  auto takeSmallest() -> Frame {
    const auto oldest = std::min_element(entries_.begin(), entries_.end(),
                                         [](const Entry& a, const Entry& b) { return a.poc < b.poc; });
    Frame frame = std::move(oldest->frame);
    entries_.erase(oldest);
    return frame;
  }

  std::vector<Entry> entries_;
};

} // namespace openmedia
