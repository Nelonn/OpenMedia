#include <xeve.h>
#include <algorithm>
#include <codecs.hpp>
#include <openmedia/video.hpp>
#include <vector>
#include <util/io_util.hpp>

namespace openmedia {

struct XeveEncoderDeleter {
  void operator()(XEVE ctx) const {
    if (ctx) {
      xeve_delete(ctx);
    }
  }
};

static auto pixelFormatToXeveColorSpace(OMPixelFormat fmt) -> int {
  switch (fmt) {
    case OM_FORMAT_YUV420P:
      return XEVE_CS_YCBCR420;
    case OM_FORMAT_YUV420P10:
      return XEVE_CS_YCBCR420_10LE;
    case OM_FORMAT_YUV420P12:
      return XEVE_CS_YCBCR420_12LE;
    case OM_FORMAT_GRAY8:
      return XEVE_CS_YCBCR400;
    case OM_FORMAT_GRAY16:
      return XEVE_CS_YCBCR400_10LE;
    default:
      return XEVE_CS_UNKNOWN;
  }
}

static auto setupImageBuffer(XEVE_IMGB* imgb, const Picture& pic, int cs) -> void {
  memset(imgb, 0, sizeof(XEVE_IMGB));
  imgb->cs = cs;
  imgb->np = static_cast<int>(pic.planes.count);

  for (int i = 0; i < imgb->np; ++i) {
    auto [plane_width, plane_height] = pic.getPlaneDimensions(i);
    imgb->a[i] = pic.planes.data[i];
    imgb->s[i] = static_cast<int>(pic.planes.linesize[i]);
    imgb->w[i] = static_cast<int>(plane_width);
    imgb->h[i] = static_cast<int>(plane_height);
    imgb->aw[i] = static_cast<int>(plane_width);
    imgb->ah[i] = static_cast<int>(plane_height);
    imgb->x[i] = 0;
    imgb->y[i] = 0;
    imgb->baddr[i] = pic.planes.data[i];
    imgb->bsize[i] = static_cast<int>(pic.planes.linesize[i] * plane_height);
  }
}

class XeveEncoder final : public Encoder {
  std::unique_ptr<void, XeveEncoderDeleter> ctx_;
  LoggerRef logger_ = {};
  bool initialized_ = false;
  VideoFormat input_format_ = {};
  int input_cs_ = 0;
  std::vector<uint8_t> bitstream_buffer_;
  std::vector<uint8_t> extradata_;
  int64_t frame_count_ = 0;
  bool flush_requested_ = false;

public:
  XeveEncoder() {}

  ~XeveEncoder() override = default;

  auto configure(const EncoderOptions& options) -> OMError override {
    if (options.format.type != OM_MEDIA_VIDEO) {
      return OM_CODEC_INVALID_PARAMS;
    }

    logger_ = options.logger ? options.logger : Logger::refDefault();

    input_format_.width = options.format.video.width;
    input_format_.height = options.format.video.height;

    OMPixelFormat pixel_format = input_format_.format;
    int cs = pixelFormatToXeveColorSpace(pixel_format);
    if (cs == XEVE_CS_UNKNOWN) {
      return OM_CODEC_INVALID_PARAMS;
    }
    input_cs_ = cs;

    XEVE_CDSC cdsc = {};
    cdsc.max_bs_buf_size = 10 * 1000 * 1000; // 10MB max bitstream buffer

    xeve_param_default(&cdsc.param);

    cdsc.param.w = static_cast<int>(options.format.video.width);
    cdsc.param.h = static_cast<int>(options.format.video.height);
    cdsc.param.cs = cs;
    cdsc.param.fps.num = options.format.video.framerate.num > 0 ? options.format.video.framerate.num : 30;
    cdsc.param.fps.den = options.format.video.framerate.den > 0 ? options.format.video.framerate.den : 1;
    cdsc.param.threads = 1;
    cdsc.param.profile = XEVE_PROFILE_BASELINE;

    const auto& rc = options.rate_control;
    auto mode = rc.getMode();
    switch (mode) {
      case RateControlMode::CRF: {
        cdsc.param.rc_type = XEVE_RC_CRF;
        if (std::holds_alternative<CrfParams>(rc.params)) {
          cdsc.param.crf = static_cast<int>(std::get<CrfParams>(rc.params).quality);
        } else {
          cdsc.param.crf = 32; // default CRF
        }
        break;
      }
      case RateControlMode::CQP: {
        cdsc.param.rc_type = XEVE_RC_CQP;
        if (std::holds_alternative<CqpParams>(rc.params)) {
          cdsc.param.qp = std::get<CqpParams>(rc.params).qp_i;
        } else {
          cdsc.param.qp = 32;
        }
        break;
      }
      case RateControlMode::ABR:
      case RateControlMode::CBR:
      case RateControlMode::VBR: {
        cdsc.param.rc_type = XEVE_RC_ABR;
        int64_t bitrate = 0;
        if (mode == RateControlMode::ABR && std::holds_alternative<AbrParams>(rc.params)) {
          bitrate = std::get<AbrParams>(rc.params).target_bitrate;
        } else if (mode == RateControlMode::CBR && std::holds_alternative<CbrParams>(rc.params)) {
          bitrate = std::get<CbrParams>(rc.params).bitrate.target_bitrate;
        } else if (mode == RateControlMode::VBR && std::holds_alternative<VbrParams>(rc.params)) {
          bitrate = std::get<VbrParams>(rc.params).bitrate.target_bitrate;
        }
        cdsc.param.bitrate = static_cast<int>(bitrate / 1000); // convert to kbps
        if (cdsc.param.bitrate == 0) {
          cdsc.param.bitrate = 5000; // default 5 Mbps
        }
        break;
      }
      default:
        cdsc.param.rc_type = XEVE_RC_CRF;
        cdsc.param.crf = 32;
        break;
    }

    if (rc.min_qp.has_value()) {
      cdsc.param.qp_min = rc.min_qp.value();
    }
    if (rc.max_qp.has_value()) {
      cdsc.param.qp_max = rc.max_qp.value();
    }

    const auto& extra = options.extra;

    if (extra.contains("xeve_preset")) {
      int32_t preset = extra.getInt32("xeve_preset");
      int profile = cdsc.param.profile;
      int tune = XEVE_TUNE_NONE;
      xeve_param_ppt(&cdsc.param, profile, preset, tune);
    }

    if (extra.contains("xeve_tune")) {
      int32_t tune = extra.getInt32("xeve_tune");
      xeve_param_ppt(&cdsc.param, cdsc.param.profile, XEVE_PRESET_DEFAULT, tune);
    }

    if (extra.contains("xeve_keyint")) {
      cdsc.param.keyint = extra.getInt32("xeve_keyint");
    }

    if (extra.contains("xeve_bframes")) {
      cdsc.param.bframes = extra.getInt32("xeve_bframes");
    }

    if (extra.contains("xeve_use_annexb")) {
      cdsc.param.use_annexb = extra.getInt32("xeve_use_annexb") != 0;
    }

    int err = 0;
    XEVE raw_ctx = xeve_create(&cdsc, &err);
    if (!raw_ctx || XEVE_FAILED(err)) {
      return OM_CODEC_OPEN_FAILED;
    }
    ctx_.reset(raw_ctx);

    bitstream_buffer_.resize(cdsc.max_bs_buf_size);

    extradata_.clear();

    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> EncodingInfo override {
    EncodingInfo info = {};
    info.extradata = extradata_;
    return info;
  }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    std::vector<Packet> packets;

    if (const auto* pic = std::get_if<Picture>(&frame.data)) {
      XEVE_IMGB imgb = {};
      setupImageBuffer(&imgb, *pic, input_cs_);
      imgb.ts[XEVE_TS_PTS] = static_cast<XEVE_MTIME>(frame.pts);
      imgb.ts[XEVE_TS_DTS] = static_cast<XEVE_MTIME>(frame.dts);

      int ret = xeve_push(ctx_.get(), &imgb);
      if (XEVE_FAILED(ret)) {
        return Err(OM_CODEC_ENCODE_FAILED);
      }
    } else if (flush_requested_) {
      // Push NULL frame to signal end of stream
      int ret = xeve_push(ctx_.get(), nullptr);
      if (XEVE_FAILED(ret)) {
        return Err(OM_CODEC_ENCODE_FAILED);
      }
      flush_requested_ = false;
    }

    XEVE_BITB bitb = {};
    bitb.addr = bitstream_buffer_.data();
    bitb.bsize = static_cast<int>(bitstream_buffer_.size());

    XEVE_STAT stat = {};
    int ret = xeve_encode(ctx_.get(), &bitb, &stat);
    if (XEVE_FAILED(ret)) {
      return Err(OM_CODEC_ENCODE_FAILED);
    }

    if (stat.write > 0) {
      Packet packet = {};
      packet.allocate(stat.write);
      memcpy(packet.bytes.data(), bitstream_buffer_.data(), stat.write);
      packet.pts = bitb.ts[XEVE_TS_PTS];
      packet.dts = bitb.ts[XEVE_TS_DTS];

      // Capture extradata from first frame (contains SPS/PPS)
      if (extradata_.empty() && stat.nalu_type == XEVE_SPS_NUT) {
        extradata_.assign(packet.bytes.begin(), packet.bytes.end());
      }

      packets.push_back(std::move(packet));
      frame_count_++;
    }

    return Ok(std::move(packets));
  }

  /*void flush() override {
    flush_requested_ = true;
  }*/

  auto updateBitrate(const RateControlParams& rc) -> OMError override {
    if (!initialized_) {
      return OM_CODEC_INVALID_PARAMS;
    }

    auto mode = rc.getMode();
    if (mode == RateControlMode::ABR || mode == RateControlMode::CBR || mode == RateControlMode::VBR) {
      int64_t bitrate = 0;
      if (mode == RateControlMode::ABR && std::holds_alternative<AbrParams>(rc.params)) {
        bitrate = std::get<AbrParams>(rc.params).target_bitrate;
      } else if (mode == RateControlMode::CBR && std::holds_alternative<CbrParams>(rc.params)) {
        bitrate = std::get<CbrParams>(rc.params).bitrate.target_bitrate;
      } else if (mode == RateControlMode::VBR && std::holds_alternative<VbrParams>(rc.params)) {
        bitrate = std::get<VbrParams>(rc.params).bitrate.target_bitrate;
      }

      int bitrate_kbps = static_cast<int>(bitrate / 1000);
      int size = sizeof(bitrate_kbps);
      int ret = xeve_config(ctx_.get(), XEVE_CFG_SET_BPS, &bitrate_kbps, &size);
      if (XEVE_FAILED(ret)) {
        return OM_CODEC_ENCODE_FAILED;
      }
    }

    return OM_SUCCESS;
  }
};

const CodecDescriptor CODEC_XEVE = {
    .codec_id = OM_CODEC_EVC,
    .type = OM_MEDIA_VIDEO,
    .name = "xeve",
    .long_name = "eXtra-fast Essential Video Encoder",
    .vendor = "Samsung Electronics / MPEG-5",
    .flags = NONE,
    .caps = CodecCaps {
        .profiles = {},
    },
    .encoder_factory = [] { return std::make_unique<XeveEncoder>(); },
};

} // namespace openmedia
