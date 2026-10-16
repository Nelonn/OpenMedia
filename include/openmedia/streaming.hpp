#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <openmedia/error.h>
#include <openmedia/format_api.hpp>
#include <openmedia/macro.h>
#include <openmedia/media.hpp>
#include <openmedia/result.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openmedia {

/** One byte range of one resource, named the way the manifest named it. */
struct OPENMEDIA_ABI SegmentRequest {
  std::string url;      // absolute, already resolved against the manifest's own URL
  int64_t offset = 0;   // start of the byte range
  int64_t length = -1;  // -1 asks for everything from `offset` on

  auto operator==(const SegmentRequest& other) const -> bool {
    return offset == other.offset && length == other.length && url == other.url;
  }
};

/** A segment the demuxer is going to want, and what it knows about it.
 *
 * Everything here is plain data, which is the point: this is what crosses from
 * the thread that demuxes to the thread that downloads, and back again as bytes.
 * Neither thread touches the other's objects.
 */
struct OPENMEDIA_ABI SegmentPlan {
  SegmentRequest request;
  int32_t track_index = -1;   // the track it feeds
  int64_t start_time_ns = 0;  // presentation time of its first sample
  int64_t duration_ns = 0;    // 0 when the manifest or playlist does not say
  // An initialization segment. Needed before any media segment of that track, and
  // again after selectRepresentation().
  bool initialization = false;
};

/**
 * Optionally, a way for a streaming demuxer to fetch a segment itself.
 *
 * Leaving this unset is the whole-hearted case: the demuxer then never blocks and
 * never calls out anywhere. readPacket() returns OM_IO_NOT_ENOUGH_DATA whenever
 * the segment it needs has not been handed to it yet, the application reads
 * upcomingSegments() to see what to download, downloads on whatever threads it
 * likes, and appendSegment()s the bytes back. Nothing but data crosses between the
 * two.
 *
 * Setting it buys convenience at the cost of blocking the calling thread: on a
 * buffer miss the demuxer calls this and waits, the way libavformat does. Good for
 * a segment sequence on local disk (see fileFetch()), for tests, and for a player
 * simple enough to demux on the thread that is already waiting anyway. Return
 * OM_IO_NOT_ENOUGH_DATA to decline a particular fetch without failing.
 *
 * Either way a failed or declined fetch leaves the demuxer exactly where it was,
 * so the same request is simply tried again on the next call.
 */
using FetchFn = std::function<Result<std::vector<uint8_t>, OMError>(const SegmentRequest&)>;

/** Reads from the local filesystem, for a segment sequence on disk and for tests.
 * Accepts `file://` URLs and plain paths, and percent-decodes both.
 */
OPENMEDIA_ABI auto fileFetch() -> FetchFn;

/**
 * Optionally, a way for a streaming demuxer to read a segment while it is still
 * arriving instead of once it is whole.
 *
 * Returns a stream over the bytes of one segment. The demuxer reads it in pieces,
 * parses each piece as it comes and hands out the packets that piece completed, so
 * a segment starts producing frames after its first few kilobytes rather than
 * after its last -- which at 4K, where one segment is tens of megabytes, is the
 * difference between playing and not. That is the whole of what this buys over
 * `fetch`, and it is preferred over `fetch` when both are set.
 *
 * read() may block, and may return fewer bytes than asked for without that meaning
 * the end: isEOF() is what says the segment is over. Neither seek() nor size() is
 * ever called, so a stream over a socket needs to implement neither.
 */
using StreamFetchFn =
    std::function<Result<std::unique_ptr<InputStream>, OMError>(const SegmentRequest&)>;

/** The streaming counterpart of fileFetch(), over the same URLs and paths. */
OPENMEDIA_ABI auto fileStreamFetch() -> StreamFetchFn;

/** What a streaming demuxer picks when the manifest or playlist offers a choice. */
struct OPENMEDIA_ABI StreamOptions {
  // Preferred audio/subtitle language as an ISO-639 code. Empty reads every track
  // on offer, which for a presentation carrying eight languages means downloading
  // all eight.
  std::string language;
  // Upper bounds for the initial choice within each set of alternatives. Zero
  // means unbounded, in which case the highest quality on offer is chosen.
  uint32_t max_bandwidth = 0;
  uint32_t max_width = 0;
  uint32_t max_height = 0;
  bool with_subtitles = true;
  // The most any one track may hold in segments handed over and not yet read. How
  // deep to prefetch is the caller's decision, but it is made against a ceiling, so
  // that a downloader running away with itself is reported rather than answered with
  // however much memory it asks for. Zero lifts the limit.
  size_t max_buffered_bytes = 64u << 20;
  // Called on a buffer miss instead of reporting OM_IO_NOT_ENOUGH_DATA.
  FetchFn fetch;
  // The same, reading each segment as it arrives rather than waiting for the whole
  // of it. Used in preference to `fetch` when both are set.
  StreamFetchFn stream_fetch;
};

/** One alternative as offered for a track, for reporting and for switching. */
struct OPENMEDIA_ABI RepresentationInfo {
  int32_t track_index = -1; // the track it feeds
  std::string id;
  std::string codecs;
  uint32_t bandwidth = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool selected = false;
};

/**
 * A demuxer over a sequence of separately addressed segments -- MPEG-DASH,
 * HLS -- with the downloading left to the caller.
 *
 * Not thread-safe, and deliberately so: one thread owns the demuxer and calls
 * everything here, including upcomingSegments() and appendSegment(). A downloader
 * running on other threads is handed SegmentPlans and hands back byte vectors, and
 * never sees this object at all.
 */
class OPENMEDIA_ABI SegmentedDemuxer : public Demuxer {
public:
  /** Whether the track list is settled.
   *
   * Tracks come out of the initialization segments, not out of the manifest -- a
   * manifest names codecs, but a decoder needs the parameter sets that only the
   * segment carries. So with no `fetch` set, tracks() is empty until every
   * stream's initialization segment has been appended, and those are the first
   * things upcomingSegments() asks for. readPacket() reports
   * OM_IO_NOT_ENOUGH_DATA until then. With `fetch` set this is already true when
   * the demuxer is opened.
   */
  virtual auto isReady() const -> bool = 0;

  /** What readPacket() needs next, nearest first, at most `max_count` of them.
   * Pure computation -- no I/O, cheap enough to ask after every packet.
   * Initialization segments come before the media that needs them.
   */
  virtual auto upcomingSegments(size_t max_count) const -> std::vector<SegmentPlan> = 0;

  /** Hands over the bytes of a segment upcomingSegments() asked for, taking
   * ownership: sample payloads are read straight out of them rather than copied.
   * A request that is no longer wanted -- seeked past, or belonging to an
   * alternative that has since been switched away from -- is dropped, which is not
   * an error.
   *
   * OM_COMMON_OVERFLOW means the track is already holding
   * StreamOptions::max_buffered_bytes and this segment was not taken: keep it and
   * offer it again once readPacket() has drained some of what is there.
   */
  virtual auto appendSegment(const SegmentRequest& request, std::vector<uint8_t> bytes)
      -> OMError = 0;

  /** Hands over one piece of a segment, for a caller that has not got all of it.
   *
   * The same segments appendSegment() delivers whole, delivered as they download:
   * pieces of one request in order, `last` set on the final piece, and an empty
   * final piece allowed. What it buys is that the segment is parsed as it arrives
   * and readPacket() starts returning its packets long before the download ends --
   * at 4K, where one segment runs to tens of megabytes, the difference between
   * playing and waiting. `bytes` is copied, so the caller may reuse its buffer the
   * moment this returns.
   *
   * Only the segment a track is reading right now is parsed piece by piece; pieces
   * of a segment further ahead accumulate until that track reaches them, which is
   * no worse than appendSegment() and no better. Prefetching therefore still goes
   * through appendSegment(), and this is for the segment being played.
   *
   * A segment that stops short -- `last` set on a download that failed half way --
   * is read as far as it got and then left behind, the same as a segment carrying
   * nothing for the track. A request must be delivered one way or the other: mixing
   * appendSegment() and appendSegmentChunk() for one request is a programming error
   * and reports OM_COMMON_INVALID_ARGUMENT.
   *
   * seek() and selectRepresentation() abandon whatever was part-delivered, so a
   * download still running across one of those is to be given up rather than
   * carried on: its remaining pieces would be read as the start of a fresh
   * delivery of the same request, which is a segment with its head missing.
   *
   * OM_COMMON_OVERFLOW means the same as in appendSegment() and means it about this
   * piece: the track is full, the piece was not taken, offer it again later.
   */
  virtual auto appendSegmentChunk(const SegmentRequest& request,
                                  std::span<const uint8_t> bytes, bool last) -> OMError = 0;

  /** Media held for a track and not yet read out, in nanoseconds. What a
   * downloader throttles on: keep fetching while this is below the target. -1 for
   * a track index that does not exist.
   */
  virtual auto bufferedDurationNs(int32_t track_index) const -> int64_t = 0;

  /** Every alternative offered for every track being read. */
  virtual auto representations() const -> std::vector<RepresentationInfo> = 0;

  /** Switches a track over to another of its alternatives. Takes effect at the
   * next segment boundary; packets already returned, and segments already
   * buffered, are unaffected.
   */
  virtual auto selectRepresentation(int32_t track_index, std::string_view id) -> OMError = 0;
};

// ---------------------------------------------------------------------------
// MPEG-DASH
// ---------------------------------------------------------------------------

/**
 * Opens an MPD that has already been fetched. The library never downloads the
 * manifest either: `mpd_xml` is its bytes and `manifest_url` is where they came
 * from, which is what relative BaseURLs and segment templates resolve against.
 */
OPENMEDIA_ABI auto openDashManifest(std::span<const uint8_t> mpd_xml,
                                    std::string_view manifest_url,
                                    const StreamOptions& options = {})
    -> Result<std::unique_ptr<SegmentedDemuxer>, OMError>;

// ---------------------------------------------------------------------------
// HLS
// ---------------------------------------------------------------------------

/** One rendition a master playlist offers, as the playlist describes it. */
struct OPENMEDIA_ABI HlsVariant {
  std::string url; // the media playlist this rendition's segments are listed in
  // The same rendition published elsewhere, for when `url` cannot be reached. A
  // master playlist repeating a variant with its attributes unchanged is naming
  // another address for it rather than another quality (RFC 8216 6.3.1), and such
  // entries are reported here rather than as variants of their own.
  std::vector<std::string> backup_urls;
  std::string codecs;
  uint32_t bandwidth = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool chosen = false; // what StreamOptions picked, and what needed() asks for
};

/**
 * HLS, built from playlists the caller has already fetched.
 *
 * HLS is one document deeper than DASH: a master playlist names variants, and the
 * segments of each variant live in a media playlist of its own. Since this library
 * downloads nothing, building a demuxer is a short conversation -- hand over a
 * playlist, be told which further playlists are wanted, hand those over too:
 *
 * ```cpp
 * auto playlists = createHlsPlaylists(options);
 * playlists->add(myGet(url), url);
 * for (const std::string& next : playlists->needed()) playlists->add(myGet(next), next);
 * auto demuxer = std::move(playlists->open()).unwrap();
 * ```
 *
 * A stream published as a single media playlist needs no second round: `needed()`
 * comes back empty and `open()` succeeds straight away.
 */
class OPENMEDIA_ABI HlsPlaylists {
public:
  virtual ~HlsPlaylists() = default;

  /** Adds a playlist of either kind, told apart by its own tags. `url` is where it
   * came from, which is what its relative URIs resolve against. */
  virtual auto add(std::span<const uint8_t> document, std::string_view url) -> OMError = 0;

  /** Media playlists still wanted before open() will succeed: the chosen variant,
   * and the renditions that go with it. Empty once everything needed is in.
   *
   * Adding a playlist that was not asked for is allowed and is how a caller gets
   * alternatives to switch between later: every variant whose media playlist is in
   * hand becomes something selectRepresentation() can move to.
   */
  virtual auto needed() const -> std::vector<std::string> = 0;

  /** The ladder the master playlist offers, best first, one entry per rendition
   * however many times the playlist listed it. Empty for a stream published as a
   * single media playlist, which is its own only rendition.
   *
   * What it is for: needed() asks for one rung, since reading a presentation means
   * reading one. Fetching further rungs and add()ing them is what makes
   * selectRepresentation() able to move between them later -- and if the chosen
   * rung cannot be fetched, add()ing one of its backup_urls instead is enough for
   * open() to succeed.
   */
  virtual auto variants() const -> std::vector<HlsVariant> = 0;

  virtual auto open() -> Result<std::unique_ptr<SegmentedDemuxer>, OMError> = 0;
};

OPENMEDIA_ABI auto createHlsPlaylists(const StreamOptions& options = {})
    -> std::unique_ptr<HlsPlaylists>;

/**
 * The one-call form, for when no second round is needed: a media playlist, or a
 * master playlist together with a StreamOptions::fetch that can go and get the
 * media playlists itself.
 *
 * Returns OM_IO_NOT_ENOUGH_DATA for a master playlist with no `fetch` set -- that
 * is the case which needs createHlsPlaylists() and a round trip.
 */
OPENMEDIA_ABI auto openHlsPlaylist(std::span<const uint8_t> document, std::string_view url,
                                   const StreamOptions& options = {})
    -> Result<std::unique_ptr<SegmentedDemuxer>, OMError>;

/**
 * One `#EXT-X-KEY` as a media playlist declared it.
 *
 * openmedia reports what the tag said and nothing else: a `skd://` URI and a
 * base64 Widevine PSSH are both just a URI here, because what a URI means is the
 * DRM's business and not a container's. A caller that knows its service reads the
 * format and interprets the URI -- fetching an AES key, starting a licence, or
 * handing it to a wrapper that holds the key this process never sees.
 */
struct OPENMEDIA_ABI HlsKey {
  std::string method;     // METHOD=, e.g. "SAMPLE-AES", "AES-128"
  std::string uri;        // URI=, resolved against the playlist's own URL
  std::string key_format; // KEYFORMAT=; "identity" when the tag did not say
  std::string iv;         // IV=, as written; empty when it is per-segment or derived
};

/**
 * The distinct `#EXT-X-KEY` tags a media playlist carries, in the order they first
 * appear.
 *
 * Empty for a playlist that is not encrypted, and for a document that is not a
 * media playlist at all -- there is nothing to key either way. openmedia parses the
 * playlist as it already does; this only surfaces what it found, so a service whose
 * keys come from a licence rather than an HTTP fetch has somewhere to read them
 * from.
 */
OPENMEDIA_ABI auto hlsPlaylistKeys(std::span<const uint8_t> document) -> std::vector<HlsKey>;

} // namespace openmedia
