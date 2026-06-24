#include <algorithm>
#include <cstring>
#include <codecs.hpp>
#include <openmedia/video.hpp>
#include <vector>
#include <util/io_util.hpp>

namespace openmedia {

class BMPDecoder final : public Decoder {
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint16_t bit_count_ = 0;
  uint32_t compression_ = 0;
  uint32_t data_offset_ = 0;
  std::vector<uint32_t> color_table_;
  bool initialized_ = false;

  auto decode_uncompressed(const uint8_t* data, size_t data_size, Picture& pic) -> bool {
    if (data_offset_ >= data_size) {
      return false;
    }

    const uint8_t* pixel_data = data + data_offset_;
    size_t remaining = data_size - data_offset_;

    uint32_t bytes_per_pixel = bit_count_ / 8;
    uint32_t row_size = ((width_ * bytes_per_pixel + 3) / 4) * 4;

    if (bit_count_ == 4) {
      row_size = ((width_ + 1) / 2 + 3) / 4 * 4;
    } else if (bit_count_ == 1) {
      row_size = ((width_ + 7) / 8 + 3) / 4 * 4;
    }

    size_t expected_size = static_cast<size_t>(row_size) * height_;
    if (remaining < expected_size) {
      return false;
    }

    bool flip_vertical = true;

    for (uint32_t y = 0; y < height_; y++) {
      uint32_t src_y = flip_vertical ? y : (height_ - 1 - y);
      uint8_t* dst = pic.planes.data[0] + y * pic.planes.linesize[0];
      const uint8_t* src = pixel_data + src_y * row_size;

      switch (bit_count_) {
        case 32: {
          for (uint32_t x = 0; x < width_; x++) {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = src[x * 4 + 3];
          }
          break;
        }
        case 24: {
          for (uint32_t x = 0; x < width_; x++) {
            dst[x * 4 + 0] = src[x * 3 + 2];
            dst[x * 4 + 1] = src[x * 3 + 1];
            dst[x * 4 + 2] = src[x * 3 + 0];
            dst[x * 4 + 3] = 255;
          }
          break;
        }
        case 16: {
          for (uint32_t x = 0; x < width_; x++) {
            uint16_t pixel = load_u32_le(src + x * 2) & 0xFFFF;
            uint8_t r, g, b;
            if ((compression_ == 3) || (compression_ == 6)) {
              r = ((pixel >> 11) & 0x1F) << 3;
              g = ((pixel >> 5) & 0x3F) << 2;
              b = (pixel & 0x1F) << 3;
            } else {
              r = ((pixel >> 10) & 0x1F) << 3;
              g = ((pixel >> 5) & 0x1F) << 3;
              b = (pixel & 0x1F) << 3;
            }
            dst[x * 4 + 0] = r;
            dst[x * 4 + 1] = g;
            dst[x * 4 + 2] = b;
            dst[x * 4 + 3] = 255;
          }
          break;
        }
        case 8: {
          for (uint32_t x = 0; x < width_; x++) {
            uint8_t index = src[x];
            if (index < color_table_.size()) {
              uint32_t color = color_table_[index];
              dst[x * 4 + 0] = (color >> 16) & 0xFF;
              dst[x * 4 + 1] = (color >> 8) & 0xFF;
              dst[x * 4 + 2] = color & 0xFF;
              dst[x * 4 + 3] = 255;
            }
          }
          break;
        }
        case 4: {
          for (uint32_t x = 0; x < width_; x++) {
            uint8_t index = (x % 2 == 0) ? (src[x / 2] >> 4) : (src[x / 2] & 0x0F);
            if (index < color_table_.size()) {
              uint32_t color = color_table_[index];
              dst[x * 4 + 0] = (color >> 16) & 0xFF;
              dst[x * 4 + 1] = (color >> 8) & 0xFF;
              dst[x * 4 + 2] = color & 0xFF;
              dst[x * 4 + 3] = 255;
            }
          }
          break;
        }
        case 1: {
          for (uint32_t x = 0; x < width_; x++) {
            uint8_t bit = (src[x / 8] >> (7 - (x % 8))) & 1;
            uint32_t color = color_table_[bit];
            dst[x * 4 + 0] = (color >> 16) & 0xFF;
            dst[x * 4 + 1] = (color >> 8) & 0xFF;
            dst[x * 4 + 2] = color & 0xFF;
            dst[x * 4 + 3] = 255;
          }
          break;
        }
      }
    }

    return true;
  }

  auto decode_rle(const uint8_t* data, size_t data_size, Picture& pic) -> bool {
    if (data_offset_ >= data_size) {
      return false;
    }

    const uint8_t* pixel_data = data + data_offset_;
    size_t remaining = data_size - data_offset_;

    memset(pic.planes.data[0], 0, static_cast<size_t>(pic.planes.linesize[0]) * height_);

    uint32_t x = 0, y = 0;
    bool flip_vertical = true;
    if (flip_vertical) {
      y = height_ - 1;
    }

    size_t offset = 0;
    while (offset + 1 < remaining) {
      uint8_t count = pixel_data[offset++];
      uint8_t type = pixel_data[offset++];

      if (count == 0) {
        if (type == 0) {
          x = 0;
          if (flip_vertical) {
            if (y == 0) break;
            y--;
          } else {
            y++;
          }
        } else if (type == 1) {
          break;
        } else if (type == 2) {
          if (offset + 1 >= remaining) break;
          x += pixel_data[offset++];
          uint8_t dy = pixel_data[offset++];
          if (flip_vertical) {
            if (dy > y) break;
            y -= dy;
          } else {
            y += dy;
          }
        } else {
          uint8_t run_length = type;
          if (offset + run_length > remaining) break;

          for (uint8_t i = 0; i < run_length && x < width_; i++) {
            uint8_t index;
            if (bit_count_ == 8) {
              index = pixel_data[offset++];
            } else {
              index = (i % 2 == 0) ? (pixel_data[offset / 2] >> 4) : (pixel_data[offset / 2] & 0x0F);
              if (i % 2 == 1) offset++;
            }

            if (x < width_ && y < height_ && index < color_table_.size()) {
              uint32_t color = color_table_[index];
              uint8_t* dst = pic.planes.data[0] + y * pic.planes.linesize[0] + x * 4;
              dst[0] = (color >> 16) & 0xFF;
              dst[1] = (color >> 8) & 0xFF;
              dst[2] = color & 0xFF;
              dst[3] = 255;
            }
            x++;
          }

          if (run_length & 1) offset++;
        }
      } else {
        if (bit_count_ == 8) {
          uint8_t index = type;
          for (uint8_t i = 0; i < count && x < width_; i++) {
            if (y < height_ && index < color_table_.size()) {
              uint32_t color = color_table_[index];
              uint8_t* dst = pic.planes.data[0] + y * pic.planes.linesize[0] + x * 4;
              dst[0] = (color >> 16) & 0xFF;
              dst[1] = (color >> 8) & 0xFF;
              dst[2] = color & 0xFF;
              dst[3] = 255;
            }
            x++;
          }
        }
      }

      if (x >= width_) {
        x = 0;
        if (flip_vertical) {
          if (y == 0) break;
          y--;
        } else {
          y++;
        }
      }
    }

    return true;
  }

public:
  BMPDecoder() = default;
  ~BMPDecoder() override = default;

  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_BMP) {
      return OM_CODEC_INVALID_PARAMS;
    }
    width_ = options.format.image.width;
    height_ = options.format.image.height;
    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!initialized_) return std::nullopt;

    DecodingInfo info;
    info.media_type = OM_MEDIA_IMAGE;
    info.video_format = {OM_FORMAT_R8G8B8A8, width_, height_};
    return info;
  }

  void flush() override {
    color_table_.clear();
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    std::vector<Frame> frames;

    if (packet.bytes.empty() || packet.bytes.size() < 54) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    data_offset_ = load_u32_le(packet.bytes.data() + 10);
    uint32_t dib_size = load_u32_le(packet.bytes.data() + 14);

    int32_t width_signed = static_cast<int32_t>(load_u32_le(packet.bytes.data() + 18));
    int32_t height_signed = static_cast<int32_t>(load_u32_le(packet.bytes.data() + 22));
    bit_count_ = load_u32_le(packet.bytes.data() + 28) & 0xFFFF;
    compression_ = load_u32_le(packet.bytes.data() + 30);

    width_ = static_cast<uint32_t>(std::abs(width_signed));
    height_ = static_cast<uint32_t>(std::abs(height_signed));

    color_table_.clear();
    uint32_t colors_used = 0;
    if (dib_size >= 54) {
      colors_used = load_u32_le(packet.bytes.data() + 46);
    }

    uint32_t num_colors = colors_used;
    if (num_colors == 0 && bit_count_ <= 8) {
      num_colors = 1 << bit_count_;
    }

    uint32_t color_table_start = 14 + dib_size;
    for (uint32_t i = 0; i < num_colors && color_table_start + i * 4 + 3 < packet.bytes.size(); i++) {
      uint32_t offset = color_table_start + i * 4;
      uint32_t color = load_u32_le(packet.bytes.data() + offset);
      color_table_.push_back(color);
    }

    Picture pic(OM_FORMAT_R8G8B8A8, width_, height_);

    bool success = false;
    if (compression_ == 0) {
      success = decode_uncompressed(packet.bytes.data(), packet.bytes.size(), pic);
    } else if (compression_ == 1 || compression_ == 2) {
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

const CodecDescriptor CODEC_BMP = {
  .codec_id = OM_CODEC_BMP,
  .type = OM_MEDIA_IMAGE,
  .name = "bmp",
  .long_name = "BMP image decoder",
  .vendor = "OpenMedia",
  .flags = NONE,
  .decoder_factory = [] { return std::make_unique<BMPDecoder>(); },
};

} // namespace openmedia
