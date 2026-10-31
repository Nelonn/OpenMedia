#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace openmedia {

struct FilteredBitstream {
  std::span<const uint8_t> bytes;
  std::vector<uint8_t> storage;
};

class BitStreamFilter {
public:
  virtual ~BitStreamFilter() = default;

  virtual auto filter(std::span<const uint8_t> input) const -> FilteredBitstream = 0;

  // Rewrite `bytes` where it lies, returning the filtered length (never more
  // than `bytes.size()`). nullopt means this filter cannot work in place for
  // this input and the caller must fall back to filter(), paying for a copy.
  //
  // This exists so a demuxer can read a sample straight into the packet buffer
  // it is going to hand out and convert it there, instead of staging it in a
  // scratch vector first.
  virtual auto filterInPlace(std::span<uint8_t> /*bytes*/) const -> std::optional<size_t> {
    return std::nullopt;
  }

  // Bytes that must precede a keyframe's payload, e.g. parameter sets hoisted
  // out of the container. Empty when the filter prepends nothing.
  virtual auto keyframePrefix() const noexcept -> std::span<const uint8_t> { return {}; }
};

} // namespace openmedia
