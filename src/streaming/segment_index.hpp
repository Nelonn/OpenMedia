#pragma once

#include <cstdint>
#include <memory>
#include <openmedia/media.h>
#include <openmedia/streaming.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openmedia {

struct Segment {
  SegmentRequest request;
  int64_t time = 0;      // media time of the first sample, in `timescale` units
  uint64_t duration = 0; // in `timescale` units, 0 when nothing says
};

// One way of naming a presentation's segments, whatever named them: given an ordinal,
// which bytes, at what media time, for how long -- and the reverse, for seeking. A DASH
// representation answers it from a template or a timeline, an HLS media playlist from
// the list it already is, and neither shape reaches any further into the demuxer.
// Ordinals count from zero whatever the source numbers its own segments.
class SegmentIndex {
public:
  virtual ~SegmentIndex() = default;

  // Nothing when the media carries its own header.
  virtual auto initSegment() const -> std::optional<SegmentRequest> = 0;

  // Nothing when the source does not bound them: a live timeline, or a template with
  // no known duration.
  virtual auto segmentCount() const -> std::optional<uint64_t> = 0;

  virtual auto segmentAt(uint64_t ordinal) const -> std::optional<Segment> = 0;

  // The last segment when `media_time` is past the end.
  virtual auto ordinalForTime(int64_t media_time) const -> uint64_t = 0;

  virtual auto timescale() const -> uint32_t = 0;

  // Where media time starts, which is not necessarily zero.
  virtual auto presentationTimeOffset() const -> int64_t { return 0; }

  // What representations() reports and selectRepresentation() matches on.
  virtual auto id() const -> std::string_view = 0;
  virtual auto bandwidth() const -> uint32_t { return 0; }
  virtual auto codecs() const -> std::string_view { return {}; }
  virtual auto width() const -> uint32_t { return 0; }
  virtual auto height() const -> uint32_t { return 0; }
};

// One track, and the interchangeable ways of fetching it: switching quality means
// moving between `alternatives`, which is why they are grouped rather than listed flat.
struct SegmentTrack {
  OMMediaType type = OM_MEDIA_NONE; // NONE when only the initialization segment knows
  std::string language;
  std::vector<std::unique_ptr<SegmentIndex>> alternatives; // best first
};

struct SegmentSource {
  std::vector<SegmentTrack> tracks;
  int64_t duration_ns = 0; // 0 when unbounded or unknown
  bool live = false;       // still growing, so it has to be re-read to see more
};

} // namespace openmedia
