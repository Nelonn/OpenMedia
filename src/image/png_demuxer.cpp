#include <cstring>
#include <util/demuxer_base.hpp>
#include <util/io_util.hpp>
#include <openmedia/format_api.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/track.hpp>

namespace openmedia {

class PNGDemuxer final : public BaseDemuxer {
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint8_t bit_depth_ = 0;
  uint8_t color_type_ = 0;
  int64_t data_start_ = 0;
  int64_t data_size_ = 0;
  bool packet_read_ = false;

  auto parse_ihdr() -> OMError {
    uint8_t ihdr_data[13];
    if (input_->read(ihdr_data) < 13) {
      return OM_IO_NOT_ENOUGH_DATA;
    }

    width_ = (ihdr_data[0] << 24) | (ihdr_data[1] << 16) | (ihdr_data[2] << 8) | ihdr_data[3];
    height_ = (ihdr_data[4] << 24) | (ihdr_data[5] << 16) | (ihdr_data[6] << 8) | ihdr_data[7];
    bit_depth_ = ihdr_data[8];
    color_type_ = ihdr_data[9];

    if (width_ == 0 || height_ == 0) {
      return OM_FORMAT_PARSE_FAILED;
    }

    // Skip remaining IHDR data and CRC
    input_->skip(4 + 4); // 4 bytes filter method + 4 bytes CRC

    return OM_SUCCESS;
  }

public:
  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    input_ = std::move(input);
    if (!input_ || !input_->isValid()) {
      return OM_IO_INVALID_STREAM;
    }

    // Check PNG signature (8 bytes)
    uint8_t signature[8];
    if (input_->read(signature) < 8) {
      return OM_IO_NOT_ENOUGH_DATA;
    }

    static const uint8_t png_signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (memcmp(signature, png_signature, 8) != 0) {
      return OM_FORMAT_PARSE_FAILED;
    }

    data_start_ = 8;

    // Parse IHDR chunk
    uint8_t chunk_header[8];
    if (input_->read(chunk_header) < 8) {
      return OM_IO_NOT_ENOUGH_DATA;
    }

    uint32_t chunk_length = (chunk_header[0] << 24) | (chunk_header[1] << 16) |
                            (chunk_header[2] << 8) | chunk_header[3];

    if (memcmp(chunk_header + 4, "IHDR", 4) != 0) {
      return OM_FORMAT_PARSE_FAILED;
    }

    auto err = parse_ihdr();
    if (err != OM_SUCCESS) {
      return err;
    }

    // Calculate data size (from after IHDR to IEND)
    data_size_ = input_->size() - input_->tell();

    // Create track
    Track track;
    track.index = 0;
    track.format.type = OM_MEDIA_IMAGE;
    track.format.codec_id = OM_CODEC_PNG;
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

  auto seek(int64_t timestamp_us, SeekMode mode) -> OMError override {
    return OM_SUCCESS; // Single image, no seeking needed
  }
};

auto create_png_demuxer() -> std::unique_ptr<Demuxer> {
  return std::make_unique<PNGDemuxer>();
}

const FormatDescriptor FORMAT_PNG = {
    .container_id = static_cast<OMCodecId>(OM_CONTAINER_PNG),
    .name = "png",
    .long_name = "PNG (Portable Network Graphics)",
    .demuxer_factory = [] { return create_png_demuxer(); },
    .muxer_factory = {},
};

} // namespace openmedia
