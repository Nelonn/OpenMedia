#include "avcodec.hpp"
#include <cstring>
#include <memory>
#include <openmedia/codec_api.hpp>
#include <openmedia/codec_registry.hpp>
#include <vector>
#include <codecs.hpp>

namespace openmedia {

auto LibAVCodec::getInstance() -> LibAVCodec& {
  static LibAVCodec instance;
  return instance;
}

auto LibAVCodec::load() -> bool {
  if (loaded_) return true;

  std::lock_guard<std::mutex> lock(load_mutex_);
  if (loaded_) return true;

  if (!LibAVUtil::getInstance().isLoaded()) {
    if (!LibAVUtil::getInstance().load()) {
      return false;
    }
  }

#if defined(_WIN32)
  const char* library_name = "avcodec-62.dll";
#elif defined(OPENMEDIA_FFMPEG_AVCODEC_LIBRARY)
  const char* library_name = OPENMEDIA_FFMPEG_AVCODEC_LIBRARY;
#elif defined(__APPLE__)
  const char* library_name = "libavcodec-62.dylib";
#else
  const char* library_name = "libavcodec-62.so";
#endif

  library_.open(library_name);
  if (!library_.success()) {
    return false;
  }

  avcodec_find_decoder = library_.getProcAddress<PFN<const AVCodec*(AVCodecID)>>("avcodec_find_decoder");
  avcodec_find_encoder = library_.getProcAddress<PFN<const AVCodec*(AVCodecID)>>("avcodec_find_encoder");
  avcodec_alloc_context3 = library_.getProcAddress<PFN<AVCodecContext*(const AVCodec*)>>("avcodec_alloc_context3");
  avcodec_open2 = library_.getProcAddress<PFN<int(AVCodecContext*, const AVCodec*, AVDictionary**)>>("avcodec_open2");
  avcodec_free_context = library_.getProcAddress<PFN<void(AVCodecContext**)>>("avcodec_free_context");
  avcodec_send_packet = library_.getProcAddress<PFN<int(AVCodecContext*, const AVPacket*)>>("avcodec_send_packet");
  avcodec_receive_frame = library_.getProcAddress<PFN<int(AVCodecContext*, AVFrame*)>>("avcodec_receive_frame");
  avcodec_send_frame = library_.getProcAddress<PFN<int(AVCodecContext*, const AVFrame*)>>("avcodec_send_frame");
  avcodec_receive_packet = library_.getProcAddress<PFN<int(AVCodecContext*, AVPacket*)>>("avcodec_receive_packet");
  avcodec_flush_buffers = library_.getProcAddress<PFN<void(AVCodecContext*)>>("avcodec_flush_buffers");
  avcodec_get_type = library_.getProcAddress<PFN<AVMediaType(AVCodecID)>>("avcodec_get_type");
  avcodec_get_name = library_.getProcAddress<PFN<const char*(AVCodecID)>>("avcodec_get_name");
  av_codec_iterate = library_.getProcAddress<PFN<const AVCodec*(void**)>>("av_codec_iterate");
  av_packet_alloc = library_.getProcAddress<PFN<AVPacket*()>>("av_packet_alloc");
  av_packet_free = library_.getProcAddress<PFN<void(AVPacket**)>>("av_packet_free");
  av_packet_unref = library_.getProcAddress<PFN<void(AVPacket*)>>("av_packet_unref");
  av_packet_ref = library_.getProcAddress<PFN<int(AVPacket*, const AVPacket*)>>("av_packet_ref");
  av_packet_clone = library_.getProcAddress<PFN<AVPacket*(const AVPacket*)>>("av_packet_clone");
  av_packet_move_ref = library_.getProcAddress<PFN<void(AVPacket*, AVPacket*)>>("av_packet_move_ref");
  av_new_packet = library_.getProcAddress<PFN<int(AVPacket*, int)>>("av_new_packet");
  av_grow_packet = library_.getProcAddress<PFN<int(AVPacket*, int)>>("av_grow_packet");
  av_shrink_packet = library_.getProcAddress<PFN<void(AVPacket*, int)>>("av_shrink_packet");

  av_parser_init = library_.getProcAddress<PFN<AVCodecParserContext*(int)>>("av_parser_init");
  av_parser_parse2 = library_.getProcAddress<PFN<int(AVCodecParserContext*, AVCodecContext*, uint8_t**, int*, const uint8_t*, int, int64_t, int64_t, int64_t)>>("av_parser_parse2");
  av_parser_close = library_.getProcAddress<PFN<void(AVCodecParserContext*)>>("av_parser_close");

  if (!avcodec_find_decoder || !avcodec_alloc_context3 || !avcodec_open2 ||
      !avcodec_free_context || !avcodec_send_packet || !avcodec_receive_frame) {
    return false;
  }

  loaded_ = true;
  return true;
}

auto LibAVCodec::isLoaded() const -> bool {
  return loaded_;
}

template <>
void AVDeleter<::AVCodecContext>::operator()(::AVCodecContext* ptr) const {
  if (ptr) LibAVCodec::getInstance().avcodec_free_context(&ptr);
}

template <>
void AVDeleter<::AVPacket>::operator()(::AVPacket* ptr) const {
  if (ptr) LibAVCodec::getInstance().av_packet_free(&ptr);
}

template <>
void AVDeleter<::AVCodecParserContext>::operator()(::AVCodecParserContext* ptr) const {
  if (ptr) LibAVCodec::getInstance().av_parser_close(ptr);
}

class FFmpegDecoder final : public Decoder {
public:
  explicit FFmpegDecoder(AVCodecID codec_id)
      : av_codec_id_(codec_id) {}

  ~FFmpegDecoder() override {
    release();
  }

  auto configure(const DecoderOptions& options) -> OMError override {
    auto& codec_loader = LibAVCodec::getInstance();
    auto& util_loader = LibAVUtil::getInstance();

    if (!util_loader.isLoaded()) {
      if (!util_loader.load()) {
        return OM_CODEC_NOT_SUPPORTED;
      }
    }
    if (!codec_loader.isLoaded()) {
      if (!codec_loader.load()) {
        return OM_CODEC_NOT_SUPPORTED;
      }
    }

    const AVCodec* codec = codec_loader.avcodec_find_decoder(av_codec_id_);
    if (!codec) {
      return OM_CODEC_NOT_SUPPORTED;
    }

    auto configure_context = [&](bool minimal) -> OMError {
      codec_ctx_.reset(codec_loader.avcodec_alloc_context3(codec));
      if (!codec_ctx_) {
        return OM_COMMON_OUT_OF_MEMORY;
      }

      if (!minimal) {
        if (options.time_base.den > 0) {
          codec_ctx_->pkt_timebase.num = options.time_base.num;
          codec_ctx_->pkt_timebase.den = options.time_base.den;
        }

        if (options.format.type == OM_MEDIA_VIDEO) {
          if (options.format.video.framerate.den > 0) {
            codec_ctx_->framerate.num = options.format.video.framerate.num;
            codec_ctx_->framerate.den = options.format.video.framerate.den;
          }
          codec_ctx_->coded_width = options.format.video.width;
          codec_ctx_->coded_height = options.format.video.height;
        }

        if (!options.extradata.empty()) {
          codec_ctx_->extradata_size = static_cast<int>(options.extradata.size());
          codec_ctx_->extradata = static_cast<uint8_t*>(
              util_loader.av_malloc(options.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
          if (!codec_ctx_->extradata) {
            return OM_COMMON_OUT_OF_MEMORY;
          }
          memcpy(codec_ctx_->extradata, options.extradata.data(), options.extradata.size());
          memset(codec_ctx_->extradata + options.extradata.size(), 0, AV_INPUT_BUFFER_PADDING_SIZE);
        }
      }

      const int ret = codec_loader.avcodec_open2(codec_ctx_.get(), codec, nullptr);
      if (ret < 0) {
        codec_ctx_.reset();
        return avErrorToOmError(ret);
      }

      return OM_SUCCESS;
    };

    OMError err = configure_context(false);
    if (err != OM_SUCCESS) {
      err = configure_context(true);
      if (err != OM_SUCCESS) {
        return err;
      }
    }

    frame_.reset(util_loader.av_frame_alloc());
    packet_.reset(codec_loader.av_packet_alloc());

    if (!frame_ || !packet_) {
      return OM_COMMON_OUT_OF_MEMORY;
    }

    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!initialized_ || !codec_ctx_) {
      return std::nullopt;
    }

    DecodingInfo info;
    info.media_type = (codec_ctx_->codec_type == AVMEDIA_TYPE_VIDEO)
                          ? OM_MEDIA_VIDEO
                          : (codec_ctx_->codec_type == AVMEDIA_TYPE_AUDIO) ? OM_MEDIA_AUDIO
                                                                           : OM_MEDIA_NONE;

    if (info.media_type == OM_MEDIA_VIDEO) {
      info.video_format = VideoFormat{
          .format = avPixelFormatToOmPixelFormat(codec_ctx_->pix_fmt),
          .width = static_cast<uint32_t>(codec_ctx_->width),
          .height = static_cast<uint32_t>(codec_ctx_->height),
          .color_space = avColorSpaceToOmColorSpace(codec_ctx_->colorspace),
          .transfer_char = avColorTransferToOmTransfer(codec_ctx_->color_trc),
          .color_primaries = avColorPrimariesToOmPrimaries(codec_ctx_->color_primaries),
          .color_range = avColorRangeToOmRange(codec_ctx_->color_range),
      };
    } else if (info.media_type == OM_MEDIA_AUDIO) {
      auto& util = LibAVUtil::getInstance();
      info.audio_format = AudioFormat{
          .sample_format = avSampleFormatToOmSampleFormat(codec_ctx_->sample_fmt),
          .bits_per_sample = static_cast<uint8_t>(codec_ctx_->bits_per_coded_sample),
          .sample_rate = static_cast<uint32_t>(codec_ctx_->sample_rate),
          .channels = static_cast<uint32_t>(codec_ctx_->ch_layout.nb_channels),
          .planar = (util.av_sample_fmt_is_planar &&
                     util.av_sample_fmt_is_planar(codec_ctx_->sample_fmt) != 0),
      };
    }

    return info;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    if (!initialized_ || !codec_ctx_ || !frame_ || !packet_) {
      return Err(OM_COMMON_NOT_INITIALIZED);
    }

    auto& codec_loader = LibAVCodec::getInstance();
    auto& util_loader = LibAVUtil::getInstance();

    std::vector<Frame> frames;

    auto pkt_data = packet.bytes;
    int ret = codec_loader.av_new_packet(packet_.get(), static_cast<int>(pkt_data.size()));
    if (ret < 0) {
      return Err(avErrorToOmError(ret));
    }
    memcpy(packet_->data, pkt_data.data(), pkt_data.size());
    packet_->pts = packet.pts;
    packet_->dts = packet.dts;
    packet_->stream_index = packet.stream_index;

    ret = codec_loader.avcodec_send_packet(codec_ctx_.get(), packet_.get());
    if (ret < 0) {
      codec_loader.av_packet_unref(packet_.get());
      return Err(avErrorToOmError(ret));
    }
    codec_loader.av_packet_unref(packet_.get());

    while (true) {
      int ret = codec_loader.avcodec_receive_frame(codec_ctx_.get(), frame_.get());
      if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
        break;
      }
      if (ret < 0) {
        return Err(avErrorToOmError(ret));
      }

      auto frame = convertAVFrameToFrame(codec_ctx_->codec_type, frame_.get());
      if (frame.has_value()) {
        frames.push_back(std::move(*frame));
      }

      util_loader.av_frame_unref(frame_.get());
    }

    return Ok(std::move(frames));
  }

  void flush() override {
    if (initialized_ && codec_ctx_) {
      auto& codec_loader = LibAVCodec::getInstance();
      if (codec_loader.avcodec_flush_buffers) {
        codec_loader.avcodec_flush_buffers(codec_ctx_.get());
      }
    }
  }

private:
  static auto convertAVFrameToFrame(AVMediaType media_type, AVFrame* av_frame) -> std::optional<Frame> {
    if (!av_frame) return std::nullopt;

    auto& util = LibAVUtil::getInstance();

    Frame frame;
    frame.pts = (av_frame->pts != AV_NOPTS_VALUE) ? static_cast<uint64_t>(av_frame->pts) : 0;
    frame.dts = (av_frame->pkt_dts != AV_NOPTS_VALUE) ? static_cast<uint64_t>(av_frame->pkt_dts) : 0;

    if (media_type == AVMEDIA_TYPE_VIDEO) {
      // Video frame conversion
      Picture picture;
      picture.format = avPixelFormatToOmPixelFormat(static_cast<AVPixelFormat>(av_frame->format));
      picture.width = static_cast<uint32_t>(av_frame->width);
      picture.height = static_cast<uint32_t>(av_frame->height);
      picture.color_space = avColorSpaceToOmColorSpace(av_frame->colorspace);
      picture.transfer_char = avColorTransferToOmTransfer(av_frame->color_trc);
      picture.color_primaries = avColorPrimariesToOmPrimaries(av_frame->color_primaries);
      picture.color_range = avColorRangeToOmRange(av_frame->color_range);
      picture.is_keyframe = (av_frame->flags & AV_FRAME_FLAG_KEY) != 0;

      // HDR metadata
      if (auto* sd = util.av_frame_get_side_data(av_frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA)) {
        auto* md = reinterpret_cast<AVMasteringDisplayMetadata*>(sd->data);
        picture.mastering_display.has_value = md->has_primaries && md->has_luminance;
        if (picture.mastering_display.has_value) {
          for (int i = 0; i < 3; ++i) {
            picture.mastering_display.display_primaries[i][0] = static_cast<uint16_t>(md->display_primaries[i][0].num * 50000 / md->display_primaries[i][0].den);
            picture.mastering_display.display_primaries[i][1] = static_cast<uint16_t>(md->display_primaries[i][1].num * 50000 / md->display_primaries[i][1].den);
          }
          picture.mastering_display.white_point[0] = static_cast<uint16_t>(md->white_point[0].num * 50000 / md->white_point[0].den);
          picture.mastering_display.white_point[1] = static_cast<uint16_t>(md->white_point[1].num * 50000 / md->white_point[1].den);
          picture.mastering_display.max_display_mastering_luminance = static_cast<uint32_t>(md->max_luminance.num * 10000 / md->max_luminance.den);
          picture.mastering_display.min_display_mastering_luminance = static_cast<uint32_t>(md->min_luminance.num * 10000 / md->min_luminance.den);
        }
      }

      if (auto* sd = util.av_frame_get_side_data(av_frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL)) {
        auto* cll = reinterpret_cast<AVContentLightMetadata*>(sd->data);
        picture.content_light_level.has_value = true;
        picture.content_light_level.max_content_light_level = static_cast<uint16_t>(cll->MaxCLL);
        picture.content_light_level.max_pic_average_light_level = static_cast<uint16_t>(cll->MaxFALL);
      }

      picture.allocate();

      int buffer_size = util.av_image_get_buffer_size(
          static_cast<AVPixelFormat>(av_frame->format),
          av_frame->width, av_frame->height, 1);

      if (buffer_size > 0) {
        int num_planes = getNumPlanes(picture.format);
        for (int i = 0; i < num_planes && i < 4; ++i) {
          if (av_frame->data[i] && picture.planes.getData(i)) {
            uint32_t plane_height = picture.getPlaneDimensions(i).second;
            int bytes_per_row = std::min(static_cast<uint32_t>(av_frame->linesize[i]), picture.planes.getLinesize(i));
            if (bytes_per_row > 0 && plane_height > 0) {
              for (uint32_t y = 0; y < plane_height; ++y) {
                memcpy(
                    picture.planes.getData(i) + static_cast<size_t>(y) * picture.planes.getLinesize(i),
                    av_frame->data[i] + static_cast<size_t>(y) * av_frame->linesize[i],
                    static_cast<size_t>(bytes_per_row));
              }
            }
          }
        }
      }

      frame.data = std::move(picture);

    } else if (media_type == AVMEDIA_TYPE_AUDIO) {
      AudioSamples samples;
      samples.format.sample_format = avSampleFormatToOmSampleFormat(static_cast<AVSampleFormat>(av_frame->format));
      samples.format.sample_rate = static_cast<uint32_t>(av_frame->sample_rate);
      samples.format.channels = static_cast<uint32_t>(av_frame->ch_layout.nb_channels);
      samples.format.planar = (util.av_sample_fmt_is_planar &&
                               util.av_sample_fmt_is_planar(static_cast<AVSampleFormat>(av_frame->format)) != 0);
      samples.bits_per_sample = static_cast<uint8_t>(util.av_get_bytes_per_sample(static_cast<AVSampleFormat>(av_frame->format)) * 8);
      samples.nb_samples = static_cast<uint32_t>(av_frame->nb_samples);

      samples.allocate();

      int bytes_per_sample = util.av_get_bytes_per_sample(static_cast<AVSampleFormat>(av_frame->format));
      size_t total_samples = static_cast<size_t>(av_frame->nb_samples) * av_frame->ch_layout.nb_channels;
      size_t buffer_size = total_samples * static_cast<size_t>(bytes_per_sample);

      if (buffer_size > 0) {
        samples.buffer = BufferPool::getInstance().get(buffer_size);
        uint8_t* dst_ptr = samples.buffer->bytes().data();

        if (samples.format.planar) {
          size_t plane_samples = static_cast<size_t>(av_frame->nb_samples) * bytes_per_sample;
          for (int i = 0; i < av_frame->ch_layout.nb_channels && i < 8; ++i) {
            if (av_frame->data[i]) {
              std::memcpy(dst_ptr + i * plane_samples, av_frame->data[i], plane_samples);
              samples.planes.setData(i, dst_ptr + i * plane_samples, static_cast<uint32_t>(plane_samples));
            }
          }
        } else {
          size_t frame_samples = static_cast<size_t>(av_frame->nb_samples) * bytes_per_sample;
          size_t channel_step = static_cast<size_t>(bytes_per_sample);

          for (int i = 0; i < av_frame->ch_layout.nb_channels && i < 8; ++i) {
            if (av_frame->data[0]) {
              uint8_t* src = av_frame->data[0] + i * channel_step;
              uint8_t* dst = dst_ptr + i * frame_samples;
              for (int s = 0; s < av_frame->nb_samples; ++s) {
                std::memcpy(dst + s * channel_step, src + s * av_frame->ch_layout.nb_channels * channel_step, channel_step);
              }
              samples.planes.setData(i, dst, static_cast<uint32_t>(frame_samples));
            }
          }
        }
      }

      frame.data = std::move(samples);
    }

    return frame;
  }

  void release() {
    codec_ctx_.reset();
    frame_.reset();
    packet_.reset();
    initialized_ = false;
  }

  AVCodecID av_codec_id_;
  AVPtr<AVCodecContext> codec_ctx_;
  AVPtr<AVFrame> frame_;
  AVPtr<AVPacket> packet_;
  bool initialized_ = false;
};

class FFmpegEncoder final : public Encoder {
public:
  explicit FFmpegEncoder(AVCodecID codec_id)
      : av_codec_id_(codec_id) {}

  ~FFmpegEncoder() override {
    release();
  }

  auto configure(const EncoderOptions& options) -> OMError override {
    auto& codec_loader = LibAVCodec::getInstance();
    auto& util_loader = LibAVUtil::getInstance();

    if (!util_loader.isLoaded()) {
      if (!util_loader.load()) {
        return OM_CODEC_NOT_SUPPORTED;
      }
    }
    if (!codec_loader.isLoaded()) {
      if (!codec_loader.load()) {
        return OM_CODEC_NOT_SUPPORTED;
      }
    }

    const AVCodec* codec = codec_loader.avcodec_find_encoder(av_codec_id_);
    if (!codec) {
      return OM_CODEC_NOT_SUPPORTED;
    }

    codec_ctx_.reset(codec_loader.avcodec_alloc_context3(codec));
    if (!codec_ctx_) {
      return OM_COMMON_OUT_OF_MEMORY;
    }

    if (options.format.type == OM_MEDIA_VIDEO) {
      configured_video_format_ = options.video_format;
      codec_ctx_->width = static_cast<int>(options.video_format.width);
      codec_ctx_->height = static_cast<int>(options.video_format.height);
      codec_ctx_->time_base.num = 1;
      codec_ctx_->time_base.den = 1000;
      if (options.format.video.framerate.den > 0) {
        codec_ctx_->framerate.num = options.format.video.framerate.num;
        codec_ctx_->framerate.den = options.format.video.framerate.den;
      }
      codec_ctx_->pix_fmt = omPixelFormatToAvPixelFormat(options.video_format.format);
      codec_ctx_->colorspace = omColorSpaceToAvColorSpace(options.video_format.color_space);
      codec_ctx_->color_trc = omColorTransferToAvTransfer(options.video_format.transfer_char);
      codec_ctx_->color_primaries = omColorPrimariesToAvPrimaries(options.video_format.color_primaries);
    } else if (options.format.type == OM_MEDIA_AUDIO) {
      codec_ctx_->sample_rate = static_cast<int>(options.audio_format.sample_rate);
      codec_ctx_->sample_fmt = omSampleFormatToAvSampleFormat(options.audio_format.sample_format);
      codec_ctx_->ch_layout.nb_channels = static_cast<int>(options.audio_format.channels);
      codec_ctx_->time_base.num = 1;
      codec_ctx_->time_base.den = codec_ctx_->sample_rate;
    }

    auto apply_rc = [&](const RateControlParams& rc) {
      std::visit([&]<typename T0>(T0&& p) {
        using T = std::decay_t<T0>;
        if constexpr (std::is_same_v<T, CbrParams>) {
          codec_ctx_->bit_rate = p.bitrate.target_bitrate;
          codec_ctx_->rc_min_rate = p.bitrate.target_bitrate;
          codec_ctx_->rc_max_rate = p.bitrate.target_bitrate;
          if (p.bitrate.vbv) {
            codec_ctx_->rc_buffer_size = static_cast<int>(p.bitrate.vbv->buffer_size);
          }
        } else if constexpr (std::is_same_v<T, VbrParams>) {
          codec_ctx_->bit_rate = p.bitrate.target_bitrate;
          if (p.bitrate.max_bitrate) {
            codec_ctx_->rc_max_rate = *p.bitrate.max_bitrate;
          }
          if (p.bitrate.vbv) {
            codec_ctx_->rc_buffer_size = static_cast<int>(p.bitrate.vbv->buffer_size);
          }
        } else if constexpr (std::is_same_v<T, CrfParams>) {
          util_loader.av_dict_set(&options_dict_, "crf", std::to_string(p.quality).c_str(), 0);
        } else if constexpr (std::is_same_v<T, CqpParams>) {
          codec_ctx_->global_quality = p.qp_i;
        } else if constexpr (std::is_same_v<T, AbrParams>) {
          codec_ctx_->bit_rate = p.target_bitrate;
        }
      }, rc.params);
    };

    apply_rc(options.rate_control);

    int ret = codec_loader.avcodec_open2(codec_ctx_.get(), codec, &options_dict_);
    if (ret < 0) {
      codec_ctx_.reset();
      return avErrorToOmError(ret);
    }

    packet_.reset(codec_loader.av_packet_alloc());
    if (!packet_) {
      return OM_COMMON_OUT_OF_MEMORY;
    }

    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> EncodingInfo override {
    EncodingInfo info;
    if (initialized_ && codec_ctx_ && codec_ctx_->extradata_size > 0) {
      info.extradata.assign(codec_ctx_->extradata, codec_ctx_->extradata + codec_ctx_->extradata_size);
    }
    info.mastering_display = configured_video_format_.mastering_display;
    info.content_light_level = configured_video_format_.content_light_level;
    return info;
  }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    if (!initialized_ || !codec_ctx_ || !packet_) {
      return Err(OM_COMMON_NOT_INITIALIZED);
    }

    auto& codec_loader = LibAVCodec::getInstance();
    auto& util_loader = LibAVUtil::getInstance();

    std::vector<Packet> packets;

    AVFrame* av_frame = nullptr;
    AVPtr<AVFrame> raii_frame;

    if (!frame.data.valueless_by_exception()) {
      raii_frame.reset(util_loader.av_frame_alloc());
      av_frame = raii_frame.get();

      if (std::holds_alternative<Picture>(frame.data)) {
        const auto& pic = std::get<Picture>(frame.data);
        av_frame->format = omPixelFormatToAvPixelFormat(pic.format);
        av_frame->width = static_cast<int>(pic.width);
        av_frame->height = static_cast<int>(pic.height);
        av_frame->pts = static_cast<int64_t>(frame.pts);
        av_frame->colorspace = omColorSpaceToAvColorSpace(pic.color_space);
        av_frame->color_trc = omColorTransferToAvTransfer(pic.transfer_char);
        av_frame->color_primaries = omColorPrimariesToAvPrimaries(pic.color_primaries);
        av_frame->color_range = omColorRangeToAvRange(pic.color_range);

        if (pic.mastering_display.has_value) {
          if (auto* sd = util_loader.av_frame_new_side_data(av_frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA, sizeof(AVMasteringDisplayMetadata))) {
            auto* md = reinterpret_cast<AVMasteringDisplayMetadata*>(sd->data);
            for (int i = 0; i < 3; ++i) {
              md->display_primaries[i][0] = av_make_q(pic.mastering_display.display_primaries[i][0], 50000);
              md->display_primaries[i][1] = av_make_q(pic.mastering_display.display_primaries[i][1], 50000);
            }
            md->white_point[0] = av_make_q(pic.mastering_display.white_point[0], 50000);
            md->white_point[1] = av_make_q(pic.mastering_display.white_point[1], 50000);
            md->max_luminance = av_make_q(pic.mastering_display.max_display_mastering_luminance, 10000);
            md->min_luminance = av_make_q(pic.mastering_display.min_display_mastering_luminance, 10000);
            md->has_primaries = 1;
            md->has_luminance = 1;
          }
        }

        if (pic.content_light_level.has_value) {
          if (auto* sd = util_loader.av_frame_new_side_data(av_frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL, sizeof(AVContentLightMetadata))) {
            auto* cll = reinterpret_cast<AVContentLightMetadata*>(sd->data);
            cll->MaxCLL = pic.content_light_level.max_content_light_level;
            cll->MaxFALL = pic.content_light_level.max_pic_average_light_level;

          }
        }

        int num_planes = getNumPlanes(pic.format);
        for (int i = 0; i < num_planes && i < 4; ++i) {
          av_frame->data[i] = pic.planes.data[i];
          av_frame->linesize[i] = static_cast<int>(pic.planes.linesize[i]);
        }
      } else if (std::holds_alternative<AudioSamples>(frame.data)) {
        const auto& samples = std::get<AudioSamples>(frame.data);
        av_frame->format = omSampleFormatToAvSampleFormat(samples.format.sample_format);
        av_frame->nb_samples = static_cast<int>(samples.nb_samples);
        av_frame->ch_layout.nb_channels = static_cast<int>(samples.format.channels);
        av_frame->sample_rate = static_cast<int>(samples.format.sample_rate);
        av_frame->pts = static_cast<int64_t>(frame.pts);

        if (samples.format.planar) {
          for (int i = 0; i < samples.format.channels && i < 8; ++i) {
            av_frame->data[i] = samples.planes.data[i];
          }
        } else {
          av_frame->data[0] = samples.planes.data[0];
        }
      }
    }

    int ret = codec_loader.avcodec_send_frame(codec_ctx_.get(), av_frame);
    if (ret < 0) {
      return Err(avErrorToOmError(ret));
    }

    while (true) {
      ret = codec_loader.avcodec_receive_packet(codec_ctx_.get(), packet_.get());
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        break;
      }
      if (ret < 0) {
        return Err(avErrorToOmError(ret));
      }

      Packet pkt;
      pkt.allocate(static_cast<size_t>(packet_->size));
      std::memcpy(pkt.bytes.data(), packet_->data, packet_->size);
      pkt.pts = packet_->pts;
      pkt.dts = packet_->dts;
      pkt.duration = packet_->duration;
      pkt.is_keyframe = (packet_->flags & AV_PKT_FLAG_KEY) != 0;
      packets.push_back(std::move(pkt));

      codec_loader.av_packet_unref(packet_.get());
    }

    return Ok(std::move(packets));
  }

  auto updateBitrate(const RateControlParams& rc) -> OMError override {
    if (!initialized_ || !codec_ctx_) {
      return OM_COMMON_NOT_INITIALIZED;
    }

    std::visit([&](auto&& p) {
      using T = std::decay_t<decltype(p)>;
      if constexpr (std::is_same_v<T, CbrParams>) {
        codec_ctx_->bit_rate = p.bitrate.target_bitrate;
        codec_ctx_->rc_min_rate = p.bitrate.target_bitrate;
        codec_ctx_->rc_max_rate = p.bitrate.target_bitrate;
      } else if constexpr (std::is_same_v<T, VbrParams>) {
        codec_ctx_->bit_rate = p.bitrate.target_bitrate;
        if (p.bitrate.max_bitrate) {
          codec_ctx_->rc_max_rate = *p.bitrate.max_bitrate;
        }
      } else if constexpr (std::is_same_v<T, AbrParams>) {
        codec_ctx_->bit_rate = p.target_bitrate;
      }
    }, rc.params);

    return OM_SUCCESS;
  }

private:
  void release() {
    auto& util_loader = LibAVUtil::getInstance();
    codec_ctx_.reset();
    packet_.reset();
    if (options_dict_) {
      util_loader.av_dict_free(&options_dict_);
    }
    initialized_ = false;
  }

  AVCodecID av_codec_id_;
  AVPtr<AVCodecContext> codec_ctx_;
  AVPtr<AVPacket> packet_;
  VideoFormat configured_video_format_ = {};
  AVDictionary* options_dict_ = nullptr;
  bool initialized_ = false;
};

static auto avCodecIdToOmCodecId(AVCodecID id) -> OMCodecId {
  switch (id) {
    case AV_CODEC_ID_H264: return OM_CODEC_H264;
    case AV_CODEC_ID_HEVC: return OM_CODEC_H265;
    case AV_CODEC_ID_VVC: return OM_CODEC_H266;
    case AV_CODEC_ID_EVC: return OM_CODEC_EVC;
    case AV_CODEC_ID_VP8: return OM_CODEC_VP8;
    case AV_CODEC_ID_VP9: return OM_CODEC_VP9;
    case AV_CODEC_ID_AV1: return OM_CODEC_AV1;
    case AV_CODEC_ID_MPEG4: return OM_CODEC_MPEG4;
    case AV_CODEC_ID_PRORES: return OM_CODEC_PRORES;
    case AV_CODEC_ID_AAC: return OM_CODEC_AAC;
    case AV_CODEC_ID_MP3: return OM_CODEC_MP3;
    case AV_CODEC_ID_OPUS: return OM_CODEC_OPUS;
    case AV_CODEC_ID_VORBIS: return OM_CODEC_VORBIS;
    case AV_CODEC_ID_FLAC: return OM_CODEC_FLAC;
    case AV_CODEC_ID_PCM_S16LE: return OM_CODEC_PCM_S16LE;
    case AV_CODEC_ID_PCM_F32LE: return OM_CODEC_PCM_F32LE;
    case AV_CODEC_ID_ALAC: return OM_CODEC_ALAC;
    case AV_CODEC_ID_AC3: return OM_CODEC_AC3;
    case AV_CODEC_ID_EAC3: return OM_CODEC_EAC3;
    case AV_CODEC_ID_MJPEG: return OM_CODEC_JPEG;
    case AV_CODEC_ID_PNG: return OM_CODEC_PNG;
    case AV_CODEC_ID_WEBP: return OM_CODEC_WEBP;
    case AV_CODEC_ID_BMP: return OM_CODEC_BMP;
    case AV_CODEC_ID_TIFF: return OM_CODEC_TIFF;
    case AV_CODEC_ID_GIF: return OM_CODEC_GIF;
    case AV_CODEC_ID_TARGA: return OM_CODEC_TGA;
    default: return OM_CODEC_NONE;
  }
}

struct DynamicFFmpegDescriptors {
  std::vector<std::unique_ptr<CodecDescriptor>> descriptors;
  std::vector<std::string> names;
  std::vector<std::string> long_names;
};

static DynamicFFmpegDescriptors FFMPEG_DESCRIPTORS;

void registerFFmpegCodecs(CodecRegistry* registry) noexcept {
  auto& loader = LibAVCodec::getInstance();
  if (!loader.load()) return;

  void* opaque = nullptr;
  while (const AVCodec* codec = loader.av_codec_iterate(&opaque)) {
    OMCodecId om_id = avCodecIdToOmCodecId(codec->id);
    if (om_id == OM_CODEC_NONE) continue;

    auto desc = std::make_unique<CodecDescriptor>();
    desc->codec_id = om_id;

    if (om_id == OM_CODEC_JPEG || om_id == OM_CODEC_PNG || om_id == OM_CODEC_WEBP ||
        om_id == OM_CODEC_BMP || om_id == OM_CODEC_TIFF || om_id == OM_CODEC_GIF ||
        om_id == OM_CODEC_TGA) {
      desc->type = OM_MEDIA_IMAGE;
    } else {
      desc->type = (codec->type == AVMEDIA_TYPE_VIDEO) ? OM_MEDIA_VIDEO :
                   (codec->type == AVMEDIA_TYPE_AUDIO) ? OM_MEDIA_AUDIO :
                   (codec->type == AVMEDIA_TYPE_SUBTITLE) ? OM_MEDIA_SUBTITLE : OM_MEDIA_NONE;
    }
    
    if (desc->type == OM_MEDIA_NONE) continue;

    std::string name = "ffmpeg_";
    name += codec->name;
    FFMPEG_DESCRIPTORS.names.push_back(std::move(name));
    desc->name = FFMPEG_DESCRIPTORS.names.back();

    if (codec->long_name) {
      std::string long_name = codec->long_name;
      long_name += " (FFmpeg)";
      FFMPEG_DESCRIPTORS.long_names.push_back(std::move(long_name));
      desc->long_name = FFMPEG_DESCRIPTORS.long_names.back();
    }

    desc->vendor = "FFmpeg";
    
    if (loader.avcodec_find_decoder(codec->id)) {
      AVCodecID av_id = codec->id;
      desc->decoder_factory = [av_id] {
        return std::make_unique<FFmpegDecoder>(av_id);
      };
    }

    if (loader.avcodec_find_encoder(codec->id)) {
      AVCodecID av_id = codec->id;
      desc->encoder_factory = [av_id] {
        return std::make_unique<FFmpegEncoder>(av_id);
      };
    }
    
    registry->registerCodec(desc.get());
    FFMPEG_DESCRIPTORS.descriptors.push_back(std::move(desc));
  }
}

} // namespace openmedia
