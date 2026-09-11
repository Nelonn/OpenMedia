#include <util/demuxer_base.hpp>
#include <util/io_util.hpp>
#include <openmedia/format_api.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/track.hpp>
#include <tiffio.h>

namespace openmedia {

class DNGDemuxer final : public BaseDemuxer {
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  bool packet_read_ = false;

  static tmsize_t dng_read(thandle_t client_data, void* buf, tmsize_t size) {
    auto* input = static_cast<InputStream*>(client_data);
    if (!input || !input->isValid()) {
      return -1;
    }
    size_t bytes_read = input->read(std::span(static_cast<uint8_t*>(buf), static_cast<size_t>(size)));
    return static_cast<tmsize_t>(bytes_read);
  }

  static tmsize_t dng_write(thandle_t, void*, tmsize_t) {
    return -1;
  }

  static toff_t dng_seek(thandle_t client_data, toff_t offset, int whence) {
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

  static int dng_close(thandle_t) {
    return 0;
  }

  static toff_t dng_size(thandle_t client_data) {
    auto* input = static_cast<InputStream*>(client_data);
    if (!input || !input->isValid()) {
      return 0;
    }
    return static_cast<toff_t>(input->size());
  }

  static int dng_map_file(thandle_t, void**, toff_t*) {
    return 0;
  }

  static void dng_unmap_file(thandle_t, void*, toff_t) {}

public:
  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    input_ = std::move(input);
    if (!input_ || !input_->isValid()) {
      return OM_IO_INVALID_STREAM;
    }

    uint8_t header[8];
    if (input_->read(header) < 8) {
      return OM_IO_NOT_ENOUGH_DATA;
    }

    bool is_tiff = false;
    if (header[0] == 'I' && header[1] == 'I' && header[2] == 0x2A && header[3] == 0x00) {
      is_tiff = true;
    } else if (header[0] == 'M' && header[1] == 'M' && header[2] == 0x00 && header[3] == 0x2A) {
      is_tiff = true;
    } else if (header[0] == 'I' && header[1] == 'I' && header[2] == 0x2B && header[3] == 0x00) {
      is_tiff = true;
    } else if (header[0] == 'M' && header[1] == 'M' && header[2] == 0x00 && header[3] == 0x2B) {
      is_tiff = true;
    }

    if (!is_tiff) {
      return OM_FORMAT_PARSE_FAILED;
    }

    input_->seek(0, Whence::BEG);
    TIFF* tiff = TIFFClientOpen(
        "dng_memory",
        "r",
        reinterpret_cast<thandle_t>(input_.get()),
        dng_read,
        dng_write,
        dng_seek,
        dng_close,
        dng_size,
        dng_map_file,
        dng_unmap_file
    );

    if (!tiff) {
      return OM_FORMAT_PARSE_FAILED;
    }

    uint32_t best_width = 0;
    uint32_t best_height = 0;

    auto check_current_dir = [&]() {
      uint32_t w = 0, h = 0;
      if (TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &w) && TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &h)) {
        if (static_cast<uint64_t>(w) * h > static_cast<uint64_t>(best_width) * best_height) {
          best_width = w;
          best_height = h;
        }
      }
    };

    do {
      check_current_dir();

      uint16_t subifd_count = 0;
      void* subifd_ptr = nullptr;
      if (TIFFGetField(tiff, TIFFTAG_SUBIFD, &subifd_count, &subifd_ptr) && subifd_count > 0 && subifd_ptr) {
        auto* subifd_offsets = static_cast<uint64_t*>(subifd_ptr);
        for (uint16_t s = 0; s < subifd_count; ++s) {
          if (TIFFSetSubDirectory(tiff, subifd_offsets[s])) {
            check_current_dir();
          }
        }
      }
    } while (TIFFReadDirectory(tiff));

    TIFFClose(tiff);

    if (best_width == 0 || best_height == 0) {
      return OM_FORMAT_PARSE_FAILED;
    }

    width_ = best_width;
    height_ = best_height;

    Track track;
    track.index = 0;
    track.format.type = OM_MEDIA_IMAGE;
    track.format.codec_id = OM_CODEC_DNG;
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

  auto seek(int32_t, int64_t, SeekMode) -> OMError override {
    return OM_SUCCESS;
  }
};

const FormatDescriptor FORMAT_DNG = {
    .container_id = OM_CONTAINER_DNG,
    .name = "dng",
    .long_name = "DNG (Digital Negative)",
    .demuxer_factory = [] { return std::make_unique<DNGDemuxer>(); },
    .muxer_factory = {},
};

}
