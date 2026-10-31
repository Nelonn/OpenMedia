#include <ALACBitUtilities.h>
#include <ALACDecoder.h>
#include <ALACEncoder.h>
#include <bit>
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

auto makePcmFormatDescription(const AudioFormat& format, uint32_t bit_depth) -> AudioFormatDescription {
  AudioFormatDescription desc = {};
  desc.mSampleRate = format.sample_rate;
  desc.mFormatID = kALACFormatLinearPCM;
  desc.mFormatFlags = kALACFormatFlagIsPacked | kALACFormatFlagIsSignedInteger;
  desc.mFramesPerPacket = 1;
  desc.mChannelsPerFrame = format.channels;
  desc.mBitsPerChannel = bit_depth;
  desc.mBytesPerFrame = format.channels * ((bit_depth + 7) / 8);
  desc.mBytesPerPacket = desc.mBytesPerFrame;
  return desc;
}

auto makeAlacFormatDescription(const AudioFormat& format, uint32_t bit_depth, uint32_t frame_size) -> AudioFormatDescription {
  AudioFormatDescription desc = {};
  desc.mSampleRate = format.sample_rate;
  desc.mFormatID = kALACFormatAppleLossless;
  // InitializeEncoder takes the bit depth from these flags and ignores mBitsPerChannel.
  desc.mFormatFlags = bit_depth == 16 ? 1 : bit_depth == 24 ? 3
                                                            : 4;
  desc.mFramesPerPacket = frame_size;
  desc.mChannelsPerFrame = format.channels;
  desc.mBitsPerChannel = bit_depth;
  return desc;
}

// The decoder only writes 24-bit audio packed. Going back to front lets the S32
// output share its buffer, since no write reaches a sample that is yet to be read.
void expandPacked24ToS32(uint8_t* data, size_t sample_count) {
  constexpr bool little = std::endian::native == std::endian::little;
  for (size_t i = sample_count; i-- > 0;) {
    const uint8_t* src = data + i * 3;
    const uint32_t raw = (static_cast<uint32_t>(src[little ? 2 : 0]) << 24) |
                         (static_cast<uint32_t>(src[1]) << 16) |
                         (static_cast<uint32_t>(src[little ? 0 : 2]) << 8);
    const int32_t sample = static_cast<int32_t>(raw) >> 8;
    std::memcpy(data + i * sizeof(sample), &sample, sizeof(sample));
  }
}

} // namespace

class ALACEncoder final : public Encoder {
  ::ALACEncoder encoder_;
  AudioFormat input_format_ = {};
  uint32_t bit_depth_ = 0;
  uint32_t frame_size_ = kALACDefaultFramesPerPacket;
  std::vector<uint8_t> extradata_;
  std::vector<uint8_t> input_buffer_;
  std::vector<uint8_t> output_buffer_;

  // The encoder reads samples packed to the coded depth, so anything else goes
  // through input_buffer_. S32 input holds them right-aligned, as the decoders do.
  auto interleavedInput(const AudioSamples& samples) -> uint8_t* {
    const size_t source_size = getBytesPerSample(samples.format.sample_format);
    const size_t coded_size = (bit_depth_ + 7) / 8;
    const size_t sample_count = static_cast<size_t>(samples.nb_samples) * samples.format.channels;

    if (!samples.format.planar) {
      if (!samples.buffer || samples.buffer->bytes().size() < sample_count * source_size) return nullptr;
      if (source_size == coded_size) return samples.buffer->bytes().data();
    } else {
      for (uint32_t channel = 0; channel < samples.format.channels; ++channel) {
        if (!samples.planes.data[channel]) return nullptr;
      }
    }

    input_buffer_.resize(sample_count * coded_size);
    const size_t skipped = std::endian::native == std::endian::big ? source_size - coded_size : 0;
    auto* out = input_buffer_.data();
    for (size_t sample = 0; sample < samples.nb_samples; ++sample) {
      for (size_t channel = 0; channel < samples.format.channels; ++channel) {
        const uint8_t* src = samples.format.planar
                                 ? samples.planes.data[channel] + sample * source_size
                                 : samples.buffer->bytes().data() + (sample * samples.format.channels + channel) * source_size;
        std::memcpy(out, src + skipped, coded_size);
        out += coded_size;
      }
    }
    return input_buffer_.data();
  }

public:
  auto configure(const EncoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_ALAC || options.format.type != OM_MEDIA_AUDIO) {
      return OM_CODEC_INVALID_PARAMS;
    }
    if (options.audio_format.sample_rate == 0 || options.audio_format.channels == 0 ||
        options.audio_format.channels > kALACMaxChannels) {
      return OM_CODEC_INVALID_PARAMS;
    }
    if (options.audio_format.sample_format != OM_SAMPLE_S16 && options.audio_format.sample_format != OM_SAMPLE_S32) {
      return OM_CODEC_INVALID_PARAMS;
    }

    bit_depth_ = sampleFormatToBitDepth(options.audio_format, options.format.audio.bit_depth);
    const bool depth_supported = options.audio_format.sample_format == OM_SAMPLE_S16
                                     ? bit_depth_ == 16
                                     : bit_depth_ == 24 || bit_depth_ == 32;
    if (!depth_supported) {
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

    // The bound InitializeEncoder allows for a packet; the bit writer never checks it.
    output_buffer_.resize(static_cast<size_t>(frame_size_) * input_format_.channels * ((10 + 32) / 8) + 1);
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

    // Its work buffers hold frame_size_ samples and a longer frame would overrun them.
    if (samples->nb_samples == 0 || samples->nb_samples > frame_size_) {
      return Err(OM_CODEC_INVALID_PARAMS);
    }

    uint8_t* input = interleavedInput(*samples);
    if (!input) {
      return Err(OM_CODEC_INVALID_PARAMS);
    }

    const auto input_desc = makePcmFormatDescription(input_format_, bit_depth_);
    const auto output_desc = makeAlacFormatDescription(input_format_, bit_depth_, frame_size_);
    // In: the size of the PCM input. Out: the size of the encoded packet.
    int32_t size = static_cast<int32_t>(samples->nb_samples * input_desc.mBytesPerFrame);

    if (encoder_.Encode(input_desc, output_desc, input, output_buffer_.data(), &size) != 0) {
      return Err(OM_CODEC_ENCODE_FAILED);
    }

    Packet packet = {};
    packet.allocate(static_cast<size_t>(size));
    std::memcpy(packet.bytes.data(), output_buffer_.data(), static_cast<size_t>(size));
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
  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_ALAC) {
      return OM_CODEC_INVALID_PARAMS;
    }

    // Init looks inside the cookie for the optional atoms before checking its size.
    if (options.extradata.size() < sizeof(ALACSpecificConfig)) {
      return OM_CODEC_INVALID_PARAMS;
    }

    if (decoder_.Init(options.extradata.data(), static_cast<uint32_t>(options.extradata.size())) != 0) {
      return OM_CODEC_OPEN_FAILED;
    }

    if (decoder_.mConfig.frameLength == 0 || decoder_.mConfig.numChannels == 0 ||
        decoder_.mConfig.numChannels > kALACMaxChannels) {
      return OM_CODEC_INVALID_PARAMS;
    }

    output_format_.channels = decoder_.mConfig.numChannels;
    output_format_.sample_rate = decoder_.mConfig.sampleRate;
    output_format_.planar = false;
    output_format_.bits_per_sample = decoder_.mConfig.bitDepth;

    switch (decoder_.mConfig.bitDepth) {
      case 16:
        output_format_.sample_format = OM_SAMPLE_S16;
        break;
      case 24:
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

    const uint32_t frame_length = decoder_.mConfig.frameLength;
    const uint32_t channels = decoder_.mConfig.numChannels;

    AudioSamples samples(output_format_, frame_length);
    uint32_t out_num_samples = 0;

    if (decoder_.Decode(&bits, samples.planes.data[0], frame_length, channels, &out_num_samples) != 0) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    if (decoder_.mConfig.bitDepth == 24) {
      expandPacked24ToS32(samples.planes.data[0], static_cast<size_t>(out_num_samples) * channels);
    }
    samples.nb_samples = out_num_samples;

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
    .decoder_factory = [] { return std::make_unique<ALACDecoder>(); },
    .encoder_factory = [] { return std::make_unique<ALACEncoder>(); },
};

} // namespace openmedia
