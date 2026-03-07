#include <vvdec/vvdec.h>
#include <algorithm>
#include <codecs.hpp>
#include <cstring>
#include <openmedia/video.hpp>
#include <vector>
#include <util/io_util.hpp>

namespace openmedia {

struct VvdecDecoderDeleter {
  void operator()(vvdecDecoder* ctx) const {
    if (ctx) {
      vvdec_decoder_close(ctx);
    }
  }
};

struct VvdecParamsDeleter {
  void operator()(vvdecParams* params) const {
    if (params) {
      vvdec_params_free(params);
    }
  }
};

struct VvdecAccessUnitDeleter {
  void operator()(vvdecAccessUnit* au) const {
    if (au) {
      vvdec_accessUnit_free(au);
    }
  }
};

static void vvdec_log_callback(void* opaque, int level, const char* format, va_list ap) {
  if (!opaque || !format) return;
  Logger* logger = static_cast<Logger*>(opaque);
  va_list args_copy;
  va_copy(args_copy, ap);
  int required_size = std::vsnprintf(nullptr, 0, format, args_copy);
  va_end(args_copy);
  if (required_size <= 0) {
    return;
  }
  std::vector<char> buffer(static_cast<size_t>(required_size) + 1);
  std::vsnprintf(buffer.data(), buffer.size(), format, ap);
  std::string_view message(buffer.data(), static_cast<size_t>(required_size));
  logger->log(OM_CATEGORY_DECODER, OM_LEVEL_INFO, message);
}

class VvdecDecoder final : public Decoder {
  std::unique_ptr<vvdecDecoder, VvdecDecoderDeleter> ctx_;
  std::unique_ptr<vvdecParams, VvdecParamsDeleter> params_;
  std::unique_ptr<vvdecAccessUnit, VvdecAccessUnitDeleter> accessUnit_;
  LoggerRef logger_ = {};
  bool initialized_ = false;
  VideoFormat output_format_ = {};

public:
  VvdecDecoder() {}

  ~VvdecDecoder() override = default;

  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_H266) {
      return OM_CODEC_INVALID_PARAMS;
    }

    logger_ = options.logger ? options.logger : Logger::refDefault();

    // Allocate and initialize decoder parameters
    params_.reset(vvdec_params_alloc());
    if (!params_) {
      return OM_CODEC_OPEN_FAILED;
    }
    vvdec_params_default(params_.get());

    // Configure decoder settings
    params_->threads = 1;
    params_->logLevel = VVDEC_INFO;
    params_->filmGrainSynthesis = true;
    params_->errHandlingFlags = VVDEC_ERR_HANDLING_TRY_CONTINUE;
    params_->opaque = logger_.get();

    // Open decoder
    vvdecDecoder* raw_ctx = vvdec_decoder_open(params_.get());
    if (!raw_ctx) {
      return OM_CODEC_OPEN_FAILED;
    }
    ctx_.reset(raw_ctx);

    // Set logging callback
    vvdec_set_logging_callback(raw_ctx, [](void* opaque, int level, const char* format, va_list ap) {
      //vvdec_log_callback(opaque, level, format, ap);
    });

    // Allocate access unit for input
    accessUnit_.reset(vvdec_accessUnit_alloc());
    if (!accessUnit_) {
      return OM_CODEC_OPEN_FAILED;
    }
    vvdec_accessUnit_default(accessUnit_.get());

    // Allocate payload buffer (1MB should be enough for most NAL units)
    vvdec_accessUnit_alloc_payload(accessUnit_.get(), 1024 * 1024);

    output_format_.width = options.format.video.width;
    output_format_.height = options.format.video.height;
    output_format_.format = OM_FORMAT_YUV420P;
    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!initialized_) return std::nullopt;

    DecodingInfo info = {};
    info.media_type = OM_MEDIA_VIDEO;
    info.video_format = output_format_;
    return info;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    std::vector<Frame> frames;

    if (!packet.bytes.empty()) {
      // Copy packet data to access unit payload
      if (static_cast<size_t>(packet.bytes.size()) > static_cast<size_t>(accessUnit_->payloadSize)) {
        // Reallocate if needed
        vvdec_accessUnit_alloc_payload(accessUnit_.get(), static_cast<int>(packet.bytes.size()));
      }

      std::memcpy(accessUnit_->payload, packet.bytes.data(), packet.bytes.size());
      accessUnit_->payloadUsedSize = static_cast<int>(packet.bytes.size());
      accessUnit_->cts = static_cast<uint64_t>(packet.pts);
      accessUnit_->dts = static_cast<uint64_t>(packet.dts);
      accessUnit_->ctsValid = (packet.pts >= 0);
      accessUnit_->dtsValid = (packet.dts >= 0);

      // Decode and get frame
      vvdecFrame* frame = nullptr;
      int res = vvdec_decode(ctx_.get(), accessUnit_.get(), &frame);
      if (res == VVDEC_OK && frame != nullptr) {
        if (auto result = processFrame(frame)) {
          frames.push_back(std::move(*result));
        } else {
          return Err(OM_CODEC_DECODE_FAILED);
        }
      } else if (res != VVDEC_OK && res != VVDEC_TRY_AGAIN) {
        return Err(OM_CODEC_DECODE_FAILED);
      }
    } else {
      // No input data - flush decoder to get remaining frames
      vvdecFrame* frame = nullptr;
      while (vvdec_flush(ctx_.get(), &frame) == VVDEC_OK && frame != nullptr) {
        if (auto result = processFrame(frame)) {
          frames.push_back(std::move(*result));
        } else {
          return Err(OM_CODEC_DECODE_FAILED);
        }
      }
    }

    return Ok(std::move(frames));
  }

  void flush() override {
    // Flushing is handled in decode() when called with empty packet
  }

private:
  auto processFrame(vvdecFrame* frame) -> std::optional<Frame> {
    OMPixelFormat pixel_format;

    switch (frame->colorFormat) {
      case VVDEC_CF_YUV400_PLANAR:
        pixel_format = (frame->bitDepth > 8) ? OM_FORMAT_GRAY16 : OM_FORMAT_GRAY8;
        break;
      case VVDEC_CF_YUV420_PLANAR:
        pixel_format = (frame->bitDepth > 8) ? OM_FORMAT_YUV420P10 : OM_FORMAT_YUV420P;
        break;
      case VVDEC_CF_YUV422_PLANAR:
        pixel_format = (frame->bitDepth > 8) ? OM_FORMAT_YUV422P10 : OM_FORMAT_YUV422P;
        break;
      case VVDEC_CF_YUV444_PLANAR:
        pixel_format = (frame->bitDepth > 8) ? OM_FORMAT_YUV444P10 : OM_FORMAT_YUV444P;
        break;
      default:
        pixel_format = OM_FORMAT_UNKNOWN;
        break;
    }

    if (pixel_format == OM_FORMAT_UNKNOWN) {
      vvdec_frame_unref(ctx_.get(), frame);
      return std::nullopt;
    }

    Picture out_pic(pixel_format, frame->width, frame->height);

    // Copy Y plane
    copyPlane(out_pic.planes.data[0], frame->planes[0].ptr,
              frame->width * frame->planes[0].bytesPerSample, frame->height, frame->planes[0].stride);

    // Copy U and V planes if not grayscale
    if (frame->colorFormat != VVDEC_CF_YUV400_PLANAR) {
      uint32_t chroma_w = frame->planes[1].width;
      uint32_t chroma_h = frame->planes[1].height;

      copyPlane(out_pic.planes.data[1], frame->planes[1].ptr,
                chroma_w * frame->planes[1].bytesPerSample, chroma_h, frame->planes[1].stride);
      copyPlane(out_pic.planes.data[2], frame->planes[2].ptr,
                chroma_w * frame->planes[2].bytesPerSample, chroma_h, frame->planes[2].stride);
    }

    Frame out_frame = {};
    out_frame.pts = frame->ctsValid ? static_cast<int64_t>(frame->cts) : -1;
    out_frame.data = std::move(out_pic);

    vvdec_frame_unref(ctx_.get(), frame);
    return out_frame;
  }
};

const CodecDescriptor CODEC_VVDEC = {
    .codec_id = OM_CODEC_H266,
    .type = OM_MEDIA_VIDEO,
    .name = "vvdec",
    .long_name = "Fraunhofer VVdeC",
    .vendor = "Fraunhofer HHI",
    .flags = NONE,
    .caps = CodecCaps {
        .profiles = {OM_PROFILE_H266_MAIN_10, OM_PROFILE_H266_MAIN_10_444},
    },
    .decoder_factory = [] { return std::make_unique<VvdecDecoder>(); },
};

} // namespace openmedia
