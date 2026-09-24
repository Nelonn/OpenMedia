#include <codecs.hpp>
#include <openmedia/audio.hpp>
#include <util/xiph.hpp>
#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>
#include <ogg/ogg.h>
#include <algorithm>
#include <cstring>
#include <span>
#include <vector>

namespace openmedia {

namespace {

auto makeOggPacket(std::span<const uint8_t> bytes, int64_t granulepos, int64_t packetno, bool bos)
    -> ogg_packet {
  ogg_packet op = {};
  op.packet = const_cast<unsigned char*>(bytes.data());
  op.bytes = static_cast<long>(bytes.size());
  op.b_o_s = bos ? 1 : 0;
  op.granulepos = granulepos;
  op.packetno = packetno;
  return op;
}

// Vorbis flags its three header packets with the low bit of the first byte.
auto isHeaderPacket(std::span<const uint8_t> bytes) -> bool {
  return !bytes.empty() && (bytes[0] & 1) != 0;
}

void interleave(float* dst, float* const* planes, uint32_t channels, uint32_t nb_samples) {
  if (channels == 1) {
    std::memcpy(dst, planes[0], static_cast<size_t>(nb_samples) * sizeof(float));
    return;
  }
  if (channels == 2) {
    const float* left = planes[0];
    const float* right = planes[1];
    for (uint32_t i = 0; i < nb_samples; ++i) {
      dst[2 * i] = left[i];
      dst[2 * i + 1] = right[i];
    }
    return;
  }
  for (uint32_t channel = 0; channel < channels; ++channel) {
    const float* src = planes[channel];
    float* out = dst + channel;
    for (uint32_t i = 0; i < nb_samples; ++i, out += channels) *out = src[i];
  }
}

void deinterleave(float* const* planes, const float* src, uint32_t channels, uint32_t nb_samples) {
  if (channels == 1) {
    std::memcpy(planes[0], src, static_cast<size_t>(nb_samples) * sizeof(float));
    return;
  }
  for (uint32_t channel = 0; channel < channels; ++channel) {
    float* dst = planes[channel];
    const float* in = src + channel;
    for (uint32_t i = 0; i < nb_samples; ++i, in += channels) {
      dst[i] = *in;
    }
  }
}

auto buildVorbisExtradata(const ogg_packet& header,
                          const ogg_packet& comments,
                          const ogg_packet& setup) -> std::vector<uint8_t> {
  std::vector<uint8_t> extradata;
  extradata.reserve(3 + header.bytes + comments.bytes + setup.bytes);
  extradata.push_back(2);
  appendXiphLacing(extradata, static_cast<size_t>(header.bytes));
  appendXiphLacing(extradata, static_cast<size_t>(comments.bytes));
  extradata.insert(extradata.end(), header.packet, header.packet + header.bytes);
  extradata.insert(extradata.end(), comments.packet, comments.packet + comments.bytes);
  extradata.insert(extradata.end(), setup.packet, setup.packet + setup.bytes);
  return extradata;
}

} // namespace

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
      deinterleave(analysis,
                   reinterpret_cast<const float*>(samples->buffer->bytes().data()),
                   samples->format.channels,
                   samples->nb_samples);
    }

    vorbis_analysis_wrote(&vd_, static_cast<int>(samples->nb_samples));
    return collectPackets(frame, samples->nb_samples);
  }

  auto updateBitrate(const RateControlParams&) -> OMError override {
    return OM_SUCCESS;
  }

private:
  auto collectPackets(const Frame& frame, uint32_t nb_samples) -> Result<std::vector<Packet>, OMError> {
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
        out.duration = nb_samples;
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
  AudioFormat output_format_ = {};
  Rational time_base_ = {};
  int64_t packet_count_ = 0;
  int64_t anchor_pts_ = 0;
  int64_t anchor_samples_ = 0;
  int64_t anchored_at_ = -1;
  int header_packets_ = 0;
  bool initialized_ = false;
  // Ogg timestamps are granule positions: they count samples and mark the end
  // of a packet. Every other container gives a start time in its own base.
  bool pts_in_samples_ = false;

public:
  VorbisDecoder() {
    vorbis_info_init(&vi_);
    vorbis_comment_init(&vc_);
  }

  ~VorbisDecoder() override {
    clearDsp();
    vorbis_comment_clear(&vc_);
    vorbis_info_clear(&vi_);
  }

  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_VORBIS) {
      return OM_CODEC_INVALID_PARAMS;
    }

    clearDsp();
    vorbis_comment_clear(&vc_);
    vorbis_info_clear(&vi_);
    vorbis_info_init(&vi_);
    vorbis_comment_init(&vc_);
    header_packets_ = 0;
    packet_count_ = 0;
    resetTimeline();
    time_base_ = options.time_base;

    // Matroska and the MP4-style containers keep the three headers in
    // extradata; Ogg has none and delivers them as the first packets instead.
    if (options.extradata.empty()) return OM_SUCCESS;

    const auto headers = splitVorbisExtradata(options.extradata);
    if (!headers) return OM_CODEC_INVALID_PARAMS;
    for (const auto& header : *headers) {
      if (const OMError err = consumeHeader(header); err != OM_SUCCESS) return err;
    }
    return initialized_ ? OM_SUCCESS : OM_CODEC_INVALID_PARAMS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!initialized_) return std::nullopt;

    DecodingInfo info = {};
    info.media_type = OM_MEDIA_AUDIO;
    info.audio_format = output_format_;
    return info;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    if (packet.bytes.empty()) return Ok(std::vector<Frame> {});

    if (isHeaderPacket(packet.bytes)) {
      if (initialized_) return Ok(std::vector<Frame> {});
      if (const OMError err = consumeHeader(packet.bytes); err != OM_SUCCESS) return Err(err);
      return Ok(std::vector<Frame> {});
    }
    if (!initialized_) return Ok(std::vector<Frame> {});

    ogg_packet op = makeOggPacket(packet.bytes,
                                  pts_in_samples_ ? packet.pts : -1,
                                  packet_count_++,
                                  false);
    if (vorbis_synthesis(&vb_, &op) != 0) return Ok(std::vector<Frame> {});
    if (vorbis_synthesis_blockin(&vd_, &vb_) != 0) return Ok(std::vector<Frame> {});

    anchorTimeline(packet.pts);

    std::vector<Frame> frames;
    float** pcm = nullptr;
    for (int samples = vorbis_synthesis_pcmout(&vd_, &pcm); samples > 0;
         samples = vorbis_synthesis_pcmout(&vd_, &pcm)) {
      AudioSamples out(output_format_, static_cast<uint32_t>(samples));
      interleave(reinterpret_cast<float*>(out.planes.data[0]),
                 pcm,
                 output_format_.channels,
                 static_cast<uint32_t>(samples));

      Frame frame;
      frame.pts = takePts(samples);
      frame.dts = frame.pts;
      frame.data = std::move(out);
      frames.push_back(std::move(frame));

      vorbis_synthesis_read(&vd_, samples);
    }
    return Ok(std::move(frames));
  }

  void flush() override {
    if (initialized_) vorbis_synthesis_restart(&vd_);
    resetTimeline();
    resetReceiveState();
  }

private:
  void clearDsp() {
    if (!initialized_) return;
    vorbis_block_clear(&vb_);
    vorbis_dsp_clear(&vd_);
    initialized_ = false;
  }

  auto consumeHeader(std::span<const uint8_t> bytes) -> OMError {
    if (header_packets_ >= 3) return OM_SUCCESS;

    ogg_packet op = makeOggPacket(bytes, -1, packet_count_++, header_packets_ == 0);
    if (vorbis_synthesis_headerin(&vi_, &vc_, &op) != 0) return OM_CODEC_INVALID_PARAMS;
    if (++header_packets_ < 3) return OM_SUCCESS;

    if (vorbis_synthesis_init(&vd_, &vi_) != 0) return OM_CODEC_OPEN_FAILED;
    if (vorbis_block_init(&vd_, &vb_) != 0) {
      vorbis_dsp_clear(&vd_);
      return OM_CODEC_OPEN_FAILED;
    }
    initialized_ = true;

    output_format_.sample_format = OM_SAMPLE_F32;
    output_format_.bits_per_sample = 32;
    output_format_.sample_rate = static_cast<uint32_t>(vi_.rate);
    output_format_.channels = static_cast<uint32_t>(vi_.channels);
    output_format_.planar = false;
    pts_in_samples_ = time_base_.num == 1 && time_base_.den == vi_.rate;
    return OM_SUCCESS;
  }

  void resetTimeline() {
    anchor_pts_ = 0;
    anchor_samples_ = 0;
    anchored_at_ = -1;
  }

  // Frames are timed from the last packet that carried a timestamp plus the
  // samples emitted since. A Matroska block splits into several frames that all
  // repeat the block's timestamp, so re-anchoring on a repeat would stall time.
  void anchorTimeline(int64_t packet_pts) {
    if (packet_pts < 0 || packet_pts == anchored_at_) return;

    anchored_at_ = packet_pts;
    anchor_samples_ = 0;
    if (!pts_in_samples_) {
      anchor_pts_ = packet_pts;
      return;
    }
    // A granule position marks the last sample the packet completed, so the
    // samples still pending have to be counted back off it.
    anchor_pts_ = std::max<int64_t>(packet_pts - vorbis_synthesis_pcmout(&vd_, nullptr), 0);
  }

  auto takePts(int64_t samples) -> int64_t {
    const int64_t pts = anchor_pts_ + samplesToTimeBase(anchor_samples_);
    anchor_samples_ += samples;
    return pts;
  }

  auto samplesToTimeBase(int64_t samples) const -> int64_t {
    if (pts_in_samples_ || time_base_.num <= 0 || time_base_.den <= 0 || vi_.rate <= 0) {
      return samples;
    }
    return samples * time_base_.den / (static_cast<int64_t>(vi_.rate) * time_base_.num);
  }
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
