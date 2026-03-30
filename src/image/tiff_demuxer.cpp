#include <util/demuxer_base.hpp>
#include <util/io_util.hpp>
#include <openmedia/format_api.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/track.hpp>
#include <tiffio.h>

namespace openmedia {

class TIFFDemuxer final : public BaseDemuxer {
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint16_t bits_per_sample_ = 0;
  uint16_t samples_per_pixel_ = 0;
  uint16_t photometric_ = 0;
  uint16_t planar_config_ = 0;
  uint16_t extra_samples_ = 0;
  bool packet_read_ = false;

  // TIFF memory read callbacks
  static tmsize_t tiff_read(thandle_t client_data, void* buf, tmsize_t size) {
    auto* input = static_cast<InputStream*>(client_data);
    if (!input || !input->isValid()) {
      return -1;
    }
    size_t bytes_read = input->read(std::span(static_cast<uint8_t*>(buf), static_cast<size_t>(size)));
    return static_cast<tmsize_t>(bytes_read);
  }

  static tmsize_t tiff_write(thandle_t /*client_data*/, void* /*buf*/, tmsize_t /*size*/) {
    return -1; // Read-only
  }

  static toff_t tiff_seek(thandle_t client_data, toff_t offset, int whence) {
    auto* input = static_cast<InputStream*>(client_data);
    if (!input || !input->isValid()) {
      return -1;
    }

    int64_t new_pos;
    switch (whence) {
      case SEEK_SET: new_pos = offset; break;
      case SEEK_CUR: new_pos = input->tell() + offset; break;
      case SEEK_END: new_pos = input->size() + offset; break;
      default: return -1;
    }

    if (new_pos < 0 || new_pos > input->size()) {
      return -1;
    }

    input->seek(new_pos, Whence::BEG);
    return static_cast<toff_t>(new_pos);
  }

  static int tiff_close(thandle_t /*client_data*/) {
    return 0; // Don't close - input is managed elsewhere
  }

  static toff_t tiff_size(thandle_t client_data) {
    auto* input = static_cast<InputStream*>(client_data);
    if (!input || !input->isValid()) {
      return 0;
    }
    return static_cast<toff_t>(input->size());
  }

  static int tiff_map_file(thandle_t /*client_data*/, void** /*base*/, toff_t* /*size*/) {
    return 0; // No memory mapping
  }

  static void tiff_unmap_file(thandle_t /*client_data*/, void* /*base*/, toff_t /*size*/) {
    // Nothing to do
  }

public:
  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    input_ = std::move(input);
    if (!input_ || !input_->isValid()) {
      return OM_IO_INVALID_STREAM;
    }

    // Check TIFF magic bytes
    // TIFF files start with either:
    // - "II" (0x49, 0x49) followed by 0x2A, 0x00 (little-endian)
    // - "MM" (0x4D, 0x4D) followed by 0x00, 0x2A (big-endian)
    uint8_t header[8];
    if (input_->read(header) < 8) {
      return OM_IO_NOT_ENOUGH_DATA;
    }

    bool is_tiff = false;
    if (header[0] == 'I' && header[1] == 'I' && header[2] == 0x2A && header[3] == 0x00) {
      is_tiff = true; // Little-endian TIFF
    } else if (header[0] == 'M' && header[1] == 'M' && header[2] == 0x00 && header[3] == 0x2A) {
      is_tiff = true; // Big-endian TIFF
    } else if (header[0] == 0x43 && header[1] == 0x52 && header[2] == 0x41 && header[3] == 0x57) {
      is_tiff = true; // BigTIFF little-endian
    } else if (header[0] == 0x57 && header[1] == 0x41 && header[2] == 0x52 && header[3] == 0x43) {
      is_tiff = true; // BigTIFF big-endian
    }

    if (!is_tiff) {
      return OM_FORMAT_PARSE_FAILED;
    }

    // Open TIFF from memory
    input_->seek(0, Whence::BEG);
    TIFF* tiff = TIFFClientOpen(
        "memory",
        "r",
        reinterpret_cast<thandle_t>(input_.get()),
        tiff_read,
        tiff_write,
        tiff_seek,
        tiff_close,
        tiff_size,
        tiff_map_file,
        tiff_unmap_file
    );

    if (!tiff) {
      return OM_FORMAT_PARSE_FAILED;
    }

    // Read TIFF tags
    TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width_);
    TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height_);
    TIFFGetField(tiff, TIFFTAG_BITSPERSAMPLE, &bits_per_sample_);
    TIFFGetField(tiff, TIFFTAG_SAMPLESPERPIXEL, &samples_per_pixel_);
    TIFFGetField(tiff, TIFFTAG_PHOTOMETRIC, &photometric_);
    TIFFGetField(tiff, TIFFTAG_PLANARCONFIG, &planar_config_);
    TIFFGetField(tiff, TIFFTAG_EXTRASAMPLES, &extra_samples_, nullptr);

    TIFFClose(tiff);

    if (width_ == 0 || height_ == 0) {
      return OM_FORMAT_PARSE_FAILED;
    }

    // Create track
    Track track;
    track.index = 0;
    track.format.type = OM_MEDIA_IMAGE;
    track.format.codec_id = OM_CODEC_TIFF;
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

auto create_tiff_demuxer() -> std::unique_ptr<Demuxer> {
  return std::make_unique<TIFFDemuxer>();
}

const FormatDescriptor FORMAT_TIFF = {
    .container_id = OM_CONTAINER_TIFF,
    .name = "tiff",
    .long_name = "TIFF (Tagged Image File Format)",
    .demuxer_factory = [] { return create_tiff_demuxer(); },
    .muxer_factory = {},
};

} // namespace openmedia
