#include <png.h>
#include <algorithm>
#include <codecs.hpp>
#include <cstring>
#include <image/png_common.hpp>
#include <openmedia/codec_extra.hpp>
#include <openmedia/video.hpp>
#include <vector>

namespace openmedia {

namespace {

struct PNGWriteBuffer {
  std::vector<uint8_t> bytes;
};

void pngWriteCallback(png_structp png_ptr, png_bytep data, png_size_t length) {
  auto* buffer = static_cast<PNGWriteBuffer*>(png_get_io_ptr(png_ptr));
  buffer->bytes.insert(buffer->bytes.end(), data, data + length);
}

void pngFlushCallback(png_structp) {}

// How a picture's pixels reach libpng: the IHDR fields plus the transforms that
// turn the host layout into PNG's own (big-endian samples, RGB order).
struct PixelLayout {
  int color_type = 0;
  int bit_depth = 0;
  bool swap_bytes = false; // 16-bit samples are little-endian in memory
  bool bgr = false;
  bool has_alpha = false;
};

auto layoutFor(OMPixelFormat format) -> std::optional<PixelLayout> {
  switch (format) {
    case OM_FORMAT_R8G8B8A8:
      return PixelLayout {PNG_COLOR_TYPE_RGBA, 8, false, false, true};
    case OM_FORMAT_B8G8R8A8:
      return PixelLayout {PNG_COLOR_TYPE_RGBA, 8, false, true, true};
    case OM_FORMAT_RGBA64:
      return PixelLayout {PNG_COLOR_TYPE_RGBA, 16, true, false, true};
    case OM_FORMAT_GRAY8:
      return PixelLayout {PNG_COLOR_TYPE_GRAY, 8, false, false, false};
    case OM_FORMAT_GRAY16:
      return PixelLayout {PNG_COLOR_TYPE_GRAY, 16, true, false, false};
    default:
      return std::nullopt;
  }
}

// sRGB covers what an ordinary PNG needs, and old decoders ignore cICP, so the
// chunk is only worth writing for a description sRGB genuinely cannot carry.
auto needsCICP(const Picture& pic) -> bool {
  switch (pic.transfer_char) {
    case OM_TRANSFER_PQ:
    case OM_TRANSFER_HLG:
      return true;
    default:
      break;
  }
  switch (pic.color_primaries) {
    case OM_PRIMARIES_BT2020:
    case OM_PRIMARIES_SMPTE431:
    case OM_PRIMARIES_SMPTE432:
      return true;
    default:
      return false;
  }
}

} // namespace

class PNGEncoder final : public Encoder {
  // libpng's own defaults stand wherever the caller said nothing, so every
  // knob starts as "unset" rather than as a guessed value.
  std::optional<int> compression_level_;
  std::optional<int> compression_strategy_;
  std::optional<int> compression_mem_level_;
  std::optional<int> compression_window_bits_;
  std::optional<int> filters_;
  bool interlace_ = false;
  bool strip_alpha_ = false;
  bool color_chunks_ = true;
  bool initialized_ = false;

  // png_create_write_struct and friends are per-image: a PNG packet is a whole
  // file, and libpng has no way to rewind a write struct.
  struct WriteContext {
    png_structp png = nullptr;
    png_infop info = nullptr;

    ~WriteContext() {
      if (png != nullptr) {
        png_destroy_write_struct(&png, info != nullptr ? &info : nullptr);
      }
    }
  };

public:
  PNGEncoder() = default;
  ~PNGEncoder() override = default;

  auto configure(const EncoderOptions& options) -> OMError override {
    initialized_ = false;

    if (options.format.codec_id != OM_CODEC_PNG) {
      return OM_CODEC_INVALID_PARAMS;
    }
    if (options.video_format.format != OM_FORMAT_UNKNOWN &&
        !layoutFor(options.video_format.format).has_value()) {
      return OM_CODEC_NOT_SUPPORTED;
    }

    const auto& extra = options.extra;

    // PNG is lossless, so options.rate_control has nothing to say here: its
    // quality and bitrate targets describe trade-offs PNG does not make.
    auto read_range = [&extra](const Key& key, int lo, int hi) -> std::optional<int> {
      if (!extra.contains(key)) return std::nullopt;
      const int value = extra.getInt32(key);
      if (value < lo || value > hi) return std::nullopt;
      return value;
    };

    if (extra.contains(PNG_ENC_COMPRESSION_LEVEL)) {
      compression_level_ = read_range(PNG_ENC_COMPRESSION_LEVEL, 0, 9);
      if (!compression_level_.has_value()) return OM_CODEC_INVALID_PARAMS;
    }
    if (extra.contains(PNG_ENC_COMPRESSION_STRATEGY)) {
      compression_strategy_ = read_range(PNG_ENC_COMPRESSION_STRATEGY, 0, 4);
      if (!compression_strategy_.has_value()) return OM_CODEC_INVALID_PARAMS;
    }
    if (extra.contains(PNG_ENC_COMPRESSION_MEM_LEVEL)) {
      compression_mem_level_ = read_range(PNG_ENC_COMPRESSION_MEM_LEVEL, 1, 9);
      if (!compression_mem_level_.has_value()) return OM_CODEC_INVALID_PARAMS;
    }
    if (extra.contains(PNG_ENC_COMPRESSION_WINDOW_BITS)) {
      compression_window_bits_ = read_range(PNG_ENC_COMPRESSION_WINDOW_BITS, 8, 15);
      if (!compression_window_bits_.has_value()) return OM_CODEC_INVALID_PARAMS;
    }
    if (extra.contains(PNG_ENC_FILTERS)) {
      const int filters = extra.getInt32(PNG_ENC_FILTERS);
      if ((filters & ~PNG_ALL_FILTERS) != 0) return OM_CODEC_INVALID_PARAMS;
      filters_ = filters;
    }

    interlace_ = extra.getBool(PNG_ENC_INTERLACE, false);
    strip_alpha_ = extra.getBool(PNG_ENC_STRIP_ALPHA, false);
    color_chunks_ = extra.getBool(PNG_ENC_COLOR_CHUNKS, true);

    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> EncodingInfo override {
    // A PNG packet is a complete file: nothing is left for a container to
    // carry out of band.
    return {};
  }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    if (!initialized_) return Err(OM_COMMON_NOT_INITIALIZED);

    std::vector<Packet> packets;

    // Nothing is buffered between calls, so a drain request has no output.
    if (!std::holds_alternative<Picture>(frame.data)) {
      return Ok(std::move(packets));
    }

    const auto& pic = std::get<Picture>(frame.data);
    if (pic.width == 0 || pic.height == 0) {
      return Ok(std::move(packets));
    }

    const auto layout = layoutFor(pic.format);
    if (!layout.has_value()) {
      return Err(OM_CODEC_NOT_SUPPORTED);
    }
    if (pic.planes.data[0] == nullptr) {
      return Err(OM_CODEC_INVALID_PARAMS);
    }

    // Everything libpng's error path would otherwise skip over lives in this
    // frame, so a longjmp back into the setjmp below still unwinds it.
    WriteContext ctx;
    PNGWriteBuffer buffer;
    std::vector<png_bytep> rows(pic.height);

    ctx.png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (ctx.png == nullptr) {
      return Err(OM_COMMON_OUT_OF_MEMORY);
    }
    ctx.info = png_create_info_struct(ctx.png);
    if (ctx.info == nullptr) {
      return Err(OM_COMMON_OUT_OF_MEMORY);
    }

    for (uint32_t y = 0; y < pic.height; ++y) {
      rows[y] = pic.planes.data[0] + static_cast<size_t>(y) * pic.planes.linesize[0];
    }

    if (setjmp(png_jmpbuf(ctx.png))) {
      return Err(OM_CODEC_ENCODE_FAILED);
    }

    png_set_write_fn(ctx.png, &buffer, pngWriteCallback, pngFlushCallback);

    if (compression_level_.has_value()) png_set_compression_level(ctx.png, *compression_level_);
    if (compression_strategy_.has_value()) png_set_compression_strategy(ctx.png, *compression_strategy_);
    if (compression_mem_level_.has_value()) png_set_compression_mem_level(ctx.png, *compression_mem_level_);
    if (compression_window_bits_.has_value()) png_set_compression_window_bits(ctx.png, *compression_window_bits_);
    if (filters_.has_value()) png_set_filter(ctx.png, PNG_FILTER_TYPE_BASE, *filters_);

    const bool drop_alpha = strip_alpha_ && layout->has_alpha;
    const int color_type = drop_alpha ? PNG_COLOR_TYPE_RGB : layout->color_type;

    png_set_IHDR(ctx.png, ctx.info, pic.width, pic.height, layout->bit_depth, color_type,
                 interlace_ ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);

    if (color_chunks_) {
      writeColorChunks(ctx, pic, color_type);
    }

    png_write_info(ctx.png, ctx.info);

    // Input transforms only take effect once the header is out; see the write
    // sequence in libpng's own example.c.
    if (drop_alpha) png_set_filler(ctx.png, 0, PNG_FILLER_AFTER);
    if (layout->bgr) png_set_bgr(ctx.png);
    if (layout->swap_bytes) png_set_swap(ctx.png);

    png_write_image(ctx.png, rows.data());
    png_write_end(ctx.png, nullptr);

    Packet packet;
    packet.allocate(buffer.bytes.size());
    memcpy(packet.bytes.data(), buffer.bytes.data(), buffer.bytes.size());
    packet.pts = frame.pts;
    packet.dts = frame.dts;
    packet.is_keyframe = true;

    packets.push_back(std::move(packet));
    return Ok(std::move(packets));
  }

  auto updateBitrate(const RateControlParams& rc) -> OMError override {
    (void) rc;
    // Lossless and one self-contained file per call: nothing to retarget.
    return OM_COMMON_NOT_SUPPORTED;
  }

private:
  static void writeColorChunks(WriteContext& ctx, const Picture& pic, int color_type) {
    const bool is_rgb = color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_RGBA;

#ifdef PNG_cICP_SUPPORTED
    // PNG stores RGB samples, so the cICP chunk fixes matrix coefficients at 0
    // (identity) and the range at full; only the two endpoints vary.
    if (is_rgb && needsCICP(pic)) {
      png_set_cICP(ctx.png, ctx.info,
                   static_cast<png_byte>(pic.color_primaries),
                   static_cast<png_byte>(pic.transfer_char),
                   /*matrix_coefficients=*/0, /*video_full_range_flag=*/1);
    }
#endif

#ifdef PNG_mDCV_SUPPORTED
    if (is_rgb && pic.mastering_display.has_value) {
      const auto& md = pic.mastering_display;
      // Chromaticities arrive in 1/50000 units and PNG wants 1/100000, and the
      // array is in SEI order: green, blue, red.
      auto chroma = [](uint16_t value) -> png_fixed_point {
        return static_cast<png_fixed_point>(value) * 2;
      };
      png_set_mDCV_fixed(ctx.png, ctx.info,
                         chroma(md.white_point[0]), chroma(md.white_point[1]),
                         chroma(md.display_primaries[2][0]), chroma(md.display_primaries[2][1]),
                         chroma(md.display_primaries[0][0]), chroma(md.display_primaries[0][1]),
                         chroma(md.display_primaries[1][0]), chroma(md.display_primaries[1][1]),
                         md.max_display_mastering_luminance,
                         md.min_display_mastering_luminance);
    }
#endif

#ifdef PNG_cLLI_SUPPORTED
    if (is_rgb && pic.content_light_level.has_value) {
      // Both chunks count in nits; PNG scales them by 10000.
      png_set_cLLI_fixed(ctx.png, ctx.info,
                         static_cast<png_uint_32>(pic.content_light_level.max_content_light_level) * 10000,
                         static_cast<png_uint_32>(pic.content_light_level.max_pic_average_light_level) * 10000);
    }
#endif
  }
};

auto createPNGEncoder() -> std::unique_ptr<Encoder> {
  return std::make_unique<PNGEncoder>();
}

} // namespace openmedia
