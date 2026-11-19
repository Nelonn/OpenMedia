#include <algorithm>
#include <dash/mpd.hpp>
#include <formats.hpp>
#include <memory>
#include <openmedia/io.hpp>
#include <openmedia/streaming.hpp>
#include <util/io_util.hpp>
#include <streaming/segment_demuxer.hpp>
#include <streaming/segment_index.hpp>
#include <utility>
#include <vector>

namespace openmedia {
namespace {

// Holds the representation by value: the manifest is a local in openDashManifest().
class DashSegmentIndex final : public SegmentIndex {
  dash::Representation rep_;
  std::optional<uint64_t> count_;
  std::optional<SegmentRequest> init_;

  // A SegmentTimeline is walked run by run, and an encoder that varies its segment
  // lengths writes one run per segment: thousands of them for a film, walked on every
  // lookup. With each run's first ordinal at hand, a lookup is a binary search.
  std::vector<uint64_t> run_first_;
  bool timeline_indexed_ = false;

  void indexTimeline() {
    if (rep_.addressing != dash::Addressing::Timeline) return;
    uint64_t ordinal = 0;
    int64_t last_start = INT64_MIN;
    run_first_.reserve(rep_.timeline.size());
    for (const dash::TimelineRun& run : rep_.timeline) {
      // What the run-by-run walk does with these is not worth reproducing here.
      if (run.duration == 0 || run.start < last_start) {
        run_first_.clear();
        return;
      }
      last_start = run.start;
      run_first_.push_back(ordinal);
      if (run.repeat < 0) break; // nothing after an open run is reachable
      ordinal += static_cast<uint64_t>(run.repeat) + 1;
    }
    timeline_indexed_ = !run_first_.empty();
  }

  // The run holding `ordinal` and how far into it, or nothing past the end.
  auto runOf(uint64_t ordinal) const -> std::optional<std::pair<size_t, uint64_t>> {
    if (count_ && ordinal >= *count_) return std::nullopt;
    const auto after = std::upper_bound(run_first_.begin(), run_first_.end(), ordinal);
    const auto run = static_cast<size_t>(after - run_first_.begin()) - 1;
    return std::pair {run, ordinal - run_first_[run]};
  }

public:
  explicit DashSegmentIndex(dash::Representation rep)
      : rep_(std::move(rep)), count_(rep_.segmentCount()), init_(rep_.initSegment()) {
    indexTimeline();
  }

  auto initSegment() const -> std::optional<SegmentRequest> override { return init_; }
  auto initAt(uint64_t) const -> std::optional<SegmentRequest> override { return init_; }

  auto segmentCount() const -> std::optional<uint64_t> override { return count_; }

  auto spanAt(uint64_t ordinal) const -> std::optional<Span> override {
    if (count_ && ordinal >= *count_) return std::nullopt;
    switch (rep_.addressing) {
      case dash::Addressing::Number:
      case dash::Addressing::List:
        return Span {rep_.presentation_time_offset +
                         static_cast<int64_t>(ordinal * rep_.segment_duration),
                     rep_.segment_duration};
      case dash::Addressing::Timeline: {
        if (!timeline_indexed_) break;
        const auto at = runOf(ordinal);
        if (!at) return std::nullopt;
        const dash::TimelineRun& run = rep_.timeline[at->first];
        return Span {run.start + static_cast<int64_t>(at->second * run.duration), run.duration};
      }
      default: break;
    }
    return SegmentIndex::spanAt(ordinal);
  }

  auto segmentAt(uint64_t ordinal) const -> std::optional<Segment> override {
    std::optional<dash::Segment> segment;
    if (timeline_indexed_) {
      const auto at = runOf(ordinal);
      if (!at) return std::nullopt;
      const dash::TimelineRun& run = rep_.timeline[at->first];
      const int64_t time = run.start + static_cast<int64_t>(at->second * run.duration);
      segment = dash::Segment {
          SegmentRequest {dash::expandTemplate(rep_.media_template, rep_.id, rep_.bandwidth,
                                               rep_.start_number + ordinal, time),
                          0, -1},
          time, run.duration};
    } else {
      segment = rep_.segmentAt(ordinal);
    }
    if (!segment) return std::nullopt;

    Segment out;
    out.request = std::move(segment->request);
    out.time = segment->time;
    out.duration = segment->duration;
    out.init = init_;
    // A period starts the timeline over; the first segment of a representation is
    // where the reader is handed a clock it has not been following.
    out.discontinuity = ordinal == 0;
    return out;
  }

  auto ordinalForTime(int64_t media_time) const -> uint64_t override {
    if (!timeline_indexed_) return rep_.ordinalForTime(media_time);
    const auto& runs = rep_.timeline;
    const size_t used = run_first_.size();
    // The last run starting at or before the target.
    const auto after = std::upper_bound(
        runs.begin(), runs.begin() + static_cast<ptrdiff_t>(used), media_time,
        [](int64_t time, const dash::TimelineRun& run) { return time < run.start; });
    if (after == runs.begin()) return 0;
    const auto r = static_cast<size_t>(after - runs.begin()) - 1;
    const dash::TimelineRun& run = runs[r];
    const uint64_t within = static_cast<uint64_t>(media_time - run.start) / run.duration;
    if (run.repeat < 0) return run_first_[r] + within;
    const uint64_t in_run = static_cast<uint64_t>(run.repeat) + 1;
    if (within < in_run) return run_first_[r] + within;
    // In the gap before the next run, which is where it is read from; or past the end.
    if (r + 1 < used) return run_first_[r + 1];
    return run_first_[r] + in_run - 1;
  }

  auto timescale() const -> uint32_t override { return rep_.timescale; }

  auto presentationTimeOffset() const -> int64_t override {
    return rep_.presentation_time_offset;
  }

  auto id() const -> std::string_view override { return rep_.id; }
  auto bandwidth() const -> uint32_t override { return rep_.bandwidth; }
  auto codecs() const -> std::string_view override { return rep_.codecs; }
  auto width() const -> uint32_t override { return rep_.width; }
  auto height() const -> uint32_t override { return rep_.height; }
};

// `SegmentBase` names the whole resource as one segment and its `sidx` is not read yet,
// so playing it would pull the entire film into memory.
auto isReadableInPieces(const dash::Representation& rep) -> bool {
  return rep.addressing != dash::Addressing::Single;
}

auto sourceFromManifest(dash::Manifest manifest, bool& dropped_whole_file) -> SegmentSource {
  SegmentSource source;
  source.duration_ns = manifest.duration_ns;
  source.live = manifest.dynamic;

  // Only the first period: crossing one means a new track list, a different presentation.
  if (manifest.periods.empty()) return source;
  dash::Period& period = manifest.periods.front();
  if (period.duration_ns > 0) source.duration_ns = period.duration_ns;

  for (dash::AdaptationSet& set : period.sets) {
    SegmentTrack track;
    track.type = set.type;
    track.language = std::move(set.language);
    for (dash::Representation& rep : set.representations) {
      if (!isReadableInPieces(rep)) {
        dropped_whole_file = true;
        continue;
      }
      track.alternatives.push_back(std::make_unique<DashSegmentIndex>(std::move(rep)));
    }
    if (!track.alternatives.empty()) source.tracks.push_back(std::move(track));
  }
  return source;
}

// The FormatRegistry entry: segments are read off the filesystem beside the MPD.
class DashFormatDemuxer final : public Demuxer {
  std::unique_ptr<Demuxer> inner_;
  std::vector<Track> no_tracks_;
  Dictionary no_metadata_;

public:
  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    close();
    if (!input || !input->isValid()) return OM_IO_INVALID_STREAM;

    // Resolving against the working directory would open unrelated files. A stream with no
    // source path goes through openDashManifest(), which takes the URL.
    const std::string source = input->sourcePath();
    if (source.empty()) return OM_COMMON_INVALID_ARGUMENT;

    const std::vector<uint8_t> document = readAll(*input);
    if (document.empty()) return OM_IO_NOT_ENOUGH_DATA;

    StreamOptions options;
    options.fetch = fileFetch();
    auto opened = openDashManifest(document, source, options);
    if (opened.isErr()) return std::move(opened).unwrapErr();
    inner_ = std::move(opened).unwrap();
    return OM_SUCCESS;
  }

  void close() override { inner_.reset(); }

  auto tracks() const -> const std::vector<Track>& override {
    return inner_ ? inner_->tracks() : no_tracks_;
  }

  auto metadata() const -> const Dictionary& override {
    return inner_ ? inner_->metadata() : no_metadata_;
  }

  auto readPacket() -> Result<Packet, OMError> override {
    return inner_ ? inner_->readPacket() : Err(OM_COMMON_NOT_INITIALIZED);
  }

  auto seek(int32_t stream_idx, int64_t timestamp, SeekMode mode) -> OMError override {
    return inner_ ? inner_->seek(stream_idx, timestamp, mode) : OM_COMMON_NOT_INITIALIZED;
  }
};

} // namespace

auto openDashManifest(std::span<const uint8_t> mpd_xml, std::string_view manifest_url,
                      const StreamOptions& options)
    -> Result<std::unique_ptr<SegmentedDemuxer>, OMError> {
  if (mpd_xml.empty()) return Err(OM_COMMON_INVALID_ARGUMENT);

  auto parsed = dash::parseManifest(mpd_xml, manifest_url);
  if (parsed.isErr()) return Err(std::move(parsed).unwrapErr());

  bool dropped_whole_file = false;
  SegmentSource source = sourceFromManifest(std::move(parsed).unwrap(), dropped_whole_file);
  if (source.tracks.empty()) {
    // Distinguish a manifest describing nothing from one describing only media this
    // demuxer will not pull in whole.
    return Err(dropped_whole_file ? OM_FORMAT_NOT_SUPPORTED : OM_FORMAT_NO_STREAMS);
  }
  return createSegmentDemuxer(std::move(source), options);
}

const FormatDescriptor FORMAT_DASH = {
    .container_id = OM_CONTAINER_DASH,
    .name = "dash",
    .long_name = "MPEG-DASH",
    .demuxer_factory = [] { return std::make_unique<DashFormatDemuxer>(); },
    .muxer_factory = {},
};

} // namespace openmedia
