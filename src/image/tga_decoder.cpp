#include <algorithm>
#include <cstring>
#include <codecs.hpp>
#include <openmedia/video.hpp>
#include <vector>
#include <util/io_util.hpp>

namespace openmedia {

class TGADecoder final : public Decoder {
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint8_t pixel_depth_ = 0;
  uint8_t image_type_ = 0;
  uint8_t has_alpha_ = 0;
  uint8_t id_length_ = 0;
  uint8_t color_map_type_ = 0;
  uint16_t cm_origin_ = 0;
  uint16_t cm_length_ = 0;
  uint8_t cm_depth_ = 0;
  bool initialized_ = false;

  auto get_pixel_format() const -> OMPixelFormat {
    if (image_type_ == 3 || image_type_ == 11) {
      return pixel_depth_ == 8 ? OM_FORMAT_GRAY8 : OM_FORMAT_GRAY16;
    }
    return OM_FORMAT_R8G8B8A8;
  }

  auto decode_uncompressed(const uint8_t* data, size_t data_size, Picture& pic) -> bool {
    size_t header_size = 18 + id_length_;
    if (color_map_type_ == 1) {
      header_size += cm_origin_ + cm_length_ * (cm_depth_ / 8);
    }

    if (header_size >= data_size) {
      return false;
    }

    const uint8_t* pixel_data = data + header_size;
    size_t remaining = data_size - header_size;

    size_t bytes_per_pixel = pixel_depth_ / 8;
    size_t expected_size = static_cast<size_t>(width_) * height_ * bytes_per_pixel;

    if (remaining < expected_size) {
      return false;
    }

    bool flip_vertical = true;

    for (uint32_t y = 0; y < height_; y++) {
      uint32_t src_y = flip_vertical ? (height_ - 1 - y) : y;
      uint8_t* dst = pic.planes.data[0] + y * pic.planes.linesize[0];
      const uint8_t* src = pixel_data + src_y * width_ * bytes_per_pixel;

      if (image_type_ == 3 || image_type_ == 11) {
        if (pixel_depth_ == 8) {
          memcpy(dst, src, width_);
        } else if (pixel_depth_ == 16) {
          for (uint32_t x = 0; x < width_; x++) {
            dst[x] = src[x * 2];
          }
        }
      } else if (image_type_ == 2 || image_type_ == 10) {
        if (pixel_depth_ == 24) {
          for (uint32_t x = 0; x < width_; x++) {
            dst[x * 4 + 0] = src[x * 3 + 2];
            dst[x * 4 + 1] = src[x * 3 + 1];
            dst[x * 4 + 2] = src[x * 3 + 0];
            dst[x * 4 + 3] = 255;
          }
        } else if (pixel_depth_ == 32) {
          for (uint32_t x = 0; x < width_; x++) {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = src[x * 4 + 3];
          }
        }
      }
    }

    return true;
  }

  auto decode_rle(const uint8_t* data, size_t data_size, Picture& pic) -> bool {
    size_t header_size = 18 + id_length_;
    if (color_map_type_ == 1) {
      header_size += cm_origin_ + cm_length_ * (cm_depth_ / 8);
    }

    if (header_size >= data_size) {
      return false;
    }

    const uint8_t* pixel_data = data + header_size;
    size_t remaining = data_size - header_size;

    size_t bytes_per_pixel = pixel_depth_ / 8;
    bool flip_vertical = true;

    size_t pixel_offset = 0;
    size_t data_offset = 0;

    while (pixel_offset < static_cast<size_t>(width_) * height_ && data_offset < remaining) {
      uint8_t rle_header = pixel_data[data_offset++];
      uint8_t rle_count = (rle_header & 0x7F) + 1;
      bool is_rle = (rle_header & 0x80) != 0;

      for (uint8_t i = 0; i < rle_count && pixel_offset < static_cast<size_t>(width_) * height_; i++) {
        uint32_t pixel_idx = static_cast<uint32_t>(pixel_offset);
        uint32_t x = pixel_idx % width_;
        uint32_t y = pixel_idx / width_;

        if (flip_vertical) {
          y = height_ - 1 - y;
        }

        uint8_t* dst = pic.planes.data[0] + y * pic.planes.linesize[0] + x * 4;
        const uint8_t* src;

        if (is_rle) {
          src = pixel_data + data_offset;
        } else {
          src = pixel_data + data_offset;
          data_offset += bytes_per_pixel;
        }

        if (image_type_ == 10) {
          if (pixel_depth_ == 24) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = 255;
          } else if (pixel_depth_ == 32) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = src[3];
          }
        }

        if (!is_rle) {
          pixel_offset++;
        }
      }

      if (is_rle) {
        data_offset += bytes_per_pixel;
        pixel_offset += rle_count;
      }
    }

    return true;
  }

public:
  TGADecoder() = default;
  ~TGADecoder() override = default;

  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_TGA) {
      return OM_CODEC_INVALID_PARAMS;
    }
    width_ = options.format.image.width;
    height_ = options.format.image.height;
    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!initialized_) return std::nullopt;

    DecodingInfo info = {};
    info.media_type = OM_MEDIA_IMAGE;
    info.video_format = {get_pixel_format(), width_, height_};
    return info;
  }

  void flush() override {}

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    std::vector<Frame> frames;

    if (packet.bytes.empty() || packet.bytes.size() < 18) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    id_length_ = packet.bytes.data()[0];
    color_map_type_ = packet.bytes.data()[1];
    image_type_ = packet.bytes.data()[2];
    cm_origin_ = packet.bytes.data()[3] | (packet.bytes.data()[4] << 8);
    cm_length_ = packet.bytes.data()[5] | (packet.bytes.data()[6] << 8);
    cm_depth_ = packet.bytes.data()[7];
    width_ = packet.bytes.data()[12] | (packet.bytes.data()[13] << 8);
    height_ = packet.bytes.data()[14] | (packet.bytes.data()[15] << 8);
    pixel_depth_ = packet.bytes.data()[16];
    uint8_t image_descriptor = packet.bytes.data()[17];
    has_alpha_ = (image_descriptor & 0x0F) > 0;

    Picture pic(get_pixel_format(), width_, height_);

    bool success = false;
    if (image_type_ == 2 || image_type_ == 3) {
      success = decode_uncompressed(packet.bytes.data(), packet.bytes.size(), pic);
    } else if (image_type_ == 10 || image_type_ == 11) {
      success = decode_rle(packet.bytes.data(), packet.bytes.size(), pic);
    }

    if (!success) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    Frame frame;
    frame.pts = packet.pts;
    frame.dts = packet.dts;
    frame.data = std::move(pic);
    frames.push_back(std::move(frame));

    return Ok(std::move(frames));
  }
};

const CodecDescriptor CODEC_TGA = {
  .codec_id = OM_CODEC_TGA,
  .type = OM_MEDIA_IMAGE,
  .name = "tga",
  .long_name = "TGA image decoder",
  .vendor = "OpenMedia",
  .flags = NONE,
  .decoder_factory = [] { return std::make_unique<TGADecoder>(); },
};

} // namespace openmedia
