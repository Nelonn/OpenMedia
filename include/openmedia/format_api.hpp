#pragma once

#include <functional>
#include <openmedia/error.h>
#include <openmedia/format_defs.h>
#include <openmedia/io.hpp>
#include <openmedia/macro.h>
#include <openmedia/packet.hpp>
#include <openmedia/result.hpp>
#include <openmedia/track.hpp>
#include <openmedia/log.hpp>

namespace openmedia {

enum class SeekMode : uint8_t {
  PREVIOUS_SYNC = 0,
  NEXT_SYNC = 1,
  CLOSEST_SYNC = 2,
  DONT_SYNC = 3,
};

class OPENMEDIA_ABI Demuxer {
public:
  virtual ~Demuxer() = default;

  virtual auto open(std::unique_ptr<InputStream> input) -> OMError = 0;
  virtual void close() = 0;

  virtual auto tracks() const -> const std::vector<Track>& = 0;
  virtual auto readPacket() -> Result<Packet, OMError> = 0;

  // When stream_idx is < 0 then timestamp is in microseconds (us), otherwise timestamp is in track time base
  virtual auto seek(int32_t stream_idx, int64_t timestamp, SeekMode mode = SeekMode::PREVIOUS_SYNC) -> OMError = 0;
};

class OPENMEDIA_ABI Muxer {
public:
  virtual ~Muxer() = default;

  virtual auto open(std::unique_ptr<OutputStream> output, LoggerRef logger = {}) -> OMError = 0;
  virtual void close() = 0;

  virtual auto addTrack(const Track& track) -> int32_t = 0;
  virtual auto writePacket(const Packet& packet) -> OMError = 0;
  virtual auto finalize() -> OMError = 0;
};

struct OPENMEDIA_ABI FormatDescriptor {
  OMContainerId container_id = OM_CONTAINER_NONE;
  std::string_view name;
  std::string_view long_name;
  std::function<std::unique_ptr<Demuxer>()> demuxer_factory = {};
  std::function<std::unique_ptr<Muxer>()> muxer_factory = {};

  auto isDemuxing() const noexcept -> bool {
    return demuxer_factory != nullptr;
  }

  auto isMuxing() const noexcept -> bool {
    return muxer_factory != nullptr;
  }
};

} // namespace openmedia
