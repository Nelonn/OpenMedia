#pragma once

#include <memory>
#include <segment_reader.hpp>

namespace openmedia {

// Reads fragmented ISO BMFF: the initialization segment settles the tracks, then each
// media segment is parsed and read out before the next arrives. The plain BMFF demuxer
// cannot stand in for it, since it indexes every sample up front and so needs the whole
// file at once.
auto createFragmentedMP4Reader() -> std::unique_ptr<SegmentReader>;

} // namespace openmedia
