#pragma once

#include <memory>
#include <openmedia/error.h>
#include <openmedia/result.hpp>
#include <openmedia/streaming.hpp>
#include <streaming/segment_index.hpp>

namespace openmedia {

// Everything protocol-specific has already happened by here: DASH and HLS each turn
// their own document into a SegmentSource and hand it over. What is left -- choosing
// among alternatives, fetching or waiting for segments, parsing them one at a time,
// interleaving tracks by decode time, seeking, switching quality -- is the same work
// either way and is done once.
auto createSegmentDemuxer(SegmentSource source, const StreamOptions& options)
    -> Result<std::unique_ptr<SegmentedDemuxer>, OMError>;

} // namespace openmedia
