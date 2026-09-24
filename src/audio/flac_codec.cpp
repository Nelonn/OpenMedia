#include <codecs.hpp>
#include <openmedia/audio.hpp>
#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace openmedia {

static_assert(sizeof(FLAC__int32) == sizeof(int32_t),
              "OM_SAMPLE_S32 planes are handed to libFLAC without conversion");

class FLACDecoder final : public Decoder {
  static constexpr size_t STREAM_INFO_SIZE = 34;

  FLAC__StreamDecoder* decoder_ = nullptr;
  std::vector<Frame> decoded_frames_;
  std::span<const uint8_t> input_;
  size_t input_offset_ = 0;
  int64_t packet_pts_ = 0;
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

    // libFLAC decodes a native stream, so the extradata has to look like one.
    // Containers such as MP4 carry only the bare 34-byte STREAMINFO body, and
    // those get the "fLaC" marker plus a last-block STREAMINFO header put back
    // in front; native FLAC already ships the full header and is fed in place.
    std::array<uint8_t, 8 + STREAM_INFO_SIZE> synthesized_header;
    if (options.extradata.size() >= 4 &&
        std::memcmp(options.extradata.data(), "fLaC", 4) == 0) {
      input_ = options.extradata;
    } else if (options.extradata.size() == STREAM_INFO_SIZE) {
      static constexpr uint8_t NATIVE_HEADER[] = {
          'f', 'L', 'a', 'C', 0x80, 0x00, 0x00, STREAM_INFO_SIZE};
      std::memcpy(synthesized_header.data(), NATIVE_HEADER, sizeof(NATIVE_HEADER));
      std::memcpy(synthesized_header.data() + sizeof(NATIVE_HEADER),
                  options.extradata.data(), STREAM_INFO_SIZE);
      input_ = synthesized_header;
    } else {
      return OM_CODEC_INVALID_PARAMS;
    }
    input_offset_ = 0;

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
      input_ = {};
      return OM_CODEC_OPEN_FAILED;
    }

    const bool ok = FLAC__stream_decoder_process_until_end_of_metadata(decoder_);
    input_ = {};
    input_offset_ = 0;

    return ok ? OM_SUCCESS : OM_CODEC_OPEN_FAILED;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!output_format_.has_value()) return std::nullopt;

    DecodingInfo info = {};
    info.media_type = OM_MEDIA_AUDIO;
    info.audio_format = *output_format_;
    return info;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    decoded_frames_.clear();

    // A drain call carries no bytes. Feeding it to libFLAC would only latch the
    // decoder into END_OF_STREAM, where process_single() is a no-op until the
    // next flush().
    if (packet.bytes.empty()) {
      return Ok(std::move(decoded_frames_));
    }

    input_ = packet.bytes;
    input_offset_ = 0;
    packet_pts_ = packet.pts;
    packet_sample_offset_ = 0;

    FLAC__stream_decoder_process_single(decoder_);

    input_ = {};
    input_offset_ = 0;

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

    const size_t remaining = self->input_.size() - self->input_offset_;
    if (remaining == 0) {
      *bytes = 0;
      return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }

    const size_t to_copy = std::min(*bytes, remaining);
    std::memcpy(buffer, self->input_.data() + self->input_offset_, to_copy);
    self->input_offset_ += to_copy;
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

    const size_t plane_bytes = static_cast<size_t>(nb_samples) * sizeof(int32_t);
    for (uint32_t c = 0; c < fmt.channels; ++c) {
      std::memcpy(samples.planes.data[c], buffer[c], plane_bytes);
    }

    Frame& audio_frame = self->decoded_frames_.emplace_back();
    audio_frame.pts = self->packet_pts_ + static_cast<int64_t>(self->packet_sample_offset_);
    audio_frame.data = std::move(samples);
    self->packet_sample_offset_ += nb_samples;

    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
  }

  static void error_callback(
      const FLAC__StreamDecoder* /*decoder*/,
      FLAC__StreamDecoderErrorStatus status,
      void* /*client_data*/) {
    log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, "FLAC decoder error: {}", FLAC__StreamDecoderErrorStatusString[status]);
  }
};

class FLACEncoder final : public Encoder {
  static constexpr uint32_t MAX_CHANNELS = 8;

  FLAC__StreamEncoder* encoder_ = nullptr;
  uint32_t channels_ = 0;
  uint32_t sample_rate_ = 0;
  uint32_t bits_per_sample_ = 16;
  std::vector<uint8_t> extradata_;
  std::vector<Packet> encoded_packets_;
  std::vector<FLAC__int32> deinterleave_buf_;
  int64_t current_pts_ = 0;
  int64_t current_dts_ = 0;
  bool initializing_ = false;
  bool initialized_ = false;

public:
  FLACEncoder() = default;

  ~FLACEncoder() override {
    destroyEncoder();
  }

  auto configure(const EncoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_FLAC && options.format.codec_id != OM_CODEC_NONE) {
      return OM_CODEC_INVALID_PARAMS;
    }
    if (options.audio_format.channels == 0 || options.audio_format.channels > MAX_CHANNELS ||
        options.audio_format.sample_rate == 0) {
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

    destroyEncoder();
    initialized_ = false;

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

    // Everything libFLAC emits before init_stream() returns is the stream
    // header, which containers want as extradata rather than as packets.
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

    const uint32_t nb_samples = samples->nb_samples;
    std::array<const FLAC__int32*, MAX_CHANNELS> channel_ptrs = {};

    if (samples->format.planar) {
      // Planar S32 already is libFLAC's layout, so it goes in without a copy.
      if (samples->planes.count < channels_) {
        return Err(OM_CODEC_INVALID_PARAMS);
      }
      for (uint32_t c = 0; c < channels_; ++c) {
        const uint8_t* plane = samples->planes.data[c];
        if (!plane) return Err(OM_CODEC_INVALID_PARAMS);
        channel_ptrs[c] = reinterpret_cast<const FLAC__int32*>(plane);
      }
    } else {
      deinterleave_buf_.resize(static_cast<size_t>(channels_) * nb_samples);
      const auto* src = reinterpret_cast<const int32_t*>(samples->buffer->bytes().data());
      for (uint32_t c = 0; c < channels_; ++c) {
        auto* dst = deinterleave_buf_.data() + static_cast<size_t>(c) * nb_samples;
        channel_ptrs[c] = dst;
        for (uint32_t i = 0; i < nb_samples; ++i) {
          dst[i] = static_cast<FLAC__int32>(src[i * channels_ + c]);
        }
      }
    }

    current_pts_ = frame.pts;
    current_dts_ = frame.dts;
    encoded_packets_.clear();

    if (!FLAC__stream_encoder_process(encoder_, channel_ptrs.data(), nb_samples)) {
      return Err(OM_CODEC_ENCODE_FAILED);
    }

    return Ok(std::move(encoded_packets_));
  }

  auto updateBitrate(const RateControlParams&) -> OMError override {
    return OM_SUCCESS;
  }

private:
  void destroyEncoder() {
    if (encoder_) {
      FLAC__stream_encoder_finish(encoder_);
      FLAC__stream_encoder_delete(encoder_);
      encoder_ = nullptr;
    }
  }

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

    Packet& packet = self->encoded_packets_.emplace_back();
    packet.allocate(bytes);
    std::memcpy(packet.bytes.data(), buffer, bytes);
    packet.pts = self->current_pts_;
    packet.dts = self->current_dts_;
    packet.duration = samples;
    packet.is_keyframe = true;
    self->current_pts_ += samples;
    self->current_dts_ += samples;

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
