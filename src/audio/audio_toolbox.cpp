#include <codecs.hpp>

#if defined(__APPLE__)

#include <AudioToolbox/AudioToolbox.h>
#include <algorithm>
#include <cstring>
#include <format>
#include <openmedia/audio.hpp>
#include <openmedia/log.hpp>
#include <span>
#include <vector>

namespace openmedia {

namespace {

#if !defined(kAudioFormatEnhancedAC3)
constexpr UInt32 kAudioFormatEnhancedAC3 = 'ec-3';
#endif

constexpr UInt32 kDefaultOutputFrames = 4096;
constexpr OSStatus kParameterErrorStatus = -50;

auto codecIdToAudioToolboxFormat(OMCodecId codec_id) noexcept -> UInt32 {
  switch (codec_id) {
    case OM_CODEC_AAC: return kAudioFormatMPEG4AAC;
    case OM_CODEC_ALAC: return kAudioFormatAppleLossless;
    case OM_CODEC_MP3: return kAudioFormatMPEGLayer3;
    case OM_CODEC_AC3: return kAudioFormatAC3;
    case OM_CODEC_EAC3: return kAudioFormatEnhancedAC3;
    default: return 0;
  }
}

auto codecNeedsMagicCookie(OMCodecId codec_id) noexcept -> bool {
  return codec_id == OM_CODEC_AAC || codec_id == OM_CODEC_ALAC;
}

void appendBigEndian16(std::vector<uint8_t>& buffer, uint16_t value) {
  buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  buffer.push_back(static_cast<uint8_t>(value & 0xFF));
}

void appendBigEndian24(std::vector<uint8_t>& buffer, uint32_t value) {
  buffer.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  buffer.push_back(static_cast<uint8_t>(value & 0xFF));
}

void appendBigEndian32(std::vector<uint8_t>& buffer, uint32_t value) {
  buffer.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
  buffer.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  buffer.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  buffer.push_back(static_cast<uint8_t>(value & 0xFF));
}

void appendDescriptor(std::vector<uint8_t>& buffer, uint8_t tag, uint32_t size) {
  buffer.push_back(tag);
  for (int shift = 21; shift >= 7; shift -= 7) {
    buffer.push_back(static_cast<uint8_t>(((size >> shift) & 0x7F) | 0x80));
  }
  buffer.push_back(static_cast<uint8_t>(size & 0x7F));
}

auto buildAacMagicCookie(std::span<const uint8_t> extradata) -> std::vector<uint8_t> {
  std::vector<uint8_t> cookie;
  cookie.reserve(5 + 3 + 5 + 13 + 5 + extradata.size());

  appendDescriptor(cookie, 0x03, static_cast<uint32_t>(3 + 5 + 13 + 5 + extradata.size()));
  appendBigEndian16(cookie, 0);
  cookie.push_back(0x00);

  appendDescriptor(cookie, 0x04, static_cast<uint32_t>(13 + 5 + extradata.size()));
  cookie.push_back(0x40);
  cookie.push_back(0x15);
  appendBigEndian24(cookie, 0);
  appendBigEndian32(cookie, 0);
  appendBigEndian32(cookie, 0);

  appendDescriptor(cookie, 0x05, static_cast<uint32_t>(extradata.size()));
  cookie.insert(cookie.end(), extradata.begin(), extradata.end());
  return cookie;
}

auto buildMagicCookie(OMCodecId codec_id, std::span<const uint8_t> extradata) -> std::vector<uint8_t> {
  if (extradata.empty()) {
    return {};
  }
  if (codec_id == OM_CODEC_AAC) {
    return buildAacMagicCookie(extradata);
  }
  return std::vector<uint8_t>(extradata.begin(), extradata.end());
}

struct DecodeContext {
  const Packet* packet = nullptr;
  uint32_t channels = 0;
  uint32_t frames_per_packet = 0;
  AudioStreamPacketDescription packet_description = {};
  bool consumed = false;
};

auto decodeCallback(AudioConverterRef,
                    UInt32* io_number_data_packets,
                    AudioBufferList* io_data,
                    AudioStreamPacketDescription** out_data_packet_description,
                    void* user_data) -> OSStatus {
  auto* context = static_cast<DecodeContext*>(user_data);
  if (!context || !io_number_data_packets || !io_data) {
    return kParameterErrorStatus;
  }

  if (!context->packet || context->packet->bytes.empty() || context->consumed) {
    *io_number_data_packets = 0;
    if (out_data_packet_description) {
      *out_data_packet_description = &context->packet_description;
      context->packet_description.mStartOffset = 0;
      context->packet_description.mVariableFramesInPacket = 0;
      context->packet_description.mDataByteSize = 0;
    }
    return noErr;
  }

  io_data->mNumberBuffers = 1;
  io_data->mBuffers[0].mNumberChannels = context->channels;
  io_data->mBuffers[0].mDataByteSize = static_cast<UInt32>(context->packet->bytes.size());
  io_data->mBuffers[0].mData = const_cast<uint8_t*>(context->packet->bytes.data());

  context->packet_description.mStartOffset = 0;
  context->packet_description.mVariableFramesInPacket = context->frames_per_packet;
  context->packet_description.mDataByteSize = static_cast<UInt32>(context->packet->bytes.size());
  if (out_data_packet_description) {
    *out_data_packet_description = &context->packet_description;
  }

  *io_number_data_packets = 1;
  context->consumed = true;
  return noErr;
}

class AudioToolboxDecoder final : public Decoder {
public:
  explicit AudioToolboxDecoder(OMCodecId codec_id)
      : codec_id_(codec_id) {}

  ~AudioToolboxDecoder() override {
    closeConverter();
  }

  auto configure(const DecoderOptions& options) -> OMError override {
    closeConverter();

    if (options.format.type != OM_MEDIA_AUDIO || options.format.codec_id != codec_id_) {
      return OM_CODEC_INVALID_PARAMS;
    }

    input_format_ = {};
    input_format_.mFormatID = codecIdToAudioToolboxFormat(codec_id_);
    if (input_format_.mFormatID == 0) {
      return OM_CODEC_NOT_SUPPORTED;
    }

    magic_cookie_ = buildMagicCookie(codec_id_, options.extradata);
    if (codecNeedsMagicCookie(codec_id_) && magic_cookie_.empty()) {
      return OM_CODEC_INVALID_PARAMS;
    }

    if (!magic_cookie_.empty()) {
      UInt32 format_size = sizeof(input_format_);
      const auto* cookie_data = magic_cookie_.data();
      const auto cookie_size = static_cast<UInt32>(magic_cookie_.size());
      OSStatus status = AudioFormatGetProperty(kAudioFormatProperty_FormatInfo,
                                               cookie_size,
                                               cookie_data,
                                               &format_size,
                                               &input_format_);
      if (status != noErr) {
        logStatus(OM_LEVEL_ERROR, "AudioFormatGetProperty(kAudioFormatProperty_FormatInfo)", status);
        return OM_CODEC_OPEN_FAILED;
      }
    }

    if (options.format.audio.sample_rate != 0) {
      input_format_.mSampleRate = static_cast<Float64>(options.format.audio.sample_rate);
    }
    if (options.format.audio.channels != 0) {
      input_format_.mChannelsPerFrame = options.format.audio.channels;
    }

    if (input_format_.mSampleRate == 0 || input_format_.mChannelsPerFrame == 0) {
      return OM_CODEC_INVALID_PARAMS;
    }

    output_stream_description_ = {};
    output_stream_description_.mSampleRate = input_format_.mSampleRate;
    output_stream_description_.mFormatID = kAudioFormatLinearPCM;
    output_stream_description_.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    output_stream_description_.mBytesPerPacket = input_format_.mChannelsPerFrame * sizeof(int16_t);
    output_stream_description_.mFramesPerPacket = 1;
    output_stream_description_.mBytesPerFrame = input_format_.mChannelsPerFrame * sizeof(int16_t);
    output_stream_description_.mChannelsPerFrame = input_format_.mChannelsPerFrame;
    output_stream_description_.mBitsPerChannel = 16;

    OSStatus status = AudioConverterNew(&input_format_, &output_stream_description_, &converter_);
    if (status != noErr || !converter_) {
      logStatus(OM_LEVEL_ERROR, "AudioConverterNew", status);
      return OM_CODEC_OPEN_FAILED;
    }

    if (!magic_cookie_.empty()) {
      status = AudioConverterSetProperty(converter_,
                                         kAudioConverterDecompressionMagicCookie,
                                         static_cast<UInt32>(magic_cookie_.size()),
                                         magic_cookie_.data());
      if (status != noErr) {
        logStatus(OM_LEVEL_ERROR, "AudioConverterSetProperty(kAudioConverterDecompressionMagicCookie)", status);
        return OM_CODEC_OPEN_FAILED;
      }
    }

    updateOutputFormat();
    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!initialized_) {
      return std::nullopt;
    }

    DecodingInfo info = {};
    info.media_type = OM_MEDIA_AUDIO;
    info.audio_format = output_format_;
    return info;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    if (!initialized_ || !converter_) {
      return Err(OM_COMMON_NOT_INITIALIZED);
    }
    if (packet.bytes.empty()) {
      return Ok(std::vector<Frame> {});
    }

    const uint32_t channels = std::max<uint32_t>(output_format_.channels, 1);
    std::vector<int16_t> decode_buffer(static_cast<size_t>(output_frame_capacity_) * channels);

    AudioBufferList output_buffers = {};
    output_buffers.mNumberBuffers = 1;
    output_buffers.mBuffers[0].mNumberChannels = channels;
    output_buffers.mBuffers[0].mDataByteSize = static_cast<UInt32>(decode_buffer.size() * sizeof(int16_t));
    output_buffers.mBuffers[0].mData = decode_buffer.data();

    UInt32 packet_count = output_frame_capacity_;
    DecodeContext context;
    context.packet = &packet;
    context.channels = input_format_.mChannelsPerFrame;
    if (packet.duration > 0) {
      context.frames_per_packet = static_cast<uint32_t>(packet.duration);
    } else if (input_format_.mFramesPerPacket > 0) {
      context.frames_per_packet = input_format_.mFramesPerPacket;
    }

    OSStatus status = AudioConverterFillComplexBuffer(converter_,
                                                      decodeCallback,
                                                      &context,
                                                      &packet_count,
                                                      &output_buffers,
                                                      nullptr);
    if (status != noErr) {
      logStatus(OM_LEVEL_ERROR, "AudioConverterFillComplexBuffer", status);
      return Err(OM_CODEC_DECODE_FAILED);
    }

    if (packet_count == 0) {
      return Ok(std::vector<Frame> {});
    }

    AudioSamples samples(output_format_, packet_count);
    samples.bits_per_sample = output_format_.bits_per_sample;
    std::memcpy(samples.planes.data[0],
                decode_buffer.data(),
                static_cast<size_t>(packet_count) * output_format_.channels * sizeof(int16_t));

    Frame frame = {};
    frame.pts = packet.pts >= 0 ? static_cast<uint64_t>(packet.pts) : 0;
    frame.dts = packet.dts >= 0 ? static_cast<uint64_t>(packet.dts) : frame.pts;
    frame.data = std::move(samples);

    std::vector<Frame> frames;
    frames.push_back(std::move(frame));
    updateOutputFormat();
    return Ok(std::move(frames));
  }

  void flush() override {
    if (converter_) {
      AudioConverterReset(converter_);
    }
  }

private:
  void closeConverter() {
    initialized_ = false;
    output_frame_capacity_ = kDefaultOutputFrames;
    output_format_ = {};
    input_format_ = {};
    output_stream_description_ = {};
    magic_cookie_.clear();
    if (converter_) {
      AudioConverterDispose(converter_);
      converter_ = nullptr;
    }
  }

  void updateOutputFormat() {
    output_format_.sample_format = OM_SAMPLE_S16;
    output_format_.bits_per_sample = 16;
    output_format_.planar = false;

    UInt32 size = sizeof(output_stream_description_);
    if (converter_ &&
        AudioConverterGetProperty(converter_,
                                  kAudioConverterCurrentOutputStreamDescription,
                                  &size,
                                  &output_stream_description_) == noErr) {
      if (output_stream_description_.mSampleRate != 0) {
        output_format_.sample_rate = static_cast<uint32_t>(output_stream_description_.mSampleRate);
      }
      if (output_stream_description_.mChannelsPerFrame != 0) {
        output_format_.channels = output_stream_description_.mChannelsPerFrame;
      }
    } else {
      output_format_.sample_rate = static_cast<uint32_t>(input_format_.mSampleRate);
      output_format_.channels = input_format_.mChannelsPerFrame;
    }

    size = sizeof(input_format_);
    if (converter_ &&
        AudioConverterGetProperty(converter_,
                                  kAudioConverterCurrentInputStreamDescription,
                                  &size,
                                  &input_format_) == noErr &&
        input_format_.mFramesPerPacket != 0) {
      output_frame_capacity_ = std::max<uint32_t>(kDefaultOutputFrames,
                                                  input_format_.mFramesPerPacket * 2);
    }

    if (output_format_.sample_rate == 0) {
      output_format_.sample_rate = static_cast<uint32_t>(input_format_.mSampleRate);
    }
    if (output_format_.channels == 0) {
      output_format_.channels = std::max<uint32_t>(input_format_.mChannelsPerFrame, 1);
    }
  }

  void logStatus(OMLogLevel level, std::string_view operation, OSStatus status) const {
    log(OM_CATEGORY_DECODER,
        level,
        "AudioToolbox {} failed: {}",
        operation,
        static_cast<int32_t>(status));
  }

  OMCodecId codec_id_ = OM_CODEC_NONE;
  AudioConverterRef converter_ = nullptr;
  AudioStreamBasicDescription input_format_ = {};
  AudioStreamBasicDescription output_stream_description_ = {};
  AudioFormat output_format_ = {};
  std::vector<uint8_t> magic_cookie_;
  UInt32 output_frame_capacity_ = kDefaultOutputFrames;
  bool initialized_ = false;
};

auto createAudioToolboxDecoder(OMCodecId codec_id) -> std::unique_ptr<Decoder> {
  return std::make_unique<AudioToolboxDecoder>(codec_id);
}

} // namespace

const CodecDescriptor CODEC_AUDIO_TOOLBOX_ALAC = {
  .codec_id = OM_CODEC_ALAC,
  .type = OM_MEDIA_AUDIO,
  .name = "audiotoolbox_alac",
  .long_name = "Apple Lossless Audio Codec (AudioToolbox)",
  .vendor = "Apple",
  .flags = NONE,
  .decoder_factory = [] { return createAudioToolboxDecoder(OM_CODEC_ALAC); },
};

const CodecDescriptor CODEC_AUDIO_TOOLBOX_MP3 = {
  .codec_id = OM_CODEC_MP3,
  .type = OM_MEDIA_AUDIO,
  .name = "audiotoolbox_mp3",
  .long_name = "MP3 (AudioToolbox)",
  .vendor = "Apple",
  .flags = NONE,
  .decoder_factory = [] { return createAudioToolboxDecoder(OM_CODEC_MP3); },
};

const CodecDescriptor CODEC_AUDIO_TOOLBOX_AAC = {
  .codec_id = OM_CODEC_AAC,
  .type = OM_MEDIA_AUDIO,
  .name = "audiotoolbox_aac",
  .long_name = "AAC (AudioToolbox)",
  .vendor = "Apple",
  .flags = NONE,
  .decoder_factory = [] { return createAudioToolboxDecoder(OM_CODEC_AAC); },
};

const CodecDescriptor CODEC_AUDIO_TOOLBOX_AC3 = {
  .codec_id = OM_CODEC_AC3,
  .type = OM_MEDIA_AUDIO,
  .name = "audiotoolbox_ac3",
  .long_name = "AC-3 (AudioToolbox)",
  .vendor = "Apple",
  .flags = NONE,
  .decoder_factory = [] { return createAudioToolboxDecoder(OM_CODEC_AC3); },
};

const CodecDescriptor CODEC_AUDIO_TOOLBOX_EAC3 = {
  .codec_id = OM_CODEC_EAC3,
  .type = OM_MEDIA_AUDIO,
  .name = "audiotoolbox_eac3",
  .long_name = "E-AC-3 (AudioToolbox)",
  .vendor = "Apple",
  .flags = NONE,
  .decoder_factory = [] { return createAudioToolboxDecoder(OM_CODEC_EAC3); },
};

} // namespace openmedia

#endif
