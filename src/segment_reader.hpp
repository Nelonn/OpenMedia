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

// Reads a presentation one segment at a time: bytes in, packets out, and nothing
// outside the segment in hand ever addressed. A caller streaming segments therefore
// never needs the whole presentation to be reachable, seekable, or of known length --
// all of which a file demuxer does need, since it indexes every sample up front.
//
// Two shapes of segment exist, and this is the interface both answer to:
//
//  * Headers arrive separately, as an initialization section -- fragmented ISO BMFF,
//    which is what DASH's `Initialization` and HLS's `#EXT-X-MAP` name. openInit()
//    settles the tracks before any media is pushed.
//  * Every segment carries its own headers -- MPEG-TS, which repeats its program
//    tables and parameter sets throughout. There is no initialization section to
//    give, so the tracks are known only once the first media segment has arrived.
class SegmentReader {
public:
  virtual ~SegmentReader() = default;

  // The initialization section, for a container that has one. A container that
  // repeats its headers in every segment is opened with nothing.
  virtual auto openInit(std::span<const uint8_t> init) -> OMError = 0;

  // Settled by openInit(), or by the first pushSegment() for a container that
  // carries its headers in the segments themselves.
  virtual auto tracks() const -> const std::vector<Track>& = 0;
  virtual auto metadata() const -> const Dictionary& = 0;

  // Where the segment about to be pushed starts in the presentation, in
  // nanoseconds, according to the manifest or playlist. Ignored by a container
  // whose timestamps are already presentation-relative; MPEG-TS needs it, because
  // its clock starts wherever the encoder's clock happened to be and only the
  // playlist knows where a segment sits.
  virtual void expectSegmentStart(int64_t /*start_ns*/) {}

  // Takes ownership: packets may point into these bytes rather than copy out of them.
  virtual auto pushSegment(std::vector<uint8_t> segment) -> OMError = 0;

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
