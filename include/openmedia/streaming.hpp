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
 * Plain data by design: this is what crosses between the demuxing thread and the
 * downloading thread, so neither touches the other's objects.
 */
struct OPENMEDIA_ABI SegmentPlan {
  SegmentRequest request;
  int32_t track_index = -1;   // the track it feeds
  int64_t start_time_ns = 0;  // presentation time of its first sample
  int64_t duration_ns = 0;    // 0 when the manifest or playlist does not say
  // Needed before any media segment of that track, and again after
  // selectRepresentation().
  bool initialization = false;
};

/**
 * Optionally, a way for a streaming demuxer to fetch a segment itself.
 *
 * Unset, the demuxer never blocks and never calls out: readPacket() reports
 * OM_IO_NOT_ENOUGH_DATA, and the caller downloads what upcomingSegments() names
 * and appendSegment()s it back on whatever threads it likes.
 *
 * Set, it buys convenience at the cost of blocking the calling thread on a buffer
 * miss -- fine for segments on local disk (see fileFetch()), for tests, and for a
 * player already demuxing on a waiting thread. Return OM_IO_NOT_ENOUGH_DATA to
 * decline one fetch without failing.
 *
 * A failed or declined fetch leaves the demuxer where it was, so the same request
 * is simply tried again on the next call.
 */
using FetchFn = std::function<Result<std::vector<uint8_t>, OMError>(const SegmentRequest&)>;

/** Reads from the local filesystem, for segments on disk and for tests. Accepts
 * `file://` URLs and plain paths, and percent-decodes both.
 */
OPENMEDIA_ABI auto fileFetch() -> FetchFn;

/**
 * Optionally, a way to read a segment while it is still arriving.
 *
 * Returns a stream over one segment's bytes, which the demuxer parses piece by
 * piece. That way a segment produces frames after its first few kilobytes rather
 * than its last -- at 4K, tens of megabytes, the difference between playing and
 * not. Preferred over `fetch` when both are set.
 *
 * read() may block, and a short read does not mean the end: isEOF() says that.
 * Neither seek() nor size() is ever called, so a socket stream needs neither.
 */
using StreamFetchFn =
    std::function<Result<std::unique_ptr<InputStream>, OMError>(const SegmentRequest&)>;

/** The streaming counterpart of fileFetch(), over the same URLs and paths. */
OPENMEDIA_ABI auto fileStreamFetch() -> StreamFetchFn;

/** What a streaming demuxer picks when the manifest or playlist offers a choice. */
struct OPENMEDIA_ABI StreamOptions {
  // Preferred audio/subtitle language, ISO-639. Empty reads every track on offer,
  // so a presentation carrying eight languages downloads all eight.
  std::string language;
  // Upper bounds for the initial choice within each set of alternatives. Zero is
  // unbounded, which takes the highest quality on offer.
  uint32_t max_bandwidth = 0;
  uint32_t max_width = 0;
  uint32_t max_height = 0;
  bool with_subtitles = true;
  // The most any one track may hold in segments handed over and not yet read, so
  // that a downloader running away with itself is reported rather than answered
  // with however much memory it asks for. Zero lifts the limit.
  size_t max_buffered_bytes = 64u << 20;
  // Called on a buffer miss instead of reporting OM_IO_NOT_ENOUGH_DATA.
  FetchFn fetch;
  // The same, read as each segment arrives. Preferred over `fetch` when both are set.
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
 * A demuxer over separately addressed segments -- MPEG-DASH, HLS -- with the
 * downloading left to the caller.
 *
 * Not thread-safe by design: one thread owns it and calls everything here. A
 * downloader on other threads only ever sees SegmentPlans and byte vectors.
 */
class OPENMEDIA_ABI SegmentedDemuxer : public Demuxer {
public:
  /** Whether the track list is settled.
   *
   * Tracks come out of the initialization segments, not the manifest: a manifest
   * names codecs, but a decoder needs the parameter sets only the segment carries.
   * So with no `fetch` set, tracks() stays empty -- and readPacket() reports
   * OM_IO_NOT_ENOUGH_DATA -- until every stream's initialization segment has been
   * appended. With `fetch` set this is already true at open.
   */
  virtual auto isReady() const -> bool = 0;

  /** What readPacket() needs next, nearest first, at most `max_count` of them, with
   * initialization segments ahead of the media that needs them. No I/O, so it is
   * cheap enough to ask after every packet.
   */
  virtual auto upcomingSegments(size_t max_count) const -> std::vector<SegmentPlan> = 0;

  /** Hands over the bytes of a segment upcomingSegments() asked for, taking
   * ownership so sample payloads can be read straight out of them. A request no
   * longer wanted -- seeked past, or switched away from -- is dropped, not an error.
   *
   * OM_COMMON_OVERFLOW means the track is already holding
   * StreamOptions::max_buffered_bytes and this segment was not taken: keep it and
   * offer it again once readPacket() has drained some of what is there.
   */
  virtual auto appendSegment(const SegmentRequest& request, std::vector<uint8_t> bytes)
      -> OMError = 0;

  /** Hands over one piece of a segment, for a caller that has not got all of it:
   * pieces of one request in order, `last` set on the final one, which may be empty.
   * `bytes` is copied, so the caller may reuse its buffer at once.
   *
   * What it buys over appendSegment() is that readPacket() starts returning a
   * segment's packets long before its download ends -- at 4K, tens of megabytes,
   * the difference between playing and waiting.
   *
   * Only the segment a track is reading right now is parsed piece by piece; pieces
   * of one further ahead merely accumulate, so prefetching still goes through
   * appendSegment() and this is for the segment being played.
   *
   * A segment that stops short -- `last` on a download that failed half way -- is
   * read as far as it got and then left behind. One request must be delivered one
   * way or the other: mixing the two calls reports OM_COMMON_INVALID_ARGUMENT.
   *
   * seek() and selectRepresentation() abandon whatever was part-delivered, so a
   * download still running across one of those is to be given up: its remaining
   * pieces would read as a fresh delivery with its head missing.
   *
   * OM_COMMON_OVERFLOW means what it does in appendSegment(), about this piece.
   */
  virtual auto appendSegmentChunk(const SegmentRequest& request,
                                  std::span<const uint8_t> bytes, bool last) -> OMError = 0;

  /** Media held for a track and not yet read out, in nanoseconds -- what a
   * downloader throttles on. -1 for a track index that does not exist.
   */
  virtual auto bufferedDurationNs(int32_t track_index) const -> int64_t = 0;

  /** Every alternative offered for every track being read. */
  virtual auto representations() const -> std::vector<RepresentationInfo> = 0;

  /** Switches a track over to another of its alternatives, at the next segment
   * boundary. Packets already returned are unaffected; segments already buffered
   * for the old alternative are dropped, since they decode against its header.
   */
  virtual auto selectRepresentation(int32_t track_index, std::string_view id) -> OMError = 0;
};

// ---------------------------------------------------------------------------
// MPEG-DASH
// ---------------------------------------------------------------------------

/**
 * Opens an MPD the caller has already fetched -- the library downloads nothing.
 * `manifest_url` is what relative BaseURLs and segment templates resolve against.
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
  // The same rendition published elsewhere, for when `url` cannot be reached: a
  // master playlist repeating a variant unchanged is naming another address rather
  // than another quality (RFC 8216 6.3.1).
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
 * HLS is one document deeper than DASH: a master playlist names variants, and each
 * variant's segments live in a media playlist of its own. Since this library
 * downloads nothing, building a demuxer is a short conversation:
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

  /** Media playlists still wanted before open() will succeed: the chosen variant
   * and the renditions that go with it. Empty once everything needed is in.
   *
   * Adding one that was not asked for is allowed, and is how a caller gets
   * alternatives to switch between: every variant whose playlist is in hand becomes
   * something selectRepresentation() can move to.
   */
  virtual auto needed() const -> std::vector<std::string> = 0;

  /** The ladder the master playlist offers, best first, one entry per rendition
   * however many times the playlist listed it. Empty for a lone media playlist.
   *
   * needed() asks for one rung, since reading a presentation means reading one.
   * Fetching further rungs and add()ing them is what lets selectRepresentation()
   * move between them later; if the chosen rung cannot be fetched, add()ing one of
   * its backup_urls instead is enough for open() to succeed.
   */
  virtual auto variants() const -> std::vector<HlsVariant> = 0;

  virtual auto open() -> Result<std::unique_ptr<SegmentedDemuxer>, OMError> = 0;
};

OPENMEDIA_ABI auto createHlsPlaylists(const StreamOptions& options = {})
    -> std::unique_ptr<HlsPlaylists>;

/**
 * The one-call form, for when no second round is needed: a media playlist, or a
 * master playlist with a StreamOptions::fetch that can get the rest itself.
 *
 * Returns OM_IO_NOT_ENOUGH_DATA for a master playlist with no `fetch` -- the case
 * that needs createHlsPlaylists() and a round trip.
 */
OPENMEDIA_ABI auto openHlsPlaylist(std::span<const uint8_t> document, std::string_view url,
                                   const StreamOptions& options = {})
    -> Result<std::unique_ptr<SegmentedDemuxer>, OMError>;

/**
 * One `#EXT-X-KEY` as a media playlist declared it.
 *
 * Reported verbatim: a `skd://` URI and a base64 Widevine PSSH are both just a URI
 * here, because what a URI means is the DRM's business, not a container's. The
 * caller reads `key_format` and interprets `uri` accordingly.
 */
struct OPENMEDIA_ABI HlsKey {
  std::string method;     // METHOD=, e.g. "SAMPLE-AES", "AES-128"
  std::string uri;        // URI=, resolved against the playlist's own URL
  std::string key_format; // KEYFORMAT=; "identity" when the tag did not say
  std::string iv;         // IV=, as written; empty when it is per-segment or derived
};

/**
 * The distinct `#EXT-X-KEY` tags a media playlist carries, first appearance first.
 *
 * Empty for a playlist that is not encrypted and for anything that is not a media
 * playlist -- there is nothing to key either way. Exposed so a service whose keys
 * come from a licence rather than an HTTP fetch has somewhere to read them from.
 */
OPENMEDIA_ABI auto hlsPlaylistKeys(std::span<const uint8_t> document) -> std::vector<HlsKey>;

} // namespace openmedia
