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
  // Called on a buffer miss instead of reporting OM_IO_NOT_ENOUGH_DATA.
  FetchFn fetch;
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
   */
  virtual auto appendSegment(const SegmentRequest& request, std::vector<uint8_t> bytes)
      -> OMError = 0;

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

} // namespace openmedia
