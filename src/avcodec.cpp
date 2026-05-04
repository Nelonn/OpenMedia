#include "avcodec.hpp"
#include <cstring>
#include <memory>
#include <openmedia/codec_api.hpp>
#include <openmedia/codec_registry.hpp>
#include <span>
#include <vector>
#include <codecs.hpp>

namespace openmedia {

namespace {

#if defined(__APPLE__)
static auto normaliseAppleAlacExtradata(OMCodecId codec_id,
                                        std::span<const uint8_t> extradata)
    -> std::vector<uint8_t> {
  if (codec_id != OM_CODEC_ALAC || extradata.size() != 24) {
    return std::vector<uint8_t>(extradata.begin(), extradata.end());
  }

  // FFmpeg's ALAC decoder expects the full 36-byte QuickTime 'alac' atom:
  // size:4, type:4, version/flags:4, config:24.
  std::vector<uint8_t> atom;
  atom.reserve(36);
  atom.insert(atom.end(), {0x00, 0x00, 0x00, 0x24, 'a', 'l', 'a', 'c',
                           0x00, 0x00, 0x00, 0x00});
  atom.insert(atom.end(), extradata.begin(), extradata.end());
  return atom;
}
#endif

} // namespace

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
  library_.open("avcodec-62.dll");
#elif defined(__APPLE__)
  static constexpr const char* kAppleCandidates[] = {
      "/opt/homebrew/lib/libavcodec.dylib",
      "/opt/homebrew/lib/libavcodec.62.dylib",
      "libavcodec-62.dylib",
  };
  for (const char* candidate : kAppleCandidates) {
    library_.open(candidate);
    if (library_.success()) break;
  }
#else
  library_.open("libavcodec-62.so");
#endif
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
  av_packet_alloc = library_.getProcAddress<PFN<AVPacket*()>>("av_packet_alloc");
  av_packet_free = library_.getProcAddress<PFN<void(AVPacket**)>>("av_packet_free");
  av_packet_unref = library_.getProcAddress<PFN<void(AVPacket*)>>("av_packet_unref");
  av_packet_ref = library_.getProcAddress<PFN<int(AVPacket*, const AVPacket*)>>("av_packet_ref");
  av_packet_clone = library_.getProcAddress<PFN<AVPacket*(const AVPacket*)>>("av_packet_clone");
  av_packet_move_ref = library_.getProcAddress<PFN<void(AVPacket*, AVPacket*)>>("av_packet_move_ref");
  av_new_packet = library_.getProcAddress<PFN<int(AVPacket*, int)>>("av_new_packet");
  av_grow_packet = library_.getProcAddress<PFN<int(AVPacket*, int)>>("av_grow_packet");
  av_shrink_packet = library_.getProcAddress<PFN<void(AVPacket*, int)>>("av_shrink_packet");

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

class FFmpegDecoder final : public Decoder {
public:
  explicit FFmpegDecoder(OMCodecId codec_id)
      : codec_id_(codec_id) {}

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

    AVCodecID av_codec_id = AV_CODEC_ID_NONE;
    switch (codec_id_) {
      // Audio codecs
      case OM_CODEC_AAC: av_codec_id = AV_CODEC_ID_AAC; break;
      case OM_CODEC_ALAC: av_codec_id = AV_CODEC_ID_ALAC; break;
      case OM_CODEC_MP3: av_codec_id = AV_CODEC_ID_MP3; break;
      case OM_CODEC_OPUS: av_codec_id = AV_CODEC_ID_OPUS; break;
      case OM_CODEC_VORBIS: av_codec_id = AV_CODEC_ID_VORBIS; break;
      case OM_CODEC_FLAC: av_codec_id = AV_CODEC_ID_FLAC; break;
      case OM_CODEC_PCM_S16LE: av_codec_id = AV_CODEC_ID_PCM_S16LE; break;
      case OM_CODEC_PCM_S32LE: av_codec_id = AV_CODEC_ID_PCM_S32LE; break;
      case OM_CODEC_AC3: av_codec_id = AV_CODEC_ID_AC3; break;
      case OM_CODEC_EAC3: av_codec_id = AV_CODEC_ID_EAC3; break;
      // Video codecs
      case OM_CODEC_H264: av_codec_id = AV_CODEC_ID_H264; break;
      case OM_CODEC_H265: av_codec_id = AV_CODEC_ID_HEVC; break;
      case OM_CODEC_H266: av_codec_id = AV_CODEC_ID_VVC; break;
      case OM_CODEC_EVC: av_codec_id = AV_CODEC_ID_EVC; break;
      case OM_CODEC_VP8: av_codec_id = AV_CODEC_ID_VP8; break;
      case OM_CODEC_VP9: av_codec_id = AV_CODEC_ID_VP9; break;
      case OM_CODEC_AV1: av_codec_id = AV_CODEC_ID_AV1; break;
      case OM_CODEC_MPEG4: av_codec_id = AV_CODEC_ID_MPEG4; break;
      case OM_CODEC_PRORES: av_codec_id = AV_CODEC_ID_PRORES; break;
      default: return OM_CODEC_NOT_SUPPORTED;
    }

    const AVCodec* codec = codec_loader.avcodec_find_decoder(av_codec_id);
    if (!codec) {
      return OM_CODEC_NOT_SUPPORTED;
    }

    auto configure_context = [&](bool minimal) -> OMError {
      codec_ctx_ = codec_loader.avcodec_alloc_context3(codec);
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

        std::span<const uint8_t> extradata = options.extradata;
#if defined(__APPLE__)
        std::vector<uint8_t> normalized_extradata =
            normaliseAppleAlacExtradata(codec_id_, options.extradata);
        extradata = normalized_extradata;
#endif
        if (!extradata.empty()) {
          codec_ctx_->extradata_size = static_cast<int>(extradata.size());
          codec_ctx_->extradata = static_cast<uint8_t*>(
              util_loader.av_malloc(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
          if (!codec_ctx_->extradata) {
            return OM_COMMON_OUT_OF_MEMORY;
          }
          memcpy(codec_ctx_->extradata, extradata.data(), extradata.size());
          memset(codec_ctx_->extradata + extradata.size(), 0, AV_INPUT_BUFFER_PADDING_SIZE);
        }
      }

      const int ret = codec_loader.avcodec_open2(codec_ctx_, codec, nullptr);
      if (ret < 0) {
        codec_loader.avcodec_free_context(&codec_ctx_);
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

    frame_ = util_loader.av_frame_alloc();
    packet_ = codec_loader.av_packet_alloc();

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
    int ret = codec_loader.av_new_packet(packet_, static_cast<int>(pkt_data.size()));
    if (ret < 0) {
      return Err(avErrorToOmError(ret));
    }
    memcpy(packet_->data, pkt_data.data(), pkt_data.size());
    packet_->pts = packet.pts;
    packet_->dts = packet.dts;
    packet_->stream_index = packet.stream_index;

    ret = codec_loader.avcodec_send_packet(codec_ctx_, packet_);
    if (ret < 0) {
      codec_loader.av_packet_unref(packet_);
      return Err(avErrorToOmError(ret));
    }
    codec_loader.av_packet_unref(packet_);

    while (true) {
      int ret = codec_loader.avcodec_receive_frame(codec_ctx_, frame_);
      if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
        break;
      }
      if (ret < 0) {
        return Err(avErrorToOmError(ret));
      }

      auto frame = convertAVFrameToFrame(frame_);
      if (frame.has_value()) {
        frames.push_back(std::move(*frame));
      }

      util_loader.av_frame_unref(frame_);
    }

    return Ok(std::move(frames));
  }

  void flush() override {
    if (initialized_ && codec_ctx_) {
      auto& codec_loader = LibAVCodec::getInstance();
      if (codec_loader.avcodec_flush_buffers) {
        codec_loader.avcodec_flush_buffers(codec_ctx_);
      }
    }
  }

private:
  auto convertAVFrameToFrame(AVFrame* av_frame) -> std::optional<Frame> {
    if (!av_frame) return std::nullopt;

    auto& util = LibAVUtil::getInstance();

    Frame frame;
    frame.pts = (av_frame->pts != AV_NOPTS_VALUE) ? static_cast<uint64_t>(av_frame->pts) : 0;
    frame.dts = (av_frame->pkt_dts != AV_NOPTS_VALUE) ? static_cast<uint64_t>(av_frame->pkt_dts) : 0;

    if (codec_ctx_->codec_type == AVMEDIA_TYPE_VIDEO) {
      // Video frame conversion
      Picture picture;
      picture.format = avPixelFormatToOmPixelFormat(static_cast<AVPixelFormat>(av_frame->format));
      picture.width = static_cast<uint32_t>(av_frame->width);
      picture.height = static_cast<uint32_t>(av_frame->height);
      picture.color_space = avColorSpaceToOmColorSpace(av_frame->colorspace);
      picture.transfer_char = avColorTransferToOmTransfer(av_frame->color_trc);
      picture.is_keyframe = (av_frame->flags & AV_FRAME_FLAG_KEY) != 0;
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

    } else if (codec_ctx_->codec_type == AVMEDIA_TYPE_AUDIO) {
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
    auto& util = LibAVUtil::getInstance();
    auto& codec_loader = LibAVCodec::getInstance();

    if (codec_ctx_) {
      codec_loader.avcodec_free_context(&codec_ctx_);
      codec_ctx_ = nullptr;
    }
    if (frame_) {
      util.av_frame_free(&frame_);
      frame_ = nullptr;
    }
    if (packet_) {
      codec_loader.av_packet_free(&packet_);
      packet_ = nullptr;
    }
    initialized_ = false;
  }

  OMCodecId codec_id_;
  AVCodecContext* codec_ctx_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* packet_ = nullptr;
  bool initialized_ = false;
};

const CodecDescriptor CODEC_FFMPEG_H264 = {
    .codec_id = OM_CODEC_H264,
    .type = OM_MEDIA_VIDEO,
    .name = "ffmpeg_h264",
    .long_name = "H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10 (FFmpeg)",
    .vendor = "FFmpeg",
    .flags = CodecFlags::NONE,
    .decoder_factory = [] { return std::make_unique<FFmpegDecoder>(OM_CODEC_H264); },
};

const CodecDescriptor CODEC_FFMPEG_H265 = {
    .codec_id = OM_CODEC_H265,
    .type = OM_MEDIA_VIDEO,
    .name = "ffmpeg_h265",
    .long_name = "HEVC (High Efficiency Video Coding) (FFmpeg)",
    .vendor = "FFmpeg",
    .flags = CodecFlags::NONE,
    .decoder_factory = [] { return std::make_unique<FFmpegDecoder>(OM_CODEC_H265); },
};

const CodecDescriptor CODEC_FFMPEG_H266 = {
    .codec_id = OM_CODEC_H266,
    .type = OM_MEDIA_VIDEO,
    .name = "ffmpeg_h266",
    .long_name = "VVC (Versatile Video Coding) (FFmpeg)",
    .vendor = "FFmpeg",
    .flags = CodecFlags::NONE,
    .decoder_factory = [] { return std::make_unique<FFmpegDecoder>(OM_CODEC_H266); },
};

const CodecDescriptor CODEC_FFMPEG_EVC = {
    .codec_id = OM_CODEC_EVC,
    .type = OM_MEDIA_VIDEO,
    .name = "ffmpeg_evc",
    .long_name = "EVC (Essential Video Coding) (FFmpeg)",
    .vendor = "FFmpeg",
    .flags = CodecFlags::NONE,
    .decoder_factory = [] { return std::make_unique<FFmpegDecoder>(OM_CODEC_EVC); },
};

const CodecDescriptor CODEC_FFMPEG_VP8 = {
    .codec_id = OM_CODEC_VP8,
    .type = OM_MEDIA_VIDEO,
    .name = "ffmpeg_vp8",
    .long_name = "Google VP8 (FFmpeg)",
    .vendor = "FFmpeg",
    .flags = CodecFlags::NONE,
    .decoder_factory = [] { return std::make_unique<FFmpegDecoder>(OM_CODEC_VP8); },
};

const CodecDescriptor CODEC_FFMPEG_VP9 = {
    .codec_id = OM_CODEC_VP9,
    .type = OM_MEDIA_VIDEO,
    .name = "ffmpeg_vp9",
    .long_name = "Google VP9 (FFmpeg)",
    .vendor = "FFmpeg",
    .flags = CodecFlags::NONE,
    .decoder_factory = [] { return std::make_unique<FFmpegDecoder>(OM_CODEC_VP9); },
};

const CodecDescriptor CODEC_FFMPEG_AV1 = {
    .codec_id = OM_CODEC_AV1,
    .type = OM_MEDIA_VIDEO,
    .name = "ffmpeg_av1",
    .long_name = "Alliance for Open Media AV1 (FFmpeg)",
    .vendor = "FFmpeg",
    .flags = CodecFlags::NONE,
    .decoder_factory = [] { return std::make_unique<FFmpegDecoder>(OM_CODEC_AV1); },
};

const CodecDescriptor CODEC_FFMPEG_PRORES = {
    .codec_id = OM_CODEC_PRORES,
    .type = OM_MEDIA_VIDEO,
    .name = "ffmpeg_prores",
    .long_name = "Apple ProRes (FFmpeg)",
    .vendor = "FFmpeg",
    .flags = CodecFlags::NONE,
    .decoder_factory = [] { return std::make_unique<FFmpegDecoder>(OM_CODEC_PRORES); },
};

const CodecDescriptor CODEC_FFMPEG_AAC = {
  .codec_id = OM_CODEC_AAC,
  .type = OM_MEDIA_AUDIO,
  .name = "ffmpeg_aac",
  .long_name = "AAC (Advanced Audio Coding) (FFmpeg)",
    .vendor = "FFmpeg",
    .flags = CodecFlags::NONE,
  .decoder_factory = [] { return std::make_unique<FFmpegDecoder>(OM_CODEC_AAC); },
};

const CodecDescriptor CODEC_FFMPEG_ALAC = {
    .codec_id = OM_CODEC_ALAC,
    .type = OM_MEDIA_AUDIO,
    .name = "ffmpeg_alac",
    .long_name = "ALAC (Apple Lossless Audio Codec) (FFmpeg)",
    .vendor = "FFmpeg",
    .flags = CodecFlags::NONE,
    .decoder_factory = [] { return std::make_unique<FFmpegDecoder>(OM_CODEC_ALAC); },
};

const CodecDescriptor CODEC_FFMPEG_MP3 = {
    .codec_id = OM_CODEC_MP3,
    .type = OM_MEDIA_AUDIO,
    .name = "ffmpeg_mp3",
    .long_name = "MP3 (MPEG audio layer 3) (FFmpeg)",
    .vendor = "FFmpeg",
    .flags = CodecFlags::NONE,
    .decoder_factory = [] { return std::make_unique<FFmpegDecoder>(OM_CODEC_MP3); },
};

const CodecDescriptor CODEC_FFMPEG_OPUS = {
    .codec_id = OM_CODEC_OPUS,
    .type = OM_MEDIA_AUDIO,
    .name = "ffmpeg_opus",
    .long_name = "Opus (FFmpeg)",
    .vendor = "FFmpeg",
    .flags = CodecFlags::NONE,
    .decoder_factory = [] { return std::make_unique<FFmpegDecoder>(OM_CODEC_OPUS); },
};

const CodecDescriptor CODEC_FFMPEG_VORBIS = {
    .codec_id = OM_CODEC_VORBIS,
    .type = OM_MEDIA_AUDIO,
    .name = "ffmpeg_vorbis",
    .long_name = "Vorbis (FFmpeg)",
    .vendor = "FFmpeg",
    .flags = CodecFlags::NONE,
    .decoder_factory = [] { return std::make_unique<FFmpegDecoder>(OM_CODEC_VORBIS); },
};

const CodecDescriptor CODEC_FFMPEG_FLAC = {
    .codec_id = OM_CODEC_FLAC,
    .type = OM_MEDIA_AUDIO,
    .name = "ffmpeg_flac",
    .long_name = "FLAC (Free Lossless Audio Codec) (FFmpeg)",
    .vendor = "FFmpeg",
    .flags = CodecFlags::NONE,
    .decoder_factory = [] { return std::make_unique<FFmpegDecoder>(OM_CODEC_FLAC); },
};

} // namespace openmedia
