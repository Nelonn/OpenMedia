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

// A DASH representation, answering the questions the segment demuxer asks.
//
// It holds the representation by value: the manifest it was parsed out of is a
// local variable in openDashManifest(), and a representation is a few strings and
// a timeline, which is cheaper to own than to keep a manifest alive for.
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
    return Segment {segment->request, segment->time, segment->duration};
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

// Whether a representation addresses its media in pieces this demuxer can read one
// at a time. `SegmentBase` names the whole resource as a single segment, and its
// `sidx` -- which says where the subsegments inside it begin -- is not read yet; so
// reading it would mean pulling an entire film into memory to play its first frame,
// and refusing is the honest answer until the index is parsed.
auto isReadableInPieces(const dash::Representation& rep) -> bool {
  return rep.addressing != dash::Addressing::Single;
}

auto sourceFromManifest(dash::Manifest manifest, bool& dropped_whole_file) -> SegmentSource {
  SegmentSource source;
  source.duration_ns = manifest.duration_ns;
  source.live = manifest.dynamic;

  // One period is played, the first. Advancing across a period boundary means new
  // initialization segments and a new track list, which is a different
  // presentation as far as every consumer of tracks() is concerned.
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

// Registered so a manifest opened through FormatRegistry behaves like any other
// container: the stream it is given is the MPD, and the segments beside it are read
// off the same filesystem. A caller that wants segments from anywhere else goes
// through openDashManifest() and says how.
class DashFormatDemuxer final : public Demuxer {
  std::unique_ptr<Demuxer> inner_;
  std::vector<Track> no_tracks_;
  Dictionary no_metadata_;

public:
  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    close();
    if (!input || !input->isValid()) return OM_IO_INVALID_STREAM;

    // A manifest names its segments relative to itself, and this wrapper reads them
    // off the filesystem beside it. A stream that cannot say where it came from
    // leaves no "beside" to read from: resolving against the working directory
    // instead would open whatever files happen to sit there, which is not where this
    // manifest came from. Such a manifest is opened through openDashManifest(), which
    // takes its URL as an argument rather than guessing at one.
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
