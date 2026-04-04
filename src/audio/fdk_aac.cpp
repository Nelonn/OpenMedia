#include <fdk-aac/aacdecoder_lib.h>
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

class FDKAACDecoder final : public Decoder {
  HANDLE_AACDECODER decoder_ = nullptr;
  AudioFormat output_format_;
  bool initialized_ = false;
  LoggerRef logger_ = {};
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
    logger_ = options.logger ? options.logger : Logger::refDefault();

    const TRANSPORT_TYPE transport =
        options.extradata.empty() ? TT_MP4_ADTS : TT_MP4_RAW;

    decoder_ = aacDecoder_Open(transport, 1);
    if (!decoder_) {
      return OM_CODEC_OPEN_FAILED;
    }

    if (options.format.profile != OM_PROFILE_NONE &&
        aotFromProfile(options.format.profile) == AOT_NONE) {
      if (logger_) {
        logger_->log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR,
                     "FDK AAC: Unsupported profile requested");
      }
      aacDecoder_Close(decoder_);
      decoder_ = nullptr;
      return OM_CODEC_INVALID_PARAMS;
    }

    if (!options.extradata.empty()) {
      UCHAR* conf = const_cast<UCHAR*>(options.extradata.data());
      UINT conf_size = static_cast<UINT>(options.extradata.size());

      if (aacDecoder_ConfigRaw(decoder_, &conf, &conf_size) != AAC_DEC_OK) {
        if (logger_) {
          logger_->log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR,
                       "FDK AAC: Failed to configure decoder");
        }
        return OM_CODEC_INVALID_PARAMS;
      }
    }

    if (aacDecoder_SetParam(decoder_, AAC_CONCEAL_METHOD, 0) != AAC_DEC_OK) {
      if (logger_) {
        logger_->log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR,
                     "FDK AAC: Unable to set concealment method");
      }
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
      if (logger_) {
        logger_->log(OM_CATEGORY_DECODER, OM_LEVEL_WARNING,
                     "FDK AAC: Decode frame failed");
      }
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
    },
    .decoder_factory = [] { return std::make_unique<FDKAACDecoder>(); },
};

} // namespace openmedia
