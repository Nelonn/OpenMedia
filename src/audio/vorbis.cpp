#include <codecs.hpp>
#include <openmedia/audio.hpp>
#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>
#include <ogg/ogg.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace openmedia {

static void appendXiphLacing(std::vector<uint8_t>& extradata, long packet_size) {
  while (packet_size >= 255) {
    extradata.push_back(255);
    packet_size -= 255;
  }
  extradata.push_back(static_cast<uint8_t>(packet_size));
}

static auto buildVorbisExtradata(const ogg_packet& header,
                          const ogg_packet& comments,
                          const ogg_packet& setup) -> std::vector<uint8_t> {
  std::vector<uint8_t> extradata;
  extradata.reserve(3 + header.bytes + comments.bytes + setup.bytes);
  extradata.push_back(2);
  appendXiphLacing(extradata, header.bytes);
  appendXiphLacing(extradata, comments.bytes);
  extradata.insert(extradata.end(), header.packet, header.packet + header.bytes);
  extradata.insert(extradata.end(), comments.packet, comments.packet + comments.bytes);
  extradata.insert(extradata.end(), setup.packet, setup.packet + setup.bytes);
  return extradata;
}

class VorbisEncoder final : public Encoder {
  vorbis_info vi_ = {};
  vorbis_comment vc_ = {};
  vorbis_dsp_state vd_ = {};
  vorbis_block vb_ = {};
  AudioFormat input_format_ = {};
  std::vector<uint8_t> extradata_;
  bool initialized_ = false;

public:
  VorbisEncoder() {
    vorbis_info_init(&vi_);
    vorbis_comment_init(&vc_);
  }

  ~VorbisEncoder() override {
    if (initialized_) {
      vorbis_block_clear(&vb_);
      vorbis_dsp_clear(&vd_);
    }
    vorbis_comment_clear(&vc_);
    vorbis_info_clear(&vi_);
  }

  auto configure(const EncoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_VORBIS || options.format.type != OM_MEDIA_AUDIO) {
      return OM_CODEC_INVALID_PARAMS;
    }
    if (options.audio_format.sample_rate == 0 || options.audio_format.channels == 0) {
      return OM_CODEC_INVALID_PARAMS;
    }
    if (options.audio_format.sample_format != OM_SAMPLE_F32) {
      return OM_CODEC_INVALID_PARAMS;
    }

    if (initialized_) {
      vorbis_block_clear(&vb_);
      vorbis_dsp_clear(&vd_);
      initialized_ = false;
    }
    vorbis_comment_clear(&vc_);
    vorbis_info_clear(&vi_);
    vorbis_info_init(&vi_);
    vorbis_comment_init(&vc_);

    float quality = 0.4f;
    if (auto* crf = std::get_if<CrfParams>(&options.rate_control.params)) {
      quality = std::clamp(crf->quality, -0.1f, 1.0f);
    }

    if (vorbis_encode_init_vbr(&vi_,
                               static_cast<long>(options.audio_format.channels),
                               static_cast<long>(options.audio_format.sample_rate),
                               quality) != 0) {
      return OM_CODEC_OPEN_FAILED;
    }
    if (vorbis_analysis_init(&vd_, &vi_) != 0 || vorbis_block_init(&vd_, &vb_) != 0) {
      vorbis_info_clear(&vi_);
      vorbis_info_init(&vi_);
      return OM_CODEC_OPEN_FAILED;
    }

    ogg_packet header = {};
    ogg_packet comments = {};
    ogg_packet setup = {};
    if (vorbis_analysis_headerout(&vd_, &vc_, &header, &comments, &setup) != 0) {
      vorbis_block_clear(&vb_);
      vorbis_dsp_clear(&vd_);
      vorbis_info_clear(&vi_);
      vorbis_info_init(&vi_);
      return OM_CODEC_OPEN_FAILED;
    }

    extradata_ = buildVorbisExtradata(header, comments, setup);
    input_format_ = options.audio_format;
    input_format_.planar = false;
    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> EncodingInfo override {
    EncodingInfo info = {};
    info.extradata = extradata_;
    return info;
  }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    if (!initialized_) {
      return Err(OM_COMMON_NOT_INITIALIZED);
    }

    const auto* samples = std::get_if<AudioSamples>(&frame.data);
    if (!samples) {
      return Err(OM_CODEC_INVALID_PARAMS);
    }
    if (samples->format.sample_format != OM_SAMPLE_F32 ||
        samples->format.channels != input_format_.channels) {
      return Err(OM_CODEC_INVALID_PARAMS);
    }

    float** analysis = vorbis_analysis_buffer(&vd_, static_cast<int>(samples->nb_samples));
    if (!analysis) {
      return Err(OM_CODEC_ENCODE_FAILED);
    }

    if (samples->format.planar) {
      for (uint32_t channel = 0; channel < samples->format.channels; ++channel) {
        const auto* src = reinterpret_cast<const float*>(samples->planes.data[channel]);
        std::memcpy(analysis[channel], src, static_cast<size_t>(samples->nb_samples) * sizeof(float));
      }
    } else {
      const auto* src = reinterpret_cast<const float*>(samples->buffer->bytes().data());
      for (uint32_t sample = 0; sample < samples->nb_samples; ++sample) {
        for (uint32_t channel = 0; channel < samples->format.channels; ++channel) {
          analysis[channel][sample] = src[sample * samples->format.channels + channel];
        }
      }
    }

    vorbis_analysis_wrote(&vd_, static_cast<int>(samples->nb_samples));
    return collectPackets(frame);
  }

  auto updateBitrate(const RateControlParams&) -> OMError override {
    return OM_SUCCESS;
  }

private:
  auto collectPackets(const Frame& frame) -> Result<std::vector<Packet>, OMError> {
    std::vector<Packet> packets;
    while (vorbis_analysis_blockout(&vd_, &vb_) == 1) {
      vorbis_analysis(&vb_, nullptr);
      vorbis_bitrate_addblock(&vb_);

      ogg_packet packet = {};
      while (vorbis_bitrate_flushpacket(&vd_, &packet) == 1) {
        Packet out = {};
        out.allocate(static_cast<size_t>(packet.bytes));
        std::memcpy(out.bytes.data(), packet.packet, static_cast<size_t>(packet.bytes));
        out.pts = frame.pts;
        out.dts = frame.dts;
        packets.push_back(std::move(out));
      }
    }
    return Ok(std::move(packets));
  }
};

class VorbisDecoder final : public Decoder {
  vorbis_info vi_ = {};
  vorbis_comment vc_ = {};
  vorbis_dsp_state vd_ = {};
  vorbis_block vb_ = {};
  int header_packets_ = 0;
  long packet_count_ = 0;
  bool initialized_ = false;
  AudioFormat output_format_;

public:
  VorbisDecoder() {
    vorbis_info_init(&vi_);
    vorbis_comment_init(&vc_);
  }

  ~VorbisDecoder() override {
    if (initialized_) {
      vorbis_block_clear(&vb_);
      vorbis_dsp_clear(&vd_);
    }
    vorbis_comment_clear(&vc_);
    vorbis_info_clear(&vi_);
  }

  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_VORBIS) {
      return OM_CODEC_INVALID_PARAMS;
    }
    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!initialized_) return std::nullopt;

    output_format_.sample_rate = static_cast<uint32_t>(vi_.rate);
    output_format_.channels = static_cast<uint32_t>(vi_.channels);
    output_format_.sample_format = OM_SAMPLE_F32;

    DecodingInfo info;
    info.media_type = OM_MEDIA_AUDIO;
    info.audio_format = output_format_;
    return info;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    ogg_packet op;
    op.packet = const_cast<unsigned char*>(packet.bytes.data());
    op.bytes = static_cast<long>(packet.bytes.size());
    op.b_o_s = (header_packets_ == 0);
    op.e_o_s = 0;
    op.granulepos = packet.pts;
    op.packetno = packet_count_++;

    if (header_packets_ < 3) {
      int ret = vorbis_synthesis_headerin(&vi_, &vc_, &op);
      if (ret == 0) {
        header_packets_++;
        if (header_packets_ == 3) {
          vorbis_synthesis_init(&vd_, &vi_);
          vorbis_block_init(&vd_, &vb_);
          initialized_ = true;
        }
      }
      return Ok(std::vector<Frame>{});
    }

    if (!initialized_) {
      return Ok(std::vector<Frame>{});
    }

    std::vector<Frame> frames;
    if (vorbis_synthesis(&vb_, &op) == 0) {
      vorbis_synthesis_blockin(&vd_, &vb_);

      float** pcm;
      int samples = vorbis_synthesis_pcmout(&vd_, &pcm);
      if (samples > 0) {
        output_format_.sample_rate = static_cast<uint32_t>(vi_.rate);
        output_format_.channels = static_cast<uint32_t>(vi_.channels);
        output_format_.sample_format = OM_SAMPLE_F32;

        AudioSamples samples_fmt(output_format_, static_cast<uint32_t>(samples));
        float* dst = reinterpret_cast<float*>(samples_fmt.planes.data[0]);
        for (int i = 0; i < samples; ++i) {
          for (int c = 0; c < vi_.channels; ++c) {
            *dst++ = pcm[c][i];
          }
        }

        Frame frame;
        frame.pts = packet.pts;
        frame.dts = packet.dts;
        frame.data = std::move(samples_fmt);
        frames.push_back(std::move(frame));

        vorbis_synthesis_read(&vd_, samples);
      }
    }

    return Ok(std::move(frames));
  }

  void flush() override {}
};

const CodecDescriptor CODEC_VORBIS = {
  .codec_id = OM_CODEC_VORBIS,
  .type = OM_MEDIA_AUDIO,
  .name = "vorbis",
  .long_name = "Vorbis Audio Codec",
  .vendor = "Xiph.Org",
  .flags = NONE,
  .caps = CodecCaps {
    .audio = AudioCodecCaps {
      .fmt_f32 = true,
      .sample_rates = {8000, 11025, 16000, 22050, 32000, 44100, 48000},
    },
  },
  .decoder_factory = [] { return std::make_unique<VorbisDecoder>(); },
  .encoder_factory = [] { return std::make_unique<VorbisEncoder>(); },
};

} // namespace openmedia
