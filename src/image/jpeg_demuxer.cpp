#include <util/demuxer_base.hpp>
#include <util/io_util.hpp>
#include <openmedia/format_api.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/track.hpp>

namespace openmedia {

class JPEGDemuxer final : public BaseDemuxer {
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  bool packet_read_ = false;

  // 0xC0..0xCF are the frame headers except for DHT, JPG and DAC.
  static auto isSOF(uint8_t marker) -> bool {
    return marker >= 0xC0 && marker <= 0xCF &&
           marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
  }

  // Returns the next marker code, or 0 at EOF. A marker is 0xFF followed by a
  // non-zero code; any number of 0xFF fill bytes may precede it, and a 0xFF
  // 0x00 pair is stuffed data, so both are skipped.
  auto readMarker() -> uint8_t {
    uint8_t byte;
    for (;;) {
      if (input_->read({&byte, 1}) < 1) {
        return 0;
      }
      if (byte != 0xFF) {
        continue;
      }
      do {
        if (input_->read({&byte, 1}) < 1) {
          return 0;
        }
      } while (byte == 0xFF);
      if (byte != 0x00) {
        return byte;
      }
    }
  }

public:
  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    input_ = std::move(input);
    if (!input_ || !input_->isValid()) {
      return OM_IO_INVALID_STREAM;
    }

    uint8_t soi[2];
    if (input_->read(soi) < 2) {
      return OM_IO_NOT_ENOUGH_DATA;
    }
    if (soi[0] != 0xFF || soi[1] != 0xD8) {
      return OM_FORMAT_PARSE_FAILED;
    }

    for (;;) {
      uint8_t marker = readMarker();

      // TEM and the restart markers carry no payload.
      if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
        continue;
      }

      // EOI, SOS or EOF before a frame header: there are no dimensions to find.
      if (marker == 0x00 || marker == 0xD9 || marker == 0xDA) {
        return OM_FORMAT_PARSE_FAILED;
      }

      uint8_t length_bytes[2];
      if (input_->read(length_bytes) < 2) {
        return OM_IO_NOT_ENOUGH_DATA;
      }

      uint16_t length = load_u16_be(length_bytes);
      if (length < 2) {
        return OM_FORMAT_PARSE_FAILED;
      }

      if (!isSOF(marker)) {
        if (!input_->skip(length - 2)) {
          return OM_IO_NOT_ENOUGH_DATA;
        }
        continue;
      }

      // SOF payload: precision, height, width, component count.
      uint8_t sof[6];
      if (length < 8 || input_->read(sof) < 6) {
        return OM_IO_NOT_ENOUGH_DATA;
      }

      height_ = load_u16_be(sof + 1);
      width_ = load_u16_be(sof + 3);
      break;
    }

    if (width_ == 0 || height_ == 0) {
      return OM_FORMAT_PARSE_FAILED;
    }

    Track track;
    track.index = 0;
    track.format.type = OM_MEDIA_IMAGE;
    track.format.codec_id = OM_CODEC_JPEG;
    track.format.video.width = width_;
    track.format.video.height = height_;
    track.time_base = {1, 1};
    track.duration = 1;

    tracks_.push_back(track);

    return OM_SUCCESS;
  }

  auto readPacket() -> Result<Packet, OMError> override {
    if (packet_read_) {
      return Err(OM_FORMAT_END_OF_FILE);
    }
    packet_read_ = true;

    int64_t size = input_->size();
    if (size <= 0) {
      return Err(OM_IO_NOT_ENOUGH_DATA);
    }
    if (!input_->seek(0, Whence::BEG)) {
      return Err(OM_IO_SEEK_FAILED);
    }

    Packet pkt;
    pkt.allocate(static_cast<size_t>(size));
    pkt.stream_index = 0;
    pkt.pos = 0;
    pkt.pts = 0;
    pkt.dts = 0;
    pkt.is_keyframe = true;

    size_t bytes_read = input_->read(pkt.bytes);
    pkt.bytes = pkt.bytes.subspan(0, bytes_read);

    return Ok(std::move(pkt));
  }

  auto seek(int32_t stream_idx, int64_t timestamp, SeekMode mode) -> OMError override {
    return OM_SUCCESS; // Single image, no seeking needed
  }
};

const FormatDescriptor FORMAT_JPEG = {
    .container_id = OM_CONTAINER_JPEG,
    .name = "jpeg",
    .long_name = "JPEG (Joint Photographic Experts Group)",
    .demuxer_factory = [] { return std::make_unique<JPEGDemuxer>(); },
    .muxer_factory = {},
};

} // namespace openmedia
