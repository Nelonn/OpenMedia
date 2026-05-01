#include <dav1d/dav1d.h>
#include <algorithm>
#include <codecs.hpp>
#include <cstring>
#include <openmedia/video.hpp>
#include <vector>
#include <util/io_util.hpp>

namespace openmedia {

struct Dav1dContextDeleter {
  void operator()(Dav1dContext* ctx) const {
    if (ctx) {
      dav1d_close(&ctx);
    }
  }
};

struct Dav1dPictureDeleter {
  void operator()(Dav1dPicture* pic) const {
    if (pic) {
      dav1d_picture_unref(pic);
      delete pic;
    }
  }
};

static void dav1d_log_callback(void* cookie, const char* format, va_list ap) {
  if (!format) return;
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
  log(OM_CATEGORY_DECODER, OM_LEVEL_INFO, message);
}

class Dav1dDecoder final : public Decoder {
  std::unique_ptr<Dav1dContext, Dav1dContextDeleter> ctx_;
  Dav1dSettings settings_ = {};
  bool initialized_ = false;
  VideoFormat output_format_ = {};

public:
  Dav1dDecoder() {}

  ~Dav1dDecoder() override = default;

  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_AV1) {
      return OM_CODEC_INVALID_PARAMS;
    }

    dav1d_default_settings(&settings_);
    settings_.n_threads = 1;
    settings_.max_frame_delay = 1;
    settings_.logger.cookie = nullptr;
    settings_.logger.callback = dav1d_log_callback;

    Dav1dContext* raw_ctx = nullptr;
    if (dav1d_open(&raw_ctx, &settings_) < 0) {
      return OM_CODEC_OPEN_FAILED;
    }

    ctx_.reset(raw_ctx);

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
      Dav1dData data;
      dav1d_data_wrap(&data, packet.bytes.data(), packet.bytes.size(),
                      [](const uint8_t*, void*) {}, nullptr);
      data.m.timestamp = packet.pts;

      int send_res = dav1d_send_data(ctx_.get(), &data);
      if (send_res < 0 && send_res != DAV1D_ERR(EAGAIN)) {
        return Err(OM_CODEC_DECODE_FAILED);
      }
    }

    Dav1dPicture pic = {};
    while (dav1d_get_picture(ctx_.get(), &pic) >= 0) {
      OMPixelFormat pixel_format;

      switch (pic.p.layout) {
        case DAV1D_PIXEL_LAYOUT_I400:
          pixel_format = (pic.p.bpc > 8) ? OM_FORMAT_GRAY16 : OM_FORMAT_GRAY8;
          break;
        case DAV1D_PIXEL_LAYOUT_I420:
          pixel_format = OM_FORMAT_YUV420P;
          break;
        case DAV1D_PIXEL_LAYOUT_I422:
          pixel_format = OM_FORMAT_YUV422P;
          break;
        case DAV1D_PIXEL_LAYOUT_I444:
          pixel_format = OM_FORMAT_YUV444P;
          break;
        default:
          pixel_format = OM_FORMAT_UNKNOWN;
          break;
      }

      if (pixel_format == OM_FORMAT_UNKNOWN) {
        return Err(OM_CODEC_DECODE_FAILED);
      }

      Picture out_pic(pixel_format, pic.p.w, pic.p.h);

      copyPlane(out_pic.planes.data[0], static_cast<const uint8_t*>(pic.data[0]),
                pic.p.w, pic.p.h, pic.stride[0]);

      if (pic.p.layout != DAV1D_PIXEL_LAYOUT_I400) {
        int chroma_w = (pic.p.layout == DAV1D_PIXEL_LAYOUT_I420 || pic.p.layout == DAV1D_PIXEL_LAYOUT_I422)
                           ? pic.p.w / 2
                           : pic.p.w;
        int chroma_h = (pic.p.layout == DAV1D_PIXEL_LAYOUT_I420)
                           ? pic.p.h / 2
                           : pic.p.h;

        copyPlane(out_pic.planes.data[1], static_cast<const uint8_t*>(pic.data[1]),
                  chroma_w, chroma_h, pic.stride[1]);
        copyPlane(out_pic.planes.data[2], static_cast<const uint8_t*>(pic.data[2]),
                  chroma_w, chroma_h, pic.stride[1]);
      }

      Frame frame = {};
      frame.pts = pic.m.timestamp;
      frame.dts = pic.m.timestamp;
      frame.data = std::move(out_pic);
      frames.push_back(std::move(frame));

      dav1d_picture_unref(&pic);
    }

    return Ok(std::move(frames));
  }

  void flush() override {
    if (ctx_) {
      dav1d_flush(ctx_.get());
    }
  }
};

const CodecDescriptor CODEC_DAV1D = {
    .codec_id = OM_CODEC_AV1,
    .type = OM_MEDIA_VIDEO,
    .name = "dav1d",
    .long_name = "dav1d",
    .vendor = "VideoLAN",
    .flags = NONE,
    .caps = CodecCaps {
        .profiles = {OM_PROFILE_AV1_MAIN, OM_PROFILE_AV1_HIGH, OM_PROFILE_AV1_PROFESSIONAL},
    },
    .decoder_factory = [] { return std::make_unique<Dav1dDecoder>(); },
};

} // namespace openmedia
