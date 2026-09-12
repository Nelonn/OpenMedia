#include <png.h>
#include <algorithm>
#include <cstring>
#include <codecs.hpp>
#include <openmedia/video.hpp>
#include <vector>
#include <util/io_util.hpp>

namespace openmedia {

struct PNGReadStruct {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t offset = 0;
};

class PNGDecoder final : public Decoder {
  png_structp png_ptr_ = nullptr;
  png_infop info_ptr_ = nullptr;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  png_byte bit_depth_ = 0;
  png_byte color_type_ = 0;
  bool initialized_ = false;

  static void png_read_callback(png_structp png_ptr, png_bytep data, png_size_t length) {
    auto* read_struct = static_cast<PNGReadStruct*>(png_get_io_ptr(png_ptr));
    if (read_struct->offset + length > read_struct->size) {
      png_error(png_ptr, "Read error");
    }
    memcpy(data, read_struct->data + read_struct->offset, length);
    read_struct->offset += length;
  }

  static auto get_pixel_format(png_byte bit_depth, png_byte color_type) -> OMPixelFormat {
    if (bit_depth == 8) {
      switch (color_type) {
        case PNG_COLOR_TYPE_GRAY: return OM_FORMAT_GRAY8;
        case PNG_COLOR_TYPE_RGB: return OM_FORMAT_R8G8B8A8;
        case PNG_COLOR_TYPE_RGBA: return OM_FORMAT_R8G8B8A8;
        case PNG_COLOR_TYPE_GRAY_ALPHA: return OM_FORMAT_GRAY8;
        default: return OM_FORMAT_R8G8B8A8;
      }
    } else if (bit_depth == 16) {
      switch (color_type) {
        case PNG_COLOR_TYPE_GRAY: return OM_FORMAT_GRAY16;
        case PNG_COLOR_TYPE_RGB: return OM_FORMAT_RGBA64;
        case PNG_COLOR_TYPE_RGBA: return OM_FORMAT_RGBA64;
        case PNG_COLOR_TYPE_GRAY_ALPHA: return OM_FORMAT_GRAY16;
        default: return OM_FORMAT_RGBA64;
      }
    }
    return OM_FORMAT_R8G8B8A8;
  }

public:
  PNGDecoder() {
    png_ptr_ = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (png_ptr_) {
      info_ptr_ = png_create_info_struct(png_ptr_);
    }
  }

  ~PNGDecoder() override {
    if (info_ptr_) {
      png_free_data(png_ptr_, info_ptr_, PNG_FREE_ALL, -1);
      png_destroy_info_struct(png_ptr_, &info_ptr_);
    }
    if (png_ptr_) {
      png_destroy_read_struct(&png_ptr_, nullptr, nullptr);
    }
  }

  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_PNG) {
      return OM_CODEC_INVALID_PARAMS;
    }
    initialized_ = true;
    width_ = options.format.video.width;
    height_ = options.format.video.height;
    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!initialized_) return std::nullopt;

    DecodingInfo info = {};
    info.media_type = OM_MEDIA_IMAGE;
    info.video_format = {OM_FORMAT_R8G8B8A8, width_, height_};
    return info;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    std::vector<Frame> frames;

    if (!png_ptr_ || !info_ptr_) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    PNGReadStruct read_struct;
    read_struct.data = packet.bytes.data();
    read_struct.size = packet.bytes.size();
    read_struct.offset = 0;

    if (setjmp(png_jmpbuf(png_ptr_))) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    png_set_read_fn(png_ptr_, &read_struct, png_read_callback);
    png_read_info(png_ptr_, info_ptr_);

    width_ = png_get_image_width(png_ptr_, info_ptr_);
    height_ = png_get_image_height(png_ptr_, info_ptr_);
    bit_depth_ = png_get_bit_depth(png_ptr_, info_ptr_);
    color_type_ = png_get_color_type(png_ptr_, info_ptr_);

    // Transformations to standardize output
    if (bit_depth_ == 16) {
#if PNG_LIBPNG_VER >= 10700
      png_set_scale_16(png_ptr_);
#else
      png_set_strip_16(png_ptr_);
#endif
      bit_depth_ = 8;
    }

    if (color_type_ == PNG_COLOR_TYPE_PALETTE) {
      png_set_palette_to_rgb(png_ptr_);
      color_type_ = PNG_COLOR_TYPE_RGB;
    }

    if (color_type_ == PNG_COLOR_TYPE_GRAY && bit_depth_ < 8) {
      png_set_expand_gray_1_2_4_to_8(png_ptr_);
      bit_depth_ = 8;
    }

    if (color_type_ == PNG_COLOR_TYPE_RGB) {
      png_set_add_alpha(png_ptr_, 0xFF, PNG_FILLER_AFTER);
      color_type_ = PNG_COLOR_TYPE_RGBA;
    } else if (color_type_ == PNG_COLOR_TYPE_GRAY) {
      png_set_expand(png_ptr_);
    }

    png_read_update_info(png_ptr_, info_ptr_);

    OMPixelFormat format = get_pixel_format(bit_depth_, color_type_);
    Picture pic(format, width_, height_);

    std::vector<png_bytep> row_pointers(height_);
    for (uint32_t y = 0; y < height_; y++) {
      row_pointers[y] = pic.planes.data[0] + y * pic.planes.linesize[0];
    }

    png_read_image(png_ptr_, row_pointers.data());
    png_read_end(png_ptr_, nullptr);

    Frame frame;
    frame.pts = packet.pts;
    frame.dts = packet.dts;
    frame.data = std::move(pic);
    frames.push_back(std::move(frame));

    return Ok(std::move(frames));
  }

  void flush() override {
    if (png_ptr_) {
      if (info_ptr_) {
        png_free_data(png_ptr_, info_ptr_, PNG_FREE_ALL, -1);
        png_destroy_info_struct(png_ptr_, &info_ptr_);
      }
      png_destroy_read_struct(&png_ptr_, nullptr, nullptr);
    }

    png_ptr_ = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (png_ptr_) {
      info_ptr_ = png_create_info_struct(png_ptr_);
    }
  }
};

const CodecDescriptor CODEC_PNG = {
  .codec_id = static_cast<OMCodecId>(OM_CODEC_PNG),
  .type = OM_MEDIA_IMAGE,
  .name = "png",
  .long_name = "PNG",
  .vendor = "libpng",
  .flags = NONE,
  .decoder_factory = []{ return std::make_unique<PNGDecoder>(); },
};

} // namespace openmedia
