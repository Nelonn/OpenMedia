#include <cstring>
#include <openmedia/format_api.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/track.hpp>
#include <util/demuxer_base.hpp>
#include <util/io_util.hpp>

namespace openmedia {

class TGADemuxer final : public BaseDemuxer {
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint8_t pixel_depth_ = 0;
  uint8_t image_type_ = 0;
  uint8_t has_alpha_ = 0;
  bool packet_read_ = false;

public:
  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    input_ = std::move(input);
    if (!input_ || !input_->isValid()) {
      return OM_IO_INVALID_STREAM;
    }

    uint8_t header[18];
    if (input_->read(header) < 18) {
      return OM_IO_NOT_ENOUGH_DATA;
    }

    uint8_t color_map_type = header[1];
    image_type_ = header[2];
    width_ = header[12] | (header[13] << 8);
    height_ = header[14] | (header[15] << 8);
    pixel_depth_ = header[16];
    uint8_t image_descriptor = header[17];

    if (width_ == 0 || height_ == 0) {
      return OM_FORMAT_PARSE_FAILED;
    }

    // 1-3 uncompressed, 9-11 RLE, of colour-mapped, true-colour, black-and-white.
    if (image_type_ == 0) {
      return OM_FORMAT_PARSE_FAILED;
    }

    // Check alpha channel from image descriptor (bits 0-3 = alpha bits, bit 5 = origin)
    has_alpha_ = (image_descriptor & 0x0F) > 0;

    if (pixel_depth_ != 8 && pixel_depth_ != 15 && pixel_depth_ != 16 &&
        pixel_depth_ != 24 && pixel_depth_ != 32) {
      return OM_FORMAT_PARSE_FAILED;
    }

    uint8_t id_length = header[0];
    int64_t skip_bytes = id_length;

    if (color_map_type == 1) {
      uint16_t cm_origin = header[3] | (header[4] << 8);
      uint16_t cm_length = header[5] | (header[6] << 8);
      uint8_t cm_depth = header[7];
      skip_bytes += cm_origin + cm_length * (cm_depth / 8);
    }

    if (skip_bytes > 0) {
      input_->skip(skip_bytes);
    }

    Track track;
    track.index = 0;
    track.format.type = OM_MEDIA_IMAGE;
    track.format.codec_id = OM_CODEC_TGA;
    track.time_base = {1, 1};
    track.duration = 1;

    track.format.video.width = width_;
    track.format.video.height = height_;

    tracks_.push_back(track);

    return OM_SUCCESS;
  }

  auto readPacket() -> Result<Packet, OMError> override {
    if (packet_read_) {
      return Err(OM_FORMAT_END_OF_FILE);
    }
    packet_read_ = true;

    Packet pkt;
    size_t size = static_cast<size_t>(input_->size());
    pkt.allocate(size);
    pkt.stream_index = 0;
    pkt.pos = 0;
    pkt.pts = 0;
    pkt.dts = 0;
    pkt.is_keyframe = true;

    input_->seek(0, Whence::BEG);
    size_t bytes_read = input_->read(pkt.bytes);
    pkt.bytes = pkt.bytes.subspan(0, bytes_read);

    return Ok(std::move(pkt));
  }

  auto seek(int32_t stream_idx, int64_t timestamp, SeekMode mode) -> OMError override {
    return OM_SUCCESS; // Single image, no seeking needed
  }
};

const FormatDescriptor FORMAT_TGA = {
    .container_id = OM_CONTAINER_TGA,
    .name = "tga",
    .long_name = "TGA (Truevision TARGA)",
    .demuxer_factory = [] { return std::make_unique<TGADemuxer>(); },
    .muxer_factory = {},
};

} // namespace openmedia
