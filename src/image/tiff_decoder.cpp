#include <tiffio.h>
#include <algorithm>
#include <cstring>
#include <codecs.hpp>
#include <openmedia/video.hpp>
#include <vector>
#include <util/io_util.hpp>

namespace openmedia {

struct TIFFMemoryData {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t offset = 0;
};

static tmsize_t tiff_decode_read(thandle_t client_data, void* buf, tmsize_t size) {
  auto* mem = static_cast<TIFFMemoryData*>(client_data);
  if (!mem || !mem->data) {
    return -1;
  }
  size_t remaining = mem->size - mem->offset;
  size_t to_read = std::min(static_cast<size_t>(size), remaining);
  if (to_read == 0) {
    return 0;
  }
  std::memcpy(buf, mem->data + mem->offset, to_read);
  mem->offset += to_read;
  return static_cast<tmsize_t>(to_read);
}

static tmsize_t tiff_decode_write(thandle_t /*client_data*/, void* /*buf*/, tmsize_t /*size*/) {
  return -1;
}

static toff_t tiff_decode_seek(thandle_t client_data, toff_t offset, int whence) {
  auto* mem = static_cast<TIFFMemoryData*>(client_data);
  if (!mem || !mem->data) {
    return -1;
  }

  size_t new_offset;
  switch (whence) {
    case SEEK_SET: new_offset = static_cast<size_t>(offset); break;
    case SEEK_CUR: new_offset = mem->offset + static_cast<size_t>(offset); break;
    case SEEK_END: new_offset = mem->size + static_cast<size_t>(offset); break;
    default: return -1;
  }

  if (new_offset > mem->size) {
    return -1;
  }

  mem->offset = new_offset;
  return static_cast<toff_t>(new_offset);
}

static int tiff_decode_close(thandle_t /*client_data*/) {
  return 0;
}

static toff_t tiff_decode_size(thandle_t client_data) {
  auto* mem = static_cast<TIFFMemoryData*>(client_data);
  if (!mem || !mem->data) {
    return 0;
  }
  return static_cast<toff_t>(mem->size);
}

static int tiff_decode_map(thandle_t /*client_data*/, void** /*base*/, toff_t* /*size*/) {
  return 0;
}

static void tiff_decode_unmap(thandle_t /*client_data*/, void* /*base*/, toff_t /*size*/) {}

class TIFFDecoder final : public Decoder {
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint16_t bits_per_sample_ = 0;
  uint16_t samples_per_pixel_ = 0;
  uint16_t photometric_ = 0;
  uint16_t planar_config_ = 0;
  uint16_t extra_samples_ = 0;
  bool initialized_ = false;

  auto get_pixel_format(uint16_t photometric, uint16_t bits, uint16_t extra) -> OMPixelFormat {
    bool has_alpha = (extra == 1 || extra == 2);

    if (bits <= 8) {
      switch (photometric) {
        case 0:
        case 1:
          return OM_FORMAT_GRAY8;
        case 2:
          return has_alpha ? OM_FORMAT_R8G8B8A8 : OM_FORMAT_R8G8B8A8;
        case 5:
          return OM_FORMAT_R8G8B8A8;
        case 6:
          return OM_FORMAT_YUV420P;
        default:
          return OM_FORMAT_R8G8B8A8;
      }
    } else if (bits <= 16) {
      switch (photometric) {
        case 0:
        case 1:
          return OM_FORMAT_GRAY16;
        case 2:
          return has_alpha ? OM_FORMAT_RGBA64 : OM_FORMAT_RGBA64;
        default:
          return OM_FORMAT_RGBA64;
      }
    }

    return OM_FORMAT_R8G8B8A8;
  }

  auto convert_cmyk_to_rgba(const uint8_t* cmyk, uint8_t* rgba, uint32_t width) -> void {
    for (uint32_t x = 0; x < width; x++) {
      uint8_t c = cmyk[x * 4 + 0];
      uint8_t m = cmyk[x * 4 + 1];
      uint8_t y = cmyk[x * 4 + 2];
      uint8_t k = cmyk[x * 4 + 3];

      float c_norm = c / 255.0f;
      float m_norm = m / 255.0f;
      float y_norm = y / 255.0f;
      float k_norm = k / 255.0f;

      float r = (1.0f - c_norm) * (1.0f - k_norm);
      float g = (1.0f - m_norm) * (1.0f - k_norm);
      float b = (1.0f - y_norm) * (1.0f - k_norm);

      rgba[x * 4 + 0] = static_cast<uint8_t>(r * 255.0f);
      rgba[x * 4 + 1] = static_cast<uint8_t>(g * 255.0f);
      rgba[x * 4 + 2] = static_cast<uint8_t>(b * 255.0f);
      rgba[x * 4 + 3] = 0xFF;
    }
  }

  auto convert_rgb_to_rgba(const uint8_t* rgb, uint8_t* rgba, uint32_t width, bool has_alpha) -> void {
    for (uint32_t x = 0; x < width; x++) {
      rgba[x * 4 + 0] = rgb[x * (has_alpha ? 4 : 3) + 0];
      rgba[x * 4 + 1] = rgb[x * (has_alpha ? 4 : 3) + 1];
      rgba[x * 4 + 2] = rgb[x * (has_alpha ? 4 : 3) + 2];
      rgba[x * 4 + 3] = has_alpha ? rgb[x * 4 + 3] : 0xFF;
    }
  }

public:
  TIFFDecoder() = default;
  ~TIFFDecoder() override = default;

  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_TIFF) {
      return OM_CODEC_INVALID_PARAMS;
    }
    initialized_ = true;
    width_ = options.format.image.width;
    height_ = options.format.image.height;
    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!initialized_) return std::nullopt;

    DecodingInfo info;
    info.media_type = OM_MEDIA_IMAGE;
    info.video_format = {OM_FORMAT_R8G8B8A8, width_, height_};
    return info;
  }

  void flush() override {}

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    std::vector<Frame> frames;

    if (packet.bytes.empty()) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    TIFFMemoryData mem_data;
    mem_data.data = packet.bytes.data();
    mem_data.size = packet.bytes.size();
    mem_data.offset = 0;

    TIFF* tiff = TIFFClientOpen(
        "memory",
        "r",
        reinterpret_cast<thandle_t>(&mem_data),
        tiff_decode_read,
        tiff_decode_write,
        tiff_decode_seek,
        tiff_decode_close,
        tiff_decode_size,
        tiff_decode_map,
        tiff_decode_unmap);

    if (!tiff) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    uint32_t width, height;
    uint16_t bits_per_sample, samples_per_pixel, photometric, planar_config;
    uint16_t extra_samples = 0;

    if (!TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width) ||
        !TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height) ||
        !TIFFGetField(tiff, TIFFTAG_BITSPERSAMPLE, &bits_per_sample) ||
        !TIFFGetField(tiff, TIFFTAG_SAMPLESPERPIXEL, &samples_per_pixel) ||
        !TIFFGetField(tiff, TIFFTAG_PHOTOMETRIC, &photometric) ||
        !TIFFGetField(tiff, TIFFTAG_PLANARCONFIG, &planar_config)) {
      TIFFClose(tiff);
      return Err(OM_CODEC_DECODE_FAILED);
    }

    TIFFGetField(tiff, TIFFTAG_EXTRASAMPLES, &extra_samples, nullptr);

    width_ = width;
    height_ = height;
    bits_per_sample_ = bits_per_sample;
    samples_per_pixel_ = samples_per_pixel;
    photometric_ = photometric;
    planar_config_ = planar_config;
    extra_samples_ = extra_samples;

    OMPixelFormat output_format;
    if (photometric == 0 || photometric == 1) {
      output_format = (bits_per_sample > 8) ? OM_FORMAT_GRAY16 : OM_FORMAT_GRAY8;
    } else {
      output_format = (bits_per_sample > 8) ? OM_FORMAT_RGBA64 : OM_FORMAT_R8G8B8A8;
    }

    Picture pic(output_format, width, height);

    tmsize_t scanline_size = TIFFScanlineSize(tiff);
    std::vector<uint8_t> scanline_buf(static_cast<size_t>(scanline_size));

    for (uint32_t y = 0; y < height; y++) {
      if (TIFFReadScanline(tiff, scanline_buf.data(), y, 0) < 0) {
        TIFFClose(tiff);
        return Err(OM_CODEC_DECODE_FAILED);
      }

      uint8_t* dst = pic.planes.data[0] + y * pic.planes.linesize[0];

      if (output_format == OM_FORMAT_R8G8B8A8) {
        if (photometric == 2) {
          convert_rgb_to_rgba(scanline_buf.data(), dst, width, samples_per_pixel >= 4);
        } else if (photometric == 5) {
          convert_cmyk_to_rgba(scanline_buf.data(), dst, width);
        } else {
          std::memcpy(dst, scanline_buf.data(), std::min(static_cast<size_t>(scanline_size),
                                                          static_cast<size_t>(width * 4)));
        }
      } else if (output_format == OM_FORMAT_GRAY8 || output_format == OM_FORMAT_GRAY16) {
        std::memcpy(dst, scanline_buf.data(), scanline_size);
      } else if (output_format == OM_FORMAT_RGBA64) {
        if (bits_per_sample <= 16) {
          auto* src16 = reinterpret_cast<uint16_t*>(scanline_buf.data());
          auto* dst16 = reinterpret_cast<uint16_t*>(dst);
          for (uint32_t x = 0; x < width; x++) {
            dst16[x * 4 + 0] = src16[x * samples_per_pixel + 0];
            dst16[x * 4 + 1] = src16[x * samples_per_pixel + 1];
            dst16[x * 4 + 2] = src16[x * samples_per_pixel + 2];
            dst16[x * 4 + 3] = (samples_per_pixel >= 4) ? src16[x * samples_per_pixel + 3] : 0xFFFF;
          }
        }
      }
    }

    TIFFClose(tiff);

    Frame frame;
    frame.pts = packet.pts;
    frame.dts = packet.dts;
    frame.data = std::move(pic);
    frames.push_back(std::move(frame));

    return Ok(std::move(frames));
  }
};

const CodecDescriptor CODEC_TIFF = {
  .codec_id = OM_CODEC_TIFF,
  .type = OM_MEDIA_IMAGE,
  .name = "tiff",
  .long_name = "TIFF image decoder",
  .vendor = "libtiff",
  .flags = NONE,
  .decoder_factory = [] { return std::make_unique<TIFFDecoder>(); },
};

} // namespace openmedia
