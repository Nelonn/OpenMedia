#include <tiffio.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <codecs.hpp>
#include <openmedia/video.hpp>
#include <vector>
#include <util/io_util.hpp>

namespace openmedia {

struct DNGMemoryData {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t offset = 0;
};

static tmsize_t dng_decode_read(thandle_t client_data, void* buf, tmsize_t size) {
  auto* mem = static_cast<DNGMemoryData*>(client_data);
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

static tmsize_t dng_decode_write(thandle_t, void*, tmsize_t) {
  return -1;
}

static toff_t dng_decode_seek(thandle_t client_data, toff_t offset, int whence) {
  auto* mem = static_cast<DNGMemoryData*>(client_data);
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

static int dng_decode_close(thandle_t) {
  return 0;
}

static toff_t dng_decode_size(thandle_t client_data) {
  auto* mem = static_cast<DNGMemoryData*>(client_data);
  if (!mem || !mem->data) {
    return 0;
  }
  return static_cast<toff_t>(mem->size);
}

static int dng_decode_map(thandle_t, void**, toff_t*) {
  return 0;
}

static void dng_decode_unmap(thandle_t, void*, toff_t) {}

class DNGDecoder final : public Decoder {
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  bool initialized_ = false;

  static auto linear_to_srgb(float v) -> uint8_t {
    float clamped = std::clamp(v, 0.0f, 1.0f);
    float srgb = (clamped <= 0.0031308f) ? (12.92f * clamped) : (1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f);
    return static_cast<uint8_t>(std::clamp(srgb * 255.0f + 0.5f, 0.0f, 255.0f));
  }

  static auto demosaic_bayer(
      const std::vector<uint16_t>& bayer,
      uint32_t width,
      uint32_t height,
      const uint8_t cfa_pattern[4],
      float black_level,
      float white_level,
      Picture& pic) -> void {
    float range = (white_level > black_level) ? (white_level - black_level) : 1.0f;

    auto get_normalized = [&](uint32_t x, uint32_t y) -> float {
      x = std::clamp(x, 0u, width - 1);
      y = std::clamp(y, 0u, height - 1);
      float raw = static_cast<float>(bayer[y * width + x]);
      return std::clamp((raw - black_level) / range, 0.0f, 1.0f);
    };

    for (uint32_t y = 0; y < height; ++y) {
      uint8_t* dst = pic.planes.data[0] + y * pic.planes.linesize[0];
      for (uint32_t x = 0; x < width; ++x) {
        uint8_t cfa_type = cfa_pattern[((y & 1) << 1) | (x & 1)];
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;

        if (cfa_type == 0) {
          r = get_normalized(x, y);
          g = (get_normalized(x > 0 ? x - 1 : 0, y) +
               get_normalized(x + 1, y) +
               get_normalized(x, y > 0 ? y - 1 : 0) +
               get_normalized(x, y + 1)) * 0.25f;
          b = (get_normalized(x > 0 ? x - 1 : 0, y > 0 ? y - 1 : 0) +
               get_normalized(x + 1, y > 0 ? y - 1 : 0) +
               get_normalized(x > 0 ? x - 1 : 0, y + 1) +
               get_normalized(x + 1, y + 1)) * 0.25f;
        } else if (cfa_type == 2) {
          b = get_normalized(x, y);
          g = (get_normalized(x > 0 ? x - 1 : 0, y) +
               get_normalized(x + 1, y) +
               get_normalized(x, y > 0 ? y - 1 : 0) +
               get_normalized(x, y + 1)) * 0.25f;
          r = (get_normalized(x > 0 ? x - 1 : 0, y > 0 ? y - 1 : 0) +
               get_normalized(x + 1, y > 0 ? y - 1 : 0) +
               get_normalized(x > 0 ? x - 1 : 0, y + 1) +
               get_normalized(x + 1, y + 1)) * 0.25f;
        } else {
          g = get_normalized(x, y);
          uint8_t left_type = cfa_pattern[((y & 1) << 1) | ((x > 0 ? x - 1 : 0) & 1)];
          if (left_type == 0 || cfa_pattern[((y & 1) << 1) | ((x + 1) & 1)] == 0) {
            r = (get_normalized(x > 0 ? x - 1 : 0, y) + get_normalized(x + 1, y)) * 0.5f;
            b = (get_normalized(x, y > 0 ? y - 1 : 0) + get_normalized(x, y + 1)) * 0.5f;
          } else {
            b = (get_normalized(x > 0 ? x - 1 : 0, y) + get_normalized(x + 1, y)) * 0.5f;
            r = (get_normalized(x, y > 0 ? y - 1 : 0) + get_normalized(x, y + 1)) * 0.5f;
          }
        }

        dst[x * 4 + 0] = linear_to_srgb(r);
        dst[x * 4 + 1] = linear_to_srgb(g);
        dst[x * 4 + 2] = linear_to_srgb(b);
        dst[x * 4 + 3] = 0xFF;
      }
    }
  }

public:
  DNGDecoder() = default;
  ~DNGDecoder() override = default;

  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_DNG) {
      return OM_CODEC_INVALID_PARAMS;
    }
    initialized_ = true;
    width_ = options.format.video.width;
    height_ = options.format.video.height;
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
    if (packet.bytes.empty()) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    DNGMemoryData mem_data;
    mem_data.data = packet.bytes.data();
    mem_data.size = packet.bytes.size();
    mem_data.offset = 0;

    TIFF* tiff = TIFFClientOpen(
        "dng_decode_mem",
        "r",
        reinterpret_cast<thandle_t>(&mem_data),
        dng_decode_read,
        dng_decode_write,
        dng_decode_seek,
        dng_decode_close,
        dng_decode_size,
        dng_decode_map,
        dng_decode_unmap
    );

    if (!tiff) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    uint32_t best_w = 0;
    uint32_t best_h = 0;
    bool found_best = false;

    do {
      uint32_t cur_w = 0, cur_h = 0;
      if (TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &cur_w) && TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &cur_h)) {
        if ((cur_w == width_ && cur_h == height_) ||
            (!found_best && static_cast<uint64_t>(cur_w) * cur_h >= static_cast<uint64_t>(best_w) * best_h)) {
          best_w = cur_w;
          best_h = cur_h;
          if (cur_w == width_ && cur_h == height_) {
            found_best = true;
            break;
          }
        }
      }

      uint16_t subifd_count = 0;
      void* subifd_ptr = nullptr;
      if (TIFFGetField(tiff, TIFFTAG_SUBIFD, &subifd_count, &subifd_ptr) && subifd_count > 0 && subifd_ptr) {
        auto* subifd_offsets = static_cast<uint64_t*>(subifd_ptr);
        for (uint16_t s = 0; s < subifd_count; ++s) {
          if (TIFFSetSubDirectory(tiff, subifd_offsets[s])) {
            if (TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &cur_w) && TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &cur_h)) {
              if (cur_w == width_ && cur_h == height_) {
                best_w = cur_w;
                best_h = cur_h;
                found_best = true;
                break;
              }
            }
          }
        }
      }
      if (found_best) break;
    } while (TIFFReadDirectory(tiff));

    if (best_w == 0 || best_h == 0) {
      TIFFClose(tiff);
      return Err(OM_CODEC_DECODE_FAILED);
    }

    uint16_t photometric = 2;
    TIFFGetField(tiff, TIFFTAG_PHOTOMETRIC, &photometric);

    uint16_t bits_per_sample = 8;
    TIFFGetField(tiff, TIFFTAG_BITSPERSAMPLE, &bits_per_sample);

    Picture pic(OM_FORMAT_R8G8B8A8, best_w, best_h);
    bool decoded = false;

    if (photometric == 32803) {
      uint8_t cfa_pattern[4] = {0, 1, 1, 2};
      void* pattern_ptr = nullptr;
      if (TIFFGetField(tiff, 533, &pattern_ptr) && pattern_ptr) {
        std::memcpy(cfa_pattern, pattern_ptr, 4);
      }

      float black_level = 0.0f;
      float white_level = (bits_per_sample > 0 && bits_per_sample <= 16)
                              ? static_cast<float>((1u << bits_per_sample) - 1)
                              : 65535.0f;

      float bl = 0.0f;
      if (TIFFGetField(tiff, 50714, &bl)) black_level = bl;
      float wl = 0.0f;
      if (TIFFGetField(tiff, 50717, &wl)) white_level = wl;

      std::vector<uint16_t> bayer_data(best_w * best_h, 0);
      bool raw_read_success = false;

      if (TIFFIsTiled(tiff)) {
        uint32_t tile_w = 0, tile_h = 0;
        TIFFGetField(tiff, TIFFTAG_TILEWIDTH, &tile_w);
        TIFFGetField(tiff, TIFFTAG_TILELENGTH, &tile_h);

        if (tile_w > 0 && tile_h > 0) {
          tmsize_t tile_size = TIFFTileSize(tiff);
          std::vector<uint8_t> tile_buf(static_cast<size_t>(tile_size));
          raw_read_success = true;

          for (uint32_t ty = 0; ty < best_h; ty += tile_h) {
            for (uint32_t tx = 0; tx < best_w; tx += tile_w) {
              if (TIFFReadEncodedTile(tiff, TIFFComputeTile(tiff, tx, ty, 0, 0), tile_buf.data(), tile_size) < 0) {
                raw_read_success = false;
                break;
              }

              for (uint32_t py = 0; py < tile_h && ty + py < best_h; ++py) {
                for (uint32_t px = 0; px < tile_w && tx + px < best_w; ++px) {
                  uint32_t gx = tx + px;
                  uint32_t gy = ty + py;
                  if (bits_per_sample <= 8) {
                    bayer_data[gy * best_w + gx] = tile_buf[py * tile_w + px];
                  } else {
                    auto* tile_u16 = reinterpret_cast<const uint16_t*>(tile_buf.data());
                    bayer_data[gy * best_w + gx] = tile_u16[py * tile_w + px];
                  }
                }
              }
            }
            if (!raw_read_success) break;
          }
        }
      } else {
        tmsize_t strip_size = TIFFStripSize(tiff);
        std::vector<uint8_t> strip_buf(static_cast<size_t>(strip_size));
        uint32_t rows_per_strip = best_h;
        TIFFGetField(tiff, TIFFTAG_ROWSPERSTRIP, &rows_per_strip);
        if (rows_per_strip == 0) rows_per_strip = best_h;

        uint32_t num_strips = TIFFNumberOfStrips(tiff);
        raw_read_success = true;

        for (uint32_t s = 0; s < num_strips; ++s) {
          if (TIFFReadEncodedStrip(tiff, s, strip_buf.data(), strip_size) < 0) {
            raw_read_success = false;
            break;
          }

          uint32_t start_y = s * rows_per_strip;
          for (uint32_t sy = 0; sy < rows_per_strip && (start_y + sy) < best_h; ++sy) {
            uint32_t gy = start_y + sy;
            if (bits_per_sample <= 8) {
              for (uint32_t gx = 0; gx < best_w; ++gx) {
                bayer_data[gy * best_w + gx] = strip_buf[sy * best_w + gx];
              }
            } else {
              auto* strip_u16 = reinterpret_cast<const uint16_t*>(strip_buf.data());
              for (uint32_t gx = 0; gx < best_w; ++gx) {
                bayer_data[gy * best_w + gx] = strip_u16[sy * best_w + gx];
              }
            }
          }
        }
      }

      if (raw_read_success) {
        demosaic_bayer(bayer_data, best_w, best_h, cfa_pattern, black_level, white_level, pic);
        decoded = true;
      }
    }

    if (!decoded) {
      std::vector<uint32_t> raster(best_w * best_h);
      if (TIFFReadRGBAImageOriented(tiff, best_w, best_h, raster.data(), ORIENTATION_TOPLEFT, 0)) {
        for (uint32_t y = 0; y < best_h; ++y) {
          uint8_t* dst = pic.planes.data[0] + y * pic.planes.linesize[0];
          for (uint32_t x = 0; x < best_w; ++x) {
            uint32_t pixel = raster[y * best_w + x];
            dst[x * 4 + 0] = TIFFGetR(pixel);
            dst[x * 4 + 1] = TIFFGetG(pixel);
            dst[x * 4 + 2] = TIFFGetB(pixel);
            dst[x * 4 + 3] = TIFFGetA(pixel);
          }
        }
        decoded = true;
      }
    }

    TIFFClose(tiff);

    if (!decoded) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    width_ = best_w;
    height_ = best_h;

    Frame frame;
    frame.pts = packet.pts;
    frame.dts = packet.dts;
    frame.data = std::move(pic);

    std::vector<Frame> frames;
    frames.push_back(std::move(frame));
    return Ok(std::move(frames));
  }
};

const CodecDescriptor CODEC_DNG = {
  .codec_id = OM_CODEC_DNG,
  .type = OM_MEDIA_IMAGE,
  .name = "dng",
  .long_name = "DNG image decoder",
  .vendor = "libtiff",
  .flags = NONE,
  .decoder_factory = [] { return std::make_unique<DNGDecoder>(); },
};

}
