#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <openmedia/codec_api.hpp>
#include <codecs.hpp>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <utility>

namespace openmedia {

namespace {

constexpr int64_t WAIT_US = 10000;
constexpr int DRAIN_ATTEMPTS = 500;
constexpr int32_t YUV420_PLANAR = 19;
constexpr int32_t YUV420_SEMI_PLANAR = 21;
constexpr int32_t PCM16_BIT = 2;
constexpr int32_t PCM_FLOAT = 4;

auto codecIdToMime(OMCodecId id) -> const char* {
  switch (id) {
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

auto validBufferInfo(const AMediaCodecBufferInfo& info) -> bool {
  return info.offset >= 0 && info.size >= 0;
}

// Check every row against the valid payload before reading it. The size
// returned by AMediaCodec_getOutputBuffer is unreliable before API 36.
auto copyRows(uint8_t* dst, size_t dst_stride, const uint8_t* src,
              size_t src_offset, size_t src_stride, size_t width,
              size_t rows, size_t payload_size) -> bool {
  if (width > dst_stride || width > src_stride || !src_stride ||
      src_offset > payload_size)
    return false;
  if (rows && (width > payload_size - src_offset ||
               rows - 1 > (payload_size - src_offset - width) / src_stride))
    return false;
  for (size_t y = 0; y < rows; ++y)
    std::memcpy(dst + y * dst_stride, src + src_offset + y * src_stride, width);
  return true;
}

} // namespace

class MediaCodecDecoder final : public Decoder {
  AMediaCodec* codec_ = nullptr;
  AMediaFormat* format_ = nullptr;
  bool started_ = false;
  bool input_eof_ = false;
  bool output_eof_ = false;
  std::vector<Frame> pending_eos_frames_;

  VideoFormat video_format_ = {};
  AudioFormat audio_format_ = {};
  OMMediaType type_ = OM_MEDIA_NONE;
  int32_t stride_ = 0;
  int32_t slice_height_ = 0;

  void close() {
    if (codec_) {
      if (started_) AMediaCodec_stop(codec_);
      AMediaCodec_delete(codec_);
      codec_ = nullptr;
    }
    if (format_) {
      AMediaFormat_delete(format_);
      format_ = nullptr;
    }
    started_ = input_eof_ = output_eof_ = false;
    pending_eos_frames_.clear();
    resetReceiveState();
  }

  auto updateOutputFormat() -> OMError {
    AMediaFormat* output = AMediaCodec_getOutputFormat(codec_);
    if (!output) return OM_CODEC_DECODE_FAILED;
    OMError result = OM_SUCCESS;
    if (type_ == OM_MEDIA_VIDEO) {
      int32_t width = 0, height = 0, color = 0;
      if (!AMediaFormat_getInt32(output, AMEDIAFORMAT_KEY_WIDTH, &width) ||
          !AMediaFormat_getInt32(output, AMEDIAFORMAT_KEY_HEIGHT, &height) ||
          !AMediaFormat_getInt32(output, AMEDIAFORMAT_KEY_COLOR_FORMAT, &color) ||
          width <= 0 || height <= 0) {
        result = OM_CODEC_DECODE_FAILED;
      } else if (color != YUV420_PLANAR && color != YUV420_SEMI_PLANAR) {
        // Packed and flexible layouts cannot be interpreted as flat I420/NV12.
        result = OM_CODEC_NOT_SUPPORTED;
      } else {
        video_format_.width = static_cast<uint32_t>(width);
        video_format_.height = static_cast<uint32_t>(height);
        video_format_.format = color == YUV420_PLANAR ? OM_FORMAT_YUV420P : OM_FORMAT_NV12;
        if (!AMediaFormat_getInt32(output, AMEDIAFORMAT_KEY_STRIDE, &stride_))
          stride_ = width;
        if (!AMediaFormat_getInt32(output, "slice-height", &slice_height_))
          slice_height_ = height;
        if (stride_ < width || slice_height_ < height)
          result = OM_CODEC_DECODE_FAILED;
      }
    } else {
      int32_t rate = 0, channels = 0, pcm = PCM16_BIT;
      if (!AMediaFormat_getInt32(output, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate) ||
          !AMediaFormat_getInt32(output, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channels) ||
          rate <= 0 || channels <= 0) {
        result = OM_CODEC_DECODE_FAILED;
      } else {
        AMediaFormat_getInt32(output, AMEDIAFORMAT_KEY_PCM_ENCODING, &pcm);
        if (pcm != PCM16_BIT && pcm != PCM_FLOAT) {
          result = OM_CODEC_NOT_SUPPORTED;
        } else {
          audio_format_.sample_rate = static_cast<uint32_t>(rate);
          audio_format_.channels = static_cast<uint32_t>(channels);
          audio_format_.sample_format = pcm == PCM16_BIT ? OM_SAMPLE_S16 : OM_SAMPLE_F32;
          audio_format_.bits_per_sample = pcm == PCM16_BIT ? 16 : 32;
          audio_format_.planar = false;
        }
      }
    }
    AMediaFormat_delete(output);
    return result;
  }

  auto appendOutput(std::vector<Frame>& frames, ssize_t index,
                    const AMediaCodecBufferInfo& info) -> OMError {
    if (!validBufferInfo(info)) return OM_CODEC_DECODE_FAILED;
    if (info.size == 0 || (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG))
      return OM_SUCCESS;

    size_t capacity = 0;
    const uint8_t* buffer = AMediaCodec_getOutputBuffer(codec_, index, &capacity);
    if (!buffer) return OM_CODEC_DECODE_FAILED;
    const uint8_t* payload = buffer + info.offset;
    const size_t size = static_cast<size_t>(info.size);
    Frame frame;
    frame.pts = frame.dts = info.presentationTimeUs;

    if (type_ == OM_MEDIA_VIDEO) {
      if (video_format_.format != OM_FORMAT_YUV420P &&
          video_format_.format != OM_FORMAT_NV12) {
        OMError status = updateOutputFormat();
        if (status != OM_SUCCESS) return status;
      }
      const size_t width = video_format_.width;
      const size_t height = video_format_.height;
      const size_t stride = static_cast<size_t>(stride_);
      const size_t slice = static_cast<size_t>(slice_height_);
      if (!stride || !slice || stride > SIZE_MAX / slice)
        return OM_CODEC_DECODE_FAILED;
      const size_t chroma_start = stride * slice;
      if (chroma_start > size) return OM_CODEC_DECODE_FAILED;
      Picture picture(video_format_.format, video_format_.width, video_format_.height);
      if (!copyRows(picture.planes.data[0], picture.planes.linesize[0],
                    payload, 0, stride, width, height, size))
        return OM_CODEC_DECODE_FAILED;
      const size_t chroma_rows = (height + 1) / 2;
      if (video_format_.format == OM_FORMAT_NV12) {
        if (!copyRows(picture.planes.data[1], picture.planes.linesize[1],
                      payload, chroma_start, stride, width, chroma_rows, size))
          return OM_CODEC_DECODE_FAILED;
      } else {
        const size_t chroma_stride = (stride + 1) / 2;
        const size_t chroma_slice = (slice + 1) / 2;
        if (chroma_stride > (size - chroma_start) / chroma_slice)
          return OM_CODEC_DECODE_FAILED;
        const size_t v_start = chroma_start + chroma_stride * chroma_slice;
        const size_t chroma_width = (width + 1) / 2;
        if (!copyRows(picture.planes.data[1], picture.planes.linesize[1],
                      payload, chroma_start, chroma_stride, chroma_width,
                      chroma_rows, size) ||
            !copyRows(picture.planes.data[2], picture.planes.linesize[2],
                      payload, v_start, chroma_stride, chroma_width,
                      chroma_rows, size))
          return OM_CODEC_DECODE_FAILED;
      }
      frame.data = std::move(picture);
    } else {
      const size_t bytes_per_sample = getBytesPerSample(audio_format_.sample_format);
      const size_t channels = audio_format_.channels;
      if (!bytes_per_sample || !channels || channels > SIZE_MAX / bytes_per_sample ||
          size % (channels * bytes_per_sample) ||
          size / (channels * bytes_per_sample) > UINT32_MAX)
        return OM_CODEC_DECODE_FAILED;
      AudioSamples samples(audio_format_,
                           static_cast<uint32_t>(size / (channels * bytes_per_sample)));
      std::memcpy(samples.buffer->bytes().data(), payload, size);
      frame.data = std::move(samples);
    }
    frames.push_back(std::move(frame));
    return OM_SUCCESS;
  }

  auto drainOutput(std::vector<Frame>& frames, int64_t wait_us) -> OMError {
    AMediaCodecBufferInfo info = {};
    ssize_t index = AMediaCodec_dequeueOutputBuffer(codec_, &info, wait_us);
    while (index != AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
      if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        OMError status = updateOutputFormat();
        if (status != OM_SUCCESS) return status;
      } else if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
        // Buffer addresses are acquired afresh for each output.
      } else if (index >= 0) {
        OMError status = appendOutput(frames, index, info);
        media_status_t released = AMediaCodec_releaseOutputBuffer(codec_, index, false);
        if (status != OM_SUCCESS) return status;
        if (released != AMEDIA_OK) return OM_CODEC_DECODE_FAILED;
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)
          output_eof_ = true;
      } else {
        return OM_CODEC_DECODE_FAILED;
      }
      if (output_eof_) break;
      index = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
    }
    return OM_SUCCESS;
  }

public:
  ~MediaCodecDecoder() override { close(); }

  auto configure(const DecoderOptions& options) -> OMError override {
    close();
    const char* mime = codecIdToMime(options.format.codec_id);
    if (!mime) return OM_CODEC_NOT_FOUND;
    type_ = options.format.type;
    if ((type_ != OM_MEDIA_VIDEO && type_ != OM_MEDIA_AUDIO) ||
        (type_ == OM_MEDIA_VIDEO) != (std::strncmp(mime, "video/", 6) == 0))
      return OM_CODEC_INVALID_PARAMS;
    codec_ = AMediaCodec_createDecoderByType(mime);
    if (!codec_) return OM_CODEC_OPEN_FAILED;
    format_ = AMediaFormat_new();
    if (!format_) return OM_COMMON_OUT_OF_MEMORY;
    AMediaFormat_setString(format_, AMEDIAFORMAT_KEY_MIME, mime);
    if (type_ == OM_MEDIA_VIDEO) {
      const auto& video = options.format.video;
      if (!video.width || !video.height || video.width > INT32_MAX || video.height > INT32_MAX)
        return OM_CODEC_INVALID_PARAMS;
      AMediaFormat_setInt32(format_, AMEDIAFORMAT_KEY_WIDTH, static_cast<int32_t>(video.width));
      AMediaFormat_setInt32(format_, AMEDIAFORMAT_KEY_HEIGHT, static_cast<int32_t>(video.height));
      video_format_.width = video.width;
      video_format_.height = video.height;
      video_format_.format = OM_FORMAT_UNKNOWN;
    } else {
      const auto& audio = options.format.audio;
      if (!audio.sample_rate || !audio.channels ||
          audio.sample_rate > INT32_MAX || audio.channels > INT32_MAX)
        return OM_CODEC_INVALID_PARAMS;
      AMediaFormat_setInt32(format_, AMEDIAFORMAT_KEY_SAMPLE_RATE, static_cast<int32_t>(audio.sample_rate));
      AMediaFormat_setInt32(format_, AMEDIAFORMAT_KEY_CHANNEL_COUNT, static_cast<int32_t>(audio.channels));
      audio_format_.sample_rate = audio.sample_rate;
      audio_format_.channels = audio.channels;
      audio_format_.sample_format = OM_SAMPLE_S16;
      audio_format_.bits_per_sample = 16;
    }
    if (!options.extradata.empty())
      AMediaFormat_setBuffer(format_, "csd-0", options.extradata.data(), options.extradata.size());
    if (AMediaCodec_configure(codec_, format_, nullptr, nullptr, 0) != AMEDIA_OK ||
        AMediaCodec_start(codec_) != AMEDIA_OK)
      return OM_CODEC_OPEN_FAILED;
    started_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!started_) return std::nullopt;
    DecodingInfo info;
    info.media_type = type_;
    if (type_ == OM_MEDIA_VIDEO) info.video_format = video_format_;
    else info.audio_format = audio_format_;
    return info;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    if (!started_) return Err(OM_COMMON_NOT_INITIALIZED);
    const bool eos = !packet.buffer;
    if (!eos && input_eof_) return Err(OM_CODEC_INVALID_PARAMS);
    if (!output_eof_) {
      OMError status = drainOutput(pending_eos_frames_, 0);
      if (status != OM_SUCCESS) return Err(status);
    }
    if (!input_eof_) {
      ssize_t index = AMediaCodec_dequeueInputBuffer(codec_, WAIT_US);
      if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
        return Err(OM_CODEC_NEED_MORE_DATA);
      if (index < 0) return Err(OM_CODEC_DECODE_FAILED);
      size_t capacity = 0;
      uint8_t* buffer = AMediaCodec_getInputBuffer(codec_, index, &capacity);
      if (!buffer || (!eos && packet.bytes.size() > capacity)) {
        AMediaCodec_queueInputBuffer(codec_, index, 0, 0, 0, 0);
        return Err(OM_CODEC_INVALID_PARAMS);
      }
      if (!eos && !packet.bytes.empty())
        std::memcpy(buffer, packet.bytes.data(), packet.bytes.size());
      if (AMediaCodec_queueInputBuffer(codec_, index, 0,
                                       eos ? 0 : packet.bytes.size(),
                                       eos ? 0 : packet.pts,
                                       eos ? AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM : 0) != AMEDIA_OK)
        return Err(OM_CODEC_DECODE_FAILED);
      if (eos) input_eof_ = true;
    }

    if (eos) {
      for (int attempt = 0; attempt < DRAIN_ATTEMPTS && !output_eof_; ++attempt) {
        OMError status = drainOutput(pending_eos_frames_, WAIT_US);
        if (status != OM_SUCCESS) return Err(status);
      }
      if (!output_eof_) return Err(OM_COMMON_TIMEOUT);
      return Ok(std::exchange(pending_eos_frames_, std::vector<Frame>{}));
    }
    OMError status = drainOutput(pending_eos_frames_, WAIT_US);
    if (status != OM_SUCCESS) return Err(status);
    return Ok(std::exchange(pending_eos_frames_, std::vector<Frame>{}));
  }

  void flush() override {
    if (!started_) return;
    if (AMediaCodec_flush(codec_) == AMEDIA_OK) {
      input_eof_ = output_eof_ = false;
      pending_eos_frames_.clear();
      resetReceiveState();
    }
  }
};

class MediaCodecEncoder final : public Encoder {
  AMediaCodec* codec_ = nullptr;
  AMediaFormat* format_ = nullptr;
  OMMediaType type_ = OM_MEDIA_NONE;
  VideoFormat video_format_ = {};
  AudioFormat audio_format_ = {};
  bool started_ = false;
  bool input_eof_ = false;
  bool output_eof_ = false;
  EncodingInfo encoding_info_ = {};
  std::vector<Packet> pending_packets_;
  std::vector<Packet> pending_eos_packets_;

  void close() {
    if (codec_) {
      if (started_) AMediaCodec_stop(codec_);
      AMediaCodec_delete(codec_);
      codec_ = nullptr;
    }
    if (format_) {
      AMediaFormat_delete(format_);
      format_ = nullptr;
    }
    started_ = input_eof_ = output_eof_ = false;
    pending_packets_.clear();
    pending_eos_packets_.clear();
    encoding_info_ = {};
  }

  void updateOutputFormat() {
    AMediaFormat* output = AMediaCodec_getOutputFormat(codec_);
    if (!output) return;
    std::vector<uint8_t> extradata;
    for (int i = 0; i < 3; ++i) {
      char key[] = "csd-0";
      key[4] = static_cast<char>('0' + i);
      void* data = nullptr;
      size_t size = 0;
      if (AMediaFormat_getBuffer(output, key, &data, &size) && data && size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        extradata.insert(extradata.end(), bytes, bytes + size);
      }
    }
    if (!extradata.empty()) encoding_info_.extradata = std::move(extradata);
    AMediaFormat_delete(output);
  }

  auto drainOutput(std::vector<Packet>& packets, int64_t wait_us) -> OMError {
    AMediaCodecBufferInfo info = {};
    ssize_t index = AMediaCodec_dequeueOutputBuffer(codec_, &info, wait_us);
    while (index != AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
      if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        updateOutputFormat();
      } else if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
        // Output buffers are looked up on demand.
      } else if (index >= 0) {
        OMError status = OM_SUCCESS;
        if (!validBufferInfo(info)) {
          status = OM_CODEC_ENCODE_FAILED;
        } else if (info.size > 0) {
          size_t capacity = 0;
          uint8_t* buffer = AMediaCodec_getOutputBuffer(codec_, index, &capacity);
          if (!buffer) {
            status = OM_CODEC_ENCODE_FAILED;
          } else if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
            if (encoding_info_.extradata.empty())
              encoding_info_.extradata.assign(buffer + info.offset,
                                               buffer + info.offset + info.size);
          } else {
            Packet packet;
            packet.allocate(static_cast<size_t>(info.size));
            std::memcpy(packet.bytes.data(), buffer + info.offset, info.size);
            packet.pts = packet.dts = info.presentationTimeUs;
            packet.is_keyframe = (info.flags & AMEDIACODEC_BUFFER_FLAG_KEY_FRAME) != 0;
            packets.push_back(std::move(packet));
          }
        }
        media_status_t released = AMediaCodec_releaseOutputBuffer(codec_, index, false);
        if (status != OM_SUCCESS || released != AMEDIA_OK) return OM_CODEC_ENCODE_FAILED;
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM)
          output_eof_ = true;
      } else {
        return OM_CODEC_ENCODE_FAILED;
      }
      if (output_eof_) break;
      index = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
    }
    return OM_SUCCESS;
  }

public:
  ~MediaCodecEncoder() override { close(); }

  auto configure(const EncoderOptions& options) -> OMError override {
    close();
    const char* mime = codecIdToMime(options.format.codec_id);
    if (!mime) return OM_CODEC_NOT_FOUND;
    type_ = options.format.type;
    if ((type_ != OM_MEDIA_VIDEO && type_ != OM_MEDIA_AUDIO) ||
        (type_ == OM_MEDIA_VIDEO) != (std::strncmp(mime, "video/", 6) == 0))
      return OM_CODEC_INVALID_PARAMS;
    codec_ = AMediaCodec_createEncoderByType(mime);
    if (!codec_) return OM_CODEC_OPEN_FAILED;
    format_ = AMediaFormat_new();
    if (!format_) return OM_COMMON_OUT_OF_MEMORY;
    AMediaFormat_setString(format_, AMEDIAFORMAT_KEY_MIME, mime);
    if (type_ == OM_MEDIA_VIDEO) {
      const auto& video = options.format.video;
      video_format_.width = video.width;
      video_format_.height = video.height;
      video_format_.format = OM_FORMAT_YUV420P;
      if (!video.width || !video.height || video.width > INT32_MAX ||
          video.height > INT32_MAX || video.framerate.den <= 0 ||
          video.framerate.num <= 0)
        return OM_CODEC_INVALID_PARAMS;
      AMediaFormat_setInt32(format_, AMEDIAFORMAT_KEY_WIDTH, static_cast<int32_t>(video.width));
      AMediaFormat_setInt32(format_, AMEDIAFORMAT_KEY_HEIGHT, static_cast<int32_t>(video.height));
      AMediaFormat_setInt32(format_, AMEDIAFORMAT_KEY_COLOR_FORMAT, YUV420_PLANAR);
      AMediaFormat_setFloat(format_, AMEDIAFORMAT_KEY_FRAME_RATE,
                            static_cast<float>(video.framerate.num) / video.framerate.den);
      AMediaFormat_setInt32(format_, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
    } else {
      audio_format_ = options.audio_format;
      const auto& audio = audio_format_;
      if (!audio.sample_rate || !audio.channels ||
          audio.sample_rate > INT32_MAX || audio.channels > INT32_MAX ||
          audio.sample_format != OM_SAMPLE_S16 || audio.planar)
        return OM_CODEC_INVALID_PARAMS;
      AMediaFormat_setInt32(format_, AMEDIAFORMAT_KEY_SAMPLE_RATE, static_cast<int32_t>(audio.sample_rate));
      AMediaFormat_setInt32(format_, AMEDIAFORMAT_KEY_CHANNEL_COUNT, static_cast<int32_t>(audio.channels));
      AMediaFormat_setInt32(format_, AMEDIAFORMAT_KEY_PCM_ENCODING, PCM16_BIT);
    }
    OMError bitrate_status = updateBitrate(options.rate_control);
    if (bitrate_status != OM_SUCCESS) return bitrate_status;
    if (AMediaCodec_configure(codec_, format_, nullptr, nullptr,
                              AMEDIACODEC_CONFIGURE_FLAG_ENCODE) != AMEDIA_OK ||
        AMediaCodec_start(codec_) != AMEDIA_OK)
      return OM_CODEC_OPEN_FAILED;
    started_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> EncodingInfo override {
    if (started_ && encoding_info_.extradata.empty()) updateOutputFormat();
    return encoding_info_;
  }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    if (!started_) return Err(OM_COMMON_NOT_INITIALIZED);
    if (input_eof_) return Err(OM_CODEC_INVALID_PARAMS);
    OMError drain_status = drainOutput(pending_packets_, 0);
    if (drain_status != OM_SUCCESS) return Err(drain_status);
    const Picture* picture = std::get_if<Picture>(&frame.data);
    const AudioSamples* samples = std::get_if<AudioSamples>(&frame.data);
    if (type_ == OM_MEDIA_VIDEO) {
      if (!picture || picture->format != OM_FORMAT_YUV420P ||
          picture->width != video_format_.width ||
          picture->height != video_format_.height ||
          picture->planes.count < 3)
        return Err(OM_FRAME_WRONG_FORMAT);
    } else if (!samples || !samples->buffer ||
               samples->format.sample_format != OM_SAMPLE_S16 ||
               samples->format.planar ||
               samples->format.channels != audio_format_.channels ||
               samples->format.sample_rate != audio_format_.sample_rate) {
      return Err(OM_FRAME_WRONG_FORMAT);
    }

    ssize_t index = AMediaCodec_dequeueInputBuffer(codec_, WAIT_US);
    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
      return Err(OM_CODEC_NEED_MORE_DATA);
    if (index < 0) return Err(OM_CODEC_ENCODE_FAILED);
    size_t capacity = 0;
    uint8_t* buffer = AMediaCodec_getInputBuffer(codec_, index, &capacity);
    if (!buffer) return Err(OM_CODEC_ENCODE_FAILED);

    size_t size = 0;
    if (type_ == OM_MEDIA_VIDEO) {
      for (uint32_t plane = 0; plane < 3; ++plane) {
        auto dims = picture->getPlaneDimensions(plane);
        const size_t width = dims.first;
        const size_t height = dims.second;
        if (size > capacity || !picture->planes.data[plane] ||
            picture->planes.linesize[plane] < width ||
            (width && height > (capacity - size) / width)) {
          AMediaCodec_queueInputBuffer(codec_, index, 0, 0, 0, 0);
          return Err(OM_CODEC_INVALID_PARAMS);
        }
        for (size_t y = 0; y < height; ++y) {
          std::memcpy(buffer + size, picture->planes.data[plane] +
                      y * picture->planes.linesize[plane], width);
          size += width;
        }
      }
    } else {
      const size_t sample_stride = static_cast<size_t>(samples->format.channels) * sizeof(int16_t);
      if (sample_stride && samples->nb_samples > SIZE_MAX / sample_stride) {
        AMediaCodec_queueInputBuffer(codec_, index, 0, 0, 0, 0);
        return Err(OM_CODEC_INVALID_PARAMS);
      }
      const size_t expected = static_cast<size_t>(samples->nb_samples) * sample_stride;
      if (expected > capacity || expected > samples->buffer->bytes().size()) {
        AMediaCodec_queueInputBuffer(codec_, index, 0, 0, 0, 0);
        return Err(OM_CODEC_INVALID_PARAMS);
      }
      size = expected;
      if (size) std::memcpy(buffer, samples->buffer->bytes().data(), size);
    }
    if (AMediaCodec_queueInputBuffer(codec_, index, 0, size, frame.pts, 0) != AMEDIA_OK) {
      return Err(OM_CODEC_ENCODE_FAILED);
    }
    OMError status = drainOutput(pending_packets_, WAIT_US);
    if (status != OM_SUCCESS) return Err(status);
    return Ok(std::exchange(pending_packets_, std::vector<Packet>{}));
  }

  auto finish() -> Result<std::vector<Packet>, OMError> override {
    if (!started_) return Err(OM_COMMON_NOT_INITIALIZED);
    if (!pending_packets_.empty()) {
      pending_eos_packets_.insert(
          pending_eos_packets_.end(),
          std::make_move_iterator(pending_packets_.begin()),
          std::make_move_iterator(pending_packets_.end()));
      pending_packets_.clear();
    }
    if (!output_eof_) {
      OMError status = drainOutput(pending_eos_packets_, 0);
      if (status != OM_SUCCESS) {
        return Err(status);
      }
    }
    if (!input_eof_) {
      ssize_t index = AMediaCodec_dequeueInputBuffer(codec_, WAIT_US);
      if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
        return Err(OM_CODEC_NEED_MORE_DATA);
      if (index < 0 || AMediaCodec_queueInputBuffer(
              codec_, index, 0, 0, 0,
              AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != AMEDIA_OK)
        return Err(OM_CODEC_ENCODE_FAILED);
      input_eof_ = true;
    }
    for (int attempt = 0; attempt < DRAIN_ATTEMPTS && !output_eof_; ++attempt) {
      OMError status = drainOutput(pending_eos_packets_, WAIT_US);
      if (status != OM_SUCCESS) {
        return Err(status);
      }
    }
    if (!output_eof_) {
      return Err(OM_COMMON_TIMEOUT);
    }
    return Ok(std::exchange(pending_eos_packets_, std::vector<Packet>{}));
  }

  auto updateBitrate(const RateControlParams& rc) -> OMError override {
    if (!format_) return OM_COMMON_NOT_INITIALIZED;
    int64_t bitrate = 0;
    std::visit([&bitrate](const auto& params) {
      if constexpr (requires { params.bitrate.target_bitrate; })
        bitrate = params.bitrate.target_bitrate;
      else if constexpr (requires { params.target_bitrate; })
        bitrate = params.target_bitrate;
    }, rc.params);
    if (bitrate == 0) return started_ ? OM_COMMON_NOT_SUPPORTED : OM_SUCCESS;
    if (bitrate < 0 || bitrate > INT32_MAX) return OM_CODEC_INVALID_PARAMS;
    if (!started_) {
      AMediaFormat_setInt32(format_, AMEDIAFORMAT_KEY_BIT_RATE, static_cast<int32_t>(bitrate));
      return OM_SUCCESS;
    }
#if __ANDROID_API__ >= 26
    if (type_ != OM_MEDIA_VIDEO) return OM_COMMON_NOT_SUPPORTED;
    AMediaFormat* params = AMediaFormat_new();
    if (!params) return OM_COMMON_OUT_OF_MEMORY;
    AMediaFormat_setInt32(params, AMEDIACODEC_KEY_VIDEO_BITRATE,
                          static_cast<int32_t>(bitrate));
    media_status_t status = AMediaCodec_setParameters(codec_, params);
    AMediaFormat_delete(params);
    return status == AMEDIA_OK ? OM_SUCCESS : OM_CODEC_ENCODE_FAILED;
#else
    return OM_COMMON_NOT_SUPPORTED;
#endif
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

DEFINE_MEDIACODEC_CODEC(H264, OM_MEDIA_VIDEO, "h264", "H.264 (AVC)");
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
