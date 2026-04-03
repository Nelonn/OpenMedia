#include <xevd.h>
#include <algorithm>
#include <codecs.hpp>
#include <cstring>
#include <openmedia/video.hpp>
#include <vector>
#include <util/io_util.hpp>

namespace openmedia {

struct XevdDecoderDeleter {
  void operator()(XEVD ctx) const {
    if (ctx) {
      xevd_delete(ctx);
    }
  }
};

static auto xevdColorSpaceToPixelFormat(int cs) -> OMPixelFormat {
  int format = XEVD_CS_GET_FORMAT(cs);
  int bit_depth = XEVD_CS_GET_BIT_DEPTH(cs);

  switch (format) {
    case XEVD_CF_YCBCR400:
      return (bit_depth > 8) ? OM_FORMAT_GRAY16 : OM_FORMAT_GRAY8;
    case XEVD_CF_YCBCR420:
      if (bit_depth <= 8) return OM_FORMAT_YUV420P;
      if (bit_depth <= 10) return OM_FORMAT_YUV420P10;
      if (bit_depth <= 12) return OM_FORMAT_YUV420P12;
      return OM_FORMAT_YUV420P16;
    case XEVD_CF_YCBCR422:
    case XEVD_CF_YCBCR422W:
      if (bit_depth <= 8) return OM_FORMAT_YUV422P;
      if (bit_depth <= 10) return OM_FORMAT_YUV422P10;
      if (bit_depth <= 12) return OM_FORMAT_YUV422P12;
      return OM_FORMAT_YUV422P16;
    case XEVD_CF_YCBCR444:
      if (bit_depth <= 8) return OM_FORMAT_YUV444P;
      if (bit_depth <= 10) return OM_FORMAT_YUV444P10;
      if (bit_depth <= 12) return OM_FORMAT_YUV444P12;
      return OM_FORMAT_YUV444P16;
    default:
      return OM_FORMAT_UNKNOWN;
  }
}

class XevdDecoder final : public Decoder {
  std::unique_ptr<void, XevdDecoderDeleter> ctx_;
  LoggerRef logger_ = {};
  bool initialized_ = false;
  VideoFormat output_format_ = {};
  int output_cs_ = 0;

public:
  XevdDecoder() {}

  ~XevdDecoder() override = default;

  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_EVC) {
      return OM_CODEC_INVALID_PARAMS;
    }

    logger_ = options.logger ? options.logger : Logger::refDefault();

    XEVD_CDSC cdsc = {};
    cdsc.threads = 1;

    int err = 0;
    XEVD raw_ctx = xevd_create(&cdsc, &err);
    if (!raw_ctx || XEVD_FAILED(err)) {
      return OM_CODEC_OPEN_FAILED;
    }
    ctx_.reset(raw_ctx);

    output_format_.width = options.format.video.width;
    output_format_.height = options.format.video.height;
    output_format_.format = OM_FORMAT_YUV420P;
    output_cs_ = XEVD_CS_YCBCR420;
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
      XEVD_BITB bitb = {};
      bitb.addr = packet.bytes.data();
      bitb.bsize = static_cast<int>(packet.bytes.size());
      bitb.ssize = static_cast<int>(packet.bytes.size());
      bitb.ts[XEVD_TS_PTS] = static_cast<XEVD_MTIME>(packet.pts);
      bitb.ts[XEVD_TS_DTS] = static_cast<XEVD_MTIME>(packet.dts);

      XEVD_STAT stat = {};
      int ret = xevd_decode(ctx_.get(), &bitb, &stat);
      if (XEVD_FAILED(ret)) {
        return Err(OM_CODEC_DECODE_FAILED);
      }
    }

    while (true) {
      XEVD_IMGB* img = nullptr;
      int ret = xevd_pull(ctx_.get(), &img);
      if (XEVD_FAILED(ret)) {
        break;
      }
      if (ret == XEVD_OK_NO_MORE_FRM || !img) {
        break;
      }

      OMPixelFormat pixel_format = xevdColorSpaceToPixelFormat(img->cs);
      if (pixel_format == OM_FORMAT_UNKNOWN) {
        img->release(img);
        return Err(OM_CODEC_DECODE_FAILED);
      }

      int width = img->w[0];
      int height = img->h[0];

      if (output_format_.width == 0 || output_format_.height == 0) {
        output_format_.width = static_cast<uint32_t>(width);
        output_format_.height = static_cast<uint32_t>(height);
        output_format_.format = pixel_format;
        output_cs_ = img->cs;
      }

      Picture out_pic(pixel_format, static_cast<uint32_t>(width), static_cast<uint32_t>(height));

      int num_planes = img->np;
      for (int i = 0; i < num_planes; ++i) {
        int plane_width = img->w[i];
        int plane_height = img->h[i];
        int stride = img->s[i];
        int byte_depth = XEVD_CS_GET_BYTE_DEPTH(img->cs);
        int bytes_per_pixel = (i == 0 || XEVD_CS_GET_FORMAT(img->cs) == XEVD_CF_YCBCR400) ? byte_depth : byte_depth;

        uint8_t* src = static_cast<uint8_t*>(img->a[i]);
        uint8_t* dst = out_pic.planes.data[i];
        int dst_stride = out_pic.planes.linesize[i];

        for (int y = 0; y < plane_height; ++y) {
          std::memcpy(dst + y * dst_stride, src + y * stride, plane_width * bytes_per_pixel);
        }
      }

      Frame frame = {};
      frame.pts = img->ts[XEVD_TS_PTS];
      frame.dts = img->ts[XEVD_TS_DTS];
      frame.data = std::move(out_pic);
      frames.push_back(std::move(frame));

      img->release(img);
    }

    return Ok(std::move(frames));
  }

  void flush() override {}
};

const CodecDescriptor CODEC_XEVD = {
    .codec_id = OM_CODEC_EVC,
    .type = OM_MEDIA_VIDEO,
    .name = "xevd",
    .long_name = "eXtra-fast Essential Video Decoder",
    .vendor = "Samsung Electronics / MPEG-5",
    .flags = NONE,
    .caps = CodecCaps {
        .profiles = {},
    },
    .decoder_factory = [] { return std::make_unique<XevdDecoder>(); },
};

} // namespace openmedia
