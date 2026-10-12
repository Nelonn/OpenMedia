#pragma once

#include <memory>
#include <segment_reader.hpp>
#include <span>

namespace openmedia {

// Reads MPEG-TS segments, which is what an HLS playlist with no `#EXT-X-MAP`
// addresses -- the shape HLS was published in for a decade and most of the web
// still serves. A transport stream repeats its program tables and its parameter
// sets, so every segment stands on its own and there is no initialization section
// to open the reader with.
auto createMpegTsReader() -> std::unique_ptr<SegmentReader>;

// Whether these bytes open a transport stream: the 0x47 sync byte at the head of
// consecutive 188-byte packets. This is what tells MPEG-TS segments apart from the
// self-contained fragmented MP4 that a playlist with no `#EXT-X-MAP` may equally
// well hold, each segment then carrying a `moov` of its own.
auto looksLikeMpegTs(std::span<const uint8_t> bytes) -> bool;

} // namespace openmedia
