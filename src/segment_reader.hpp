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

// Addresses nothing outside the segment in hand, so a presentation need not be reachable,
// seekable or of known length. Headers come either separately (openInit, fragmented BMFF)
// or inside every segment (MPEG-TS), where tracks are known after the first one.
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

  // Differs from having tracks only while a segment arrives: MPEG-TS names tracks in its PMT
  // but describes them when each one's first access unit turns up.
  virtual auto tracksDescribed() const -> bool { return !tracks().empty(); }

  // `start_ns` is where the segment sits in the presentation, which MPEG-TS cannot know: its
  // clock starts wherever the encoder's did. `discontinuity` voids what was learned of the clock.
  virtual void expectSegmentStart(int64_t /*start_ns*/, bool /*discontinuity*/) {}

  // Takes ownership: packets may point into these bytes rather than copy out of them.
  virtual auto pushSegment(std::vector<uint8_t> segment) -> OMError = 0;

  // beginSegment(), feedSegment() per piece, endSegment(). In between, needsSegment() means
  // "nothing readable yet" rather than "next segment", which segmentOpen() tells apart.

  virtual auto beginSegment() -> OMError = 0;

  // Reads as much of `bytes` as completes something, keeping the remainder. The
  // packets that produced are readable on return.
  virtual auto feedSegment(std::span<const uint8_t> bytes) -> OMError = 0;

  // Reports what pushSegment() would have; OM_FORMAT_NO_STREAMS covers a segment with nothing
  // for these tracks and a download that stopped before anything readable arrived.
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
