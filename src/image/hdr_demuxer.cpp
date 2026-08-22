#include <cstring>
#include <image/hdr_common.hpp>
#include <util/demuxer_base.hpp>
#include <openmedia/format_api.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/track.hpp>
#include <vector>

namespace openmedia {

class HDRDemuxer final : public BaseDemuxer {
  std::vector<uint8_t> raw_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  bool packet_read_ = false;

public:
  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    input_ = std::move(input);
    if (!input_ || !input_->isValid()) {
      return OM_IO_INVALID_STREAM;
    }

    const int64_t size = input_->size();
    if (size < 0) {
      return OM_IO_INVALID_STREAM;
    }
    raw_.resize(static_cast<size_t>(size));
    if (size > 0 && input_->read({raw_.data(), raw_.size()}) < static_cast<size_t>(size)) {
      return OM_IO_NOT_ENOUGH_DATA;
    }

    hdr::Header h;
    if (!hdr::parseHeader(raw_.data(), raw_.size(), h)) {
      return OM_FORMAT_PARSE_FAILED;
    }
    width_ = h.width;
    height_ = h.height;

    Track track;
    track.index = 0;
    track.format.type = OM_MEDIA_IMAGE;
    track.format.codec_id = OM_CODEC_RGBE;
    track.time_base = {1, 1};
    track.duration = 1;
    track.format.image.width = width_;
    track.format.image.height = height_;
    tracks_.push_back(track);

    return OM_SUCCESS;
  }

  auto readPacket() -> Result<Packet, OMError> override {
    if (packet_read_) {
      return Err(OM_FORMAT_END_OF_FILE);
    }
    packet_read_ = true;

    Packet pkt;
    pkt.allocate(raw_.size());
    if (!raw_.empty()) {
      memcpy(pkt.bytes.data(), raw_.data(), raw_.size());
    }
    pkt.bytes = pkt.bytes.subspan(0, raw_.size());
    pkt.stream_index = 0;
    pkt.pos = 0;
    pkt.pts = 0;
    pkt.dts = 0;
    pkt.is_keyframe = true;

    return Ok(std::move(pkt));
  }

  auto seek(int32_t /*stream_idx*/, int64_t timestamp, SeekMode /*mode*/) -> OMError override {
    if (timestamp <= 0) {
      packet_read_ = false;
    }
    return OM_SUCCESS;
  }
};

const FormatDescriptor FORMAT_HDR = {
    .container_id = OM_CONTAINER_HDR,
    .name = "hdr",
    .long_name = "Radiance HDR (RGBE)",
    .demuxer_factory = [] { return std::make_unique<HDRDemuxer>(); },
    .muxer_factory = {},
};

} // namespace openmedia
