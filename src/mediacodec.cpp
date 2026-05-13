#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <openmedia/codec_api.hpp>
#include <openmedia/video.hpp>
#include <openmedia/audio.hpp>
#include <codecs.hpp>
#include <cstring>
#include <algorithm>

namespace openmedia {

static auto codecIdToMime(OMCodecId codec_id) -> const char* {
  switch (codec_id) {
    case OM_CODEC_H264: return "video/avc";
    case OM_CODEC_H265: return "video/hevc";
    case OM_CODEC_VP8: return "video/x-vnd.on2.vp8";
    case OM_CODEC_VP9: return "video/x-vnd.on2.vp9";
    case OM_CODEC_AV1: return "video/av01";
    case OM_CODEC_AAC: return "audio/mp4a-latm";
    case OM_CODEC_MP3: return "audio/mpeg";
    case OM_CODEC_OPUS: return "audio/opus";
    case OM_CODEC_VORBIS: return "audio/vorbis";
    case OM_CODEC_FLAC: return "audio/flac";
    default: return nullptr;
  }
}

static auto androidColorFormatToOM(int32_t color_format) -> OMPixelFormat {
  switch (color_format) {
    case 19: return OM_FORMAT_NV12; // COLOR_FormatYUV420SemiPlanar
    case 21: return OM_FORMAT_YUV420P; // COLOR_FormatYUV420Planar
    case 39: return OM_FORMAT_YUV420P; // COLOR_FormatYUV420PackedPlanar
    case 0x7f420888: return OM_FORMAT_YUV420P; // COLOR_FormatYUV420Flexible
    default: return OM_FORMAT_UNKNOWN;
  }
}

class MediaCodecDecoder final : public Decoder {
  AMediaCodec* codec_ = nullptr;
  AMediaFormat* format_ = nullptr;
  bool started_ = false;
  bool input_eof_ = false;

  VideoFormat video_format_ = {};
  AudioFormat audio_format_ = {};
  OMMediaType type_ = OM_MEDIA_NONE;
  int32_t color_format_ = 0;
  int32_t stride_ = 0;
  int32_t slice_height_ = 0;

public:
  MediaCodecDecoder() = default;
  ~MediaCodecDecoder() override {
    if (codec_) {
      if (started_) AMediaCodec_stop(codec_);
      AMediaCodec_delete(codec_);
    }
    if (format_) AMediaFormat_delete(format_);
  }

  auto configure(const DecoderOptions& options) -> OMError override {
    const char* mime = codecIdToMime(options.format.codec_id);
    if (!mime) return OM_CODEC_NOT_FOUND;

    codec_ = AMediaCodec_createDecoderByType(mime);
    if (!codec_) return OM_CODEC_OPEN_FAILED;

    format_ = AMediaFormat_new();
    AMediaFormat_setString(format_, AMEDIAFORMAT_KEY_MIME, mime);
    type_ = options.format.type;
    if (type_ == OM_MEDIA_VIDEO) {
      AMediaFormat_setInt32(format_, AMEDIAFORMAT_KEY_WIDTH, options.format.video.width);
      AMediaFormat_setInt32(format_, AMEDIAFORMAT_KEY_HEIGHT, options.format.video.height);
      video_format_.width = options.format.video.width;
      video_format_.height = options.format.video.height;
    } else if (type_ == OM_MEDIA_AUDIO) {
      AMediaFormat_setInt32(format_, AMEDIAFORMAT_KEY_SAMPLE_RATE, options.format.audio.sample_rate);
      AMediaFormat_setInt32(format_, AMEDIAFORMAT_KEY_CHANNEL_COUNT, options.format.audio.channels);
      audio_format_.sample_rate = options.format.audio.sample_rate;
      audio_format_.channels = options.format.audio.channels;
    }

    if (!options.extradata.empty()) {
      AMediaFormat_setBuffer(format_, "csd-0", options.extradata.data(), options.extradata.size());
    }

    media_status_t status = AMediaCodec_configure(codec_, format_, nullptr, nullptr, 0);
    if (status != AMEDIA_OK) return OM_CODEC_OPEN_FAILED;

    status = AMediaCodec_start(codec_);
    if (status != AMEDIA_OK) return OM_CODEC_OPEN_FAILED;

    started_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    DecodingInfo info;
    info.media_type = type_;
    if (type_ == OM_MEDIA_VIDEO) info.video_format = video_format_;
    else if (type_ == OM_MEDIA_AUDIO) info.audio_format = audio_format_;
    return info;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    if (!codec_) return Err(OM_COMMON_NOT_INITIALIZED);

    if (packet.buffer) {
      ssize_t buf_idx = AMediaCodec_dequeueInputBuffer(codec_, 1000);
      if (buf_idx >= 0) {
        size_t buf_size;
        uint8_t* buf = AMediaCodec_getInputBuffer(codec_, buf_idx, &buf_size);
        size_t to_copy = std::min(buf_size, packet.bytes.size());
        memcpy(buf, packet.bytes.data(), to_copy);
        AMediaCodec_queueInputBuffer(codec_, buf_idx, 0, to_copy, packet.pts, 0);
      }
    } else {
      ssize_t buf_idx = AMediaCodec_dequeueInputBuffer(codec_, 1000);
      if (buf_idx >= 0) {
        AMediaCodec_queueInputBuffer(codec_, buf_idx, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
        input_eof_ = true;
      }
    }

    std::vector<Frame> frames;
    AMediaCodecBufferInfo info;
    ssize_t out_idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 1000);
    while (out_idx >= 0 || out_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      if (out_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        AMediaFormat* new_format = AMediaCodec_getOutputFormat(codec_);
        if (type_ == OM_MEDIA_VIDEO) {
          AMediaFormat_getInt32(new_format, AMEDIAFORMAT_KEY_WIDTH, (int32_t*)&video_format_.width);
          AMediaFormat_getInt32(new_format, AMEDIAFORMAT_KEY_HEIGHT, (int32_t*)&video_format_.height);
          AMediaFormat_getInt32(new_format, AMEDIAFORMAT_KEY_COLOR_FORMAT, &color_format_);
          if (!AMediaFormat_getInt32(new_format, AMEDIAFORMAT_KEY_STRIDE, &stride_)) {
              stride_ = video_format_.width;
          }
          if (!AMediaFormat_getInt32(new_format, "slice-height", &slice_height_)) {
              slice_height_ = video_format_.height;
          }
          video_format_.format = androidColorFormatToOM(color_format_);
        } else if (type_ == OM_MEDIA_AUDIO) {
          AMediaFormat_getInt32(new_format, AMEDIAFORMAT_KEY_SAMPLE_RATE, (int32_t*)&audio_format_.sample_rate);
          AMediaFormat_getInt32(new_format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, (int32_t*)&audio_format_.channels);
        }
        AMediaFormat_delete(new_format);
      } else {
        if (info.size > 0) {
          Frame frame;
          frame.pts = info.presentationTimeUs;
          frame.dts = frame.pts;

          size_t out_buf_size;
          uint8_t* out_buf = AMediaCodec_getOutputBuffer(codec_, out_idx, &out_buf_size);

          if (type_ == OM_MEDIA_VIDEO) {
            Picture pic(video_format_.format, video_format_.width, video_format_.height);
            if (video_format_.format == OM_FORMAT_NV12) {
              uint8_t* dst_y = pic.planes.data[0];
              uint8_t* dst_uv = pic.planes.data[1];
              uint8_t* src_y = out_buf + info.offset;
              uint8_t* src_uv = src_y + stride_ * slice_height_;

              for (uint32_t y = 0; y < video_format_.height; ++y) {
                  memcpy(dst_y + y * pic.planes.linesize[0], src_y + y * stride_, video_format_.width);
              }
              for (uint32_t y = 0; y < (video_format_.height + 1) / 2; ++y) {
                  memcpy(dst_uv + y * pic.planes.linesize[1], src_uv + y * stride_, video_format_.width);
              }
            } else if (video_format_.format == OM_FORMAT_YUV420P) {
                uint8_t* dst_y = pic.planes.data[0];
                uint8_t* dst_u = pic.planes.data[1];
                uint8_t* dst_v = pic.planes.data[2];
                uint8_t* src_y = out_buf + info.offset;
                uint8_t* src_u = src_y + stride_ * slice_height_;
                uint8_t* src_v = src_u + (stride_ / 2) * (slice_height_ / 2);

                for (uint32_t y = 0; y < video_format_.height; ++y) {
                    memcpy(dst_y + y * pic.planes.linesize[0], src_y + y * stride_, video_format_.width);
                }
                for (uint32_t y = 0; y < (video_format_.height + 1) / 2; ++y) {
                    memcpy(dst_u + y * pic.planes.linesize[1], src_u + y * stride_ / 2, (video_format_.width + 1) / 2);
                    memcpy(dst_v + y * pic.planes.linesize[2], src_v + y * stride_ / 2, (video_format_.width + 1) / 2);
                }
            }
            frame.data = std::move(pic);
          } else if (type_ == OM_MEDIA_AUDIO) {
            AudioSamples samples(audio_format_, info.size / (audio_format_.channels * getBytesPerSample(audio_format_.sample_format)));
            memcpy(samples.buffer->bytes().data(), out_buf + info.offset, info.size);
            frame.data = std::move(samples);
          }
          frames.push_back(std::move(frame));
        }
        AMediaCodec_releaseOutputBuffer(codec_, out_idx, false);
      }
      out_idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
    }

    return Ok(std::move(frames));
  }

  void flush() override {
    if (codec_ && started_) {
      AMediaCodec_flush(codec_);
      input_eof_ = false;
    }
  }
};

class MediaCodecEncoder final : public Encoder {
  AMediaCodec* codec_ = nullptr;
  AMediaFormat* format_ = nullptr;
  bool started_ = false;

public:
  MediaCodecEncoder() = default;
  ~MediaCodecEncoder() override {
    if (codec_) {
      if (started_) AMediaCodec_stop(codec_);
      AMediaCodec_delete(codec_);
    }
    if (format_) AMediaFormat_delete(format_);
  }

  auto configure(const EncoderOptions& options) -> OMError override {
    const char* mime = codecIdToMime(options.format.codec_id);
    if (!mime) return OM_CODEC_NOT_FOUND;

    codec_ = AMediaCodec_createEncoderByType(mime);
    if (!codec_) return OM_CODEC_OPEN_FAILED;

    format_ = AMediaFormat_new();
    AMediaFormat_setString(format_, AMEDIAFORMAT_KEY_MIME, mime);
    if (options.format.type == OM_MEDIA_VIDEO) {
      AMediaFormat_setInt32(format_, AMEDIAFORMAT_KEY_WIDTH, options.format.video.width);
      AMediaFormat_setInt32(format_, AMEDIAFORMAT_KEY_HEIGHT, options.format.video.height);
      AMediaFormat_setInt32(format_, AMEDIAFORMAT_KEY_COLOR_FORMAT, 21); // COLOR_FormatYUV420Planar
      AMediaFormat_setFloat(format_, AMEDIAFORMAT_KEY_FRAME_RATE, (float)options.format.video.framerate.num / options.format.video.framerate.den);
      AMediaFormat_setInt32(format_, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
    } else if (options.format.type == OM_MEDIA_AUDIO) {
      AMediaFormat_setInt32(format_, AMEDIAFORMAT_KEY_SAMPLE_RATE, options.format.audio.sample_rate);
      AMediaFormat_setInt32(format_, AMEDIAFORMAT_KEY_CHANNEL_COUNT, options.format.audio.channels);
    }

    updateBitrate(options.rate_control);

    media_status_t status = AMediaCodec_configure(codec_, format_, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE);
    if (status != AMEDIA_OK) return OM_CODEC_OPEN_FAILED;

    status = AMediaCodec_start(codec_);
    if (status != AMEDIA_OK) return OM_CODEC_OPEN_FAILED;

    started_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> EncodingInfo override {
    EncodingInfo info;
    AMediaFormat* out_format = AMediaCodec_getOutputFormat(codec_);
    uint8_t* csd;
    size_t csd_size;
    if (AMediaFormat_getBuffer(out_format, "csd-0", (void**)&csd, &csd_size)) {
      info.extradata.assign(csd, csd + csd_size);
    }
    if (AMediaFormat_getBuffer(out_format, "csd-1", (void**)&csd, &csd_size)) {
      info.extradata.insert(info.extradata.end(), csd, csd + csd_size);
    }
    AMediaFormat_delete(out_format);
    return info;
  }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    if (!codec_) return Err(OM_COMMON_NOT_INITIALIZED);

    if (const auto* pic = std::get_if<Picture>(&frame.data)) {
      ssize_t buf_idx = AMediaCodec_dequeueInputBuffer(codec_, 1000);
      if (buf_idx >= 0) {
        size_t buf_size;
        uint8_t* buf = AMediaCodec_getInputBuffer(codec_, buf_idx, &buf_size);
        // Copy YUV data to buf
        size_t offset = 0;
        for (uint32_t i = 0; i < getNumPlanes(pic->format); ++i) {
          auto dims = pic->getPlaneDimensions(i);
          uint32_t stride = pic->planes.linesize[i];
          uint32_t width = dims.first * getBytesPerPixel(pic->format, i);
          for (uint32_t y = 0; y < dims.second; ++y) {
            memcpy(buf + offset, pic->planes.data[i] + y * stride, width);
            offset += width;
          }
        }
        AMediaCodec_queueInputBuffer(codec_, buf_idx, 0, offset, frame.pts, 0);
      }
    } else if (const auto* samples = std::get_if<AudioSamples>(&frame.data)) {
       ssize_t buf_idx = AMediaCodec_dequeueInputBuffer(codec_, 1000);
       if (buf_idx >= 0) {
         size_t buf_size;
         uint8_t* buf = AMediaCodec_getInputBuffer(codec_, buf_idx, &buf_size);
         size_t to_copy = std::min(buf_size, samples->buffer->bytes().size());
         memcpy(buf, samples->buffer->bytes().data(), to_copy);
         AMediaCodec_queueInputBuffer(codec_, buf_idx, 0, to_copy, frame.pts, 0);
       }
    }

    std::vector<Packet> packets;
    AMediaCodecBufferInfo info;
    ssize_t out_idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 1000);
    while (out_idx >= 0) {
      if (info.size > 0) {
        Packet packet;
        packet.allocate(info.size);
        size_t out_buf_size;
        uint8_t* out_buf = AMediaCodec_getOutputBuffer(codec_, out_idx, &out_buf_size);
        memcpy(packet.bytes.data(), out_buf + info.offset, info.size);
        packet.pts = info.presentationTimeUs;
        packet.dts = packet.pts;
        packet.is_keyframe = (info.flags & AMEDIACODEC_BUFFER_FLAG_KEY_FRAME) != 0;
        packets.push_back(std::move(packet));
      }
      AMediaCodec_releaseOutputBuffer(codec_, out_idx, false);
      out_idx = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
    }
    return Ok(std::move(packets));
  }

  auto updateBitrate(const RateControlParams& rc) -> OMError override {
    int32_t bitrate = 0;
    std::visit([&bitrate]<typename T0>(T0&& p) {
        using T = std::decay_t<T0>;
        if constexpr (requires { p.bitrate.target_bitrate; }) {
            bitrate = (int32_t)p.bitrate.target_bitrate;
        } else if constexpr (requires { p.target_bitrate; }) {
            bitrate = (int32_t)p.target_bitrate;
        }
    }, rc.params);

    if (bitrate > 0) {
        AMediaFormat_setInt32(format_, AMEDIAFORMAT_KEY_BIT_RATE, bitrate);
        // AMediaCodec_setParameters is API 26+
        /*if (started_) {
            AMediaFormat* params = AMediaFormat_new();
            AMediaFormat_setInt32(params, AMEDIAFORMAT_KEY_BIT_RATE, bitrate);
            AMediaCodec_setParameters(codec_, params);
            AMediaFormat_delete(params);
        }*/
    }
    return OM_SUCCESS;
  }
};

#define DEFINE_MEDIACODEC_CODEC(id, type_val, name_str, long_name_str) \
  const CodecDescriptor CODEC_MEDIACODEC_##id = { \
    .codec_id = OM_CODEC_##id, \
    .type = type_val, \
    .name = "mediacodec_" name_str, \
    .long_name = "MediaCodec " long_name_str, \
    .flags = HARDWARE, \
    .decoder_factory = []() { return std::make_unique<MediaCodecDecoder>(); }, \
    .encoder_factory = []() { return std::make_unique<MediaCodecEncoder>(); } \
  }

DEFINE_MEDIACODEC_CODEC(H264, OM_MEDIA_VIDEO, "h264", "H.264");
DEFINE_MEDIACODEC_CODEC(H265, OM_MEDIA_VIDEO, "h265", "H.265 (HEVC)");
DEFINE_MEDIACODEC_CODEC(VP8, OM_MEDIA_VIDEO, "vp8", "VP8");
DEFINE_MEDIACODEC_CODEC(VP9, OM_MEDIA_VIDEO, "vp9", "VP9");
DEFINE_MEDIACODEC_CODEC(AV1, OM_MEDIA_VIDEO, "av1", "AV1");
DEFINE_MEDIACODEC_CODEC(AAC, OM_MEDIA_AUDIO, "aac", "AAC");
DEFINE_MEDIACODEC_CODEC(MP3, OM_MEDIA_AUDIO, "mp3", "MP3");
DEFINE_MEDIACODEC_CODEC(OPUS, OM_MEDIA_AUDIO, "opus", "Opus");
DEFINE_MEDIACODEC_CODEC(VORBIS, OM_MEDIA_AUDIO, "vorbis", "Vorbis");
DEFINE_MEDIACODEC_CODEC(FLAC, OM_MEDIA_AUDIO, "flac", "FLAC");

} // namespace openmedia
