#include <util/demuxer_base.hpp>
#include <util/io_util.hpp>
#include <openmedia/format_api.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/track.hpp>

namespace openmedia {

class JPEGDemuxer final : public BaseDemuxer {
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  std::vector<uint8_t> header_buf_;
  bool packet_read_ = false;

  auto readByte(uint8_t& byte) -> bool {
    if (input_->read({&byte, 1}) < 1) {
      return false;
    }
    header_buf_.push_back(byte);
    return true;
  }

  auto readBytes(uint8_t* dst, size_t n) -> size_t {
    size_t result = input_->read({dst, n});
    header_buf_.insert(header_buf_.end(), dst, dst + result);
    return result;
  }

  auto parseSofMarker() -> OMError {
    uint8_t sof_data[9];
    if (readBytes(sof_data, 9) < 9) {
      return OM_IO_NOT_ENOUGH_DATA;
    }

    // SOF marker format:
    // [0-1]: length (includes these 2 bytes)
    // [2]:   precision (bits per sample)
    // [3-4]: height
    // [5-6]: width
    // [7]:   number of components

    height_ = (sof_data[3] << 8) | sof_data[4];
    width_  = (sof_data[5] << 8) | sof_data[6];

    if (width_ == 0 || height_ == 0) {
      return OM_FORMAT_PARSE_FAILED;
    }

    return OM_SUCCESS;
  }

  auto skipVariableMarker() -> OMError {
    uint8_t length_bytes[2];
    if (readBytes(length_bytes, 2) < 2) {
      return OM_IO_NOT_ENOUGH_DATA;
    }

    uint16_t length = (length_bytes[0] << 8) | length_bytes[1];
    if (length < 2) {
      return OM_FORMAT_PARSE_FAILED;
    }

    // Read and accumulate the rest of the marker data (length includes the 2 length bytes)
    size_t remaining = length - 2;
    std::vector<uint8_t> marker_data(remaining);
    size_t bytes_read = input_->read(std::span(marker_data.data(), remaining));
    header_buf_.insert(header_buf_.end(), marker_data.begin(), marker_data.begin() + bytes_read);

    if (bytes_read < remaining) {
      return OM_IO_NOT_ENOUGH_DATA;
    }

    return OM_SUCCESS;
  }

public:
  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    input_ = std::move(input);
    if (!input_ || !input_->isValid()) {
      return OM_IO_INVALID_STREAM;
    }

    // Check JPEG SOI marker (0xFFD8)
    uint8_t soi[2];
    if (readBytes(soi, 2) < 2) {
      return OM_IO_NOT_ENOUGH_DATA;
    }

    if (soi[0] != 0xFF || soi[1] != 0xD8) {
      return OM_FORMAT_PARSE_FAILED;
    }

    // Parse JPEG markers to find SOF and image dimensions
    bool found_sof = false;
    while (!input_->isEOF()) {
      uint8_t marker_prefix;
      if (!readByte(marker_prefix)) {
        break;
      }

      if (marker_prefix != 0xFF) {
        continue;
      }

      uint8_t marker;
      if (!readByte(marker)) {
        break;
      }

      // Skip padding 0xFF bytes
      while (marker == 0xFF) {
        if (!readByte(marker)) {
          goto done;
        }
      }

      if (marker == 0x00) { // Stuff byte
        continue;
      }

      if (marker == 0xD9) { // EOI
        break;
      }

      // SOF markers (Start Of Frame)
      // SOF0: 0xC0, SOF1: 0xC1, SOF2: 0xC2, SOF3: 0xC3
      // SOF5: 0xC5, SOF6: 0xC6, SOF7: 0xC7
      // SOF9: 0xC9, SOF10: 0xCA, SOF11: 0xCB
      // SOF13: 0xCD, SOF14: 0xCE, SOF15: 0xCF
      if ((marker >= 0xC0 && marker <= 0xC3) ||
          (marker >= 0xC5 && marker <= 0xC7) ||
          (marker >= 0xC9 && marker <= 0xCB) ||
          (marker >= 0xCD && marker <= 0xCF)) {
        auto err = parseSofMarker();
        if (err != OM_SUCCESS) {
          return err;
        }
        found_sof = true;
        // Stop accumulating — the rest will be read fresh in read_packet
        break;
      }

      // SOS (Start Of Scan) - accumulate scan data until EOI
      if (marker == 0xDA) {
        while (!input_->isEOF()) {
          uint8_t byte;
          if (!readByte(byte)) {
            break;
          }
          if (byte == 0xFF) {
            uint8_t next;
            if (!readByte(next)) {
              break;
            }
            if (next == 0xD9) { // EOI
              break;
            }
            if (next != 0x00) { // Not a stuff byte, put back via re-process
              // next is already accumulated; just continue scanning
            }
          }
        }
        break;
      }

      // Skip standalone markers (no length field)
      if (marker >= 0xD0 && marker <= 0xD8) {
        continue;
      }

      // Markers with length field
      auto err = skipVariableMarker();
      if (err != OM_SUCCESS) {
        break;
      }
    }

    done:
    if (!found_sof || width_ == 0 || height_ == 0) {
      return OM_FORMAT_PARSE_FAILED;
    }

    // Create track
    Track track;
    track.index = 0;
    track.format.type = OM_MEDIA_IMAGE;
    track.format.codec_id = OM_CODEC_JPEG;
    track.format.image.width = width_;
    track.format.image.height = height_;
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

const FormatDescriptor FORMAT_JPEG = {
    .container_id = OM_CONTAINER_JPEG,
    .name = "jpeg",
    .long_name = "JPEG (Joint Photographic Experts Group)",
    .demuxer_factory = [] { return std::make_unique<JPEGDemuxer>(); },
    .muxer_factory = {},
};

} // namespace openmedia
