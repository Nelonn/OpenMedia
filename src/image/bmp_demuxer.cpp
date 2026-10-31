#include <cstring>
#include <util/demuxer_base.hpp>
#include <util/io_util.hpp>
#include <openmedia/format_api.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/track.hpp>

namespace openmedia {

class BMPDemuxer final : public BaseDemuxer {
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint16_t bit_count_ = 0;
  uint32_t compression_ = 0;
  uint32_t data_offset_ = 0;
  bool packet_read_ = false;

public:
  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    input_ = std::move(input);
    if (!input_ || !input_->isValid()) {
      return OM_IO_INVALID_STREAM;
    }

    uint8_t file_header[14];
    if (input_->read(file_header) < 14) {
      return OM_IO_NOT_ENOUGH_DATA;
    }

    // Check BMP signature ("BM")
    if (file_header[0] != 'B' || file_header[1] != 'M') {
      return OM_FORMAT_PARSE_FAILED;
    }

    data_offset_ = file_header[10] | (file_header[11] << 8) |
                   (file_header[12] << 16) | (file_header[13] << 24);

    if (data_offset_ < 54) { // Minimum BMP header size
      return OM_FORMAT_PARSE_FAILED;
    }

    // Read DIB header (at least 40 bytes for BITMAPINFOHEADER)
    uint8_t dib_header[40];
    if (input_->read(dib_header) < 40) {
      return OM_IO_NOT_ENOUGH_DATA;
    }

    uint32_t dib_size = dib_header[0] | (dib_header[1] << 8) |
                        (dib_header[2] << 16) | (dib_header[3] << 24);

    if (dib_size < 40) {
      return OM_FORMAT_PARSE_FAILED;
    }

    int32_t width_signed = dib_header[4] | (dib_header[5] << 8) |
                           (dib_header[6] << 16) | (dib_header[7] << 24);
    int32_t height_signed = dib_header[8] | (dib_header[9] << 8) |
                            (dib_header[10] << 16) | (dib_header[11] << 24);
    uint16_t planes = dib_header[12] | (dib_header[13] << 8);
    bit_count_ = dib_header[14] | (dib_header[15] << 8);
    compression_ = dib_header[16] | (dib_header[17] << 8) |
                   (dib_header[18] << 16) | (dib_header[19] << 24);

    width_ = static_cast<uint32_t>(std::abs(width_signed));
    height_ = static_cast<uint32_t>(std::abs(height_signed));

    if (width_ == 0 || height_ == 0 || planes != 1) {
      return OM_FORMAT_PARSE_FAILED;
    }

    // Check compression (only support uncompressed and RLE8/RLE4)
    if (compression_ != 0 && compression_ != 1 && compression_ != 2) {
      return OM_FORMAT_PARSE_FAILED;
    }

    if (bit_count_ != 1 && bit_count_ != 4 && bit_count_ != 8 &&
        bit_count_ != 16 && bit_count_ != 24 && bit_count_ != 32) {
      return OM_FORMAT_PARSE_FAILED;
    }

    Track track;
    track.index = 0;
    track.format.type = OM_MEDIA_IMAGE;
    track.format.codec_id = OM_CODEC_BMP;
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

const FormatDescriptor FORMAT_BMP = {
    .container_id = OM_CONTAINER_BMP,
    .name = "bmp",
    .long_name = "BMP (Bitmap)",
    .demuxer_factory = [] { return std::make_unique<BMPDemuxer>(); },
    .muxer_factory = {},
};

} // namespace openmedia
