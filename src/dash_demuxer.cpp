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

public:
  explicit DashSegmentIndex(dash::Representation rep) : rep_(std::move(rep)) {}

  auto initSegment() const -> std::optional<SegmentRequest> override {
    return rep_.initSegment();
  }

  auto segmentCount() const -> std::optional<uint64_t> override { return rep_.segmentCount(); }

  auto segmentAt(uint64_t ordinal) const -> std::optional<Segment> override {
    const auto segment = rep_.segmentAt(ordinal);
    if (!segment) return std::nullopt;

    Segment out;
    out.request = segment->request;
    out.time = segment->time;
    out.duration = segment->duration;
    out.init = rep_.initSegment();
    // A period starts the timeline over; the first segment of a representation is
    // where the reader is handed a clock it has not been following.
    out.discontinuity = ordinal == 0;
    return out;
  }

  auto ordinalForTime(int64_t media_time) const -> uint64_t override {
    return rep_.ordinalForTime(media_time);
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
