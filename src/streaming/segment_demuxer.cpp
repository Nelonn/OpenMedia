#include <streaming/segment_demuxer.hpp>

#include <algorithm>
#include <bmff_fragment.hpp>
#include <cctype>
#include <map>
#include <openmedia/io.hpp>
#include <openmedia/metadata_keys.hpp>
#include <optional>
#include <util/io_util.hpp>
#include <util/timescale.hpp>
#include <vector>

namespace openmedia {
namespace {

// How far ahead of what it is reading a stream will recognise one of its own
// segments. Naming a segment means building its URL, so this is also the cost of an
// append that matches nothing -- which is why it covers a generous prefetch depth
// rather than an unbounded one. StreamOptions::max_buffered_bytes stops a caller
// getting this far ahead in the first place.
constexpr uint64_t RECOGNITION_WINDOW = 64;

// A segment that yields no sample for the track it belongs to is skipped rather
// than played, but a run of them means something is wrong with the addressing
// rather than with one segment, and looping over thousands would just hide it.
constexpr int MAX_EMPTY_SEGMENTS = 8;

auto percentDecode(std::string_view text) -> std::string {
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size()) {
      const int hi = hex(text[i + 1]);
      const int lo = hex(text[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(text[i]);
  }
  return out;
}

auto urlToLocalPath(std::string_view url) -> std::string {
  std::string_view path = url;
  if (path.starts_with("file://")) {
    path.remove_prefix(7);
    // `file:///C:/x` and `file://C:/x` are both written in the wild; the leading
    // slash in front of a drive letter is not part of the path.
    if (path.size() >= 3 && path[0] == '/' && path[2] == ':') path.remove_prefix(1);
  }
  const size_t cut = path.find_first_of("?#");
  if (cut != std::string_view::npos) path = path.substr(0, cut);
  return percentDecode(path);
}

class SegmentDemuxer final : public SegmentedDemuxer {
  struct Stream {
    const SegmentTrack* track = nullptr;
    size_t alternative = 0;
    std::unique_ptr<FragmentedMP4Reader> reader;

    // Reader track index to the index this demuxer publishes it under. One
    // alternative carries one track in all but pathological cases, but nothing in
    // the format says it must, so the mapping is explicit.
    std::vector<int32_t> public_tracks;

    bool needs_init = true;
    bool finished = false;
    uint64_t read_ordinal = 0; // the media segment the reader is on

    std::optional<std::vector<uint8_t>> init_inbox;
    std::map<uint64_t, std::vector<uint8_t>> inbox;
    size_t buffered_bytes = 0;

    void hold(uint64_t ordinal, std::vector<uint8_t> bytes) {
      buffered_bytes += bytes.size();
      if (const auto previous = inbox.find(ordinal); previous != inbox.end()) {
        buffered_bytes -= previous->second.size();
      }
      inbox.insert_or_assign(ordinal, std::move(bytes));
    }

    auto take(uint64_t ordinal) -> std::optional<std::vector<uint8_t>> {
      const auto found = inbox.find(ordinal);
      if (found == inbox.end()) return std::nullopt;
      std::vector<uint8_t> bytes = std::move(found->second);
      buffered_bytes -= bytes.size();
      inbox.erase(found);
      return bytes;
    }

    void dropHeld() {
      inbox.clear();
      buffered_bytes = 0;
    }

    auto index() const -> const SegmentIndex& { return *track->alternatives[alternative]; }
  };

  SegmentSource source_;
  StreamOptions options_;
  std::vector<Stream> streams_;
  std::vector<Track> tracks_;
  Dictionary metadata_;
  bool ready_ = false;

public:
  auto open(std::unique_ptr<InputStream>) -> OMError override {
    // Opening this demuxer means handing it a described presentation and a way to
    // name segments, which is more than a byte stream carries. The DASH and HLS
    // entry points do the stream-shaped version of this.
    return OM_FORMAT_NOT_SUPPORTED;
  }

  auto start(SegmentSource source, const StreamOptions& options) -> OMError {
    source_ = std::move(source);
    options_ = options;

    for (const SegmentTrack* track : selectTracks()) {
      Stream stream;
      stream.track = track;
      stream.alternative = chooseAlternative(*track);

      // Without an initialization segment the media is either a plain MP4
      // addressed by byte range or an MPEG-TS stream. Both want a different reader
      // than one fed whole fragmented-MP4 segments.
      if (!track->alternatives[stream.alternative]->initSegment()) continue;

      stream.reader = createFragmentedMP4Reader();
      if (!stream.reader) return OM_COMMON_OUT_OF_MEMORY;
      streams_.push_back(std::move(stream));
    }

    if (streams_.empty()) return OM_FORMAT_NO_STREAMS;

    // With a fetch function the caller has said it is content to block, so the
    // tracks can be settled here and `tracks()` is populated on return, the way
    // every other demuxer behaves.
    if (options_.fetch) {
      for (Stream& stream : streams_) {
        const OMError error = ensureInit(stream);
        if (error != OM_SUCCESS) return error;
      }
      publishTracks();
      if (!ready_) return OM_FORMAT_NO_STREAMS;
    }
    return OM_SUCCESS;
  }

  void close() override {
    streams_.clear();
    tracks_.clear();
    metadata_ = Dictionary();
    source_ = SegmentSource();
    ready_ = false;
  }

  auto tracks() const -> const std::vector<Track>& override { return tracks_; }
  auto metadata() const -> const Dictionary& override { return metadata_; }
  auto isReady() const -> bool override { return ready_; }

  // --- What the caller downloads ---------------------------------------------

  auto upcomingSegments(size_t max_count) const -> std::vector<SegmentPlan> override {
    std::vector<SegmentPlan> plans;
    if (max_count == 0 || streams_.empty()) return plans;

    // An initialization segment blocks everything behind it, so all of them come
    // first and in stream order, without competing on time with media.
    for (size_t i = 0; i < streams_.size(); ++i) {
      const Stream& stream = streams_[i];
      if (!stream.needs_init || stream.init_inbox) continue;
      const auto request = stream.index().initSegment();
      if (!request) continue;
      SegmentPlan plan;
      plan.request = *request;
      plan.track_index = firstTrackOf(i);
      plan.initialization = true;
      plans.push_back(std::move(plan));
      if (plans.size() >= max_count) return plans;
    }

    // Beyond that, the segment wanted soonest is the one whose media starts
    // earliest, whichever stream it belongs to -- which is also the order that
    // keeps every track's buffer growing at the same rate.
    std::vector<SegmentPlan> candidates;
    for (size_t i = 0; i < streams_.size(); ++i) {
      const Stream& stream = streams_[i];
      if (stream.finished) continue;
      const SegmentIndex& index = stream.index();
      const auto bound = index.segmentCount();

      uint64_t ordinal = stream.read_ordinal;
      for (size_t taken = 0; taken < max_count; ++ordinal) {
        if (bound && ordinal >= *bound) break;
        if (stream.inbox.contains(ordinal)) continue; // already in hand
        const auto segment = index.segmentAt(ordinal);
        if (!segment) break;

        SegmentPlan plan;
        plan.request = segment->request;
        plan.track_index = firstTrackOf(i);
        plan.start_time_ns = scaleToNs(segment->time, index.timescale());
        plan.duration_ns = scaleToNs(static_cast<int64_t>(segment->duration), index.timescale());
        candidates.push_back(std::move(plan));
        ++taken;
      }
    }

    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const SegmentPlan& a, const SegmentPlan& b) {
                       return a.start_time_ns < b.start_time_ns;
                     });
    for (SegmentPlan& plan : candidates) {
      if (plans.size() >= max_count) break;
      plans.push_back(std::move(plan));
    }
    return plans;
  }

  auto appendSegment(const SegmentRequest& request, std::vector<uint8_t> bytes)
      -> OMError override {
    if (bytes.empty()) return OM_COMMON_INVALID_ARGUMENT;

    for (Stream& stream : streams_) {
      if (const auto init = stream.index().initSegment(); init && *init == request) {
        if (!stream.needs_init) return OM_SUCCESS; // already parsed; nothing to do
        stream.init_inbox = std::move(bytes);
        settleInitsIfComplete();
        return OM_SUCCESS;
      }
    }

    for (Stream& stream : streams_) {
      const SegmentIndex& index = stream.index();
      const auto bound = index.segmentCount();
      for (uint64_t ordinal = stream.read_ordinal;
           ordinal < stream.read_ordinal + RECOGNITION_WINDOW; ++ordinal) {
        if (bound && ordinal >= *bound) break;
        const auto segment = index.segmentAt(ordinal);
        if (!segment) break;
        if (!(segment->request == request)) continue;
        // Full is not the same as unwanted: the caller is told to keep these bytes
        // and offer them again, rather than have them silently dropped and then
        // wait forever for a segment it believes it already delivered.
        if (options_.max_buffered_bytes != 0 &&
            stream.buffered_bytes + bytes.size() > options_.max_buffered_bytes) {
          return OM_COMMON_OVERFLOW;
        }
        stream.hold(ordinal, std::move(bytes));
        return OM_SUCCESS;
      }
    }

    // Seeked past, or belonging to an alternative since switched away from. The
    // caller cannot always know that in time, and throwing the bytes away is the
    // whole of the correct response.
    return OM_SUCCESS;
  }

  auto bufferedDurationNs(int32_t track_index) const -> int64_t override {
    const Stream* stream = streamOfTrack(track_index);
    if (!stream) return -1;

    int64_t total = stream->reader->remainingDurationNs();
    const SegmentIndex& index = stream->index();
    for (const auto& [ordinal, bytes] : stream->inbox) {
      if (ordinal < stream->read_ordinal) continue;
      const auto segment = index.segmentAt(ordinal);
      if (!segment) continue;
      total += scaleToNs(static_cast<int64_t>(segment->duration), index.timescale());
    }
    return total;
  }

  auto representations() const -> std::vector<RepresentationInfo> override {
    std::vector<RepresentationInfo> infos;
    for (size_t i = 0; i < streams_.size(); ++i) {
      const Stream& stream = streams_[i];
      for (size_t a = 0; a < stream.track->alternatives.size(); ++a) {
        const SegmentIndex& index = *stream.track->alternatives[a];
        infos.push_back({firstTrackOf(i), std::string(index.id()), std::string(index.codecs()),
                         index.bandwidth(), index.width(), index.height(),
                         a == stream.alternative});
      }
    }
    return infos;
  }

  auto selectRepresentation(int32_t track_index, std::string_view id) -> OMError override {
    Stream* stream = streamOfTrack(track_index);
    if (!stream) return OM_FORMAT_STREAM_NOT_FOUND;

    const auto& alternatives = stream->track->alternatives;
    const auto found =
        std::find_if(alternatives.begin(), alternatives.end(),
                     [&](const std::unique_ptr<SegmentIndex>& candidate) {
                       return candidate->id() == id;
                     });
    if (found == alternatives.end()) return OM_COMMON_INVALID_ARGUMENT;

    const size_t chosen = static_cast<size_t>(found - alternatives.begin());
    if (chosen == stream->alternative) return OM_SUCCESS;

    // Where the switch lands: the segment after the one being read. Whatever is
    // buffered belongs to the old alternative and is dropped, since its bytes
    // decode against the old initialization segment.
    const int64_t resume_ns = scaleToNs(currentMediaTime(*stream), stream->index().timescale());

    stream->alternative = chosen;
    stream->dropHeld();
    stream->init_inbox.reset();
    stream->needs_init = true;
    stream->finished = false;
    stream->reader->flushSegments();

    const SegmentIndex& index = stream->index();
    stream->read_ordinal = index.ordinalForTime(nsToScale(resume_ns, index.timescale()));
    ready_ = false;
    return OM_SUCCESS;
  }

  // --- Demuxing ---------------------------------------------------------------

  auto readPacket() -> Result<Packet, OMError> override {
    if (streams_.empty()) return Err(OM_COMMON_NOT_INITIALIZED);
    if (!ready_) {
      const OMError error = settleInits();
      if (error != OM_SUCCESS) return Err(error);
    }

    // Every stream that could still produce a packet has to be holding one before
    // the earliest can be picked out; otherwise a stream whose bytes have not
    // arrived is read as though it had ended, and its packets turn up later than
    // the ones they should have come before.
    OMError starved = OM_SUCCESS;
    int64_t starved_at = INT64_MAX;

    for (Stream& stream : streams_) {
      if (stream.finished || !stream.reader->needsSegment()) continue;
      const OMError error = advance(stream);
      if (error == OM_SUCCESS) continue;
      starved = error;
      starved_at = std::min(starved_at, pendingStartNs(stream));
    }

    Stream* earliest = nullptr;
    int64_t earliest_dts = INT64_MAX;
    for (Stream& stream : streams_) {
      const int64_t dts = stream.reader->nextDecodeTimeNs();
      if (dts < earliest_dts) {
        earliest_dts = dts;
        earliest = &stream;
      }
    }

    // A stream still waiting for bytes only holds everything up if what it is
    // waiting for comes before what is ready. Video segments routinely land ahead
    // of the audio covering the same instant, and stalling on that would make
    // every prefetch race decide whether playback stutters.
    if (starved != OM_SUCCESS && starved_at <= earliest_dts) return Err(starved);
    if (!earliest || earliest_dts == INT64_MAX) return Err(OM_FORMAT_END_OF_FILE);

    auto packet = earliest->reader->readPacket();
    if (packet.isErr()) return packet;

    Packet value = std::move(packet).unwrap();
    const size_t reader_track = static_cast<size_t>(value.stream_index);
    value.stream_index = reader_track < earliest->public_tracks.size()
                             ? earliest->public_tracks[reader_track]
                             : -1;
    if (value.stream_index < 0) return Err(OM_FORMAT_STREAM_NOT_FOUND);
    return Ok(std::move(value));
  }

  auto seek(int32_t stream_idx, int64_t timestamp, SeekMode mode) -> OMError override {
    (void)mode; // segment granularity is all a manifest offers; see below
    if (streams_.empty()) return OM_COMMON_NOT_INITIALIZED;
    if (timestamp < 0) return OM_COMMON_INVALID_ARGUMENT;
    if (stream_idx >= static_cast<int32_t>(tracks_.size())) return OM_FORMAT_STREAM_NOT_FOUND;

    // A presentation time, in nanoseconds, is the one thing every stream can be
    // positioned by: each has its own timescale and its own presentation offset.
    int64_t target_ns = 0;
    if (stream_idx < 0) {
      target_ns = timestamp * INT64_C(1'000);
    } else {
      // The caller's timestamp is in the track's own time base, which is the unit
      // the packets are in -- the media timescale, not whatever unit the manifest
      // counted segments in. Reading it back off the published track is what keeps
      // a seek and the timestamps it is aimed at in the same unit; for DASH the two
      // units normally agree, and for HLS they never do.
      if (tracks_[static_cast<size_t>(stream_idx)].time_base.den <= 0) {
        return OM_FORMAT_INVALID_TIMESTAMP;
      }
      target_ns = scaleToNs(
          timestamp, static_cast<uint32_t>(tracks_[static_cast<size_t>(stream_idx)].time_base.den));
    }

    for (Stream& stream : streams_) {
      const SegmentIndex& index = stream.index();
      stream.reader->flushSegments();
      stream.dropHeld();
      stream.finished = false;
      stream.read_ordinal = index.ordinalForTime(nsToScale(target_ns, index.timescale()));
    }

    // Landing on a segment boundary is as precise as a manifest lets a seek be,
    // and every segment starts at a sync sample, so this is already what
    // PREVIOUS_SYNC asks for. Trimming to an exact frame is the caller's to do by
    // dropping packets, since only it knows what its decoder has been given.
    return OM_SUCCESS;
  }

private:
  // Primary subtag only: `en` is what `en-US` is asking for.
  static auto sameLanguage(std::string_view a, std::string_view b) -> bool {
    const auto primary = [](std::string_view tag) {
      const size_t dash = tag.find('-');
      return dash == std::string_view::npos ? tag : tag.substr(0, dash);
    };
    const std::string_view left = primary(a);
    const std::string_view right = primary(b);
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](char x, char y) {
             return std::tolower(static_cast<unsigned char>(x)) ==
                    std::tolower(static_cast<unsigned char>(y));
           });
  }

  // Everything read is also downloaded, so this is where a presentation offering
  // eight audio languages stops costing eight audio streams. A track whose type the
  // source did not state is kept: in HLS only the initialization segment knows.
  auto selectTracks() const -> std::vector<const SegmentTrack*> {
    std::vector<const SegmentTrack*> chosen;
    for (const SegmentTrack& track : source_.tracks) {
      if (track.alternatives.empty()) continue;
      if (track.type == OM_MEDIA_SUBTITLE && !options_.with_subtitles) continue;
      chosen.push_back(&track);
    }
    if (options_.language.empty()) return chosen;

    // Per type, keep only what was asked for -- but only when the asked-for
    // language is actually on offer, since dropping every audio track because none
    // of them is French would leave the presentation silent.
    for (const OMMediaType type : {OM_MEDIA_AUDIO, OM_MEDIA_SUBTITLE}) {
      const bool offered = std::any_of(chosen.begin(), chosen.end(),
                                      [&](const SegmentTrack* track) {
                                        return track->type == type &&
                                               sameLanguage(track->language, options_.language);
                                      });
      if (!offered) continue;
      chosen.erase(std::remove_if(chosen.begin(), chosen.end(),
                                  [&](const SegmentTrack* track) {
                                    return track->type == type &&
                                           !sameLanguage(track->language, options_.language);
                                  }),
                   chosen.end());
    }
    return chosen;
  }

  auto chooseAlternative(const SegmentTrack& track) const -> size_t {
    // Sorted best-first by whoever built the source, so the first entry inside
    // every cap is the best one that fits, and falling off the end means nothing
    // fits and the least bad choice is the smallest.
    for (size_t i = 0; i < track.alternatives.size(); ++i) {
      const SegmentIndex& index = *track.alternatives[i];
      if (options_.max_bandwidth != 0 && index.bandwidth() > options_.max_bandwidth) continue;
      if (options_.max_width != 0 && index.width() > options_.max_width) continue;
      if (options_.max_height != 0 && index.height() > options_.max_height) continue;
      return i;
    }
    return track.alternatives.size() - 1;
  }

  auto firstTrackOf(size_t stream_index) const -> int32_t {
    const Stream& stream = streams_[stream_index];
    return stream.public_tracks.empty() ? -1 : stream.public_tracks.front();
  }

  auto streamOfTrack(int32_t track_index) -> Stream* {
    for (Stream& stream : streams_) {
      for (const int32_t index : stream.public_tracks) {
        if (index == track_index) return &stream;
      }
    }
    return nullptr;
  }

  auto streamOfTrack(int32_t track_index) const -> const Stream* {
    for (const Stream& stream : streams_) {
      for (const int32_t index : stream.public_tracks) {
        if (index == track_index) return &stream;
      }
    }
    return nullptr;
  }

  // In the stream's own timescale.
  auto currentMediaTime(const Stream& stream) const -> int64_t {
    const int64_t dts = stream.reader->nextDecodeTimeNs();
    if (dts != INT64_MAX) return nsToScale(dts, stream.index().timescale());
    const auto segment = stream.index().segmentAt(stream.read_ordinal);
    return segment ? segment->time : 0;
  }

  auto pendingStartNs(const Stream& stream) const -> int64_t {
    if (stream.needs_init) return INT64_MIN; // nothing can be read before this
    const auto segment = stream.index().segmentAt(stream.read_ordinal);
    if (!segment) return INT64_MAX;
    return scaleToNs(segment->time, stream.index().timescale());
  }

  // What was handed in, or else a fetch.
  auto obtain(const SegmentRequest& request, std::optional<std::vector<uint8_t>>& held)
      -> Result<std::vector<uint8_t>, OMError> {
    if (held) {
      std::vector<uint8_t> bytes = std::move(*held);
      held.reset();
      return Ok(std::move(bytes));
    }
    if (!options_.fetch) return Err(OM_IO_NOT_ENOUGH_DATA);
    return options_.fetch(request);
  }

  auto ensureInit(Stream& stream) -> OMError {
    if (!stream.needs_init) return OM_SUCCESS;

    const auto request = stream.index().initSegment();
    if (!request) return OM_FORMAT_NOT_SUPPORTED;

    auto bytes = obtain(*request, stream.init_inbox);
    if (bytes.isErr()) return std::move(bytes).unwrapErr();

    const OMError error = stream.reader->openInit(std::move(bytes).unwrap());
    if (error != OM_SUCCESS) return error;
    stream.needs_init = false;
    return OM_SUCCESS;
  }

  // In stream order, so a track index does not depend on which initialization
  // segment happened to arrive first.
  void publishTracks() {
    tracks_.clear();
    for (Stream& stream : streams_) {
      const SegmentIndex& index = stream.index();
      stream.public_tracks.clear();

      for (const Track& reader_track : stream.reader->tracks()) {
        Track track = reader_track;
        // Timestamps come off `tfdt` and `trun`, so their unit is the media
        // timescale the initialization segment declared -- not whatever unit the
        // manifest happened to count segments in. For DASH the two normally agree;
        // for HLS there is no declared unit at all, since a playlist measures
        // segments in seconds. Taking it from the samples' own header is right
        // either way, and is the only thing that makes a published time_base
        // describe the packets actually handed out.
        const uint32_t media_timescale = stream.reader->timescaleOf(reader_track.index);
        track.index = static_cast<int32_t>(tracks_.size());
        track.time_base = {1, static_cast<int32_t>(media_timescale)};
        if (track.bitrate == 0) track.bitrate = index.bandwidth();
        // Where this track sits in the presentation, converted into the unit the
        // timestamps are in.
        track.start_time = rescaleTime(index.presentationTimeOffset(), index.timescale(),
                                       media_timescale);
        if (track.duration <= 0 && source_.duration_ns > 0) {
          track.duration = nsToScale(source_.duration_ns, media_timescale);
        }
        if (!stream.track->language.empty() && track.metadata.get(LANGUAGE) == nullptr) {
          track.metadata.setString(LANGUAGE, stream.track->language);
        }
        stream.public_tracks.push_back(track.index);
        tracks_.push_back(std::move(track));
      }
    }
    ready_ = !tracks_.empty();
  }

  void settleInitsIfComplete() {
    for (const Stream& stream : streams_) {
      if (stream.needs_init && !stream.init_inbox) return;
    }
    settleInits();
  }

  // A partial track list would renumber as the rest arrived, so there is none until
  // every stream's initialization segment is in.
  auto settleInits() -> OMError {
    for (Stream& stream : streams_) {
      const OMError error = ensureInit(stream);
      if (error != OM_SUCCESS) return error;
    }
    publishTracks();
    return ready_ ? OM_SUCCESS : OM_FORMAT_NO_STREAMS;
  }

  auto advance(Stream& stream) -> OMError {
    for (int attempt = 0; attempt < MAX_EMPTY_SEGMENTS; ++attempt) {
      const SegmentIndex& index = stream.index();
      if (const auto bound = index.segmentCount(); bound && stream.read_ordinal >= *bound) {
        stream.finished = true;
        return OM_SUCCESS;
      }
      const auto segment = index.segmentAt(stream.read_ordinal);
      if (!segment) {
        stream.finished = true;
        return OM_SUCCESS;
      }

      std::optional<std::vector<uint8_t>> held = stream.take(stream.read_ordinal);

      auto bytes = obtain(segment->request, held);
      if (bytes.isErr()) {
        // Put it back: a fetch that failed or declined must leave the stream able to
        // try the very same segment again.
        if (held) stream.hold(stream.read_ordinal, std::move(*held));
        return std::move(bytes).unwrapErr();
      }

      const OMError error = stream.reader->pushSegment(std::move(bytes).unwrap());
      ++stream.read_ordinal;
      if (error == OM_SUCCESS) return OM_SUCCESS;
      if (error != OM_FORMAT_NO_STREAMS) return error;
      // A segment with nothing in it for this track: skip it and read on.
    }
    return OM_FORMAT_CORRUPTED;
  }
};

} // namespace

auto createSegmentDemuxer(SegmentSource source, const StreamOptions& options)
    -> Result<std::unique_ptr<SegmentedDemuxer>, OMError> {
  auto demuxer = std::make_unique<SegmentDemuxer>();
  const OMError error = demuxer->start(std::move(source), options);
  if (error != OM_SUCCESS) return Err(error);
  return Ok(std::unique_ptr<SegmentedDemuxer>(std::move(demuxer)));
}

auto fileFetch() -> FetchFn {
  return [](const SegmentRequest& request) -> Result<std::vector<uint8_t>, OMError> {
    const std::string path = urlToLocalPath(request.url);
    auto input = InputStream::createFileStream(path);
    if (!input || !input->isValid()) return Err(OM_IO_OPEN_FAILED);

    if (request.offset != 0 && !input->seek(request.offset, Whence::BEG)) {
      return Err(OM_IO_SEEK_FAILED);
    }
    if (request.length < 0) return Ok(readAll(*input));

    std::vector<uint8_t> bytes(static_cast<size_t>(request.length));
    const size_t read = input->read(bytes);
    if (read != bytes.size()) return Err(OM_IO_NOT_ENOUGH_DATA);
    return Ok(std::move(bytes));
  };
}

} // namespace openmedia
