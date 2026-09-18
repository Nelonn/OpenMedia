#pragma once

#include <openmedia/frame.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace openmedia {

// Holds decoded pictures back until everything that precedes them in
// presentation order has arrived.
//
// Hardware decoders hand pictures back in decode order. For anything with
// B-frames that is not presentation order, so emitting them as they come makes
// the timestamps run backwards. Every stream states how far it may reorder --
// H.264 in the VUI or, failing that, through the DPB capacity its level
// guarantees; HEVC outright in the SPS -- and once that many pictures are in
// hand the earliest of them can no longer be overtaken, so it is safe to show.
//
// The D3D11 and D3D12 decoders both need exactly this, which is why it lives in
// neither of them.
class ReorderQueue {
public:
  // Takes `frame` and returns whichever picture is now safe to show, if any. A
  // depth of zero means the stream never reorders, and the frame passes
  // straight through.
  auto push(Frame frame, int32_t poc, size_t depth) -> std::vector<Frame> {
    if (depth == 0) return one(std::move(frame));

    entries_.push_back({poc, std::move(frame)});
    if (entries_.size() <= depth) return {};
    return one(takeSmallest());
  }

  // Releases everything still held, in presentation order. Used at a stream
  // restart, where the picture counts begin again and comparing across the
  // boundary would order pictures against ones they have nothing to do with,
  // and when the decoder is drained at end of stream.
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

  auto empty() const -> bool { return entries_.empty(); }

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

  // min_element keeps the first of equal counts, so pictures a stream gives the
  // same count -- which is every picture of a stream whose counts we could not
  // derive -- come back out in the order they arrived.
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
