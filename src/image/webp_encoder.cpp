#include <webp/encode.h>
#include <algorithm>
#include <codecs.hpp>
#include <cstring>
#include <image/webp_common.hpp>
#include <openmedia/codec_extra.hpp>
#include <openmedia/video.hpp>
#include <type_traits>
#include <vector>

namespace openmedia {

namespace {

// WebPPicture owns heap buffers the moment an import succeeds, and WebPEncode
// can allocate more of its own while converting between ARGB and YUV, so every
// exit from encode() has to go through WebPPictureFree. Freeing a picture that
// only borrows our plane pointers is a no-op, so this is safe either way.
struct PictureGuard {
  WebPPicture* picture;
  ~PictureGuard() { WebPPictureFree(picture); }
};

struct WriterGuard {
  WebPMemoryWriter* writer;
  ~WriterGuard() { WebPMemoryWriterClear(writer); }
};

auto isSupportedFormat(OMPixelFormat format) -> bool {
  return format == OM_FORMAT_R8G8B8A8 ||
         format == OM_FORMAT_B8G8R8A8 ||
         format == OM_FORMAT_YUV420P;
}

} // namespace

class WEBPEncoder final : public Encoder {
  WebPConfig config_ = {};
  bool initialized_ = false;

public:
  WEBPEncoder() = default;
  ~WEBPEncoder() override = default;

  auto configure(const EncoderOptions& options) -> OMError override {
    initialized_ = false;

    if (options.format.codec_id != OM_CODEC_WEBP) {
      return OM_CODEC_INVALID_PARAMS;
    }
    if (options.video_format.format != OM_FORMAT_UNKNOWN &&
        !isSupportedFormat(options.video_format.format)) {
      return OM_CODEC_NOT_SUPPORTED;
    }

    const auto& extra = options.extra;

    // A still image has no bitrate to hit, so CRF is the only rate control
    // WebP can honour. A default-constructed RateControlParams reads as CRF 0,
    // which would silently ask for the worst picture the encoder can make -
    // treat that as "unset" and fall back to libwebp's own default of 75.
    float quality = 75.0f;
    if (const auto* crf = std::get_if<CrfParams>(&options.rate_control.params);
        crf != nullptr && crf->quality > 0.0f) {
      quality = crf->quality;
    }
    quality = std::clamp(extra.getFloat(WEBP_ENC_QUALITY, quality), 0.0f, 100.0f);

    const auto preset = static_cast<WebPPreset>(
        std::clamp(extra.getInt32(WEBP_ENC_PRESET, WEBP_PRESET_DEFAULT),
                   static_cast<int32_t>(WEBP_PRESET_DEFAULT),
                   static_cast<int32_t>(WEBP_PRESET_TEXT)));

    WebPConfig config = {};
    if (!WebPConfigPreset(&config, preset, quality)) {
      return OM_CODEC_OPEN_FAILED; // libwebp ABI mismatch
    }

    // The lossless preset is the coarse knob: it rewrites lossless, quality and
    // method together, so it takes the place of the plain lossless flag.
    if (extra.contains(WEBP_ENC_LOSSLESS_PRESET)) {
      const int level = std::clamp(extra.getInt32(WEBP_ENC_LOSSLESS_PRESET), 0, 9);
      if (!WebPConfigLosslessPreset(&config, level)) {
        return OM_CODEC_INVALID_PARAMS;
      }
    } else if (extra.getBool(WEBP_ENC_LOSSLESS, false)) {
      config.lossless = 1;
    }

    auto set_int32 = [&extra]<typename T>(const Key& key, T& field) {
      if (!extra.contains(key)) return;
      field = static_cast<std::remove_reference_t<T>>(extra.getInt32(key));
    };
    auto set_float = [&extra](const Key& key, float& field) {
      if (extra.contains(key)) field = extra.getFloat(key);
    };
    auto set_bool = [&extra](const Key& key, int& field) {
      if (extra.contains(key)) field = extra.getBool(key) ? 1 : 0;
    };

    set_int32(WEBP_ENC_METHOD, config.method);
    set_int32(WEBP_ENC_IMAGE_HINT, config.image_hint);
    set_int32(WEBP_ENC_NEAR_LOSSLESS, config.near_lossless);
    set_bool(WEBP_ENC_EXACT, config.exact);
    set_int32(WEBP_ENC_TARGET_SIZE, config.target_size);
    set_float(WEBP_ENC_TARGET_PSNR, config.target_PSNR);
    set_int32(WEBP_ENC_ALPHA_COMPRESSION, config.alpha_compression);
    set_int32(WEBP_ENC_ALPHA_FILTERING, config.alpha_filtering);
    set_int32(WEBP_ENC_ALPHA_QUALITY, config.alpha_quality);
    set_int32(WEBP_ENC_SEGMENTS, config.segments);
    set_int32(WEBP_ENC_SNS_STRENGTH, config.sns_strength);
    set_int32(WEBP_ENC_FILTER_STRENGTH, config.filter_strength);
    set_int32(WEBP_ENC_FILTER_SHARPNESS, config.filter_sharpness);
    set_int32(WEBP_ENC_FILTER_TYPE, config.filter_type);
    set_bool(WEBP_ENC_AUTOFILTER, config.autofilter);
    set_int32(WEBP_ENC_PASS, config.pass);
    set_int32(WEBP_ENC_PREPROCESSING, config.preprocessing);
    set_int32(WEBP_ENC_PARTITIONS, config.partitions);
    set_int32(WEBP_ENC_PARTITION_LIMIT, config.partition_limit);
    set_bool(WEBP_ENC_EMULATE_JPEG_SIZE, config.emulate_jpeg_size);
    set_bool(WEBP_ENC_LOW_MEMORY, config.low_memory);
    set_bool(WEBP_ENC_SHARP_YUV, config.use_sharp_yuv);
    set_int32(WEBP_ENC_QMIN, config.qmin);
    set_int32(WEBP_ENC_QMAX, config.qmax);

    // libwebp only asks whether threading is wanted at all; it picks the
    // worker count itself. An explicit request for one thread turns it off.
    if (extra.contains(CODEC_THREADS)) {
      config.thread_level = extra.getInt32(CODEC_THREADS) == 1 ? 0 : 1;
    }

    // QP bounds are the general form of libwebp's own quality clamps.
    if (options.rate_control.min_qp.has_value()) {
      config.qmin = std::clamp(*options.rate_control.min_qp, 0, 100);
    }
    if (options.rate_control.max_qp.has_value()) {
      config.qmax = std::clamp(*options.rate_control.max_qp, 0, 100);
    }

    if (!WebPValidateConfig(&config)) {
      return OM_CODEC_INVALID_PARAMS;
    }

    config_ = config;
    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> EncodingInfo override {
    // WebP carries everything it needs inside the bitstream: no extradata,
    // and no HDR metadata for a muxer to pick up.
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
    if (pic.width > WEBP_MAX_DIMENSION || pic.height > WEBP_MAX_DIMENSION) {
      return Err(OM_CODEC_INVALID_PARAMS);
    }
    if (!isSupportedFormat(pic.format)) {
      return Err(OM_CODEC_NOT_SUPPORTED);
    }

    WebPPicture wpic;
    if (!WebPPictureInit(&wpic)) {
      return Err(OM_CODEC_ENCODE_FAILED); // libwebp ABI mismatch
    }
    PictureGuard picture_guard {&wpic};

    wpic.width = static_cast<int>(pic.width);
    wpic.height = static_cast<int>(pic.height);

    switch (pic.format) {
      case OM_FORMAT_R8G8B8A8:
        wpic.use_argb = 1;
        if (!WebPPictureImportRGBA(&wpic, pic.planes.data[0],
                                   static_cast<int>(pic.planes.linesize[0]))) {
          return Err(OM_COMMON_OUT_OF_MEMORY);
        }
        break;
      case OM_FORMAT_B8G8R8A8:
        wpic.use_argb = 1;
        if (!WebPPictureImportBGRA(&wpic, pic.planes.data[0],
                                   static_cast<int>(pic.planes.linesize[0]))) {
          return Err(OM_COMMON_OUT_OF_MEMORY);
        }
        break;
      case OM_FORMAT_YUV420P:
        // Lossy WebP's native input: hand over the planes as they are and skip
        // a conversion. Under a lossless config libwebp converts to ARGB
        // itself, which is why this is not gated on config_.lossless.
        wpic.use_argb = 0;
        wpic.colorspace = WEBP_YUV420;
        wpic.y = pic.planes.data[0];
        wpic.u = pic.planes.data[1];
        wpic.v = pic.planes.data[2];
        wpic.y_stride = static_cast<int>(pic.planes.linesize[0]);
        wpic.uv_stride = static_cast<int>(pic.planes.linesize[1]);
        break;
      default:
        return Err(OM_CODEC_NOT_SUPPORTED);
    }

    WebPMemoryWriter writer;
    WebPMemoryWriterInit(&writer);
    WriterGuard writer_guard {&writer};
    wpic.writer = WebPMemoryWrite;
    wpic.custom_ptr = &writer;

    if (!WebPEncode(&config_, &wpic)) {
      switch (wpic.error_code) {
        case VP8_ENC_ERROR_OUT_OF_MEMORY:
        case VP8_ENC_ERROR_BITSTREAM_OUT_OF_MEMORY:
          return Err(OM_COMMON_OUT_OF_MEMORY);
        case VP8_ENC_ERROR_INVALID_CONFIGURATION:
        case VP8_ENC_ERROR_BAD_DIMENSION:
          return Err(OM_CODEC_INVALID_PARAMS);
        default:
          return Err(OM_CODEC_ENCODE_FAILED);
      }
    }

    Packet packet;
    packet.allocate(writer.size);
    std::memcpy(packet.bytes.data(), writer.mem, writer.size);
    packet.pts = frame.pts;
    packet.dts = frame.dts;
    packet.is_keyframe = true;

    packets.push_back(std::move(packet));
    return Ok(std::move(packets));
  }

  auto updateBitrate(const RateControlParams& rc) -> OMError override {
    (void) rc;
    // Every call encodes one self-contained still, so there is no running
    // bitrate to retarget.
    return OM_COMMON_NOT_SUPPORTED;
  }
};

auto createWEBPEncoder() -> std::unique_ptr<Encoder> {
  return std::make_unique<WEBPEncoder>();
}

} // namespace openmedia
