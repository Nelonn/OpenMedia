#include <streaming/segment_demuxer.hpp>

#include <algorithm>
#include <array>
#include <adts.hpp>
#include <bmff_fragment.hpp>
#include <cctype>
#include <cstddef>
#include <map>
#include <mpegts.hpp>
#include <openmedia/io.hpp>
#include <openmedia/metadata_keys.hpp>
#include <optional>
#include <util/io_util.hpp>
#include <util/timescale.hpp>
#include <vector>

namespace openmedia {
namespace {

// How far ahead a stream will recognise one of its own segments. Naming a segment
// means building its URL, so this is also what an append that matches nothing costs
// -- hence a generous prefetch depth rather than an unbounded one.
constexpr uint64_t RECOGNITION_WINDOW = 64;

// A segment yielding no sample for its track is skipped, but a run of them means
// the addressing is wrong rather than one segment, and looping over thousands of
// them would hide that.
constexpr int MAX_EMPTY_SEGMENTS = 8;

// Segments stepped over without reading anything -- gaps, and segments whose cipher
// this caller cannot answer for -- in one call. Nothing was read to find out, so
// these do not count as empty ones; but an index that bounds nothing and announces
// only gaps would otherwise be walked for ever.
constexpr uint64_t MAX_SKIPPED_SEGMENTS = 4096;

// How much of a segment is read at a time from a StreamOptions::stream_fetch: small
// enough that a 4K segment's first picture does not wait on megabytes it does not
// need, large enough not to be a syscall per transport packet.
constexpr size_t PULL_CHUNK = 64u << 10;

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
    // Both `file:///C:/x` and `file://C:/x` are written in the wild, and the slash
    // in front of a drive letter is not part of the path.
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
    std::unique_ptr<SegmentReader> reader;

    // Reader track index to the index this demuxer publishes it under. Nothing in
    // the format says an alternative carries only one track, so it is explicit.
    std::vector<int32_t> public_tracks;

    bool needs_init = true;
    bool finished = false;
    uint64_t read_ordinal = 0; // the media segment the reader is on

    // The initialization section openInit() was last given, so that a playlist
    // replacing it part-way through is noticed and the reader rebuilt against the
    // new one -- rather than fed segments whose parameter sets it has never seen.
    // Nothing for a stream whose segments carry their own headers.
    std::optional<SegmentRequest> loaded_init;

    // Part-way through `read_ordinal` and waiting for the rest. Its bytes are not
    // held here: they went straight into the reader.
    bool streaming = false;
    // Where the streamed segment is read from, when StreamOptions named somewhere
    // rather than the caller pushing its pieces in.
    std::unique_ptr<InputStream> pulling;
    // The decode time last handed out, which is where whatever is missing must come
    // at or after.
    int64_t last_read_ns = INT64_MIN;

    std::optional<std::vector<uint8_t>> init_inbox;
    bool init_complete = false;

    // A segment handed over, whole or in pieces. Half a segment is worth holding:
    // once this track reaches it, the reader reads what there is while the rest
    // arrives.
    struct Held {
      std::vector<uint8_t> bytes;
      bool complete = true;
    };
    std::map<uint64_t, Held> inbox;
    size_t buffered_bytes = 0;

    void hold(uint64_t ordinal, std::vector<uint8_t> bytes, bool complete) {
      buffered_bytes += bytes.size();
      if (const auto previous = inbox.find(ordinal); previous != inbox.end()) {
        buffered_bytes -= previous->second.bytes.size();
      }
      inbox.insert_or_assign(ordinal, Held {std::move(bytes), complete});
    }

    void appendHeld(uint64_t ordinal, std::span<const uint8_t> more, bool last) {
      Held& held = inbox[ordinal];
      held.bytes.insert(held.bytes.end(), more.begin(), more.end());
      held.complete = last;
      buffered_bytes += more.size();
    }

    auto takeHeld(uint64_t ordinal) -> std::optional<Held> {
      const auto found = inbox.find(ordinal);
      if (found == inbox.end()) return std::nullopt;
      Held held = std::move(found->second);
      buffered_bytes -= held.bytes.size();
      inbox.erase(found);
      return held;
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
    // This demuxer needs a described presentation and a way to name segments, which
    // is more than a byte stream carries; the DASH and HLS entry points take one.
    return OM_FORMAT_NOT_SUPPORTED;
  }

  auto start(SegmentSource source, const StreamOptions& options) -> OMError {
    source_ = std::move(source);
    options_ = options;

    for (const SegmentTrack* track : selectTracks()) {
      Stream stream;
      stream.track = track;
      stream.alternative = chooseAlternative(*track);

      // An initialization section settles it: fragmented MP4. Without one the
      // segments are MPEG-TS, bare ADTS, or self-contained fragmented MP4, and only
      // the bytes say which -- so this one stands in until the first segment
      // arrives, an unprimed reader saying it needs a segment and holding no
      // packet, which is the truth of the matter.
      stream.reader = track->alternatives[stream.alternative]->initSegment()
                          ? createFragmentedMP4Reader()
                          : createMpegTsReader();
      if (!stream.reader) return OM_COMMON_OUT_OF_MEMORY;

      // `#EXT-X-START` asks playback to begin somewhere other than the start, which
      // for a live playlist is the difference between joining at the live edge and
      // starting from whatever is still in the window.
      if (source_.start_offset_ns > 0) {
        const SegmentIndex& index = *track->alternatives[stream.alternative];
        stream.read_ordinal =
            index.ordinalForTime(nsToScale(source_.start_offset_ns, index.timescale()));
      }
      streams_.push_back(std::move(stream));
    }

    if (streams_.empty()) return OM_FORMAT_NO_STREAMS;

    // A fetch function means the caller is content to block, so tracks() can be
    // populated on return the way every other demuxer behaves.
    if (options_.fetch || options_.stream_fetch) {
      for (Stream& stream : streams_) {
        const OMError error = ensureInit(stream);
        if (error != OM_SUCCESS) return error;
      }
      return publishTracks();
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

    // An initialization segment blocks everything behind it, so they all come first,
    // in stream order, without competing on time with media. Which one a stream
    // wants is whatever the segment it is about to read decodes against, so a
    // playlist that replaces it mid-way asks for the replacement here.
    for (size_t i = 0; i < streams_.size(); ++i) {
      const Stream& stream = streams_[i];
      if (stream.init_inbox) continue; // already in hand or still arriving
      const auto request = initWantedBy(stream);
      if (!request) continue;
      SegmentPlan plan;
      plan.request = *request;
      plan.track_index = firstTrackOf(i);
      plan.initialization = true;
      plans.push_back(std::move(plan));
      if (plans.size() >= max_count) return plans;
    }

    // Beyond that, the segment wanted soonest is whichever starts earliest, which
    // is also the order that keeps every track's buffer growing at the same rate.
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
        // Being read as it arrives, so asking again would download it twice.
        if (stream.streaming && ordinal == stream.read_ordinal) continue;
        const auto segment = index.segmentAt(ordinal);
        if (!segment) break;
        // Announced but not published: asking for it would be asking for a download
        // that cannot succeed, over and over.
        if (segment->gap) continue;

        SegmentPlan plan;
        plan.request = segment->request;
        plan.track_index = firstTrackOf(i);
        plan.start_time_ns = scaleToNs(segment->time, index.timescale());
        plan.duration_ns = scaleToNs(static_cast<int64_t>(segment->duration), index.timescale());
        plan.program_date_time_ms = segment->program_date_time_ms;
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

    // Every stream wanting this initialization section, not merely the first:
    // renditions of one presentation share a `moov` often enough, and serving one
    // of them would leave the others waiting for bytes already delivered.
    bool is_init = false;
    for (Stream& stream : streams_) {
      const auto init = initWantedBy(stream);
      if (!init || !(*init == request)) continue;
      is_init = true;
      if (stream.init_inbox && !stream.init_complete) return OM_COMMON_INVALID_ARGUMENT;
      stream.init_inbox = bytes;
      stream.init_complete = true;
    }
    if (is_init) {
      settleInitsIfComplete();
      return OM_SUCCESS;
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
        // One request is delivered one way or the other: taking the whole of a
        // segment whose pieces are already going in would leave those that follow
        // nowhere to go.
        if (isPartlyDelivered(stream, ordinal)) return OM_COMMON_INVALID_ARGUMENT;
        // Full is not unwanted: dropping these silently would leave the caller
        // waiting forever for a segment it believes it delivered.
        if (options_.max_buffered_bytes != 0 &&
            stream.buffered_bytes + bytes.size() > options_.max_buffered_bytes) {
          return OM_COMMON_OVERFLOW;
        }
        stream.hold(ordinal, std::move(bytes), /*complete=*/true);
        // These may be the bytes a stream with no initialization section was
        // waiting on to describe itself.
        if (stream.needs_init) settleInitsIfComplete();
        return OM_SUCCESS;
      }
    }

    // Seeked past, or switched away from. The caller cannot always know that in
    // time, so dropping the bytes is the whole of the correct response.
    return OM_SUCCESS;
  }

  auto appendSegmentChunk(const SegmentRequest& request, std::span<const uint8_t> bytes,
                          bool last) -> OMError override {
    bool is_init = false;
    bool init_done = false;
    for (Stream& stream : streams_) {
      const auto init = initWantedBy(stream);
      if (!init || !(*init == request)) continue;
      is_init = true;
      if (stream.init_complete) return OM_COMMON_INVALID_ARGUMENT;
      if (!stream.init_inbox) stream.init_inbox.emplace();
      stream.init_inbox->insert(stream.init_inbox->end(), bytes.begin(), bytes.end());
      // Half an initialization segment is nothing to read: it is where the track
      // list comes from, and half a track list is not one.
      if (!last) continue;
      stream.init_complete = true;
      init_done = true;
    }
    if (is_init) {
      if (init_done) settleInitsIfComplete();
      return OM_SUCCESS;
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

        // A cipher over the whole segment has no answer for part of one, so such a
        // segment is accumulated and deciphered once it is whole -- which is what
        // holding it does, and is why it is checked before the streaming branch.
        if (segment->encryption) {
          if (options_.max_buffered_bytes != 0 &&
              stream.buffered_bytes + bytes.size() > options_.max_buffered_bytes) {
            return OM_COMMON_OVERFLOW;
          }
          stream.appendHeld(ordinal, bytes, last);
          return OM_SUCCESS;
        }

        // The segment being read right now: its pieces go straight to the reader,
        // which is the whole point of the call.
        if (stream.streaming && ordinal == stream.read_ordinal) {
          if (const OMError fed = stream.reader->feedSegment(bytes); fed != OM_SUCCESS) {
            return fed;
          }
          // These may be the bytes a stream with no initialization section was
          // waiting on to describe itself.
          if (stream.needs_init) settleInitsIfComplete();
          if (last) return closeStreamed(stream);
          return OM_SUCCESS;
        }

        if (const auto held = stream.inbox.find(ordinal);
            held != stream.inbox.end() && held->second.complete) {
          return OM_COMMON_INVALID_ARGUMENT; // delivered whole already
        }
        // Not reached yet, so the pieces accumulate; the reader picks up however
        // much has turned up once this track gets there.
        if (options_.max_buffered_bytes != 0 &&
            stream.buffered_bytes + bytes.size() > options_.max_buffered_bytes) {
          return OM_COMMON_OVERFLOW;
        }
        stream.appendHeld(ordinal, bytes, last);
        if (stream.needs_init) settleInitsIfComplete();
        return OM_SUCCESS;
      }
    }
    return OM_SUCCESS;
  }

  auto bufferedDurationNs(int32_t track_index) const -> int64_t override {
    const Stream* stream = streamOfTrack(track_index);
    if (!stream) return -1;

    int64_t total = stream->reader->remainingDurationNs();
    const SegmentIndex& index = stream->index();
    for (const auto& [ordinal, held] : stream->inbox) {
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

    // Named by start time, because the two alternatives number their segments
    // independently and that instant is what they have in common. `read_ordinal` is
    // the segment the old alternative had not reached; resuming from the time being
    // read instead would land inside a segment already delivered and hand out its
    // packets a second time.
    const auto boundary = stream->index().segmentAt(stream->read_ordinal);
    const int64_t resume_ns =
        boundary ? scaleToNs(boundary->time, stream->index().timescale()) : -1;

    // What is buffered decodes against the old initialization segment.
    stream->alternative = chosen;
    stream->dropHeld();
    stream->init_inbox.reset();
    stream->init_complete = false;
    stream->needs_init = true;
    stream->loaded_init.reset();
    stream->finished = false;
    stream->streaming = false;
    stream->pulling.reset();
    stream->last_read_ns = INT64_MIN;
    stream->reader->flushSegments();

    const SegmentIndex& index = stream->index();
    if (resume_ns >= 0) {
      stream->read_ordinal = index.ordinalForTime(nsToScale(resume_ns, index.timescale()));
    } else {
      // The old alternative was read to its end, and a switch is not a reason to
      // play anything over again.
      stream->read_ordinal = index.segmentCount().value_or(0);
    }
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
    // the earliest can be picked out, or a stream whose bytes have not arrived reads
    // as though it had ended and its packets turn up out of order.
    //
    // Two things are tracked about a stream that could not advance: when what it is
    // missing falls due, and which failure to report. Waiting for bytes is a
    // different matter from failing to read them, and a stream reporting the second
    // must not have it overwritten by another stream reporting the first -- that was
    // what let a segment failing to fetch forever look like a prefetch running
    // slightly behind.
    OMError starved = OM_SUCCESS;
    int64_t starved_at = INT64_MAX;
    OMError failed = OM_SUCCESS;

    for (Stream& stream : streams_) {
      if (stream.finished || !stream.reader->needsSegment()) continue;
      const OMError error = advance(stream);
      if (error == OM_SUCCESS) continue;
      if (error == OM_IO_NOT_ENOUGH_DATA) {
        if (starved == OM_SUCCESS) starved = error;
        starved_at = std::min(starved_at, pendingStartNs(stream));
        continue;
      }
      // Nothing is going to arrive that answers this, so it is reported whatever
      // else is readable: the first such failure, since the ones after it are as
      // likely to be its consequence as its own problem.
      if (failed == OM_SUCCESS) failed = error;
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

    // A stream merely waiting for bytes only holds everything up when what it waits
    // for comes before what is ready. Video segments routinely land ahead of the
    // audio covering the same instant, and stalling on that would let every prefetch
    // race decide whether playback stutters. A stream that failed is different and
    // holds nothing up -- but once there is nothing left to hand out, its failure is
    // the answer rather than a clean end of file.
    if (starved != OM_SUCCESS && starved_at <= earliest_dts) return Err(starved);
    if (!earliest || earliest_dts == INT64_MAX) {
      if (failed != OM_SUCCESS) return Err(failed);
      if (starved != OM_SUCCESS) return Err(starved);
      return Err(OM_FORMAT_END_OF_FILE);
    }

    // What this stream is still missing comes at or after this, so a stream part-way
    // through a segment holds up only the streams behind it.
    earliest->last_read_ns = earliest_dts;

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

    // Nanoseconds of presentation time is the one thing every stream can be
    // positioned by: each has its own timescale and presentation offset.
    int64_t target_ns = 0;
    if (stream_idx < 0) {
      target_ns = timestamp * INT64_C(1'000);
    } else {
      // The caller's timestamp is in the track's own time base -- the media
      // timescale, not whatever unit the manifest counted segments in. Reading it
      // back off the published track keeps a seek and the timestamps it aims at in
      // the same unit; for DASH the two normally agree, for HLS never.
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
      // What was half-read belonged to where the reader was; the pieces still coming
      // will match nothing and be dropped.
      stream.streaming = false;
      stream.pulling.reset();
      stream.last_read_ns = INT64_MIN;
      stream.read_ordinal = index.ordinalForTime(nsToScale(target_ns, index.timescale()));
    }

    // A segment boundary is as precise as a manifest lets a seek be, and every
    // segment starts at a sync sample, so this is already PREVIOUS_SYNC. Trimming to
    // an exact frame is the caller's: only it knows what its decoder has been given.
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
  // eight audio languages stops costing eight audio streams. A track of unstated
  // type is kept: in HLS only the initialization segment knows.
  auto selectTracks() const -> std::vector<const SegmentTrack*> {
    std::vector<const SegmentTrack*> chosen;
    for (const SegmentTrack& track : source_.tracks) {
      if (track.alternatives.empty()) continue;
      if (track.type == OM_MEDIA_SUBTITLE && !options_.with_subtitles) continue;
      chosen.push_back(&track);
    }
    if (options_.language.empty()) return chosen;

    // Per type, keep only what was asked for -- but only when it is on offer, since
    // dropping every audio track because none is French would leave the
    // presentation silent.
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
    // Sorted best-first by whoever built the source, so the first entry inside every
    // cap is the best fit; falling off the end means nothing fits, and the least bad
    // choice is then the smallest.
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

  auto pendingStartNs(const Stream& stream) const -> int64_t {
    if (stream.needs_init) return INT64_MIN; // nothing can be read before this
    // Part-way through a segment, what is missing comes after what has been read out
    // of it -- not at the segment's start, which would stall every other track for a
    // whole segment's length.
    if (stream.streaming && stream.last_read_ns != INT64_MIN) return stream.last_read_ns;
    const auto segment = stream.index().segmentAt(stream.read_ordinal);
    if (!segment) return INT64_MAX;
    return scaleToNs(segment->time, stream.index().timescale());
  }

  // Whether this ordinal's pieces are going in one at a time, so the whole of it
  // must not also be offered.
  static auto isPartlyDelivered(const Stream& stream, uint64_t ordinal) -> bool {
    if (stream.streaming && ordinal == stream.read_ordinal) return true;
    const auto held = stream.inbox.find(ordinal);
    return held != stream.inbox.end() && !held->second.complete;
  }

  // What was handed in, or else a fetch.
  auto obtain(const SegmentRequest& request, std::optional<std::vector<uint8_t>>& held)
      -> Result<std::vector<uint8_t>, OMError> {
    if (held) {
      std::vector<uint8_t> bytes = std::move(*held);
      held.reset();
      return Ok(std::move(bytes));
    }
    if (options_.fetch) return options_.fetch(request);
    // An initialization segment -- and the first segment of a stream that has none
    // -- is wanted whole however it is read, because the track list comes out of it
    // and half a track list is not one.
    if (options_.stream_fetch) {
      auto input = options_.stream_fetch(request);
      if (input.isErr()) return Err(std::move(input).unwrapErr());
      const std::unique_ptr<InputStream> source = std::move(input).unwrap();
      if (!source || !source->isValid()) return Err(OM_IO_OPEN_FAILED);
      std::vector<uint8_t> bytes = readAll(*source);
      if (bytes.empty()) return Err(OM_IO_NOT_ENOUGH_DATA);
      return Ok(std::move(bytes));
    }
    return Err(OM_IO_NOT_ENOUGH_DATA);
  }

  // --- Reading a segment as it arrives ----------------------------------------

  // Puts the reader at the start of a segment with however much is in hand. From
  // here the reader is mid-segment, so read_ordinal does not move until it ends.
  auto openStreamed(Stream& stream, const Segment& segment, std::span<const uint8_t> arrived)
      -> OMError {
    const SegmentIndex& index = stream.index();
    stream.reader->expectSegmentStart(scaleToNs(segment.time, index.timescale()),
                                      segment.discontinuity);
    if (const OMError opened = stream.reader->beginSegment(); opened != OM_SUCCESS) {
      return opened;
    }
    stream.streaming = true;
    if (arrived.empty()) return OM_SUCCESS;
    const OMError fed = stream.reader->feedSegment(arrived);
    // Left mid-segment with no way on, the stream would wait for the rest of a
    // segment nothing is going to finish.
    if (fed != OM_SUCCESS) closeStreamed(stream);
    return fed;
  }

  auto closeStreamed(Stream& stream) -> OMError {
    const OMError error = stream.reader->endSegment();
    stream.streaming = false;
    stream.pulling.reset();
    ++stream.read_ordinal;
    // A segment with nothing for this track -- including a download that stopped
    // before anything readable arrived -- is skipped, not failed.
    return error == OM_FORMAT_NO_STREAMS ? OM_SUCCESS : error;
  }

  // Reads from the segment's own stream until the reader holds a packet or the
  // segment runs out -- which is what makes stream_fetch worth having: the first
  // picture costs the first chunk rather than the whole segment.
  auto pumpStreamed(Stream& stream) -> OMError {
    while (stream.reader->needsSegment()) {
      const size_t read = stream.pulling->read(pull_buffer_);
      if (read == 0) {
        // A short read is not the end -- a stream over a socket returns what has
        // arrived -- so only isEOF() may close the segment. Treating every empty
        // read as the end truncated whatever had not been sent yet.
        if (stream.pulling->isEOF()) return closeStreamed(stream);
        return OM_IO_NOT_ENOUGH_DATA;
      }
      const OMError fed = stream.reader->feedSegment(std::span(pull_buffer_).first(read));
      if (fed != OM_SUCCESS) {
        closeStreamed(stream);
        return fed;
      }
    }
    return OM_SUCCESS;
  }

  // --- Initialization sections -------------------------------------------------

  // The initialization section the stream needs next, which is whatever the segment
  // it is about to read decodes against -- not whatever the presentation opened
  // with. A playlist may replace it part-way through, and the segments after that
  // point decode against parameter sets the ones before it never had.
  //
  // Nothing once the reader already holds the right one, and nothing at all for a
  // stream whose segments carry their own headers.
  auto initWantedBy(const Stream& stream) const -> std::optional<SegmentRequest> {
    const auto segment = stream.index().segmentAt(stream.read_ordinal);
    const std::optional<SegmentRequest> wanted =
        segment ? segment->init : stream.index().initSegment();
    if (!wanted) return std::nullopt;
    if (stream.loaded_init && *stream.loaded_init == *wanted) return std::nullopt;
    return wanted;
  }

  // Opens a reader against an initialization section, replacing whatever was there.
  // A reader of its own every time, because what the new section describes may have
  // nothing to do with what the old one did.
  auto openInit(Stream& stream, const SegmentRequest& request, std::vector<uint8_t> bytes)
      -> OMError {
    auto reader = createFragmentedMP4Reader();
    if (!reader) return OM_COMMON_OUT_OF_MEMORY;
    if (const OMError error = reader->openInit(bytes); error != OM_SUCCESS) return error;

    stream.reader = std::move(reader);
    stream.loaded_init = request;
    stream.init_inbox.reset();
    stream.init_complete = false;
    stream.needs_init = false;
    return OM_SUCCESS;
  }

  auto ensureInit(Stream& stream) -> OMError {
    if (!stream.needs_init) return OM_SUCCESS;

    if (const auto request = initWantedBy(stream)) {
      // Still arriving in pieces, and part of one describes nothing.
      if (stream.init_inbox && !stream.init_complete) return OM_IO_NOT_ENOUGH_DATA;
      auto bytes = obtain(*request, stream.init_inbox);
      if (bytes.isErr()) return std::move(bytes).unwrapErr();
      return openInit(stream, *request, std::move(bytes).unwrap());
    }
    return primeFromFirstSegment(stream);
  }

  // --- Protected segments ------------------------------------------------------

  // Whether a segment can be read at all, and what has to happen to it first.
  //
  // A cipher over a whole segment leaves nothing a container parser can recognise,
  // so there is no reading it without a key -- and only the caller can hold one.
  // Saying so outright is the point: parsed anyway, the ciphertext fails to look
  // like any container, and what came back was corruption rather than the one fact
  // that matters.
  auto decryptSegment(const Segment& segment, std::vector<uint8_t> bytes)
      -> Result<std::vector<uint8_t>, OMError> {
    if (!segment.encryption) return Ok(std::move(bytes));
    // A sample-level scheme leaves the container readable, and each packet carries
    // its own protection; nothing here applies to it, and a reader that does not
    // report it is better stopped than believed.
    if (!segment.encryption->whole_segment) return Err(OM_FORMAT_ENCRYPTED);
    if (!options_.decrypt) return Err(OM_FORMAT_ENCRYPTED);

    auto plain = options_.decrypt(*segment.encryption, bytes);
    if (plain.isErr()) return plain;
    std::vector<uint8_t> out = std::move(plain).unwrap();
    if (out.empty()) return Err(OM_FORMAT_ENCRYPTED);
    return Ok(std::move(out));
  }

  // --- Priming a stream from its own media --------------------------------------

  // Which reader these bytes call for. A stream the manifest gave no initialization
  // section for is one of three shapes, and only the bytes say which: a transport
  // stream, bare ADTS frames behind an ID3 timestamp, or fragmented MP4 carrying a
  // header in every segment. Named once so the two places that prime a stream
  // cannot come to different answers about the same bytes.
  struct ReaderProbe {
    std::unique_ptr<SegmentReader> reader;
    // Whether the segment doubles as the header, so openInit() has to be given it
    // before the samples behind it in the same bytes are pushed.
    bool self_contained = false;
  };

  static auto probeReader(std::span<const uint8_t> bytes) -> ReaderProbe {
    if (looksLikeMpegTs(bytes)) return {createMpegTsReader(), false};
    if (looksLikeAdts(bytes)) return {createAdtsReader(), false};
    return {createFragmentedMP4Reader(), true};
  }

  // A stream with no initialization section is settled by its own first media
  // segment: the headers are in there, repeated, and the samples beside them read as
  // media like any others.
  auto primeFromFirstSegment(Stream& stream) -> OMError {
    for (int attempt = 0; attempt < MAX_EMPTY_SEGMENTS; ++attempt) {
      const SegmentIndex& index = stream.index();
      const auto segment = index.segmentAt(stream.read_ordinal);
      if (!segment) return OM_FORMAT_NO_STREAMS;

      if (stream.streaming) {
        if (!stream.reader->tracksDescribed()) return OM_IO_NOT_ENOUGH_DATA;
        // Settled out of a segment the reader is part-way through, so read_ordinal
        // stays put until that segment ends.
        stream.needs_init = false;
        return OM_SUCCESS;
      }

      // Nothing to prime from and nothing coming: step over it rather than report a
      // stream that cannot be described, which it can -- by the next segment.
      if (segment->gap) {
        ++stream.read_ordinal;
        continue;
      }

      // A segment still arriving can describe the stream before it is whole, as long
      // as what arrived says which shape it is. A transport stream does: its program
      // tables and parameter sets sit in its first packets and repeat throughout.
      // Self-contained fragmented MP4 does not, since openInit() needs the whole
      // `moov` -- so that one waits for the segment, once. Neither can be done at
      // all for a segment that has to be deciphered first.
      if (const auto partial = stream.inbox.find(stream.read_ordinal);
          !segment->encryption && partial != stream.inbox.end() &&
          !partial->second.complete) {
        ReaderProbe probe = probeReader(partial->second.bytes);
        if (probe.self_contained) return OM_IO_NOT_ENOUGH_DATA;
        const std::optional<Stream::Held> held = stream.takeHeld(stream.read_ordinal);

        stream.reader = std::move(probe.reader);
        if (!stream.reader) return OM_COMMON_OUT_OF_MEMORY;
        if (const OMError opened = stream.reader->openInit({}); opened != OM_SUCCESS) {
          return opened;
        }
        if (const OMError error = openStreamed(stream, *segment, held->bytes);
            error != OM_SUCCESS) {
          return error;
        }
        if (!stream.reader->tracksDescribed()) return OM_IO_NOT_ENOUGH_DATA;
        stream.needs_init = false;
        return OM_SUCCESS;
      }

      std::optional<Stream::Held> inbox_held = stream.takeHeld(stream.read_ordinal);
      std::optional<std::vector<uint8_t>> held;
      if (inbox_held && inbox_held->complete) held = std::move(inbox_held->bytes);
      auto fetched = obtain(segment->request, held);
      if (fetched.isErr()) {
        // Put it back, so a fetch that failed or declined leaves the stream able to
        // try the same segment again.
        if (held) stream.hold(stream.read_ordinal, std::move(*held), /*complete=*/true);
        return std::move(fetched).unwrapErr();
      }

      auto plain = decryptSegment(*segment, std::move(fetched).unwrap());
      if (plain.isErr()) return std::move(plain).unwrapErr();
      std::vector<uint8_t> bytes = std::move(plain).unwrap();

      // A reader of its own every time, the clean slate openInit() gives the
      // fragmented-MP4 path: priming also happens after a quality switch, and the
      // rendition switched to may be laid out nothing like the last -- another
      // program, other stream identifiers.
      ReaderProbe probe = probeReader(bytes);
      stream.reader = std::move(probe.reader);
      if (!stream.reader) return OM_COMMON_OUT_OF_MEMORY;
      // Self-contained fragmented MP4 reads its own header before the samples behind
      // it in the same bytes, so that one segment is handed over twice -- the only
      // copy any of this makes.
      const OMError opened = probe.self_contained ? stream.reader->openInit(bytes)
                                                  : stream.reader->openInit({});
      if (opened != OM_SUCCESS) return opened;

      stream.reader->expectSegmentStart(scaleToNs(segment->time, index.timescale()),
                                        segment->discontinuity);
      const OMError pushed = stream.reader->pushSegment(std::move(bytes));
      ++stream.read_ordinal;
      if (pushed != OM_SUCCESS && pushed != OM_FORMAT_NO_STREAMS) return pushed;

      // Priming belongs to the stream, not one segment: one that described no track
      // leaves it unprimed, and the next is tried.
      if (!stream.reader->tracks().empty()) {
        stream.needs_init = false;
        return OM_SUCCESS;
      }
    }
    return OM_FORMAT_CORRUPTED;
  }

  // In stream order, so a track index does not depend on which initialization
  // segment arrived first.
  //
  // Rebuilt rather than patched, since a reader opened against a new initialization
  // section describes its tracks afresh -- but written back element by element where
  // the count is unchanged, which is every case that is not a malformed ladder. That
  // keeps the vector tracks() handed out, and any reference into it, valid across a
  // quality switch; a switch that would renumber the tracks is refused instead,
  // because a caller holding track 1 has no way to learn it now means something else.
  auto publishTracks() -> OMError {
    std::vector<Track> next;
    Dictionary metadata;
    for (Stream& stream : streams_) {
      const SegmentIndex& index = stream.index();
      stream.public_tracks.clear();

      // Whatever the segments themselves said about the presentation, which for a
      // protected stream is where the `pssh` headers a licence request starts from
      // come out. Nothing else carries them, so dropping them left a caller holding
      // encrypted tracks and no way to ask for a key.
      for (const auto& [key, value] : stream.reader->metadata()) {
        if (metadata.get(key) == nullptr) metadata.set(key, value);
      }

      for (const Track& reader_track : stream.reader->tracks()) {
        Track track = reader_track;
        // Timestamps come off `tfdt` and `trun`, so their unit is the media
        // timescale the initialization segment declared -- not whatever unit the
        // manifest counted segments in. Taking it from the samples' own header is
        // the only thing that makes a published time_base describe the packets
        // actually handed out.
        const uint32_t media_timescale = stream.reader->timescaleOf(reader_track.index);
        track.index = static_cast<int32_t>(next.size());
        track.time_base = {1, static_cast<int32_t>(media_timescale)};
        if (track.bitrate == 0) track.bitrate = index.bandwidth();
        // A video track whose own header never stated its size, which happens when
        // the parameter sets are in the segments. The manifest's figure is then all
        // a caller sizing a window before the first frame has to go on.
        if (track.format.type == OM_MEDIA_VIDEO && track.format.video.width == 0) {
          track.format.video.width = index.width();
          track.format.video.height = index.height();
        }
        // The same for a channel count the manifest declared and the media has not
        // got round to stating.
        if (track.format.type == OM_MEDIA_AUDIO && track.format.audio.channels == 0) {
          track.format.audio.channels = stream.track->channels;
        }
        // Where this track sits in the presentation, in the timestamps' own unit.
        track.start_time = rescaleTime(index.presentationTimeOffset(), index.timescale(),
                                       media_timescale);
        if (track.duration <= 0 && source_.duration_ns > 0) {
          track.duration = nsToScale(source_.duration_ns, media_timescale);
        }
        // What the manifest says about the track, which the media itself does not:
        // a rendition's language, its label, and the part it plays. Added rather
        // than overwritten, since the media saying so itself is the better source.
        if (!stream.track->language.empty() && track.metadata.get(LANGUAGE) == nullptr) {
          track.metadata.setString(LANGUAGE, stream.track->language);
        }
        if (!stream.track->name.empty() && track.metadata.get(TITLE) == nullptr) {
          track.metadata.setString(TITLE, stream.track->name);
        }
        track.disposition = static_cast<OMDisposition>(
            static_cast<uint16_t>(track.disposition) |
            static_cast<uint16_t>(stream.track->disposition));
        // HLS declares protection in the playlist, and a transport stream carries no
        // sign of it at all, so without this a SAMPLE-AES track looks clear right up
        // to the point where readPacket() refuses it.
        describeEncryption(stream, track, metadata);
        stream.public_tracks.push_back(track.index);
        next.push_back(std::move(track));
      }
    }

    if (next.empty()) {
      ready_ = false;
      return OM_FORMAT_NO_STREAMS;
    }
    metadata_ = std::move(metadata);
    if (tracks_.empty() || tracks_.size() == next.size()) {
      if (tracks_.empty()) {
        tracks_ = std::move(next);
      } else {
        for (size_t i = 0; i < next.size(); ++i) tracks_[i] = std::move(next[i]);
      }
      ready_ = true;
      return OM_SUCCESS;
    }
    // A published track list cannot change shape underneath its caller.
    ready_ = false;
    return OM_FORMAT_NO_STREAMS;
  }

  // What the manifest says about a track's protection, for the schemes the media
  // itself does not declare. A key is named per segment, so the one in force where
  // the stream is reading is the one to report; a stream that starts in the clear
  // and is protected later says so when it gets there.
  void describeEncryption(const Stream& stream, Track& track, Dictionary& metadata) const {
    const auto segment = stream.index().segmentAt(stream.read_ordinal);
    if (!segment || !segment->encryption) return;
    const SegmentEncryption& key = *segment->encryption;

    track.metadata.setBool(ENCRYPTED, true);
    track.metadata.setString(ENCRYPTION_SCHEME, key.method);
    metadata.setBool(ENCRYPTED, true);
    // The URI is init data in the only sense HLS has one: for `identity` it is where
    // to fetch the key, and for everything else it is what the key system is handed.
    if (!key.uri.empty() && metadata.get(ENCRYPTION_INIT_DATA) == nullptr) {
      metadata.setString(ENCRYPTION_INIT_DATA, key.uri);
    }
  }

  void settleInitsIfComplete() {
    for (const Stream& stream : streams_) {
      if (!stream.needs_init) continue;
      if (initWantedBy(stream)) {
        // Half an initialization segment describes nothing.
        if (!stream.init_complete) return;
        continue;
      }
      // A stream with no initialization section is primed by its first media
      // segment, which arrives through the same door as every other.
      if (!stream.streaming && !stream.inbox.contains(stream.read_ordinal)) return;
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
    return publishTracks();
  }

  auto advance(Stream& stream) -> OMError {
    // Only a segment that held nothing for this track counts against the bound:
    // opening one costs a turn round the loop and is not a failure.
    uint64_t skipped = 0;
    for (int empty = 0; empty < MAX_EMPTY_SEGMENTS;) {
      if (skipped > MAX_SKIPPED_SEGMENTS) return OM_FORMAT_CORRUPTED;
      // Part-way through a segment: what the reader wants is the rest of that one.
      if (stream.streaming) {
        if (!stream.pulling) {
          // The rest is the caller's to hand over, through appendSegmentChunk().
          return OM_IO_NOT_ENOUGH_DATA;
        }
        const OMError error = pumpStreamed(stream);
        if (error != OM_SUCCESS) return error;
        if (!stream.reader->needsSegment()) return OM_SUCCESS;
        ++empty; // it ended with nothing in it for this track
        continue;
      }

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

      // Announced but not published. Nothing will ever be fetched for it, so the
      // only thing to do is step over it -- and it is not one of the empty segments
      // the bound above is counting, since nothing was read to find that out.
      if (segment->gap) {
        ++stream.read_ordinal;
        ++skipped;
        continue;
      }

      // The playlist has replaced the initialization section. What follows decodes
      // against parameter sets the current reader has never seen, so it is opened
      // against the new one before the segment behind it is pushed.
      if (initWantedBy(stream)) {
        const OMError error = ensureInitForNextSegment(stream);
        if (error != OM_SUCCESS) return error;
        // openInit() recorded it, so the test above is now false. Counted all the
        // same, so that an index somehow asking for one for ever cannot spin here.
        ++skipped;
        continue;
      }

      // A segment the caller is still downloading is read from where it has got to.
      // One that has to be deciphered cannot be: it is held until it is whole.
      if (const auto partial = stream.inbox.find(stream.read_ordinal);
          !segment->encryption && partial != stream.inbox.end() &&
          !partial->second.complete) {
        const std::optional<Stream::Held> held = stream.takeHeld(stream.read_ordinal);
        const OMError error = openStreamed(stream, *segment, held->bytes);
        if (error != OM_SUCCESS) return error;
        if (!stream.reader->needsSegment()) return OM_SUCCESS;
        return OM_IO_NOT_ENOUGH_DATA; // what has arrived completed no packet yet
      }

      // Nothing in hand and somewhere to read from: read as it comes rather than
      // waiting for the whole of it, which at 4K is tens of megabytes. A protected
      // segment is exempt -- a block cipher has no answer for part of one.
      if (!stream.inbox.contains(stream.read_ordinal) && options_.stream_fetch &&
          !segment->encryption) {
        auto input = options_.stream_fetch(segment->request);
        if (input.isErr()) return std::move(input).unwrapErr();
        stream.pulling = std::move(input).unwrap();
        if (!stream.pulling || !stream.pulling->isValid()) {
          stream.pulling.reset();
          return OM_IO_OPEN_FAILED;
        }
        const OMError error = openStreamed(stream, *segment, {});
        if (error != OM_SUCCESS) {
          stream.streaming = false;
          stream.pulling.reset();
          return error;
        }
        continue; // read by the branch at the top of the next turn round
      }

      std::optional<Stream::Held> inbox_held = stream.takeHeld(stream.read_ordinal);
      std::optional<std::vector<uint8_t>> held;
      // Half a segment is no use to a cipher, so it goes back and waits for the rest
      // rather than being handed over as though it were the whole of one.
      if (inbox_held && (inbox_held->complete || !segment->encryption)) {
        held = std::move(inbox_held->bytes);
      } else if (inbox_held) {
        stream.hold(stream.read_ordinal, std::move(inbox_held->bytes), /*complete=*/false);
        return OM_IO_NOT_ENOUGH_DATA;
      }

      auto bytes = obtain(segment->request, held);
      if (bytes.isErr()) {
        // Put it back: a failed or declined fetch has to leave the stream able to
        // try the same segment again.
        if (held) stream.hold(stream.read_ordinal, std::move(*held), /*complete=*/true);
        return std::move(bytes).unwrapErr();
      }

      auto plain = decryptSegment(*segment, std::move(bytes).unwrap());
      if (plain.isErr()) {
        const OMError error = std::move(plain).unwrapErr();
        // A scheme or key format this caller does not handle is this segment's
        // problem, not the presentation's, so it is stepped over the way a gap is.
        if (error == OM_FORMAT_NOT_SUPPORTED) {
          ++stream.read_ordinal;
          ++skipped;
          continue;
        }
        return error;
      }

      stream.reader->expectSegmentStart(scaleToNs(segment->time, index.timescale()),
                                        segment->discontinuity);
      const OMError error = stream.reader->pushSegment(std::move(plain).unwrap());
      ++stream.read_ordinal;
      if (error == OM_SUCCESS) return OM_SUCCESS;
      if (error != OM_FORMAT_NO_STREAMS) return error;
      // A segment with nothing in it for this track: skip it and read on.
      ++empty;
    }
    return OM_FORMAT_CORRUPTED;
  }

  // The initialization section the next segment decodes against, fetched and opened
  // mid-playlist. The tracks are republished, because a new section is free to
  // describe them differently -- a different picture size, other parameter sets.
  auto ensureInitForNextSegment(Stream& stream) -> OMError {
    const auto request = initWantedBy(stream);
    if (!request) return OM_SUCCESS;
    if (stream.init_inbox && !stream.init_complete) return OM_IO_NOT_ENOUGH_DATA;

    auto bytes = obtain(*request, stream.init_inbox);
    if (bytes.isErr()) return std::move(bytes).unwrapErr();
    if (const OMError error = openInit(stream, *request, std::move(bytes).unwrap());
        error != OM_SUCCESS) {
      return error;
    }
    return publishTracks();
  }

  // One buffer for every pull, rather than 64 KiB of stack per call on whatever
  // thread the caller happens to demux on.
  std::vector<uint8_t> pull_buffer_ = std::vector<uint8_t>(PULL_CHUNK);
};

} // namespace

auto createSegmentDemuxer(SegmentSource source, const StreamOptions& options)
    -> Result<std::unique_ptr<SegmentedDemuxer>, OMError> {
  auto demuxer = std::make_unique<SegmentDemuxer>();
  const OMError error = demuxer->start(std::move(source), options);
  if (error != OM_SUCCESS) return Err(error);
  return Ok(std::unique_ptr<SegmentedDemuxer>(std::move(demuxer)));
}

namespace {

// Reads one byte range and nothing past it, so a segment given as a range of a
// larger resource ends where the playlist said rather than at the end of the file.
class RangeStream final : public InputStream {
  std::unique_ptr<InputStream> source_;
  int64_t remaining_;

public:
  RangeStream(std::unique_ptr<InputStream> source, int64_t length)
      : source_(std::move(source)), remaining_(length) {}

  auto read(std::span<uint8_t> buffer) -> size_t override {
    const size_t want = std::min<size_t>(buffer.size(), static_cast<size_t>(
                                                            std::max<int64_t>(remaining_, 0)));
    if (want == 0) return 0;
    const size_t read = source_->read(buffer.first(want));
    remaining_ -= static_cast<int64_t>(read);
    return read;
  }

  auto tell() const -> int64_t override { return source_->tell(); }
  auto isEOF() const -> bool override { return remaining_ <= 0 || source_->isEOF(); }
  auto isValid() const -> bool override { return source_ && source_->isValid(); }

  auto skip(size_t bytes) -> bool override {
    if (static_cast<int64_t>(bytes) > remaining_) return false;
    if (!source_->skip(bytes)) return false;
    remaining_ -= static_cast<int64_t>(bytes);
    return true;
  }

  // Read front to back, and nothing here asks otherwise, so there is no position
  // for a seek to be relative to.
  auto canSeek() const -> bool override { return false; }
  auto seek(int64_t, Whence) -> bool override { return false; }
  auto size() const -> int64_t override { return remaining_; }
};

} // namespace

auto fileStreamFetch() -> StreamFetchFn {
  return [](const SegmentRequest& request) -> Result<std::unique_ptr<InputStream>, OMError> {
    const std::string path = urlToLocalPath(request.url);
    auto input = InputStream::createFileStream(path);
    if (!input || !input->isValid()) return Err(OM_IO_OPEN_FAILED);

    if (request.offset != 0 && !input->seek(request.offset, Whence::BEG)) {
      return Err(OM_IO_SEEK_FAILED);
    }
    if (request.length < 0) return Ok(std::move(input));
    return Ok(std::unique_ptr<InputStream>(
        std::make_unique<RangeStream>(std::move(input), request.length)));
  };
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

    // Read out in a loop: one read() is allowed to return less than it was asked
    // for, and treating that as the end made a short read look like a truncated
    // segment.
    std::vector<uint8_t> bytes(static_cast<size_t>(request.length));
    size_t filled = 0;
    while (filled < bytes.size()) {
      const size_t read = input->read(std::span(bytes).subspan(filled));
      if (read == 0) break;
      filled += read;
    }
    if (filled != bytes.size()) return Err(OM_IO_NOT_ENOUGH_DATA);
    return Ok(std::move(bytes));
  };
}

} // namespace openmedia
