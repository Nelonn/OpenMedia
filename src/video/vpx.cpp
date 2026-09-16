#include <vpx/vp8cx.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vpx_encoder.h>
#include <vpx/vpx_image.h>
#include <algorithm>
#include <codecs.hpp>
#include <cstring>
#include <memory>
#include <openmedia/video.hpp>
#include <util/io_util.hpp>
#include <vector>

namespace openmedia {

class VpxDecoder final : public Decoder {
  vpx_codec_ctx_t ctx_ = {};
  bool initialized_ = false;
  OMCodecId codec_id_;
  VideoFormat output_format_ = {};

public:
  explicit VpxDecoder(OMCodecId codec_id)
      : codec_id_(codec_id) {}

  ~VpxDecoder() override {
    if (initialized_) {
      vpx_codec_destroy(&ctx_);
    }
  }

  auto configure(const DecoderOptions& options) -> OMError override {
    if (initialized_) {
      vpx_codec_destroy(&ctx_);
      initialized_ = false;
    }

    vpx_codec_iface_t* iface = nullptr;
    if (codec_id_ == OM_CODEC_VP8) {
      iface = vpx_codec_vp8_dx();
    } else if (codec_id_ == OM_CODEC_VP9) {
      iface = vpx_codec_vp9_dx();
    } else {
      return OM_CODEC_INVALID_PARAMS;
    }

    vpx_codec_dec_cfg_t cfg = {};
    cfg.threads = 1; // Default to 1, can be tuned via extra options if needed
    cfg.w = options.format.video.width;
    cfg.h = options.format.video.height;

    if (vpx_codec_dec_init(&ctx_, iface, &cfg, 0) != VPX_CODEC_OK) {
      return OM_CODEC_OPEN_FAILED;
    }

    output_format_.width = options.format.video.width;
    output_format_.height = options.format.video.height;
    output_format_.color_space = options.format.video.color_space;
    output_format_.transfer_char = options.format.video.transfer_char;
    output_format_.color_primaries = options.format.video.color_primaries;
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
    if (!initialized_) return Err(OM_CODEC_DECODE_FAILED);

    const uint8_t* data = packet.bytes.empty() ? nullptr : packet.bytes.data();
    const size_t size = packet.bytes.size();

    if (vpx_codec_decode(&ctx_, data, static_cast<unsigned int>(size), nullptr, 0) != VPX_CODEC_OK) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    std::vector<Frame> frames;
    vpx_codec_iter_t iter = nullptr;
    vpx_image_t* img = nullptr;

    while ((img = vpx_codec_get_frame(&ctx_, &iter)) != nullptr) {
      OMPixelFormat fmt = OM_FORMAT_UNKNOWN;
      if (img->fmt == VPX_IMG_FMT_I420) {
        fmt = OM_FORMAT_YUV420P;
      } else if (img->fmt == VPX_IMG_FMT_I422) {
        fmt = OM_FORMAT_YUV422P;
      } else if (img->fmt == VPX_IMG_FMT_I444) {
        fmt = OM_FORMAT_YUV444P;
      } else if (img->fmt == VPX_IMG_FMT_I42016) {
        fmt = OM_FORMAT_YUV420P10;
      } else if (img->fmt == VPX_IMG_FMT_I42216) {
        fmt = OM_FORMAT_YUV422P10;
      } else if (img->fmt == VPX_IMG_FMT_I44416) {
        fmt = OM_FORMAT_YUV444P10;
      }

      if (fmt == OM_FORMAT_UNKNOWN) continue;

      Picture pic(fmt, img->d_w, img->d_h);

      switch (img->cs) {
        case VPX_CS_BT_601: pic.color_space = OM_COLOR_SPACE_BT601; break;
        case VPX_CS_BT_709: pic.color_space = OM_COLOR_SPACE_BT709; break;
        case VPX_CS_SMPTE_170: pic.color_space = OM_COLOR_SPACE_BT601; break;
        case VPX_CS_SMPTE_240: pic.color_space = OM_COLOR_SPACE_SMPTE240M; break;
        case VPX_CS_BT_2020: pic.color_space = OM_COLOR_SPACE_BT2020; break;
        case VPX_CS_SRGB: pic.color_space = OM_COLOR_SPACE_RGB; break;
        default: pic.color_space = OM_COLOR_SPACE_UNKNOWN; break;
      }

      for (int i = 0; i < 3; ++i) {
        auto dims = pic.getPlaneDimensions(i);
        const uint32_t row_bytes = dims.first * getBytesPerPixel(fmt, i);
        copyPlane(pic.planes.data[i], pic.planes.linesize[i],
                  img->planes[i], img->stride[i],
                  row_bytes, dims.second);
      }

      Frame frame = {};
      frame.pts = packet.pts;
      frame.dts = packet.dts;
      frame.data = std::move(pic);
      frames.push_back(std::move(frame));
    }

    return Ok(std::move(frames));
  }

  void flush() override {
    if (initialized_) {
      vpx_codec_decode(&ctx_, nullptr, 0, nullptr, 0);
    }
  }
};

class VpxEncoder final : public Encoder {
  // HDR10 static metadata travels in the container for VP9; hold on to it
  // so getInfo() can pass it to the muxer.
  OMMasteringDisplayMetadata mastering_display_ = {};
  OMContentLightLevel content_light_level_ = {};
  vpx_color_space_t color_space_ = VPX_CS_UNKNOWN;
  vpx_color_range_t color_range_ = VPX_CR_STUDIO_RANGE;

  static auto toVpxColorSpace(OMColorSpace c) -> vpx_color_space_t {
    switch (c) {
      case OM_COLOR_SPACE_BT601:     return VPX_CS_BT_601;
      case OM_COLOR_SPACE_BT709:     return VPX_CS_BT_709;
      case OM_COLOR_SPACE_BT2020:    return VPX_CS_BT_2020;
      case OM_COLOR_SPACE_SMPTE240M: return VPX_CS_SMPTE_240;
      case OM_COLOR_SPACE_RGB:       return VPX_CS_SRGB;
      default:                       return VPX_CS_UNKNOWN;
    }
  }

  vpx_codec_ctx_t ctx_ = {};
  bool initialized_ = false;
  OMCodecId codec_id_;
  VideoFormat format_ = {};
  vpx_codec_enc_cfg_t cfg_ = {};

public:
  explicit VpxEncoder(OMCodecId codec_id)
      : codec_id_(codec_id) {}

  ~VpxEncoder() override {
    if (initialized_) {
      vpx_codec_destroy(&ctx_);
    }
  }

  auto configure(const EncoderOptions& options) -> OMError override {
    if (initialized_) {
      vpx_codec_destroy(&ctx_);
      initialized_ = false;
    }

    vpx_codec_iface_t* iface = nullptr;
    if (codec_id_ == OM_CODEC_VP8) {
      iface = vpx_codec_vp8_cx();
    } else if (codec_id_ == OM_CODEC_VP9) {
      iface = vpx_codec_vp9_cx();
    } else {
      return OM_CODEC_INVALID_PARAMS;
    }

    vpx_codec_enc_cfg_t cfg = {};
    if (vpx_codec_enc_config_default(iface, &cfg, 0) != VPX_CODEC_OK) {
      return OM_CODEC_OPEN_FAILED;
    }

    cfg.g_w = options.video_format.width;
    cfg.g_h = options.video_format.height;
    cfg.g_timebase.num = options.format.video.framerate.den;
    cfg.g_timebase.den = options.format.video.framerate.num;
    cfg.g_threads = 1;

    if (auto* cbr = std::get_if<CbrParams>(&options.rate_control.params)) {
      cfg.rc_end_usage = VPX_CBR;
      cfg.rc_target_bitrate = static_cast<unsigned int>(cbr->bitrate.target_bitrate / 1000);
    } else if (auto* vbr = std::get_if<VbrParams>(&options.rate_control.params)) {
      cfg.rc_end_usage = VPX_VBR;
      cfg.rc_target_bitrate = static_cast<unsigned int>(vbr->bitrate.target_bitrate / 1000);
    }

    if (codec_id_ == OM_CODEC_VP9) {
      if (options.format.profile == OM_PROFILE_VP9_2) {
        cfg.g_profile = 2;
        cfg.g_bit_depth = VPX_BITS_10;
        cfg.g_input_bit_depth = 10;
      } else if (options.format.profile == OM_PROFILE_VP9_3) {
        cfg.g_profile = 3;
        cfg.g_bit_depth = VPX_BITS_10;
        cfg.g_input_bit_depth = 10;
      }
    }

    color_space_ = toVpxColorSpace(options.format.video.color_space);
    color_range_ = (options.format.video.color_range == OM_COLOR_RANGE_FULL)
                       ? VPX_CR_FULL_RANGE : VPX_CR_STUDIO_RANGE;

    if (vpx_codec_enc_init(&ctx_, iface, &cfg, (cfg.g_bit_depth > VPX_BITS_8) ? VPX_CODEC_USE_HIGHBITDEPTH : 0) != VPX_CODEC_OK) {
      return OM_CODEC_OPEN_FAILED;
    }

    // These are sequence-level and must be set on the codec, not per picture.
    if (codec_id_ == OM_CODEC_VP9) {
      vpx_codec_control(&ctx_, VP9E_SET_COLOR_SPACE, static_cast<int>(color_space_));
      vpx_codec_control(&ctx_, VP9E_SET_COLOR_RANGE, static_cast<int>(color_range_));
    }

    cfg_ = cfg;
    format_ = options.video_format;
    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> EncodingInfo override {
    EncodingInfo info = {};
    info.mastering_display = mastering_display_;
    info.content_light_level = content_light_level_;
    return info;
  }

  // Pulls whatever libvpx has ready into packets.
  auto collect() -> std::vector<Packet> {
    std::vector<Packet> packets;
    vpx_codec_iter_t iter = nullptr;
    const vpx_codec_cx_pkt_t* pkt = nullptr;
    while ((pkt = vpx_codec_get_cx_data(&ctx_, &iter)) != nullptr) {
      if (pkt->kind != VPX_CODEC_CX_FRAME_PKT) continue;
      Packet p;
      p.allocate(pkt->data.frame.sz);
      std::memcpy(p.bytes.data(), pkt->data.frame.buf, pkt->data.frame.sz);
      p.pts = pkt->data.frame.pts;
      p.dts = pkt->data.frame.pts;
      p.duration = pkt->data.frame.duration;
      p.is_keyframe = (pkt->data.frame.flags & VPX_FRAME_IS_KEY) != 0;
      packets.push_back(std::move(p));
    }
    return packets;
  }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    if (!initialized_) return Err(OM_CODEC_ENCODE_FAILED);

    // An empty frame drains the encoder.
    const bool flush = !std::holds_alternative<Picture>(frame.data) ||
                       std::get<Picture>(frame.data).width == 0;
    if (flush) {
      if (vpx_codec_encode(&ctx_, nullptr, 0, 1, 0, VPX_DL_REALTIME) != VPX_CODEC_OK)
        return Err(OM_CODEC_ENCODE_FAILED);
      return Ok(collect());
    }

    const auto& pic = std::get<Picture>(frame.data);
    vpx_image_t img = {};
    vpx_img_fmt_t vpx_fmt = VPX_IMG_FMT_NONE;

    if (pic.format == OM_FORMAT_YUV420P) {
      vpx_fmt = VPX_IMG_FMT_I420;
    } else if (pic.format == OM_FORMAT_YUV422P) {
      vpx_fmt = VPX_IMG_FMT_I422;
    } else if (pic.format == OM_FORMAT_YUV444P) {
      vpx_fmt = VPX_IMG_FMT_I444;
    } else if (pic.format == OM_FORMAT_YUV420P10) {
      vpx_fmt = VPX_IMG_FMT_I42016;
    } else if (pic.format == OM_FORMAT_YUV422P10) {
      vpx_fmt = VPX_IMG_FMT_I42216;
    } else if (pic.format == OM_FORMAT_YUV444P10) {
      vpx_fmt = VPX_IMG_FMT_I44416;
    }

    if (vpx_fmt == VPX_IMG_FMT_NONE) {
      return Err(OM_CODEC_ENCODE_FAILED);
    }

    vpx_img_wrap(&img, vpx_fmt, pic.width, pic.height, 1, const_cast<uint8_t*>(pic.planes.data[0]));
    img.bit_depth = (vpx_fmt & VPX_IMG_FMT_HIGHBITDEPTH) ? 10 : 8;
    for (int i = 0; i < 3; ++i) {
      img.planes[i] = const_cast<uint8_t*>(pic.planes.data[i]);
      img.stride[i] = pic.planes.linesize[i];
    }

    switch (pic.color_space) {
      case OM_COLOR_SPACE_BT601: img.cs = VPX_CS_BT_601; break;
      case OM_COLOR_SPACE_BT709: img.cs = VPX_CS_BT_709; break;
      case OM_COLOR_SPACE_BT2020: img.cs = VPX_CS_BT_2020; break;
      case OM_COLOR_SPACE_SMPTE240M: img.cs = VPX_CS_SMPTE_240; break;
      case OM_COLOR_SPACE_RGB: img.cs = VPX_CS_SRGB; break;
      default: img.cs = VPX_CS_UNKNOWN; break;
    }

    img.range = (pic.color_range == OM_COLOR_RANGE_FULL) ? VPX_CR_FULL_RANGE
                                                         : VPX_CR_STUDIO_RANGE;

    // VP9 has no SEI, so HDR10 static metadata lives in the container (the
    // SmpteSt2086 elements in WebM). Keep what the picture carries so getInfo()
    // can hand it to the muxer; libvpx itself has nowhere to put it.
    if (pic.mastering_display.has_value) mastering_display_ = pic.mastering_display;
    if (pic.content_light_level.has_value) content_light_level_ = pic.content_light_level;

    vpx_codec_pts_t pts = frame.pts;
    if (vpx_codec_encode(&ctx_, &img, pts, 1, 0, VPX_DL_REALTIME) != VPX_CODEC_OK) {
      return Err(OM_CODEC_ENCODE_FAILED);
    }

    return Ok(collect());
  }

  auto updateBitrate(const RateControlParams& rc) -> OMError override {
    if (!initialized_) return OM_COMMON_NOT_INITIALIZED;

    vpx_codec_enc_cfg_t cfg = cfg_;

    if (auto* cbr = std::get_if<CbrParams>(&rc.params)) {
      cfg.rc_target_bitrate = static_cast<unsigned int>(cbr->bitrate.target_bitrate / 1000);
    } else if (auto* vbr = std::get_if<VbrParams>(&rc.params)) {
      cfg.rc_target_bitrate = static_cast<unsigned int>(vbr->bitrate.target_bitrate / 1000);
    }

    if (vpx_codec_enc_config_set(&ctx_, &cfg) != VPX_CODEC_OK) {
      return OM_CODEC_INVALID_PARAMS;
    }

    cfg_ = cfg;
    return OM_SUCCESS;
  }
};

const CodecDescriptor CODEC_VP8 = {
    .codec_id = OM_CODEC_VP8,
    .type = OM_MEDIA_VIDEO,
    .name = "vp8",
    .long_name = "VP8",
    .vendor = "Google",
    .flags = NONE,
    .caps = CodecCaps {
        .threading = true,
        .video = VideoCodecCaps {
            .pix_fmts = {OM_FORMAT_YUV420P},
        },
    },
    .decoder_factory = [] { return std::make_unique<VpxDecoder>(OM_CODEC_VP8); },
    .encoder_factory = [] { return std::make_unique<VpxEncoder>(OM_CODEC_VP8); },
};

const CodecDescriptor CODEC_VP9 = {
    .codec_id = OM_CODEC_VP9,
    .type = OM_MEDIA_VIDEO,
    .name = "vp9",
    .long_name = "VP9",
    .vendor = "Google",
    .flags = NONE,
    .caps = CodecCaps {
        .profiles = {OM_PROFILE_VP9_0, OM_PROFILE_VP9_1, OM_PROFILE_VP9_2, OM_PROFILE_VP9_3},
        .threading = true,
        .video = VideoCodecCaps {
            .pix_fmts = {OM_FORMAT_YUV420P, OM_FORMAT_YUV422P, OM_FORMAT_YUV444P, OM_FORMAT_YUV420P10, OM_FORMAT_YUV422P10, OM_FORMAT_YUV444P10},
        },
    },
    .decoder_factory = [] { return std::make_unique<VpxDecoder>(OM_CODEC_VP9); },
    .encoder_factory = [] { return std::make_unique<VpxEncoder>(OM_CODEC_VP9); },
};

} // namespace openmedia
