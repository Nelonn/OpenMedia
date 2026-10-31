#pragma once

#include <memory>
#include <segment_reader.hpp>

namespace openmedia {

// The plain BMFF demuxer indexes every sample up front, so it needs the whole file.
auto createFragmentedMP4Reader() -> std::unique_ptr<SegmentReader>;

} // namespace openmedia
