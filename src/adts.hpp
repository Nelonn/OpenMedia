#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include <segment_reader.hpp>

namespace openmedia {

// HLS audio segments may contain an ID3 timestamp followed by bare ADTS frames.
auto looksLikeAdts(std::span<const uint8_t> bytes) -> bool;
auto createAdtsReader() -> std::unique_ptr<SegmentReader>;

} // namespace openmedia
