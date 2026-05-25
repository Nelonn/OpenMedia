#include <ALACBitUtilities.h>
#include <ALACDecoder.h>
#include <ALACEncoder.h>
#include <cstring>
#include <openmedia/audio.hpp>
#include <codecs.hpp>
#include <vector>

namespace openmedia {

namespace {

auto sampleFormatToBitDepth(const AudioFormat& format, uint32_t encoded_bit_depth) -> uint32_t {
  if (encoded_bit_depth != 0) return encoded_bit_depth;
  if (format.bits_per_sample != 0) return format.bits_per_sample;
  return static_cast<uint32_t>(getBytesPerSample(format.sample_format) * 8);
}

auto makePcmFormatDescription(const AudioFormat& format, uint32_t bit_depth, uint32_t frames_per_packet) -> AudioFormatDescription {
  AudioFormatDescription desc = {};
  desc.mSampleRate = format.sample_rate;
  desc.mFormatID = kALACFormatLinearPCM;
  desc.mFormatFlags = kALACFormatFlagIsPacked;
  if (format.sample_format == OM_SAMPLE_S16 || format.sample_format == OM_SAMPLE_S32) {
    desc.mFormatFlags |= kALACFormatFlagIsSignedInteger;
  }
  desc.mFramesPerPacket = frames_per_packet;
  desc.mChannelsPerFrame = format.channels;
  desc.mBitsPerChannel = bit_depth;
  desc.mBytesPerFrame = format.channels * ((bit_depth + 7) / 8);
  desc.mBytesPerPacket = desc.mBytesPerFrame * frames_per_packet;
  return desc;
}

auto makeAlacFormatDescription(const AudioFormat& format, uint32_t bit_depth, uint32_t frame_size) -> AudioFormatDescription {
  AudioFormatDescription desc = {};
  desc.mSampleRate = format.sample_rate;
  desc.mFormatID = kALACFormatAppleLossless;
  desc.mFramesPerPacket = frame_size;
  desc.mChannelsPerFrame = format.channels;
  desc.mBitsPerChannel = bit_depth;
  return desc;
}

auto copyInterleavedPcm(const AudioSamples& samples, std::vector<uint8_t>& dst) -> bool {
  const size_t bytes_per_sample = getBytesPerSample(samples.format.sample_format);
  const size_t total_size = static_cast<size_t>(samples.nb_samples) * samples.format.channels * bytes_per_sample;
  dst.resize(total_size);

  if (!samples.format.planar) {
    if (!samples.buffer || samples.buffer->bytes().size() < total_size) return false;
    std::memcpy(dst.data(), samples.buffer->bytes().data(), total_size);
    return true;
  }

  auto* out = dst.data();
  for (uint32_t sample = 0; sample < samples.nb_samples; ++sample) {
    for (uint32_t channel = 0; channel < samples.format.channels; ++channel) {
      if (!samples.planes.data[channel]) return false;
      const auto* src = samples.planes.data[channel] + static_cast<size_t>(sample) * bytes_per_sample;
      std::memcpy(out, src, bytes_per_sample);
      out += bytes_per_sample;
    }
  }
  return true;
}

}

class ALACEncoder final : public Encoder {
  ::ALACEncoder encoder_;
  AudioFormat input_format_ = {};
  uint32_t bit_depth_ = 0;
  uint32_t frame_size_ = kALACDefaultFramesPerPacket;
  std::vector<uint8_t> extradata_;

public:
  auto configure(const EncoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_ALAC || options.format.type != OM_MEDIA_AUDIO) {
      return OM_CODEC_INVALID_PARAMS;
    }
    if (options.audio_format.sample_rate == 0 || options.audio_format.channels == 0) {
      return OM_CODEC_INVALID_PARAMS;
    }
    if (options.audio_format.sample_format != OM_SAMPLE_S16 && options.audio_format.sample_format != OM_SAMPLE_S32) {
      return OM_CODEC_INVALID_PARAMS;
    }

    bit_depth_ = sampleFormatToBitDepth(options.audio_format, options.format.audio.bit_depth);
    if (bit_depth_ != 16 && bit_depth_ != 24 && bit_depth_ != 32) {
      return OM_CODEC_INVALID_PARAMS;
    }

    input_format_ = options.audio_format;
    input_format_.planar = false;
    input_format_.bits_per_sample = static_cast<uint8_t>(bit_depth_);

    encoder_.SetFrameSize(frame_size_);
    const auto output_desc = makeAlacFormatDescription(input_format_, bit_depth_, frame_size_);
    if (encoder_.InitializeEncoder(output_desc) != 0) {
      return OM_CODEC_OPEN_FAILED;
    }

    uint32_t cookie_size = encoder_.GetMagicCookieSize(input_format_.channels);
    extradata_.resize(cookie_size);
    encoder_.GetMagicCookie(extradata_.data(), &cookie_size);
    extradata_.resize(cookie_size);
    return OM_SUCCESS;
  }

  auto getInfo() -> EncodingInfo override {
    EncodingInfo info = {};
    info.extradata = extradata_;
    return info;
  }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    const auto* samples = std::get_if<AudioSamples>(&frame.data);
    if (!samples) {
      return Err(OM_CODEC_INVALID_PARAMS);
    }
    if (samples->format.sample_format != input_format_.sample_format ||
        samples->format.channels != input_format_.channels) {
      return Err(OM_CODEC_INVALID_PARAMS);
    }

    std::vector<uint8_t> input_buffer;
    if (!copyInterleavedPcm(*samples, input_buffer)) {
      return Err(OM_CODEC_INVALID_PARAMS);
    }

    std::vector<uint8_t> output_buffer(input_buffer.size() + 4096);
    int32_t output_size = static_cast<int32_t>(output_buffer.size());
    const auto input_desc = makePcmFormatDescription(input_format_, bit_depth_, samples->nb_samples);
    const auto output_desc = makeAlacFormatDescription(input_format_, bit_depth_, samples->nb_samples);

    if (encoder_.Encode(input_desc,
                        output_desc,
                        input_buffer.data(),
                        output_buffer.data(),
                        &output_size) != 0) {
      return Err(OM_CODEC_ENCODE_FAILED);
    }

    Packet packet = {};
    packet.allocate(static_cast<size_t>(output_size));
    std::memcpy(packet.bytes.data(), output_buffer.data(), static_cast<size_t>(output_size));
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

class ALACDecoder final : public Decoder {
  ::ALACDecoder decoder_;
  AudioFormat output_format_;
  bool initialized_ = false;

public:
  ALACDecoder() = default;

  ~ALACDecoder() override = default;

  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_ALAC) {
      return OM_CODEC_INVALID_PARAMS;
    }

    if (options.extradata.empty()) {
      return OM_CODEC_INVALID_PARAMS;
    }

    if (decoder_.Init(options.extradata.data(), static_cast<uint32_t>(options.extradata.size())) != 0) {
      return OM_CODEC_OPEN_FAILED;
    }

    output_format_.channels = decoder_.mConfig.numChannels;
    output_format_.sample_rate = decoder_.mConfig.sampleRate;
    output_format_.planar = false;

    switch (decoder_.mConfig.bitDepth) {
      case 16:
        output_format_.sample_format = OM_SAMPLE_S16;
        break;
      case 24:
        output_format_.sample_format = OM_SAMPLE_S32;
        break;
      case 32:
        output_format_.sample_format = OM_SAMPLE_S32;
        break;
      default:
        return OM_CODEC_INVALID_PARAMS;
    }

    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!initialized_) return std::nullopt;

    DecodingInfo info = {};
    info.media_type = OM_MEDIA_AUDIO;
    info.audio_format = output_format_;
    return info;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    BitBuffer bits;
    BitBufferInit(&bits, packet.bytes.data(), static_cast<uint32_t>(packet.bytes.size()));

    uint32_t frame_length = decoder_.mConfig.frameLength;
    uint32_t channels = decoder_.mConfig.numChannels;

    AudioSamples samples(output_format_, frame_length);
    uint32_t out_num_samples = 0;

    if (decoder_.Decode(&bits, samples.planes.data[0], frame_length, channels, &out_num_samples) != 0) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    if (out_num_samples != frame_length) {
      samples.nb_samples = out_num_samples;
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

const CodecDescriptor CODEC_ALAC = {
  .codec_id = OM_CODEC_ALAC,
  .type = OM_MEDIA_AUDIO,
  .name = "alac",
  .long_name = "Apple Lossless Audio Codec",
  .vendor = "Apple",
  .flags = NONE,
  .decoder_factory = []{ return std::make_unique<ALACDecoder>(); },
  .encoder_factory = []{ return std::make_unique<ALACEncoder>(); },
};

} // namespace openmedia
