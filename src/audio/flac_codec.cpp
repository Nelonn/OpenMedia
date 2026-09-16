#include <codecs.hpp>
#include <openmedia/audio.hpp>
#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>
#include <algorithm>
#include <cstring>
#include <vector>
#include <iostream>
#include <format>
#include <array>

namespace openmedia {

class FLACDecoder final : public Decoder {
  static constexpr uint8_t STREAM_INFO_SIZE = 34;

  FLAC__StreamDecoder* decoder_ = nullptr;
  std::vector<Frame> decoded_frames_;
  std::vector<uint8_t> init_header_;
  const Packet* current_packet_ = nullptr;
  size_t packet_offset_ = 0;
  uint8_t packet_reads_ = 0;
  uint64_t packet_sample_offset_ = 0;
  std::optional<AudioFormat> output_format_;

public:
  FLACDecoder() {
    decoder_ = FLAC__stream_decoder_new();
  }

  ~FLACDecoder() override {
    if (decoder_) {
      FLAC__stream_decoder_delete(decoder_);
    }
  }

  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_FLAC) {
      return OM_CODEC_INVALID_PARAMS;
    }
    if (options.extradata.empty()) {
      return OM_CODEC_INVALID_PARAMS;
    }

    // libFLAC decodes a native stream, so the extradata has to look like one.
    // Containers such as MP4 carry only the bare 34-byte STREAMINFO body, and
    // those get the "fLaC" marker plus a last-block STREAMINFO header put back
    // in front; native FLAC already ships the full header.
    if (options.extradata.size() >= 4 &&
        std::memcmp(options.extradata.data(), "fLaC", 4) == 0) {
      init_header_.assign(options.extradata.begin(), options.extradata.end());
    } else if (options.extradata.size() == STREAM_INFO_SIZE) {
      static constexpr uint8_t NATIVE_HEADER[] = {
          'f', 'L', 'a', 'C', 0x80, 0x00, 0x00, STREAM_INFO_SIZE};
      init_header_.assign(std::begin(NATIVE_HEADER), std::end(NATIVE_HEADER));
      init_header_.insert(init_header_.end(),
                          options.extradata.begin(), options.extradata.end());
    } else {
      return OM_CODEC_INVALID_PARAMS;
    }

    auto status = FLAC__stream_decoder_init_stream(
        decoder_,
        read_callback,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        write_callback,
        metadata_callback,
        error_callback,
        this);

    if (status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
      return OM_CODEC_OPEN_FAILED;
    }

    Packet p;
    p.allocate(init_header_.size());
    std::memcpy(p.buffer->bytes().data(), init_header_.data(), init_header_.size());
    p.pts = 0;
    current_packet_ = &p;
    packet_offset_ = 0;

    if (!FLAC__stream_decoder_process_until_end_of_metadata(decoder_)) {
      current_packet_ = nullptr;
      return OM_CODEC_OPEN_FAILED;
    }

    current_packet_ = nullptr;
    packet_offset_ = 0;

    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!output_format_.has_value()) return std::nullopt;

    DecodingInfo info = {};
    info.media_type = OM_MEDIA_AUDIO;
    info.audio_format = *output_format_;
    return info;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    current_packet_ = &packet;
    packet_offset_ = 0;
    packet_reads_ = 0;
    packet_sample_offset_ = 0;
    decoded_frames_.clear();

    FLAC__stream_decoder_process_single(decoder_);

    current_packet_ = nullptr;
    packet_offset_ = 0;

    return Ok(std::move(decoded_frames_));
  }

  void flush() override {
    FLAC__stream_decoder_flush(decoder_);
  }

private:
  static auto read_callback(
      const FLAC__StreamDecoder* /*decoder*/,
      FLAC__byte buffer[], size_t* bytes,
      void* client_data) -> FLAC__StreamDecoderReadStatus {
    auto* self = static_cast<FLACDecoder*>(client_data);
    self->packet_reads_++;
    if (!self->current_packet_ ||
        self->packet_offset_ >= self->current_packet_->bytes.size()) {
      *bytes = 0;
      return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }

    size_t to_copy = std::min(*bytes,
                              self->current_packet_->bytes.size() - self->packet_offset_);
    memcpy(buffer,
           self->current_packet_->bytes.data() + self->packet_offset_,
           to_copy);
    self->packet_offset_ += to_copy;
    *bytes = to_copy;

    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
  }

  static void metadata_callback(
      const FLAC__StreamDecoder* /*decoder*/,
      const FLAC__StreamMetadata* metadata,
      void* client_data) {
    if (metadata->type != FLAC__METADATA_TYPE_STREAMINFO) return;

    const auto& si = metadata->data.stream_info;
    AudioFormat fmt = {};
    fmt.sample_format = OM_SAMPLE_S32;
    fmt.bits_per_sample = si.bits_per_sample;
    fmt.sample_rate = si.sample_rate;
    fmt.channels = si.channels;
    fmt.planar = true;
    static_cast<FLACDecoder*>(client_data)->output_format_ = fmt;
  }

  static auto write_callback(
      const FLAC__StreamDecoder* /*decoder*/,
      const FLAC__Frame* frame,
      const FLAC__int32* const buffer[],
      void* client_data) -> FLAC__StreamDecoderWriteStatus {
    auto* self = static_cast<FLACDecoder*>(client_data);

    const uint32_t nb_samples = frame->header.blocksize;

    AudioFormat fmt = {};
    fmt.sample_format = OM_SAMPLE_S32;
    fmt.bits_per_sample = frame->header.bits_per_sample;
    fmt.sample_rate = frame->header.sample_rate;
    fmt.channels = frame->header.channels;
    fmt.planar = true;

    AudioSamples samples(fmt, nb_samples);

    for (unsigned c = 0; c < fmt.channels; ++c) {
      const FLAC__int32* src = buffer[c];
      int32_t* dst = reinterpret_cast<int32_t*>(samples.planes.data[c]);
      memcpy(dst, src, nb_samples * sizeof(int32_t));
    }

    Frame audio_frame;
    audio_frame.pts = self->current_packet_->pts + static_cast<int64_t>(self->packet_sample_offset_);
    audio_frame.data = std::move(samples);
    self->packet_sample_offset_ += static_cast<uint64_t>(frame->header.blocksize);

    self->decoded_frames_.push_back(std::move(audio_frame));

    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
  }

  static void error_callback(
      const FLAC__StreamDecoder* /*decoder*/,
      FLAC__StreamDecoderErrorStatus status,
      void* client_data) {
    log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, "FLAC decoder error: {}", FLAC__StreamDecoderErrorStatusString[status]);
  }
};

class FLACEncoder final : public Encoder {
  FLAC__StreamEncoder* encoder_ = nullptr;
  AudioFormat input_format_ = {};
  uint32_t channels_ = 0;
  uint32_t sample_rate_ = 0;
  uint32_t bits_per_sample_ = 16;
  std::vector<uint8_t> extradata_;
  std::vector<Packet> encoded_packets_;
  std::vector<std::vector<FLAC__int32>> pcm_buffers_;
  int64_t current_pts_ = 0;
  int64_t current_dts_ = 0;
  bool initializing_ = false;
  bool initialized_ = false;

public:
  FLACEncoder() = default;

  ~FLACEncoder() override {
    if (encoder_) {
      FLAC__stream_encoder_finish(encoder_);
      FLAC__stream_encoder_delete(encoder_);
      encoder_ = nullptr;
    }
  }

  auto configure(const EncoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_FLAC && options.format.codec_id != OM_CODEC_NONE) {
      return OM_CODEC_INVALID_PARAMS;
    }
    if (options.audio_format.channels == 0 || options.audio_format.channels > 8 || options.audio_format.sample_rate == 0) {
      return OM_CODEC_INVALID_PARAMS;
    }
    if (options.audio_format.sample_format != OM_SAMPLE_S32) {
      return OM_CODEC_INVALID_PARAMS;
    }
    bits_per_sample_ = options.audio_format.bits_per_sample;
    if (bits_per_sample_ < 4 || bits_per_sample_ > 32) {
      return OM_CODEC_INVALID_PARAMS;
    }

    channels_ = options.audio_format.channels;
    sample_rate_ = options.audio_format.sample_rate;
    input_format_ = options.audio_format;

    if (encoder_) {
      FLAC__stream_encoder_finish(encoder_);
      FLAC__stream_encoder_delete(encoder_);
      encoder_ = nullptr;
    }

    encoder_ = FLAC__stream_encoder_new();
    if (!encoder_) {
      return OM_COMMON_OUT_OF_MEMORY;
    }

    uint32_t compression_level = 5;
    if (auto v = options.extra.get("compression_level")) {
      if (auto val = v->getInt32()) {
        compression_level = static_cast<uint32_t>(std::clamp(*val, 0, 8));
      }
    }

    FLAC__stream_encoder_set_channels(encoder_, channels_);
    FLAC__stream_encoder_set_bits_per_sample(encoder_, bits_per_sample_);
    FLAC__stream_encoder_set_sample_rate(encoder_, sample_rate_);
    FLAC__stream_encoder_set_compression_level(encoder_, compression_level);

    if (auto v = options.extra.get("blocksize")) {
      if (auto val = v->getInt32()) {
        if (*val >= 16 && *val <= 65535) {
          FLAC__stream_encoder_set_blocksize(encoder_, static_cast<uint32_t>(*val));
        }
      }
    }

    extradata_.clear();
    initializing_ = true;

    auto status = FLAC__stream_encoder_init_stream(
        encoder_,
        write_callback,
        nullptr,
        nullptr,
        nullptr,
        this);

    initializing_ = false;

    if (status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
      FLAC__stream_encoder_delete(encoder_);
      encoder_ = nullptr;
      return OM_CODEC_OPEN_FAILED;
    }

    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> EncodingInfo override {
    EncodingInfo info = {};
    info.extradata = extradata_;
    return info;
  }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    if (!initialized_ || !encoder_) {
      return Err(OM_COMMON_NOT_INITIALIZED);
    }

    const auto* samples = std::get_if<AudioSamples>(&frame.data);
    if (!samples) {
      return Err(OM_CODEC_INVALID_PARAMS);
    }
    if (samples->format.sample_format != OM_SAMPLE_S32) {
      return Err(OM_CODEC_INVALID_PARAMS);
    }
    if (samples->format.channels != channels_ || samples->nb_samples == 0) {
      return Err(OM_CODEC_INVALID_PARAMS);
    }
    if (!samples->buffer) {
      return Err(OM_CODEC_INVALID_PARAMS);
    }

    pcm_buffers_.resize(channels_);
    for (uint32_t c = 0; c < channels_; ++c) {
      pcm_buffers_[c].resize(samples->nb_samples);
    }

    if (samples->format.planar) {
      for (uint32_t c = 0; c < channels_; ++c) {
        const auto* src = reinterpret_cast<const int32_t*>(samples->planes.data[c]);
        if (!src) return Err(OM_CODEC_INVALID_PARAMS);
        auto* dst = pcm_buffers_[c].data();
        for (uint32_t i = 0; i < samples->nb_samples; ++i) {
          dst[i] = static_cast<FLAC__int32>(src[i]);
        }
      }
    } else {
      const auto* src = reinterpret_cast<const int32_t*>(samples->buffer->bytes().data());
      for (uint32_t i = 0; i < samples->nb_samples; ++i) {
        for (uint32_t c = 0; c < channels_; ++c) {
          pcm_buffers_[c][i] = static_cast<FLAC__int32>(src[i * channels_ + c]);
        }
      }
    }

    std::array<const FLAC__int32*, 8> channel_ptrs;
    for (uint32_t c = 0; c < channels_; ++c) {
      channel_ptrs[c] = pcm_buffers_[c].data();
    }

    current_pts_ = frame.pts;
    current_dts_ = frame.dts;
    encoded_packets_.clear();

    if (!FLAC__stream_encoder_process(encoder_, channel_ptrs.data(), samples->nb_samples)) {
      return Err(OM_CODEC_ENCODE_FAILED);
    }

    return Ok(std::move(encoded_packets_));
  }

  auto updateBitrate(const RateControlParams&) -> OMError override {
    return OM_SUCCESS;
  }

private:
  static auto write_callback(
      const FLAC__StreamEncoder*,
      const FLAC__byte buffer[],
      size_t bytes,
      unsigned samples,
      unsigned,
      void* client_data) -> FLAC__StreamEncoderWriteStatus {
    auto* self = static_cast<FLACEncoder*>(client_data);
    if (self->initializing_) {
      self->extradata_.insert(self->extradata_.end(), buffer, buffer + bytes);
      return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
    }

    Packet packet;
    packet.allocate(bytes);
    std::memcpy(packet.bytes.data(), buffer, bytes);
    packet.pts = self->current_pts_;
    packet.dts = self->current_dts_;
    packet.duration = samples;
    packet.is_keyframe = true;
    self->current_pts_ += samples;
    self->current_dts_ += samples;
    self->encoded_packets_.push_back(std::move(packet));

    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
  }
};

const CodecDescriptor CODEC_FLAC = {
  .codec_id = OM_CODEC_FLAC,
  .type = OM_MEDIA_AUDIO,
  .name = "flac",
  .long_name = "Free Lossless Audio Codec",
  .vendor = "Xiph.Org",
  .flags = NONE,
  .caps = CodecCaps {
    .audio = AudioCodecCaps {
      .fmt_s32 = true,
    },
  },
  .decoder_factory = []{ return std::make_unique<FLACDecoder>(); },
  .encoder_factory = []{ return std::make_unique<FLACEncoder>(); },
};

} // namespace openmedia
