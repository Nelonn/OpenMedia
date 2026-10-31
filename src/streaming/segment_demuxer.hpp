#pragma once

#include <memory>
#include <openmedia/error.h>
#include <openmedia/result.hpp>
#include <openmedia/streaming.hpp>
#include <streaming/segment_index.hpp>

namespace openmedia {

auto createSegmentDemuxer(SegmentSource source, const StreamOptions& options)
    -> Result<std::unique_ptr<SegmentedDemuxer>, OMError>;

} // namespace openmedia
