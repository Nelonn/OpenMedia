#include <opus.h>
#include <opus_multistream.h>
#include <algorithm>
#include <codecs.hpp>
#include <memory>
#include <openmedia/audio.hpp>
#include <openmedia/codec_extra.hpp>
#include <util/cpp.hpp>
#include <variant>
#include <vector>

namespace openmedia {

struct OpusDecoderDeleter {
  void operator()(::OpusDecoder* d) const { opus_decoder_destroy(d); }
};

struct OpusMSDecoderDeleter {
  void operator()(::OpusMSDecoder* d) const { opus_multistream_decoder_destroy(d); }
};

using DecoderPtr = std::unique_ptr<::OpusDecoder, OpusDecoderDeleter>;
using MSDecoderPtr = std::unique_ptr<::OpusMSDecoder, OpusMSDecoderDeleter>;

class OpusDecoder final : public Decoder {
  std::variant<std::monostate, DecoderPtr, MSDecoderPtr> decoder_;
  int channels_ = 0;
  int sample_rate_ = 0;
  AudioFormat output_format_;

  using DecodeFn = fn_ptr<int(void*, const uint8_t*, opus_int32, float*, int, int)>;
  using CtlFn = fn_ptr<int(void*, int, ...)>;
  DecodeFn decode_fn_ = nullptr;
  CtlFn ctl_fn_ = nullptr;
  void* raw_ptr_ = nullptr;

public:
  OpusDecoder() {
    output_format_.sample_format = OM_SAMPLE_F32;
    output_format_.bits_per_sample = 32;
  }

  static auto isHeadMagic(std::span<const uint8_t> payload) -> bool {
    if (payload.size() < 8) return false;
    return memcmp(payload.data(), "OpusHead", 8) == 0;
  }

  auto handleHead(std::span<const uint8_t> payload) -> OMError {
    if (payload.size() < 19) return OM_SUCCESS;
    channels_ = payload.data()[9];
    sample_rate_ = 48000;
    int mapping_family = payload.data()[18];

    decoder_ = std::monostate {};
    decode_fn_ = nullptr;
    ctl_fn_ = nullptr;
    raw_ptr_ = nullptr;

    int error = 0;
    if (mapping_family == 0) {
      auto ptr = opus_decoder_create(sample_rate_, channels_, &error);
      if (error == OPUS_OK) {
        auto& d = decoder_.emplace<DecoderPtr>(ptr);
        decode_fn_ = reinterpret_cast<DecodeFn>(opus_decode_float);
        ctl_fn_ = reinterpret_cast<CtlFn>(opus_decoder_ctl);
        raw_ptr_ = d.get();
      }
    } else {
      if (payload.size() < 21 + channels_) {
        return OM_CODEC_INVALID_PARAMS;
      }
      int streams = payload.data()[19];
      int coupled_streams = payload.data()[20];
      const uint8_t* mapping = &payload.data()[21];
      auto ptr = opus_multistream_decoder_create(sample_rate_, channels_, streams, coupled_streams, mapping, &error);
      if (error == OPUS_OK) {
        auto& d = decoder_.emplace<MSDecoderPtr>(ptr);
        decode_fn_ = reinterpret_cast<DecodeFn>(opus_multistream_decode_float);
        ctl_fn_ = reinterpret_cast<CtlFn>(opus_multistream_decoder_ctl);
        raw_ptr_ = d.get();
      }
    }

    if (error != OPUS_OK || !decode_fn_) {
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
    if (!raw_ptr_) return std::nullopt;

    output_format_.sample_rate = static_cast<uint32_t>(sample_rate_);
    output_format_.channels = static_cast<uint32_t>(channels_);

    DecodingInfo info = {};
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
      return Ok(std::vector<Frame> {});
    }

    if (packet.bytes.size() >= 8 && memcmp(packet.bytes.data(), "OpusTags", 8) == 0) {
      return Ok(std::vector<Frame> {});
    }

    if (!raw_ptr_) {
      return Ok(std::vector<Frame> {});
    }

    constexpr int MAX_SAMPLES = 5760;
    output_format_.sample_rate = static_cast<uint32_t>(sample_rate_);
    output_format_.channels = static_cast<uint32_t>(channels_);

    AudioSamples samples_fmt(output_format_, MAX_SAMPLES);
    int samples = decode_fn_(raw_ptr_, packet.bytes.data(),
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

    return Ok(std::vector<Frame> {});
  }

  void flush() override {
    if (ctl_fn_ && raw_ptr_) {
      ctl_fn_(raw_ptr_, OPUS_RESET_STATE);
    }
  }
};

struct OpusEncoderDeleter {
  void operator()(::OpusEncoder* e) const { opus_encoder_destroy(e); }
};

struct OpusMSEncoderDeleter {
  void operator()(::OpusMSEncoder* e) const { opus_multistream_encoder_destroy(e); }
};

using EncoderPtr = std::unique_ptr<::OpusEncoder, OpusEncoderDeleter>;
using MSEncoderPtr = std::unique_ptr<::OpusMSEncoder, OpusMSEncoderDeleter>;

class OpusEncoder final : public Encoder {
  std::variant<std::monostate, EncoderPtr, MSEncoderPtr> encoder_;
  int sample_rate_ = 48000;
  int channels_ = 2;
  int streams_ = 1;
  int frame_size_ = 960; // 20ms @ 48kHz
  int application_ = OPUS_APPLICATION_AUDIO;
  AudioFormat input_format_;
  std::vector<uint8_t> extradata_;

  using EncodeFn = fn_ptr<int(void*, const float*, int, uint8_t*, opus_int32)>;
  using CtlFn = fn_ptr<int(void*, int, ...)>;
  EncodeFn encode_fn_ = nullptr;
  CtlFn ctl_fn_ = nullptr;
  void* raw_ptr_ = nullptr;

public:
  OpusEncoder() {
    input_format_.sample_format = OM_SAMPLE_F32;
    input_format_.bits_per_sample = 32;
    input_format_.planar = false;
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

    encoder_ = std::monostate {};
    encode_fn_ = nullptr;
    ctl_fn_ = nullptr;
    raw_ptr_ = nullptr;

    const auto& extra = options.extra;

    int error = 0;
    int mapping_family = 0;
    int streams = 1;
    int coupled_streams = channels_ - 1;
    uint8_t mapping[255] = {0, 1};

    if (extra.contains(OPUS_ENC_MAPPING_FAMILY)) {
      mapping_family = extra.getInt32(OPUS_ENC_MAPPING_FAMILY);
    } else if (channels_ > 2) {
      mapping_family = 1;
    }

    if (mapping_family == 1) {
      if (channels_ <= 8) {
        static constexpr uint8_t OPUS_MAPPING_FAMILY1[8][3] = {
            {1, 0, 1},
            {1, 1, 2},
            {2, 1, 3},
            {2, 2, 4},
            {3, 2, 5},
            {4, 2, 6},
            {4, 3, 7},
            {5, 3, 8},
        };
        static constexpr uint8_t OPUS_MAPPINGS[8][8] = {
            {0},
            {0, 1},
            {0, 2, 1},
            {0, 1, 2, 3},
            {0, 4, 1, 2, 3},
            {0, 4, 1, 2, 3, 5},
            {0, 4, 1, 2, 3, 5, 6},
            {0, 6, 1, 2, 3, 4, 5, 7},
        };
        streams = OPUS_MAPPING_FAMILY1[channels_ - 1][0];
        coupled_streams = OPUS_MAPPING_FAMILY1[channels_ - 1][1];
        memcpy(mapping, OPUS_MAPPINGS[channels_ - 1], channels_);
      } else {
        mapping_family = 255;
      }
    }

    if (mapping_family == 255) {
      streams = channels_;
      coupled_streams = 0;
      for (int i = 0; i < channels_; ++i) {
        mapping[i] = i;
      }
    }

    streams_ = streams;

    if (mapping_family == 0) {
      auto ptr = opus_encoder_create(sample_rate_, channels_, application_, &error);
      if (error == OPUS_OK) {
        auto& e = encoder_.emplace<EncoderPtr>(ptr);
        encode_fn_ = reinterpret_cast<EncodeFn>(opus_encode_float);
        ctl_fn_ = reinterpret_cast<CtlFn>(opus_encoder_ctl);
        raw_ptr_ = e.get();
      }
    } else {
      auto ptr = opus_multistream_encoder_create(sample_rate_, channels_, streams, coupled_streams, mapping, application_, &error);
      if (error == OPUS_OK) {
        auto& e = encoder_.emplace<MSEncoderPtr>(ptr);
        encode_fn_ = reinterpret_cast<EncodeFn>(opus_multistream_encode_float);
        ctl_fn_ = reinterpret_cast<CtlFn>(opus_multistream_encoder_ctl);
        raw_ptr_ = e.get();
      }
    }

    if (error != OPUS_OK || !encode_fn_) {
      return OM_CODEC_OPEN_FAILED;
    }

    auto set_ctl = [&](int op, int32_t val) {
      ctl_fn_(raw_ptr_, op, val);
    };

    if (extra.contains(OPUS_ENC_APPLICATION)) {
      set_ctl(OPUS_SET_APPLICATION_REQUEST, extra.getInt32(OPUS_ENC_APPLICATION));
    }
    if (extra.contains(OPUS_ENC_BITRATE)) {
      set_ctl(OPUS_SET_BITRATE_REQUEST, extra.getInt32(OPUS_ENC_BITRATE));
    }
    if (extra.contains(OPUS_ENC_VBR)) {
      set_ctl(OPUS_SET_VBR_REQUEST, extra.getInt32(OPUS_ENC_VBR) != 0);
    }
    if (extra.contains(OPUS_ENC_COMPLEXITY)) {
      set_ctl(OPUS_SET_COMPLEXITY_REQUEST, extra.getInt32(OPUS_ENC_COMPLEXITY));
    }
    if (extra.contains(OPUS_ENC_FRAME_SIZE)) {
      frame_size_ = extra.getInt32(OPUS_ENC_FRAME_SIZE);
    }
    if (extra.contains(OPUS_ENC_FORCE_CHANNELS)) {
      set_ctl(OPUS_SET_FORCE_CHANNELS_REQUEST, extra.getInt32(OPUS_ENC_FORCE_CHANNELS));
    }
    if (extra.contains(OPUS_ENC_SIGNAL_TYPE)) {
      set_ctl(OPUS_SET_SIGNAL_REQUEST, extra.getInt32(OPUS_ENC_SIGNAL_TYPE));
    }
    if (extra.contains(OPUS_ENC_BANDWIDTH)) {
      set_ctl(OPUS_SET_BANDWIDTH_REQUEST, extra.getInt32(OPUS_ENC_BANDWIDTH));
    }
    if (extra.contains(OPUS_ENC_PACKET_LOSS_PERC)) {
      set_ctl(OPUS_SET_PACKET_LOSS_PERC_REQUEST, extra.getInt32(OPUS_ENC_PACKET_LOSS_PERC));
    }
    if (extra.contains(OPUS_ENC_FEC)) {
      set_ctl(OPUS_SET_INBAND_FEC_REQUEST, extra.getInt32(OPUS_ENC_FEC));
    }
    if (extra.contains(OPUS_ENC_DTX)) {
      set_ctl(OPUS_SET_DTX_REQUEST, extra.getInt32(OPUS_ENC_DTX) != 0);
    }
    if (extra.contains(OPUS_ENC_LSB_DEPTH)) {
      set_ctl(OPUS_SET_LSB_DEPTH_REQUEST, extra.getInt32(OPUS_ENC_LSB_DEPTH));
    }

    int lookahead = 0;
    ctl_fn_(raw_ptr_, OPUS_GET_LOOKAHEAD(&lookahead));

    size_t head_size = 19 + (mapping_family > 0 ? 2 + channels_ : 0);
    extradata_.resize(head_size);
    memcpy(extradata_.data(), "OpusHead", 8);
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
    extradata_[18] = static_cast<uint8_t>(mapping_family);
    if (mapping_family > 0) {
      extradata_[19] = static_cast<uint8_t>(streams);
      extradata_[20] = static_cast<uint8_t>(coupled_streams);
      memcpy(&extradata_[21], mapping, channels_);
    }

    return OM_SUCCESS;
  }

  auto getInfo() -> EncodingInfo override {
    EncodingInfo info;
    info.extradata = extradata_;
    return info;
  }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    if (!raw_ptr_) {
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
    int samples_per_channel = static_cast<int>(audio_data->nb_samples);
    std::vector<uint8_t> packet_buffer(4000 * streams_);

    int offset = 0;
    while (offset < samples_per_channel) {
      int remaining = samples_per_channel - offset;
      int to_encode = std::min(remaining, frame_size_);
      int encoded_bytes = encode_fn_(raw_ptr_, input + offset * channels_, to_encode,
                                     packet_buffer.data(),
                                     static_cast<opus_int32>(packet_buffer.size()));

      if (encoded_bytes > 0) {
        Packet packet;
        packet.allocate(static_cast<size_t>(encoded_bytes));
        memcpy(packet.bytes.data(), packet_buffer.data(), static_cast<size_t>(encoded_bytes));
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
    if (!raw_ptr_) {
      return OM_CODEC_INVALID_PARAMS;
    }

    int32_t bitrate = 0;
    if (auto* vbr = std::get_if<VbrParams>(&rc.params)) {
      bitrate = static_cast<opus_int32>(vbr->bitrate.target_bitrate);
    } else if (auto* cbr = std::get_if<CbrParams>(&rc.params)) {
      bitrate = static_cast<opus_int32>(cbr->bitrate.target_bitrate);
    } else if (auto* abr = std::get_if<AbrParams>(&rc.params)) {
      bitrate = static_cast<opus_int32>(abr->target_bitrate);
    }

    if (bitrate > 0) {
      ctl_fn_(raw_ptr_, OPUS_SET_BITRATE(bitrate));
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
    .caps = CodecCaps {
        .audio = AudioCodecCaps {
            .fmt_f32 = true,
            .sample_rates = {8000, 12000, 16000, 24000, 48000},
        },
    },
    .options = {
        OPUS_ENC_APPLICATION,
        OPUS_ENC_BITRATE,
        OPUS_ENC_VBR,
        OPUS_ENC_COMPLEXITY,
        OPUS_ENC_FRAME_SIZE,
        OPUS_ENC_FORCE_CHANNELS,
        OPUS_ENC_SIGNAL_TYPE,
        OPUS_ENC_BANDWIDTH,
        OPUS_ENC_PACKET_LOSS_PERC,
        OPUS_ENC_FEC,
        OPUS_ENC_DTX,
        OPUS_ENC_LSB_DEPTH,
        OPUS_ENC_LOOKAHEAD,
        OPUS_ENC_MAPPING_FAMILY,
    },
    .decoder_factory = [] { return std::make_unique<OpusDecoder>(); },
    .encoder_factory = [] { return std::make_unique<OpusEncoder>(); },
};

} // namespace openmedia
