#include <codecs.hpp>
#include <openmedia/audio.hpp>
#include <openmedia/codec_extra.hpp>
#include <opus.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace openmedia {

class OpusDecoder final : public Decoder {
  ::OpusDecoder* decoder_ = nullptr;
  int channels_ = 0;
  int sample_rate_ = 0;
  AudioFormat output_format_;

public:
  OpusDecoder() {
    output_format_.sample_format = OM_SAMPLE_F32;
    output_format_.bits_per_sample = 32;
  }

  ~OpusDecoder() override {
    if (decoder_) {
      opus_decoder_destroy(decoder_);
    }
  }

  static auto isHeadMagic(std::span<const uint8_t> payload) -> bool {
    if (payload.size() < 8) return false;
    return memcmp(payload.data(), "OpusHead", 8) == 0;
  }

  auto handleHead(std::span<const uint8_t> payload) -> OMError {
    if (payload.size() < 19) return OM_SUCCESS;
    channels_ = payload.data()[9];
    sample_rate_ = 48000;
    int error = 0;
    if (decoder_) {
      opus_decoder_destroy(decoder_);
    }
    decoder_ = opus_decoder_create(sample_rate_, channels_, &error);
    if (error != OPUS_OK) {
      return OM_CODEC_OPEN_FAILED;
    }
    return OM_SUCCESS;
  }

  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_OPUS) {
      return OM_CODEC_INVALID_PARAMS;
    }
    if (!options.extradata.empty() && isHeadMagic(options.extradata)) {
      OMError err = handleHead(options.extradata);
      if (err != OM_SUCCESS) {
        return err;
      }
    }
    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!decoder_) return std::nullopt;

    output_format_.sample_rate = static_cast<uint32_t>(sample_rate_);
    output_format_.channels = static_cast<uint32_t>(channels_);

    DecodingInfo info;
    info.media_type = OM_MEDIA_AUDIO;
    info.audio_format = output_format_;
    return info;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    if (isHeadMagic(packet.bytes)) {
      OMError err = handleHead(packet.bytes);
      if (err != OM_SUCCESS) {
        return Err(err);
      }
      return Ok(std::vector<Frame>{});
    }

    if (packet.bytes.size() >= 8 && memcmp(packet.bytes.data(), "OpusTags", 8) == 0) {
      return Ok(std::vector<Frame>{});
    }

    if (!decoder_) {
      return Ok(std::vector<Frame>{});
    }

    constexpr int MAX_SAMPLES = 5760;
    output_format_.sample_rate = static_cast<uint32_t>(sample_rate_);
    output_format_.channels = static_cast<uint32_t>(channels_);

    AudioSamples samples_fmt(output_format_, MAX_SAMPLES);
    int samples = opus_decode_float(decoder_, packet.bytes.data(),
                                    static_cast<opus_int32>(packet.bytes.size()),
                                    reinterpret_cast<float*>(samples_fmt.planes.data[0]),
                                    MAX_SAMPLES, 0);

    if (samples > 0) {
      samples_fmt.nb_samples = static_cast<uint32_t>(samples);

      Frame frame;
      frame.pts = packet.pts;
      frame.dts = packet.dts;
      frame.data = std::move(samples_fmt);

      std::vector<Frame> frames;
      frames.push_back(std::move(frame));
      return Ok(std::move(frames));
    }

    return Ok(std::vector<Frame>{});
  }

  void flush() override {
    if (decoder_) {
      opus_decoder_ctl(decoder_, OPUS_RESET_STATE);
    }
  }
};

class OpusEncoder final : public Encoder {
  ::OpusEncoder* encoder_ = nullptr;
  int sample_rate_ = 48000;
  int channels_ = 2;
  int frame_size_ = 960; // 20ms @ 48kHz
  int application_ = OPUS_APPLICATION_AUDIO;
  AudioFormat input_format_;
  std::vector<uint8_t> extradata_;

public:
  OpusEncoder() {
    input_format_.sample_format = OM_SAMPLE_F32;
    input_format_.bits_per_sample = 32;
    input_format_.planar = false;
  }

  ~OpusEncoder() override {
    if (encoder_) {
      opus_encoder_destroy(encoder_);
    }
  }

  auto configure(const EncoderOptions& options) -> OMError override {
    if (options.format.type != OM_MEDIA_AUDIO) {
      return OM_CODEC_INVALID_PARAMS;
    }

    sample_rate_ = static_cast<int>(options.format.audio.sample_rate);
    channels_ = static_cast<int>(options.format.audio.channels);

    input_format_.sample_rate = options.format.audio.sample_rate;
    input_format_.channels = options.format.audio.channels;
    input_format_.sample_format = OM_SAMPLE_F32;
    input_format_.bits_per_sample = 32;
    input_format_.planar = false;

    if (channels_ < 1 || channels_ > 255) {
      return OM_CODEC_INVALID_PARAMS;
    }

    frame_size_ = sample_rate_ / 50; // 20ms

    int error = 0;
    encoder_ = opus_encoder_create(sample_rate_, channels_, application_, &error);
    if (error != OPUS_OK || !encoder_) {
      return OM_CODEC_OPEN_FAILED;
    }

    const auto& extra = options.extra;

    if (extra.contains(OPUS_ENC_APPLICATION)) {
      int32_t app = extra.getInt32(OPUS_ENC_APPLICATION);
      opus_encoder_ctl(encoder_, OPUS_SET_APPLICATION(app));
    }

    if (extra.contains(OPUS_ENC_BITRATE)) {
      int32_t bitrate = extra.getInt32(OPUS_ENC_BITRATE);
      opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bitrate));
    }

    if (extra.contains(OPUS_ENC_VBR)) {
      int32_t vbr = extra.getInt32(OPUS_ENC_VBR);
      opus_encoder_ctl(encoder_, OPUS_SET_VBR(vbr != 0));
    }

    if (extra.contains(OPUS_ENC_COMPLEXITY)) {
      int32_t complexity = extra.getInt32(OPUS_ENC_COMPLEXITY);
      opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(complexity));
    }

    if (extra.contains(OPUS_ENC_FRAME_SIZE)) {
      int32_t frame_size = extra.getInt32(OPUS_ENC_FRAME_SIZE);
      frame_size_ = frame_size;
    }

    if (extra.contains(OPUS_ENC_FORCE_CHANNELS)) {
      int32_t force_ch = extra.getInt32(OPUS_ENC_FORCE_CHANNELS);
      opus_encoder_ctl(encoder_, OPUS_SET_FORCE_CHANNELS(force_ch));
    }

    if (extra.contains(OPUS_ENC_SIGNAL_TYPE)) {
      int32_t signal = extra.getInt32(OPUS_ENC_SIGNAL_TYPE);
      opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(signal));
    }

    if (extra.contains(OPUS_ENC_BANDWIDTH)) {
      int32_t bandwidth = extra.getInt32(OPUS_ENC_BANDWIDTH);
      opus_encoder_ctl(encoder_, OPUS_SET_BANDWIDTH(bandwidth));
    }

    if (extra.contains(OPUS_ENC_PACKET_LOSS_PERC)) {
      int32_t loss = extra.getInt32(OPUS_ENC_PACKET_LOSS_PERC);
      opus_encoder_ctl(encoder_, OPUS_SET_PACKET_LOSS_PERC(loss));
    }

    if (extra.contains(OPUS_ENC_FEC)) {
      int32_t fec = extra.getInt32(OPUS_ENC_FEC);
      opus_encoder_ctl(encoder_, OPUS_SET_INBAND_FEC(fec));
    }

    if (extra.contains(OPUS_ENC_DTX)) {
      int32_t dtx = extra.getInt32(OPUS_ENC_DTX);
      opus_encoder_ctl(encoder_, OPUS_SET_DTX(dtx != 0));
    }

    if (extra.contains(OPUS_ENC_LSB_DEPTH)) {
      int32_t lsb = extra.getInt32(OPUS_ENC_LSB_DEPTH);
      opus_encoder_ctl(encoder_, OPUS_SET_LSB_DEPTH(lsb));
    }

    int lookahead = 0;
    opus_encoder_ctl(encoder_, OPUS_GET_LOOKAHEAD(&lookahead));

    extradata_.resize(19);
    std::memcpy(extradata_.data(), "OpusHead", 8);
    extradata_[8] = 1;
    extradata_[9] = static_cast<uint8_t>(channels_);
    extradata_[10] = 0;
    extradata_[11] = 0;
    extradata_[12] = 48000 & 0xFF;
    extradata_[13] = (48000 >> 8) & 0xFF;
    extradata_[14] = (48000 >> 16) & 0xFF;
    extradata_[15] = (48000 >> 24) & 0xFF;
    extradata_[16] = 0;
    extradata_[17] = 0;
    extradata_[18] = 0;

    return OM_SUCCESS;
  }

  auto getInfo() -> EncodingInfo override {
    EncodingInfo info;
    info.extradata = extradata_;
    return info;
  }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    if (!encoder_) {
      return Err(OM_CODEC_ENCODE_FAILED);
    }

    const auto* audio_data = std::get_if<AudioSamples>(&frame.data);
    if (!audio_data) {
      return Err(OM_CODEC_INVALID_PARAMS);
    }

    if (audio_data->format.sample_format != OM_SAMPLE_F32) {
      return Err(OM_CODEC_INVALID_PARAMS);
    }

    std::vector<Packet> packets;

    const float* input = reinterpret_cast<const float*>(audio_data->planes.data[0]);
    int total_samples = static_cast<int>(audio_data->nb_samples);
    int samples_per_channel = total_samples;

    std::vector<uint8_t> packet_buffer(4000);

    int offset = 0;
    while (offset < samples_per_channel) {
      int remaining = samples_per_channel - offset;
      int to_encode = std::min(remaining, frame_size_);

      int encoded_bytes;
      if (channels_ == 1) {
        encoded_bytes = opus_encode_float(encoder_, input + offset, to_encode,
                                          packet_buffer.data(),
                                          static_cast<opus_int32>(packet_buffer.size()));
      } else {
        encoded_bytes = opus_encode_float(encoder_, input + offset * channels_, to_encode,
                                          packet_buffer.data(),
                                          static_cast<opus_int32>(packet_buffer.size()));
      }

      if (encoded_bytes > 0) {
        Packet packet;
        packet.allocate(static_cast<size_t>(encoded_bytes));
        std::memcpy(packet.bytes.data(), packet_buffer.data(), static_cast<size_t>(encoded_bytes));
        packet.pts = frame.pts;
        packet.dts = frame.dts;
        packets.push_back(std::move(packet));
      } else {
        return Err(OM_CODEC_ENCODE_FAILED);
      }

      offset += to_encode;
    }

    return Ok(std::move(packets));
  }

  auto updateBitrate(const RateControlParams& rc) -> OMError override {
    if (!encoder_) {
      return OM_CODEC_INVALID_PARAMS;
    }

    if (auto* vbr = std::get_if<VbrParams>(&rc.params)) {
      opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(static_cast<opus_int32>(vbr->bitrate.target_bitrate)));
    } else if (auto* cbr = std::get_if<CbrParams>(&rc.params)) {
      opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(static_cast<opus_int32>(cbr->bitrate.target_bitrate)));
    } else if (auto* abr = std::get_if<AbrParams>(&rc.params)) {
      opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(static_cast<opus_int32>(abr->target_bitrate)));
    }

    return OM_SUCCESS;
  }
};

const CodecDescriptor CODEC_OPUS = {
  .codec_id = OM_CODEC_OPUS,
  .type = OM_MEDIA_AUDIO,
  .name = "opus",
  .long_name = "Opus",
  .vendor = "Xiph.Org",
  .flags = NONE,
  .decoder_factory = [] { return std::make_unique<OpusDecoder>(); },
  .encoder_factory = [] { return std::make_unique<OpusEncoder>(); },
};

} // namespace openmedia
