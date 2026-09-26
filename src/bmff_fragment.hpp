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

// Reads fragmented ISO BMFF one segment at a time: the initialization segment settles
// the tracks, then each media segment is parsed and read out before the next arrives.
// Nothing outside the segment in hand is ever addressed, so a caller streaming segments
// never needs the whole presentation to be reachable, seekable, or of known length --
// all of which the plain BMFF demuxer does need, since it indexes every sample up front.
class FragmentedMP4Reader {
public:
  virtual ~FragmentedMP4Reader() = default;

  virtual auto openInit(std::span<const uint8_t> init) -> OMError = 0;

  virtual auto tracks() const -> const std::vector<Track>& = 0;
  virtual auto metadata() const -> const Dictionary& = 0;

  // Takes ownership: packets point into these bytes rather than copy out of them.
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

auto createFragmentedMP4Reader() -> std::unique_ptr<FragmentedMP4Reader>;

} // namespace openmedia
