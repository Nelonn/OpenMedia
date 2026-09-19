#include <dav1d/dav1d.h>
#include <algorithm>
#include <codecs.hpp>
#include <cstring>
#include <openmedia/codec_extra.hpp>
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

  static auto wrapPacket(const Packet& packet, Dav1dData& data) -> int {
    if (packet.buffer) {
      auto* ref = new std::shared_ptr<Buffer>(packet.buffer);
      int res = dav1d_data_wrap(&data, packet.bytes.data(), packet.bytes.size(),
                                [](const uint8_t*, void* cookie) { delete static_cast<std::shared_ptr<Buffer>*>(cookie); },
                                ref);
      if (res < 0) delete ref;
      return res;
    }
    uint8_t* dst = dav1d_data_create(&data, packet.bytes.size());
    if (!dst) return DAV1D_ERR(ENOMEM);
    std::memcpy(dst, packet.bytes.data(), packet.bytes.size());
    return 0;
  }

  // The layout and depth dav1d decoded to, as a pixel format. Past eight bits every sample takes two
  // bytes, the significant bits at the bottom.
  static auto toPixelFormat(Dav1dPixelLayout layout, int bpc) -> OMPixelFormat {
    const bool deep = bpc > 8;
    switch (layout) {
      case DAV1D_PIXEL_LAYOUT_I400: return deep ? OM_FORMAT_GRAY16 : OM_FORMAT_GRAY8;
      case DAV1D_PIXEL_LAYOUT_I420:
        return !deep ? OM_FORMAT_YUV420P : bpc <= 10 ? OM_FORMAT_YUV420P10 : OM_FORMAT_YUV420P12;
      case DAV1D_PIXEL_LAYOUT_I422:
        return !deep ? OM_FORMAT_YUV422P : bpc <= 10 ? OM_FORMAT_YUV422P10 : OM_FORMAT_YUV422P12;
      case DAV1D_PIXEL_LAYOUT_I444:
        return !deep ? OM_FORMAT_YUV444P : bpc <= 10 ? OM_FORMAT_YUV444P10 : OM_FORMAT_YUV444P12;
    }
    return OM_FORMAT_UNKNOWN;
  }

  static auto toColorSpace(Dav1dMatrixCoefficients matrix) -> OMColorSpace {
    switch (matrix) {
      case DAV1D_MC_BT709: return OM_COLOR_SPACE_BT709;
      case DAV1D_MC_FCC: return OM_COLOR_SPACE_FCC;
      case DAV1D_MC_BT470BG:
      case DAV1D_MC_BT601: return OM_COLOR_SPACE_BT601;
      case DAV1D_MC_SMPTE240: return OM_COLOR_SPACE_SMPTE240M;
      case DAV1D_MC_SMPTE_YCGCO: return OM_COLOR_SPACE_YCGCO;
      case DAV1D_MC_BT2020_NCL: return OM_COLOR_SPACE_BT2020;
      case DAV1D_MC_BT2020_CL: return OM_COLOR_SPACE_BT2020_CL;
      case DAV1D_MC_CHROMAT_NCL: return OM_COLOR_SPACE_CHROMA_DERIVED_NCL;
      case DAV1D_MC_CHROMAT_CL: return OM_COLOR_SPACE_CHROMA_DERIVED_CL;
      case DAV1D_MC_ICTCP: return OM_COLOR_SPACE_ICTCP;
      case DAV1D_MC_IDENTITY: return OM_COLOR_SPACE_RGB;
      default: break;
    }
    return OM_COLOR_SPACE_UNKNOWN;
  }

  static auto toTransfer(Dav1dTransferCharacteristics transfer) -> OMTransferCharacteristic {
    switch (transfer) {
      case DAV1D_TRC_BT709: return OM_TRANSFER_BT709;
      case DAV1D_TRC_BT470M: return OM_TRANSFER_GAMMA22;
      case DAV1D_TRC_BT470BG: return OM_TRANSFER_GAMMA28;
      case DAV1D_TRC_BT601: return OM_TRANSFER_BT601;
      case DAV1D_TRC_SMPTE240: return OM_TRANSFER_SMPTE240M;
      case DAV1D_TRC_LINEAR: return OM_TRANSFER_LINEAR;
      case DAV1D_TRC_LOG100: return OM_TRANSFER_LOG;
      case DAV1D_TRC_LOG100_SQRT10: return OM_TRANSFER_LOG_SQRT;
      case DAV1D_TRC_IEC61966: return OM_TRANSFER_IEC61966_2_4;
      case DAV1D_TRC_BT1361: return OM_TRANSFER_BT1361_ECG;
      case DAV1D_TRC_SRGB: return OM_TRANSFER_SRGB;
      case DAV1D_TRC_BT2020_10BIT: return OM_TRANSFER_BT2020_10;
      case DAV1D_TRC_BT2020_12BIT: return OM_TRANSFER_BT2020_12;
      case DAV1D_TRC_SMPTE2084: return OM_TRANSFER_SMPTE2084;
      case DAV1D_TRC_SMPTE428: return OM_TRANSFER_SMPTE428;
      case DAV1D_TRC_HLG: return OM_TRANSFER_ARIB_STD_B67;
      default: break;
    }
    return OM_TRANSFER_UNKNOWN;
  }

  // AV1 numbers its primaries the way H.273 does, and so does OMColorPrimaries.
  static auto toPrimaries(Dav1dColorPrimaries primaries) -> OMColorPrimaries {
    switch (primaries) {
      case DAV1D_COLOR_PRI_BT709:
      case DAV1D_COLOR_PRI_BT470M:
      case DAV1D_COLOR_PRI_BT470BG:
      case DAV1D_COLOR_PRI_BT601:
      case DAV1D_COLOR_PRI_SMPTE240:
      case DAV1D_COLOR_PRI_FILM:
      case DAV1D_COLOR_PRI_BT2020:
      case DAV1D_COLOR_PRI_XYZ:
      case DAV1D_COLOR_PRI_SMPTE431:
      case DAV1D_COLOR_PRI_SMPTE432:
      case DAV1D_COLOR_PRI_EBU3213: return static_cast<OMColorPrimaries>(primaries);
      default: break;
    }
    return OM_PRIMARIES_UNKNOWN;
  }

  // What the stream says of its colour: the sequence header's description, and the HDR metadata
  // that came with this picture. AV1 writes the mastering display in fixed point -- chromaticities
  // in 0.16, luminance in 24.8 and 18.14 -- and OpenMedia in SMPTE ST 2086's units.
  static void describeColor(const Dav1dPicture& pic, Picture& out) {
    if (const Dav1dSequenceHeader* seq = pic.seq_hdr) {
      out.color_space = toColorSpace(seq->mtrx);
      out.transfer_char = toTransfer(seq->trc);
      out.color_primaries = toPrimaries(seq->pri);
      out.color_range = seq->color_range ? OM_COLOR_RANGE_JPEG : OM_COLOR_RANGE_MPEG;
    }
    if (const Dav1dMasteringDisplay* md = pic.mastering_display) {
      for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 2; ++j) {
          out.mastering_display.display_primaries[i][j] = static_cast<uint16_t>(md->primaries[i][j] * 50000ull / 65536);
        }
      }
      out.mastering_display.white_point[0] = static_cast<uint16_t>(md->white_point[0] * 50000ull / 65536);
      out.mastering_display.white_point[1] = static_cast<uint16_t>(md->white_point[1] * 50000ull / 65536);
      out.mastering_display.max_display_mastering_luminance = static_cast<uint32_t>(md->max_luminance * 10000ull / 256);
      out.mastering_display.min_display_mastering_luminance = static_cast<uint32_t>(md->min_luminance * 10000ull / 16384);
      out.mastering_display.has_value = true;
    }
    if (const Dav1dContentLightLevel* cll = pic.content_light) {
      out.content_light_level.max_content_light_level = cll->max_content_light_level;
      out.content_light_level.max_pic_average_light_level = cll->max_frame_average_light_level;
      out.content_light_level.has_value = true;
    }
  }

  static auto appendPicture(Dav1dPicture& pic, std::vector<Frame>& frames) -> OMError {
    std::unique_ptr<Dav1dPicture, decltype(&dav1d_picture_unref)> guard(&pic, &dav1d_picture_unref);

    const OMPixelFormat pixel_format = toPixelFormat(pic.p.layout, pic.p.bpc);
    if (pixel_format == OM_FORMAT_UNKNOWN) {
      return OM_CODEC_DECODE_FAILED;
    }

    Picture out_pic(pixel_format, pic.p.w, pic.p.h);
    describeColor(pic, out_pic);

    // Rows are copied by what they hold in bytes -- two a sample past eight bits -- from dav1d's
    // stride into the picture's own. Chroma planes round up: an odd width or height still has a
    // chroma sample for its last column or row.
    const uint32_t bytes_per_sample = pic.p.bpc > 8 ? 2 : 1;
    const uint32_t luma_w = static_cast<uint32_t>(pic.p.w);
    const uint32_t luma_h = static_cast<uint32_t>(pic.p.h);
    copyPlane(out_pic.planes.data[0], out_pic.planes.linesize[0], static_cast<const uint8_t*>(pic.data[0]),
              pic.stride[0], luma_w * bytes_per_sample, luma_h);

    if (pic.p.layout != DAV1D_PIXEL_LAYOUT_I400) {
      const bool half_width = pic.p.layout == DAV1D_PIXEL_LAYOUT_I420 || pic.p.layout == DAV1D_PIXEL_LAYOUT_I422;
      const bool half_height = pic.p.layout == DAV1D_PIXEL_LAYOUT_I420;
      const uint32_t chroma_w = half_width ? (luma_w + 1) / 2 : luma_w;
      const uint32_t chroma_h = half_height ? (luma_h + 1) / 2 : luma_h;

      // dav1d gives both chroma planes one stride.
      copyPlane(out_pic.planes.data[1], out_pic.planes.linesize[1], static_cast<const uint8_t*>(pic.data[1]),
                pic.stride[1], chroma_w * bytes_per_sample, chroma_h);
      copyPlane(out_pic.planes.data[2], out_pic.planes.linesize[2], static_cast<const uint8_t*>(pic.data[2]),
                pic.stride[1], chroma_w * bytes_per_sample, chroma_h);
    }

    Frame frame = {};
    frame.pts = pic.m.timestamp;
    frame.dts = pic.m.timestamp;
    frame.data = std::move(out_pic);
    frames.push_back(std::move(frame));

    return OM_SUCCESS;
  }

public:
  Dav1dDecoder() {}

  ~Dav1dDecoder() override = default;

  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_AV1) {
      return OM_CODEC_INVALID_PARAMS;
    }

    const auto& extra = options.extra;

    dav1d_default_settings(&settings_);
    settings_.n_threads = std::max(extra.getInt32(CODEC_THREADS, 1), 0);
    settings_.max_frame_delay = extra.getBool(CODEC_DEC_LOW_DELAY) ? 1 : 0;
    if (extra.contains(DAV1D_DEC_MAX_FRAME_DELAY)) {
      settings_.max_frame_delay = std::max(extra.getInt32(DAV1D_DEC_MAX_FRAME_DELAY), 0);
    }
    settings_.apply_grain = extra.getBool(CODEC_DEC_APPLY_FILM_GRAIN, true);
    settings_.operating_point = std::clamp(extra.getInt32(DAV1D_DEC_OPERATING_POINT, 0), 0, 31);
    settings_.all_layers = extra.getBool(DAV1D_DEC_ALL_LAYERS, settings_.all_layers != 0);
    settings_.frame_size_limit = static_cast<unsigned>(std::max(extra.getInt32(DAV1D_DEC_FRAME_SIZE_LIMIT, 0), 0));
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

    if (packet.bytes.empty()) {
      // Drain: repeated dav1d_get_picture calls without new data wait for in-flight frames
      for (;;) {
        Dav1dPicture pic = {};
        int res = dav1d_get_picture(ctx_.get(), &pic);
        if (res == DAV1D_ERR(EAGAIN)) break;
        if (res < 0) return Err(OM_CODEC_DECODE_FAILED);
        if (auto err = appendPicture(pic, frames); err != OM_SUCCESS) return Err(err);
      }
      return Ok(std::move(frames));
    }

    Dav1dData data = {};
    if (wrapPacket(packet, data) < 0) {
      return Err(OM_CODEC_DECODE_FAILED);
    }
    data.m.timestamp = packet.pts;

    // Frame threads keep references to the data, so it is refcounted rather than borrowed.
    // EAGAIN means the data was not taken: pull a picture to make room and resend.
    while (data.sz > 0) {
      int res = dav1d_send_data(ctx_.get(), &data);
      if (res < 0 && res != DAV1D_ERR(EAGAIN)) {
        dav1d_data_unref(&data);
        return Err(OM_CODEC_DECODE_FAILED);
      }

      Dav1dPicture pic = {};
      res = dav1d_get_picture(ctx_.get(), &pic);
      if (res == DAV1D_ERR(EAGAIN)) continue;
      if (res < 0) {
        dav1d_data_unref(&data);
        return Err(OM_CODEC_DECODE_FAILED);
      }
      if (auto err = appendPicture(pic, frames); err != OM_SUCCESS) {
        dav1d_data_unref(&data);
        return Err(err);
      }
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
        .threading = true,
    },
    .options = {
        CODEC_THREADS,
        CODEC_DEC_LOW_DELAY,
        CODEC_DEC_APPLY_FILM_GRAIN,
        DAV1D_DEC_MAX_FRAME_DELAY,
        DAV1D_DEC_OPERATING_POINT,
        DAV1D_DEC_ALL_LAYERS,
        DAV1D_DEC_FRAME_SIZE_LIMIT,
    },
    .decoder_factory = [] { return std::make_unique<Dav1dDecoder>(); },
};

} // namespace openmedia
