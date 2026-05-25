#include <fdk-aac/aacdecoder_lib.h>
#include <fdk-aac/aacenc_lib.h>
#include <algorithm>
#include <array>
#include <codecs.hpp>
#include <cstring>
#include <openmedia/audio.hpp>
#include <vector>

namespace openmedia {

struct ProfileEntry {
  OMProfile om_profile;
  AUDIO_OBJECT_TYPE fdk_aot;
};

static constexpr ProfileEntry PROFILE_MAP[] = {
    {OM_PROFILE_AAC_MAIN, AOT_AAC_MAIN},
    {OM_PROFILE_AAC_LC, AOT_AAC_LC},
    {OM_PROFILE_AAC_SSR, AOT_AAC_SSR},
    {OM_PROFILE_AAC_LTP, AOT_AAC_LTP},
    {OM_PROFILE_AAC_HE, AOT_SBR},
    {OM_PROFILE_AAC_HE_V2, AOT_PS},
    {OM_PROFILE_AAC_LD, AOT_ER_AAC_LD},
    {OM_PROFILE_AAC_ELD, AOT_ER_AAC_ELD},
    {OM_PROFILE_AAC_USAC, AOT_USAC},
    {OM_PROFILE_MPEG2_AAC_LOW, AOT_AAC_LC},
    {OM_PROFILE_MPEG2_AAC_HE, AOT_SBR},
};

static constexpr auto aotFromProfile(OMProfile profile) -> AUDIO_OBJECT_TYPE {
  for (const auto& entry : PROFILE_MAP) {
    if (entry.om_profile == profile) return entry.fdk_aot;
  }
  return AOT_NONE;
}

static auto channelModeFromCount(uint32_t channels) -> CHANNEL_MODE {
  switch (channels) {
    case 1: return MODE_1;
    case 2: return MODE_2;
    case 3: return MODE_1_2;
    case 4: return MODE_1_2_1;
    case 5: return MODE_1_2_2;
    case 6: return MODE_1_2_2_1;
    default: return MODE_INVALID;
  }
}

class FDKAACEncoder final : public Encoder {
  HANDLE_AACENCODER encoder_ = nullptr;
  AudioFormat input_format_ = {};
  std::vector<uint8_t> extradata_;
  std::vector<int16_t> pending_samples_;
  std::vector<uint8_t> output_buffer_;
  int frame_length_ = 0;

public:
  ~FDKAACEncoder() override {
    if (encoder_) {
      aacEncClose(&encoder_);
    }
  }

  auto configure(const EncoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_AAC || options.format.type != OM_MEDIA_AUDIO) {
      return OM_CODEC_INVALID_PARAMS;
    }
    if (options.audio_format.sample_rate == 0 || options.audio_format.channels == 0) {
      return OM_CODEC_INVALID_PARAMS;
    }
    if (options.audio_format.sample_format != OM_SAMPLE_S16) {
      return OM_CODEC_INVALID_PARAMS;
    }

    if (encoder_) {
      aacEncClose(&encoder_);
      encoder_ = nullptr;
    }

    const CHANNEL_MODE channel_mode = channelModeFromCount(options.audio_format.channels);
    if (channel_mode == MODE_INVALID) {
      return OM_CODEC_NOT_SUPPORTED;
    }

    AACENC_ERROR err = aacEncOpen(&encoder_, 0, options.audio_format.channels);
    if (err != AACENC_OK || !encoder_) {
      return OM_CODEC_OPEN_FAILED;
    }

    const AUDIO_OBJECT_TYPE aot =
        options.format.profile == OM_PROFILE_NONE ? AOT_AAC_LC : aotFromProfile(options.format.profile);
    if (aot == AOT_NONE) {
      aacEncClose(&encoder_);
      encoder_ = nullptr;
      return OM_CODEC_INVALID_PARAMS;
    }

    auto set_param = [&](AACENC_PARAM param, UINT value) -> bool {
      return aacEncoder_SetParam(encoder_, param, value) == AACENC_OK;
    };

    if (!set_param(AACENC_AOT, static_cast<UINT>(aot)) ||
        !set_param(AACENC_SAMPLERATE, options.audio_format.sample_rate) ||
        !set_param(AACENC_CHANNELMODE, static_cast<UINT>(channel_mode)) ||
        !set_param(AACENC_CHANNELORDER, 1) ||
        !set_param(AACENC_TRANSMUX, 0) ||
        !set_param(AACENC_AFTERBURNER, 1)) {
      aacEncClose(&encoder_);
      encoder_ = nullptr;
      return OM_CODEC_OPEN_FAILED;
    }

    if (auto* abr = std::get_if<AbrParams>(&options.rate_control.params)) {
      set_param(AACENC_BITRATE, static_cast<UINT>(abr->target_bitrate));
    } else if (auto* cbr = std::get_if<CbrParams>(&options.rate_control.params)) {
      set_param(AACENC_BITRATE, static_cast<UINT>(cbr->bitrate.target_bitrate));
    } else if (auto* vbr = std::get_if<VbrParams>(&options.rate_control.params)) {
      set_param(AACENC_BITRATE, static_cast<UINT>(vbr->bitrate.target_bitrate));
    }

    if (aacEncEncode(encoder_, nullptr, nullptr, nullptr, nullptr) != AACENC_OK) {
      aacEncClose(&encoder_);
      encoder_ = nullptr;
      return OM_CODEC_OPEN_FAILED;
    }

    AACENC_InfoStruct info = {};
    if (aacEncInfo(encoder_, &info) != AACENC_OK || info.frameLength <= 0) {
      aacEncClose(&encoder_);
      encoder_ = nullptr;
      return OM_CODEC_OPEN_FAILED;
    }

    frame_length_ = info.frameLength;
    extradata_.assign(info.confBuf, info.confBuf + info.confSize);
    output_buffer_.resize(std::max<int>(info.maxOutBufBytes, 8192));
    pending_samples_.clear();

    input_format_ = options.audio_format;
    input_format_.planar = false;
    input_format_.bits_per_sample = 16;
    return OM_SUCCESS;
  }

  auto getInfo() -> EncodingInfo override {
    EncodingInfo info = {};
    info.extradata = extradata_;
    return info;
  }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    if (!encoder_ || frame_length_ <= 0) {
      return Err(OM_COMMON_NOT_INITIALIZED);
    }

    const auto* samples = std::get_if<AudioSamples>(&frame.data);
    if (!samples) {
      return Err(OM_CODEC_INVALID_PARAMS);
    }
    if (samples->format.sample_format != OM_SAMPLE_S16 ||
        samples->format.channels != input_format_.channels) {
      return Err(OM_CODEC_INVALID_PARAMS);
    }

    appendSamples(*samples);

    std::vector<Packet> packets;
    const size_t frame_stride = static_cast<size_t>(frame_length_) * input_format_.channels;
    while (pending_samples_.size() >= frame_stride) {
      auto encoded = encodeFrame(pending_samples_.data(), frame);
      if (encoded.isErr()) {
        return Err(encoded.unwrapErr());
      }
      auto chunk_packets = encoded.unwrap();
      packets.insert(packets.end(),
                     std::make_move_iterator(chunk_packets.begin()),
                     std::make_move_iterator(chunk_packets.end()));
      pending_samples_.erase(pending_samples_.begin(), pending_samples_.begin() + static_cast<std::ptrdiff_t>(frame_stride));
    }

    return Ok(std::move(packets));
  }

  auto updateBitrate(const RateControlParams& rc) -> OMError override {
    if (!encoder_) return OM_COMMON_NOT_INITIALIZED;

    UINT bitrate = 0;
    if (auto* abr = std::get_if<AbrParams>(&rc.params)) {
      bitrate = static_cast<UINT>(abr->target_bitrate);
    } else if (auto* cbr = std::get_if<CbrParams>(&rc.params)) {
      bitrate = static_cast<UINT>(cbr->bitrate.target_bitrate);
    } else if (auto* vbr = std::get_if<VbrParams>(&rc.params)) {
      bitrate = static_cast<UINT>(vbr->bitrate.target_bitrate);
    }

    if (bitrate == 0) return OM_SUCCESS;
    return aacEncoder_SetParam(encoder_, AACENC_BITRATE, bitrate) == AACENC_OK ? OM_SUCCESS : OM_CODEC_INVALID_PARAMS;
  }

private:
  void appendSamples(const AudioSamples& samples) {
    const size_t sample_count = static_cast<size_t>(samples.nb_samples) * samples.format.channels;
    const auto old_size = pending_samples_.size();
    pending_samples_.resize(old_size + sample_count);
    auto* dst = pending_samples_.data() + old_size;

    if (!samples.format.planar) {
      std::memcpy(dst, samples.buffer->bytes().data(), sample_count * sizeof(int16_t));
      return;
    }

    for (uint32_t sample = 0; sample < samples.nb_samples; ++sample) {
      for (uint32_t channel = 0; channel < samples.format.channels; ++channel) {
        const auto* src = reinterpret_cast<const int16_t*>(samples.planes.data[channel]);
        *dst++ = src[sample];
      }
    }
  }

  auto encodeFrame(const int16_t* input, const Frame& frame) -> Result<std::vector<Packet>, OMError> {
    void* in_ptr = const_cast<int16_t*>(input);
    int in_identifier = IN_AUDIO_DATA;
    int in_size = static_cast<int>(frame_length_ * input_format_.channels * sizeof(int16_t));
    int in_elem_size = sizeof(int16_t);
    AACENC_BufDesc in_buf = {};
    in_buf.numBufs = 1;
    in_buf.bufs = &in_ptr;
    in_buf.bufferIdentifiers = &in_identifier;
    in_buf.bufSizes = &in_size;
    in_buf.bufElSizes = &in_elem_size;

    void* out_ptr = output_buffer_.data();
    int out_identifier = OUT_BITSTREAM_DATA;
    int out_size = static_cast<int>(output_buffer_.size());
    int out_elem_size = sizeof(uint8_t);
    AACENC_BufDesc out_buf = {};
    out_buf.numBufs = 1;
    out_buf.bufs = &out_ptr;
    out_buf.bufferIdentifiers = &out_identifier;
    out_buf.bufSizes = &out_size;
    out_buf.bufElSizes = &out_elem_size;

    AACENC_InArgs in_args = {};
    in_args.numInSamples = frame_length_ * input_format_.channels;
    AACENC_OutArgs out_args = {};

    if (aacEncEncode(encoder_, &in_buf, &out_buf, &in_args, &out_args) != AACENC_OK) {
      return Err(OM_CODEC_ENCODE_FAILED);
    }

    std::vector<Packet> packets;
    if (out_args.numOutBytes > 0) {
      Packet packet = {};
      packet.allocate(static_cast<size_t>(out_args.numOutBytes));
      std::memcpy(packet.bytes.data(), output_buffer_.data(), static_cast<size_t>(out_args.numOutBytes));
      packet.pts = frame.pts;
      packet.dts = frame.dts;
      packets.push_back(std::move(packet));
    }
    return Ok(std::move(packets));
  }
};

class FDKAACDecoder final : public Decoder {
  HANDLE_AACDECODER decoder_ = nullptr;
  AudioFormat output_format_;
  bool initialized_ = false;
  std::vector<INT_PCM> decode_buffer_;
  CStreamInfo* stream_info_ = nullptr;

public:
  FDKAACDecoder() = default;

  ~FDKAACDecoder() override {
    if (decoder_) {
      aacDecoder_Close(decoder_);
    }
  }

  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_AAC) {
      return OM_CODEC_INVALID_PARAMS;
    }

    const TRANSPORT_TYPE transport =
        options.extradata.empty() ? TT_MP4_ADTS : TT_MP4_RAW;

    decoder_ = aacDecoder_Open(transport, 1);
    if (!decoder_) {
      return OM_CODEC_OPEN_FAILED;
    }

    if (options.format.profile != OM_PROFILE_NONE &&
        aotFromProfile(options.format.profile) == AOT_NONE) {
      log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, "FDK AAC: Unsupported profile requested");
      aacDecoder_Close(decoder_);
      decoder_ = nullptr;
      return OM_CODEC_INVALID_PARAMS;
    }

    if (!options.extradata.empty()) {
      UCHAR* conf = const_cast<UCHAR*>(options.extradata.data());
      UINT conf_size = static_cast<UINT>(options.extradata.size());

      if (aacDecoder_ConfigRaw(decoder_, &conf, &conf_size) != AAC_DEC_OK) {
        log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, "FDK AAC: Failed to configure decoder");
        return OM_CODEC_INVALID_PARAMS;
      }
    }

    if (aacDecoder_SetParam(decoder_, AAC_CONCEAL_METHOD, 0) != AAC_DEC_OK) {
      log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, "FDK AAC: Unable to set concealment method");
      return OM_CODEC_INVALID_PARAMS;
    }

    initialized_ = true;
    decode_buffer_.resize(2048 * 8);

    stream_info_ = aacDecoder_GetStreamInfo(decoder_);

    if (stream_info_->aacSampleRate == 0 || stream_info_->channelConfig == 0) {
      return OM_CODEC_OPEN_FAILED;
    }

    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!initialized_ || !decoder_) return std::nullopt;

    CStreamInfo* info = aacDecoder_GetStreamInfo(decoder_);
    if (!info) return std::nullopt;

    output_format_.sample_rate = info->sampleRate;
    output_format_.channels = info->numChannels;
    output_format_.planar = false;
    output_format_.sample_format = OM_SAMPLE_S16;

    DecodingInfo dec_info = {};
    dec_info.media_type = OM_MEDIA_AUDIO;
    dec_info.audio_format = output_format_;
    return dec_info;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    if (!decoder_ || packet.bytes.empty()) {
      return Ok(std::vector<Frame> {});
    }

    UCHAR* in_buf = packet.bytes.data();
    UINT in_size = static_cast<UINT>(packet.bytes.size());
    UINT bytes_valid = in_size;

    AAC_DECODER_ERROR error =
        aacDecoder_Fill(decoder_, &in_buf, &in_size, &bytes_valid);
    if (error != AAC_DEC_OK) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    error = aacDecoder_DecodeFrame(
        decoder_,
        decode_buffer_.data(),
        static_cast<INT>(decode_buffer_.size()),
        0);

    if (error == AAC_DEC_NOT_ENOUGH_BITS) {
      return Err(OM_CODEC_NEED_MORE_DATA);
    }

    if (error != AAC_DEC_OK) {
      log(OM_CATEGORY_DECODER, OM_LEVEL_WARNING, "FDK AAC: Decode frame failed");
      return Err(OM_CODEC_DECODE_FAILED);
    }

    const INT channels = stream_info_->channelConfig;
    const INT samples_per_frame = stream_info_->frameSize;

    AudioFormat fmt = {};
    fmt.sample_rate = static_cast<uint32_t>(stream_info_->aacSampleRate);
    fmt.channels = static_cast<uint32_t>(channels);
    fmt.planar = false;
    fmt.sample_format = OM_SAMPLE_S16;

    AudioSamples samples(fmt, samples_per_frame);
    samples.nb_samples = samples_per_frame;
    memcpy(samples.planes.data[0], decode_buffer_.data(), samples_per_frame * channels * sizeof(int16_t));

    Frame frame;
    frame.pts = packet.pts;
    frame.dts = packet.dts;
    frame.data = std::move(samples);

    std::vector<Frame> frames;
    frames.push_back(std::move(frame));

    return Ok(std::move(frames));
  }

  void flush() override {
    if (decoder_) {
      aacDecoder_SetParam(decoder_, AAC_TPDEC_CLEAR_BUFFER, 1);
    }
  }
};

static constexpr auto buildSupportedProfiles() {
  std::array<OMProfile, std::size(PROFILE_MAP)> profiles {};
  for (std::size_t i = 0; i < std::size(PROFILE_MAP); ++i) {
    profiles[i] = PROFILE_MAP[i].om_profile;
  }
  return profiles;
}

static constexpr auto k_supported_profiles = buildSupportedProfiles();

const CodecDescriptor CODEC_FDK_AAC = {
    .codec_id = OM_CODEC_AAC,
    .type = OM_MEDIA_AUDIO,
    .name = "fdk_aac",
    .long_name = "Fraunhofer FDK AAC",
    .vendor = "Fraunhofer IIS",
    .flags = NONE,
    .caps = CodecCaps {
        .profiles = {k_supported_profiles.begin(), k_supported_profiles.end()},
        .audio = AudioCodecCaps {
            .fmt_s16 = true,
            .sample_rates = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000},
        },
    },
    .decoder_factory = [] { return std::make_unique<FDKAACDecoder>(); },
    .encoder_factory = [] { return std::make_unique<FDKAACEncoder>(); },
};

} // namespace openmedia
