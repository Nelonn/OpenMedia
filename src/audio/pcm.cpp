#include <codecs.hpp>
#include <openmedia/audio.hpp>
#include <algorithm>
#include <cstring>
#include <vector>

namespace openmedia {

static auto codecIdToSampleFormat(OMCodecId codec_id) -> OMSampleFormat {
  switch (codec_id) {
    case OM_CODEC_PCM_U8: return OM_SAMPLE_U8;
    case OM_CODEC_PCM_S16LE: return OM_SAMPLE_S16;
    case OM_CODEC_PCM_S32LE: return OM_SAMPLE_S32;
    case OM_CODEC_PCM_F32LE: return OM_SAMPLE_F32;
    case OM_CODEC_PCM_F64LE: return OM_SAMPLE_F64;
    default: return OM_SAMPLE_UNKNOWN;
  }
}

static auto writeInterleavedPcm(const AudioSamples& samples, std::span<uint8_t> dst) -> bool {
  const size_t bytes_per_sample = getBytesPerSample(samples.format.sample_format);
  const size_t channels = samples.format.channels;
  const size_t nb_samples = samples.nb_samples;
  const size_t total_size = nb_samples * channels * bytes_per_sample;
  if (dst.size() < total_size) return false;

  if (!samples.format.planar) {
    if (!samples.buffer) return false;
    const auto src = samples.buffer->bytes();
    if (src.size() < total_size) return false;
    memcpy(dst.data(), src.data(), total_size);
    return true;
  }

  auto* out = dst.data();
  for (size_t sample = 0; sample < nb_samples; ++sample) {
    for (size_t channel = 0; channel < channels; ++channel) {
      if (!samples.planes.data[channel]) return false;
      const auto* src = samples.planes.data[channel] + sample * bytes_per_sample;
      memcpy(out, src, bytes_per_sample);
      out += bytes_per_sample;
    }
  }
  return true;
}

class PCMEncoder final : public Encoder {
  AudioFormat input_format_ = {};
  OMCodecId codec_id_ = OM_CODEC_NONE;

public:
  auto configure(const EncoderOptions& options) -> OMError override {
    if (options.format.type != OM_MEDIA_AUDIO) {
      return OM_CODEC_INVALID_PARAMS;
    }

    const OMSampleFormat sample_format = codecIdToSampleFormat(options.format.codec_id);
    if (sample_format == OM_SAMPLE_UNKNOWN) {
      return OM_CODEC_NOT_SUPPORTED;
    }
    if (options.audio_format.sample_rate == 0 || options.audio_format.channels == 0) {
      return OM_CODEC_INVALID_PARAMS;
    }
    if (options.audio_format.sample_format != sample_format) {
      return OM_CODEC_INVALID_PARAMS;
    }

    codec_id_ = options.format.codec_id;
    input_format_ = options.audio_format;
    input_format_.sample_format = sample_format;
    input_format_.bits_per_sample = static_cast<uint8_t>(getBytesPerSample(sample_format) * 8);
    return OM_SUCCESS;
  }

  auto getInfo() -> EncodingInfo override { return {}; }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    if (codec_id_ == OM_CODEC_NONE) {
      return Err(OM_COMMON_NOT_INITIALIZED);
    }

    const auto* samples = std::get_if<AudioSamples>(&frame.data);
    if (!samples) {
      return Err(OM_CODEC_INVALID_PARAMS);
    }
    if (samples->format.sample_format != input_format_.sample_format ||
        samples->format.channels != input_format_.channels) {
      return Err(OM_CODEC_INVALID_PARAMS);
    }

    const size_t bytes_per_sample = getBytesPerSample(input_format_.sample_format);
    const size_t packet_size = static_cast<size_t>(samples->nb_samples) * input_format_.channels * bytes_per_sample;

    Packet packet = {};
    packet.allocate(packet_size);
    if (!writeInterleavedPcm(*samples, packet.bytes)) {
      return Err(OM_CODEC_INVALID_PARAMS);
    }
    packet.pts = frame.pts;
    packet.dts = frame.dts;
    packet.is_keyframe = true;

    std::vector<Packet> packets;
    packets.push_back(std::move(packet));
    return Ok(std::move(packets));
  }

  auto updateBitrate(const RateControlParams&) -> OMError override {
    return OM_SUCCESS;
  }
};

auto create_pcm_encoder() -> std::unique_ptr<Encoder> {
  return std::make_unique<PCMEncoder>();
}

class PCMDecoder final : public Decoder {
  AudioFormat format_;

public:
  PCMDecoder() = default;
  ~PCMDecoder() override = default;

  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.audio.channels == 0 || options.format.audio.sample_rate == 0) {
      return OM_CODEC_INVALID_PARAMS;
    }

    format_.sample_rate = options.format.audio.sample_rate;
    format_.channels = options.format.audio.channels;

    switch (options.format.codec_id) {
      case OM_CODEC_PCM_U8:    format_.sample_format = OM_SAMPLE_U8; break;
      case OM_CODEC_PCM_S16LE: format_.sample_format = OM_SAMPLE_S16; break;
      case OM_CODEC_PCM_S32LE: format_.sample_format = OM_SAMPLE_S32; break;
      case OM_CODEC_PCM_F32LE: format_.sample_format = OM_SAMPLE_F32; break;
      case OM_CODEC_PCM_F64LE: format_.sample_format = OM_SAMPLE_F64; break;
      default: return OM_CODEC_NOT_SUPPORTED;
    }

    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    DecodingInfo info = {};
    info.media_type = OM_MEDIA_AUDIO;
    info.audio_format = format_;
    return info;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    if (packet.bytes.empty()) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    size_t bps = getBytesPerSample(format_.sample_format);
    size_t alignment = static_cast<size_t>(format_.channels) * bps;
    if (alignment == 0) {
      return Err(OM_CODEC_INVALID_PARAMS);
    }

    uint32_t nb_samples = static_cast<uint32_t>(packet.bytes.size() / alignment);
    if (nb_samples == 0) {
      return Ok(std::vector<Frame>{});
    }

    AudioSamples samples(format_, nb_samples);
    if (samples.buffer && samples.planes.count > 0 && samples.planes.data[0]) {
      memcpy(samples.planes.data[0], packet.bytes.data(), static_cast<size_t>(nb_samples) * alignment);
    }

    Frame frame;
    frame.pts = packet.pts;
    frame.dts = packet.dts;
    frame.data = std::move(samples);

    std::vector<Frame> frames;
    frames.push_back(std::move(frame));
    return Ok(std::move(frames));
  }

  void flush() override {}
};

auto create_pcm_decoder() -> std::unique_ptr<Decoder> {
  return std::make_unique<PCMDecoder>();
}

const CodecDescriptor CODEC_PCM_U8 = {
  .codec_id = OM_CODEC_PCM_U8,
  .type = OM_MEDIA_AUDIO,
  .name = "pcm_u8",
  .long_name = "PCM Unsigned 8-bit",
  .vendor = "OpenMedia",
  .flags = NONE,
  .decoder_factory = create_pcm_decoder,
  .encoder_factory = create_pcm_encoder,
};

const CodecDescriptor CODEC_PCM_S16LE = {
  .codec_id = OM_CODEC_PCM_S16LE,
  .type = OM_MEDIA_AUDIO,
  .name = "pcm_s16le",
  .long_name = "PCM Signed 16-bit",
  .vendor = "OpenMedia",
  .flags = NONE,
  .decoder_factory = create_pcm_decoder,
  .encoder_factory = create_pcm_encoder,
};

const CodecDescriptor CODEC_PCM_S32LE = {
  .codec_id = OM_CODEC_PCM_S32LE,
  .type = OM_MEDIA_AUDIO,
  .name = "pcm_s32le",
  .long_name = "PCM Signed 32-bit",
  .vendor = "OpenMedia",
  .flags = NONE,
  .decoder_factory = create_pcm_decoder,
  .encoder_factory = create_pcm_encoder,
};

const CodecDescriptor CODEC_PCM_F32LE = {
  .codec_id = OM_CODEC_PCM_F32LE,
  .type = OM_MEDIA_AUDIO,
  .name = "pcm_f32le",
  .long_name = "PCM Float 32-bit",
  .vendor = "OpenMedia",
  .flags = NONE,
  .decoder_factory = create_pcm_decoder,
  .encoder_factory = create_pcm_encoder,
};

const CodecDescriptor CODEC_PCM_F64LE = {
  .codec_id = OM_CODEC_PCM_F64LE,
  .type = OM_MEDIA_AUDIO,
  .name = "pcm_f64le",
  .long_name = "PCM Float 64-bit",
  .vendor = "OpenMedia",
  .flags = NONE,
  .decoder_factory = create_pcm_decoder,
  .encoder_factory = create_pcm_encoder,
};

} // namespace openmedia
