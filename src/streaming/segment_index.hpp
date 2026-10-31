#pragma once

#include <cstdint>
#include <memory>
#include <openmedia/media.h>
#include <openmedia/streaming.hpp>
#include <openmedia/track.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openmedia {

struct Segment {
  SegmentRequest request;
  int64_t time = 0;      // media time of the first sample, in `timescale` units
  uint64_t duration = 0; // in `timescale` units, 0 when nothing says

  // Per segment because a playlist may replace it part-way through.
  std::optional<SegmentRequest> init;

  // Announced but not available (`#EXT-X-GAP`), so there is nothing to fetch and
  // the only thing to do with it is step over it.
  bool gap = false;

  // Timestamps, and possibly the encoding itself, start over at this segment.
  // Whatever the reader had worked out about the clock does not carry across it.
  bool discontinuity = false;

  // Wall-clock time of the first sample, milliseconds since the Unix epoch; -1
  // where nothing dates it.
  int64_t program_date_time_ms = -1;

  // How the whole of these bytes is protected, or null where they are in the clear.
  // Shared because one key covers a run of segments and is stated once for them.
  std::shared_ptr<const SegmentEncryption> encryption;
};

// Given an ordinal: which bytes, at what media time, for how long, and the reverse for
// seeking. Ordinals count from zero whatever the source numbers its own segments.
class SegmentIndex {
public:
  virtual ~SegmentIndex() = default;

  // What the presentation opens with; a given segment decodes against Segment::init.
  virtual auto initSegment() const -> std::optional<SegmentRequest> = 0;

  // Nothing where the source does not bound them: a live timeline, or a template
  // with no known duration.
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

// One track and the interchangeable ways of fetching it, grouped rather than listed
// flat because switching quality means moving between `alternatives`.
struct SegmentTrack {
  OMMediaType type = OM_MEDIA_NONE; // NONE when only the initialization segment knows
  std::string language;
  // A label for people, where the manifest gave one -- HLS `NAME=`, DASH `Label`.
  std::string name;
  // What the manifest says about the track's part in the presentation, over and
  // above its type: default, forced, for the hard of hearing.
  OMDisposition disposition = OM_DISPOSITION_NONE;
  // The channel count the manifest declared, for reporting before the
  // initialization segment settles it. 0 when nothing said.
  uint32_t channels = 0;
  std::vector<std::unique_ptr<SegmentIndex>> alternatives; // best first
};

struct SegmentSource {
  std::vector<SegmentTrack> tracks;
  int64_t duration_ns = 0; // 0 when unbounded or unknown
  bool live = false;       // still growing, so it has to be re-read to see more
  // Where the manifest asks playback to begin (`#EXT-X-START`). 0 means the start,
  // which is also what a manifest that does not say means.
  int64_t start_offset_ns = 0;
  // How long a live playlist stays current (`#EXT-X-TARGETDURATION`). 0 when nothing says.
  int64_t target_duration_ns = 0;
};

} // namespace openmedia
