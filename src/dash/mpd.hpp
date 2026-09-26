#pragma once

#include <cstdint>
#include <openmedia/streaming.hpp>
#include <openmedia/error.h>
#include <openmedia/media.h>
#include <openmedia/media.hpp>
#include <openmedia/result.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <util/timescale.hpp>
#include <util/url.hpp>
#include <vector>

namespace openmedia::dash {

struct ByteRange {
  int64_t offset = 0;
  int64_t length = -1; // -1 reaches to the end of the resource

  auto isWhole() const -> bool { return offset == 0 && length < 0; }
};

/** One `S` element of a SegmentTimeline. */
struct TimelineRun {
  int64_t start = 0;     // @t, in the representation's timescale
  uint64_t duration = 0; // @d
  int64_t repeat = 0;    // @r; -1 repeats until the next run or forever
};

struct ListEntry {
  std::string url;
  ByteRange range;
};

/** How a representation names its segments. The four forms are exclusive, and a
 * manifest that gives none addresses its media as one self-contained resource.
 */
enum class Addressing : uint8_t {
  None,
  Number,   // SegmentTemplate with $Number$ and @duration
  Timeline, // SegmentTemplate with a SegmentTimeline
  List,     // SegmentList
  Single,   // SegmentBase, or nothing said at all
};

struct Segment {
  SegmentRequest request;
  int64_t time = 0;      // media time of the first sample, in the rep's timescale
  uint64_t duration = 0; // in the rep's timescale, 0 when the manifest is silent
};

struct Representation {
  std::string id;
  uint32_t bandwidth = 0;
  std::string mime_type;
  std::string codecs;
  uint32_t width = 0;
  uint32_t height = 0;
  Rational framerate = {};
  uint32_t sample_rate = 0;
  uint32_t channels = 0;

  Addressing addressing = Addressing::None;
  uint32_t timescale = 1;
  uint64_t start_number = 1;
  uint64_t segment_duration = 0;        // @duration, in `timescale` units
  int64_t presentation_time_offset = 0; // @presentationTimeOffset, same units
  std::vector<TimelineRun> timeline;
  std::vector<ListEntry> list;

  // `$Number$` and `$Time$` are all that is left in here; the identifiers that
  // depend only on the representation are substituted while parsing, so nothing
  // downstream has to carry the representation around to name a segment.
  std::string media_template;

  std::string init_url; // resolved; empty when the media carries its own header
  ByteRange init_range;
  std::string media_url; // Addressing::Single
  ByteRange index_range; // where a SegmentBase keeps its `sidx`

  int64_t period_duration_ns = 0; // 0 when the period has no known end

  /** The initialization segment, if this representation has one apart from its
   * media. */
  auto initSegment() const -> std::optional<SegmentRequest>;

  /** How many media segments there are, or nothing when the manifest does not
   * bound them -- a live timeline, or a template with no period duration. */
  auto segmentCount() const -> std::optional<uint64_t>;

  /** The `ordinal`-th media segment, counting from zero whatever @startNumber
   * says. Nothing when `ordinal` is past the end of a bounded representation. */
  auto segmentAt(uint64_t ordinal) const -> std::optional<Segment>;

  /** The segment holding `time` (in this representation's timescale), or the
   * last one when `time` is past the end. */
  auto ordinalForTime(int64_t time) const -> uint64_t;

  /** Total media time covered, in the representation's timescale; 0 when
   * unbounded. */
  auto coveredDuration() const -> uint64_t;

  auto mediaType() const -> OMMediaType;
};

struct AdaptationSet {
  OMMediaType type = OM_MEDIA_NONE;
  std::string language;
  std::vector<Representation> representations;
};

struct Period {
  std::string id;
  int64_t start_ns = 0;
  int64_t duration_ns = 0; // 0 when the period has no known end
  std::vector<AdaptationSet> sets;
};

struct Manifest {
  bool dynamic = false;
  int64_t duration_ns = 0;
  int64_t min_buffer_ns = 0;
  // -1 when the manifest is not to be reloaded. A dynamic manifest that gives a
  // period here expects the caller to fetch it again that often; OpenMedia never
  // fetches anything, so it reports the figure and leaves the reload to the
  // application, which can hand the new bytes to openDashManifest().
  int64_t minimum_update_period_ns = -1;
  std::string availability_start_time;
  std::vector<Period> periods;
};

auto parseManifest(std::span<const uint8_t> document, std::string_view manifest_url)
    -> Result<Manifest, OMError>;

// --- Exposed for their own sake, and so they can be tested on their own -------

/** An xs:duration as nanoseconds. Only the forms a manifest uses: `PT1H2M3.5S`,
 * `P1DT4H`, and so on. Negative on anything unparseable. */
auto parseIsoDuration(std::string_view text) -> int64_t;

/** Substitutes `$Number$`, `$Time$`, `$RepresentationID$`, `$Bandwidth$` and
 * `$$`, honouring the `%0<width>d` format each identifier may carry. */
auto expandTemplate(std::string_view tmpl, std::string_view representation_id,
                    uint32_t bandwidth, uint64_t number, int64_t time) -> std::string;

/** Substitutes only what depends on the representation -- `$RepresentationID$`
 * and `$Bandwidth$` -- and writes `$Number$` and `$Time$` back out untouched for
 * segmentAt() to fill in later. Naming a segment then needs nothing but its
 * number and its time. */
auto expandRepresentationTemplate(std::string_view tmpl, std::string_view representation_id,
                                  uint32_t bandwidth) -> std::string;

} // namespace openmedia::dash
