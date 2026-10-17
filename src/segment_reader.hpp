#pragma once

#include <cstdint>
#include <memory>
#include <openmedia/dictionary.hpp>
#include <openmedia/error.h>
#include <openmedia/packet.hpp>
#include <openmedia/result.hpp>
#include <openmedia/track.hpp>
#include <span>
#include <vector>

namespace openmedia {

// Reads a presentation one segment at a time, addressing nothing outside the segment
// in hand. That is what lets a caller stream segments without the whole presentation
// being reachable, seekable or of known length -- all of which a file demuxer needs,
// since it indexes every sample up front.
//
// Two shapes of segment answer to this interface:
//
//  * Headers arrive separately, as an initialization section (fragmented ISO BMFF:
//    DASH's `Initialization`, HLS's `#EXT-X-MAP`). openInit() settles the tracks
//    before any media is pushed.
//  * Every segment carries its own headers (MPEG-TS, which repeats its program
//    tables and parameter sets throughout), so the tracks are known only once the
//    first media segment has arrived.
class SegmentReader {
public:
  virtual ~SegmentReader() = default;

  // The initialization section, or nothing for a container that repeats its
  // headers in every segment.
  virtual auto openInit(std::span<const uint8_t> init) -> OMError = 0;

  // Settled by openInit(), or by the first pushSegment() for a container that
  // carries its headers in the segments themselves.
  virtual auto tracks() const -> const std::vector<Track>& = 0;
  virtual auto metadata() const -> const Dictionary& = 0;

  // Whether the track list is complete enough to open decoders against, which comes
  // apart from merely existing only when a segment is read as it arrives: MPEG-TS
  // names its tracks in a program map but describes them -- parameter sets, picture
  // size, channel layout -- only when each one's first access unit turns up, a few
  // hundred kilobytes later in a 4K stream.
  virtual auto tracksDescribed() const -> bool { return !tracks().empty(); }

  // What the manifest says about the segment about to be pushed: where it starts in
  // the presentation, and whether the playlist declared a discontinuity in front of
  // it. The start is ignored where timestamps are already presentation-relative;
  // MPEG-TS needs it, because its clock starts wherever the encoder's did and only
  // the playlist knows where a segment sits. A declared discontinuity says outright
  // what a reader would otherwise have to infer from the timestamps: that nothing it
  // worked out about the clock carries over.
  virtual void expectSegmentStart(int64_t /*start_ns*/, bool /*discontinuity*/) {}

  // Takes ownership: packets may point into these bytes rather than copy out of them.
  virtual auto pushSegment(std::vector<uint8_t> segment) -> OMError = 0;

  // --- A segment that is still arriving ---------------------------------------
  //
  // Both shapes can be read as they download, because both are written front to
  // back: a transport stream is a run of 188-byte packets, and a fragmented MP4
  // segment names its samples in a `moof` sitting in front of the `mdat` holding
  // them. So what has arrived parses without waiting for what has not -- the only
  // way a 4K segment, tens of megabytes, shows pictures before it finishes.
  //
  // beginSegment(), then feedSegment() for each piece in order, then endSegment().
  // Between the two, needsSegment() means "nothing readable yet" rather than "on
  // to the next segment", which is what segmentOpen() tells apart.

  virtual auto beginSegment() -> OMError = 0;

  // Reads as much of `bytes` as completes something, keeping the remainder. The
  // packets that produced are readable on return.
  virtual auto feedSegment(std::span<const uint8_t> bytes) -> OMError = 0;

  // No more is coming. Reports what pushSegment() would have, so
  // OM_FORMAT_NO_STREAMS covers both a segment holding nothing for these tracks and
  // a download that stopped before anything readable arrived.
  virtual auto endSegment() -> OMError = 0;

  // Between beginSegment() and endSegment().
  virtual auto segmentOpen() const -> bool = 0;

  virtual auto needsSegment() const -> bool = 0;
  virtual auto readPacket() -> Result<Packet, OMError> = 0;

  // For interleaving several readers by time. INT64_MAX when there is no packet.
  virtual auto nextDecodeTimeNs() const -> int64_t = 0;

  virtual auto remainingDurationNs() const -> int64_t = 0;
  virtual auto timescaleOf(int32_t stream_index) const -> uint32_t = 0;

  // Drops the segment in hand. The tracks survive, which is how a seek lands.
  virtual void flushSegments() = 0;
};

} // namespace openmedia
