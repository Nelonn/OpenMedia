#include <vvdec/vvdec.h>
#include <algorithm>
#include <codecs.hpp>
#include <iostream>
#include <iostream>
#include <openmedia/video.hpp>
#include <util/io_util.hpp>
#include <vector>

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
  //Logger* logger = static_cast<Logger*>(opaque);
  va_list args_copy;
  va_copy(args_copy, ap);
  int required_size = std::vsnprintf(nullptr, 0, format, args_copy);
  va_end(args_copy);
  if (required_size <= 0) return;
  std::vector<char> buffer(static_cast<size_t>(required_size) + 1);
  std::vsnprintf(buffer.data(), buffer.size(), format, ap);
  std::string_view message(buffer.data(), static_cast<size_t>(required_size));
  std::cerr << "VVdec: " << message << std::endl;
  //logger->log(OM_CATEGORY_DECODER, OM_LEVEL_INFO, message);
}

static constexpr uint8_t ANNEX_B_START_CODE[] = {0x00, 0x00, 0x00, 0x01};
static constexpr size_t ANNEX_B_START_CODE_LEN = sizeof(ANNEX_B_START_CODE);

static auto isAnnexB(const uint8_t* data, size_t size) -> bool {
  if (size < 4) return false;
  return data[0] == 0x00 && data[1] == 0x00 &&
         (data[2] == 0x01 ||
          (data[2] == 0x00 && data[3] == 0x01));
}

static auto convertToAnnexB(const uint8_t* data, size_t size) -> std::vector<uint8_t> {
  std::vector<uint8_t> out;
  out.reserve(size + 16);

  size_t offset = 0;
  while (offset + 4 <= size) {
    uint32_t nal_size = (static_cast<uint32_t>(data[offset]) << 24) |
                        (static_cast<uint32_t>(data[offset + 1]) << 16) |
                        (static_cast<uint32_t>(data[offset + 2]) << 8) |
                        (static_cast<uint32_t>(data[offset + 3]));
    offset += 4;

    if (nal_size == 0 || offset + nal_size > size) break;

    out.insert(out.end(), ANNEX_B_START_CODE,
               ANNEX_B_START_CODE + ANNEX_B_START_CODE_LEN);
    out.insert(out.end(), data + offset, data + offset + nal_size);
    offset += nal_size;
  }
  return out;
}

class VvdecDecoder final : public Decoder {
  std::unique_ptr<vvdecDecoder, VvdecDecoderDeleter> ctx_;
  std::unique_ptr<vvdecParams, VvdecParamsDeleter> params_;
  std::unique_ptr<vvdecAccessUnit, VvdecAccessUnitDeleter> access_unit_;
  LoggerRef logger_ = {};
  bool initialized_ = false;
  VideoFormat output_format_ = {};
  bool extradata_sent_ = false;
  std::vector<uint8_t> extradata_;

public:
  VvdecDecoder() {}
  ~VvdecDecoder() override = default;

  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_H266) {
      return OM_CODEC_INVALID_PARAMS;
    }

    logger_ = options.logger ? options.logger : Logger::refDefault();

    params_.reset(vvdec_params_alloc());
    if (!params_) return OM_CODEC_OPEN_FAILED;
    vvdec_params_default(params_.get());

    params_->threads = 1;
    params_->logLevel = VVDEC_INFO;
    params_->filmGrainSynthesis = true;
    params_->errHandlingFlags = VVDEC_ERR_HANDLING_TRY_CONTINUE;
    params_->opaque = logger_.get();

    vvdecDecoder* raw_ctx = vvdec_decoder_open(params_.get());
    if (!raw_ctx) return OM_CODEC_OPEN_FAILED;
    ctx_.reset(raw_ctx);

    vvdec_set_logging_callback(raw_ctx, vvdec_log_callback);

    access_unit_.reset(vvdec_accessUnit_alloc());
    if (!access_unit_) return OM_CODEC_OPEN_FAILED;
    vvdec_accessUnit_default(access_unit_.get());
    vvdec_accessUnit_alloc_payload(access_unit_.get(), 1024 * 1024);

    if (!options.extradata.empty()) {
      const uint8_t* ed = options.extradata.data();
      size_t ed_size = options.extradata.size();

      if (isAnnexB(ed, ed_size)) {
        extradata_.assign(ed, ed + ed_size);
      } else {
        extradata_ = convertToAnnexB(ed, ed_size);
      }
    }

    if (!extradata_.empty()) {
      std::vector<Frame> dummy;
      auto err = sendAccessUnit(extradata_.data(), extradata_.size(),
                                -1, -1, /*is_rap=*/true, dummy);
      if (err != OM_SUCCESS) return OM_CODEC_OPEN_FAILED;
    }

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
      const uint8_t* raw = packet.bytes.data();
      size_t raw_size = packet.bytes.size();

      std::vector<uint8_t> converted;
      if (!isAnnexB(raw, raw_size)) {
        converted = convertToAnnexB(raw, raw_size);
        raw = converted.data();
        raw_size = converted.size();
      }

      auto err = sendAccessUnit(raw, raw_size, packet.pts, packet.dts, packet.is_keyframe, frames);
      if (err != OM_SUCCESS) {
        return Err(err);
      }
    } else {
      vvdecFrame* frame = nullptr;
      int res;
      while ((res = vvdec_flush(ctx_.get(), &frame)) == VVDEC_OK ||
             res == VVDEC_EOF) {
        if (frame != nullptr) {
          if (auto result = processFrame(frame)) {
            frames.push_back(std::move(*result));
          } else {
            return Err(OM_CODEC_DECODE_FAILED);
          }
        }
        if (res == VVDEC_EOF) break;
      }
    }

    return Ok(std::move(frames));
  }

  void flush() override {
  }

private:
  // Send one normalised Annex-B buffer to vvdec, collect any resulting frame.
  auto sendAccessUnit(const uint8_t* data, size_t size,
                      int64_t pts, int64_t dts,
                      bool is_rap,
                      std::vector<Frame>& out_frames) -> OMError {

    if (size > static_cast<size_t>(access_unit_->payloadSize)) {
      vvdec_accessUnit_alloc_payload(access_unit_.get(),
                                     static_cast<int>(size));
    }

    std::memcpy(access_unit_->payload, data, size);
    access_unit_->payloadUsedSize = static_cast<int>(size);
    access_unit_->cts = static_cast<uint64_t>(pts);
    access_unit_->dts = static_cast<uint64_t>(dts);
    access_unit_->ctsValid = (pts >= 0);
    access_unit_->dtsValid = (dts >= 0);
    access_unit_->rap = is_rap;

    vvdecFrame* frame = nullptr;
    int res = vvdec_decode(ctx_.get(), access_unit_.get(), &frame);

    if (res == VVDEC_OK) {
      // Decoder produced a frame (may be nullptr for parameter sets).
      if (frame != nullptr) {
        if (auto result = processFrame(frame)) {
          out_frames.push_back(std::move(*result));
        } else {
          return OM_CODEC_DECODE_FAILED;
        }
      }
    } else if (res == VVDEC_TRY_AGAIN) {
      // Decoder buffering; not an error - just no output yet.
    } else if (res == VVDEC_EOF) {
      // End of stream signalled during normal decode; drain remaining frames.
      while (frame != nullptr) {
        if (auto result = processFrame(frame)) {
          out_frames.push_back(std::move(*result));
        } else {
          return OM_CODEC_DECODE_FAILED;
        }
        frame = nullptr;
        vvdec_flush(ctx_.get(), &frame);
      }
    } else {
      return OM_CODEC_DECODE_FAILED;
    }

    return OM_SUCCESS;
  }

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

    copyPlane(out_pic.planes.data[0], frame->planes[0].ptr,
              frame->width * frame->planes[0].bytesPerSample,
              frame->height, frame->planes[0].stride);

    if (frame->colorFormat != VVDEC_CF_YUV400_PLANAR) {
      uint32_t chroma_w = frame->planes[1].width;
      uint32_t chroma_h = frame->planes[1].height;

      copyPlane(out_pic.planes.data[1], frame->planes[1].ptr,
                chroma_w * frame->planes[1].bytesPerSample,
                chroma_h, frame->planes[1].stride);
      copyPlane(out_pic.planes.data[2], frame->planes[2].ptr,
                chroma_w * frame->planes[2].bytesPerSample,
                chroma_h, frame->planes[2].stride);
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
