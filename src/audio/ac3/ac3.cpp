#include <algorithm>
#include <array>
#include <audio/ac3/ac3_decoder.hpp>
#include <bit>
#include <codecs.hpp>
#include <memory>
#include <openmedia/audio.hpp>
#include <openmedia/codec_extra.hpp>
#include <vector>

namespace openmedia {

namespace {

constexpr int MAX_OUTPUT_CHANNELS = 8;
constexpr int MAX_DEPENDENT_SUBSTREAMS = 8;

auto readDrcScale(const Dictionary& extra) -> float {
  const Value* value = extra.get(AC3_DEC_DRC_SCALE);
  if (!value) return 1.0f;
  if (auto v = value->getFloat()) return *v;
  if (auto v = value->getDouble()) return static_cast<float>(*v);
  if (auto v = value->getInt32()) return static_cast<float>(*v);
  return 1.0f;
}

struct Substream {
  std::span<const uint8_t> frame;
  ac3::FrameHeader header;
  int slots[ac3::MAX_FBW_CHANNELS + 1] = {}; // output position of every coded channel, -1 if dropped
};

// One independent substream with the dependent substreams that add or
// replace its channels, i.e. one syncframe worth of output audio.
struct Program {
  Substream independent;
  std::vector<Substream> dependents;
  uint64_t layout = 0; // ac3::ChannelMask bits, output order is ascending bit order
  int channels = 0;

  auto samples() const -> uint32_t { return independent.header.num_blocks * ac3::BLOCK_SIZE; }
  auto sampleRate() const -> uint32_t { return independent.header.sample_rate; }
};

// Chooses the output channels of a program from the channel locations of its
// substreams and assigns every coded channel its output slot.
void mapChannels(Program& program) {
  const size_t count = program.dependents.size() + 1;
  const auto substream = [&](size_t i) -> Substream& { return i == 0 ? program.independent : program.dependents[i - 1]; };

  uint64_t locations[MAX_DEPENDENT_SUBSTREAMS + 1][ac3::MAX_FBW_CHANNELS + 1] = {};
  uint64_t layout = 0;
  for (size_t i = 0; i < count; ++i) {
    const ac3::FrameHeader& header = substream(i).header;
    ac3::channelLocations(header, locations[i]);
    for (int ch = 0; ch < header.channels(); ++ch) layout |= locations[i][ch];
  }
  // Keep the channels that come first in output order when there are too many.
  while (std::popcount(layout) > MAX_OUTPUT_CHANNELS) {
    layout &= ~(uint64_t {1} << (63 - std::countl_zero(layout)));
  }
  program.layout = layout;
  program.channels = std::popcount(layout);

  for (size_t i = 0; i < count; ++i) {
    Substream& s = substream(i);
    for (int ch = 0; ch < s.header.channels(); ++ch) {
      const uint64_t location = locations[i][ch];
      s.slots[ch] = (layout & location) ? std::popcount(layout & (location - 1)) : -1;
    }
  }
}

}

class AC3Decoder final : public Decoder {
  ac3::SubstreamDecoder independent_;
  std::array<std::unique_ptr<ac3::SubstreamDecoder>, MAX_DEPENDENT_SUBSTREAMS> dependents_;
  float drc_scale_ = 1.0f;
  Rational time_base_ = {};
  AudioFormat output_format_ = {};
  std::vector<Program> programs_;

public:
  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_AC3 && options.format.codec_id != OM_CODEC_EAC3) {
      return OM_CODEC_INVALID_PARAMS;
    }
    time_base_ = options.time_base;
    drc_scale_ = readDrcScale(options.extra);
    independent_.setDrcScale(drc_scale_);

    // Advertise the container's format until the first frame is decoded.
    output_format_.sample_format = OM_SAMPLE_F32;
    output_format_.sample_rate = options.format.audio.sample_rate;
    output_format_.channels = options.format.audio.channels;
    output_format_.planar = false;
    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    DecodingInfo info;
    info.media_type = OM_MEDIA_AUDIO;
    info.audio_format = output_format_;
    return info;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    splitPrograms(packet.bytes);
    if (programs_.empty()) return Err(OM_CODEC_DECODE_FAILED);

    // Consecutive programs of one format share a frame.
    std::vector<Frame> frames;
    uint32_t offset = 0; // samples emitted from this packet so far
    for (size_t first = 0; first < programs_.size();) {
      size_t last = first + 1;
      uint32_t samples = programs_[first].samples();
      while (last < programs_.size() && programs_[last].layout == programs_[first].layout &&
             programs_[last].sampleRate() == programs_[first].sampleRate()) {
        samples += programs_[last++].samples();
      }

      output_format_.sample_format = OM_SAMPLE_F32;
      output_format_.sample_rate = programs_[first].sampleRate();
      output_format_.channels = static_cast<uint32_t>(programs_[first].channels);
      output_format_.planar = false;

      AudioSamples audio(output_format_, samples);
      auto* out = reinterpret_cast<float*>(audio.planes.data[0]);
      for (size_t i = first; i < last; ++i) {
        decodeProgram(programs_[i], out);
        out += static_cast<size_t>(programs_[i].samples()) * programs_[i].channels;
      }

      Frame frame;
      frame.pts = packet.pts + samplesToTimeBase(offset, output_format_.sample_rate);
      frame.dts = packet.dts + samplesToTimeBase(offset, output_format_.sample_rate);
      frame.data = std::move(audio);
      frames.push_back(std::move(frame));
      offset += samples;
      first = last;
    }
    return Ok(std::move(frames));
  }

  void flush() override {
    independent_.reset();
    for (auto& dependent : dependents_) {
      if (dependent) dependent->reset();
    }
  }

private:
  // Splits a packet into programs. Only the first E-AC-3 program
  // (independent substream 0) is kept.
  void splitPrograms(std::span<const uint8_t> data) {
    programs_.clear();
    bool skipping = false;
    size_t pos = 0;
    while (pos + 8 <= data.size()) {
      const auto header = ac3::parseFrameHeader(data.subspan(pos));
      if (!header) {
        ++pos; // resynchronize
        continue;
      }
      if (pos + header->frame_size > data.size()) break;
      const Substream substream {data.subspan(pos, header->frame_size), *header};
      pos += header->frame_size;

      if (header->type != ac3::FrameType::DEPENDENT) {
        skipping = header->eac3 && header->substream_id != 0;
        if (!skipping) programs_.push_back({substream});
      } else if (!skipping && !programs_.empty()) {
        // A dependent substream must match the block layout of its program.
        Program& program = programs_.back();
        const ac3::FrameHeader& main = program.independent.header;
        if (header->num_blocks == main.num_blocks && header->sample_rate == main.sample_rate &&
            program.dependents.size() < MAX_DEPENDENT_SUBSTREAMS) {
          program.dependents.push_back(substream);
        }
      }
    }
    for (Program& program : programs_) mapChannels(program);
  }

  // Decodes a program into `out`, which holds program.samples() interleaved
  // sample frames. Undecodable substreams leave silence behind.
  void decodeProgram(const Program& program, float* out) {
    const size_t count = static_cast<size_t>(program.samples()) * program.channels;
    const ac3::PcmTarget main_target {out, program.channels, program.independent.slots};
    if (!ac3::checkFrameCrc(program.independent.frame) || !independent_.decode(program.independent.frame, main_target)) {
      independent_.reset();
      std::fill_n(out, count, 0.0f);
      return;
    }

    for (const Substream& dependent : program.dependents) {
      auto& decoder = dependents_[dependent.header.substream_id & (MAX_DEPENDENT_SUBSTREAMS - 1)];
      if (!decoder) {
        decoder = std::make_unique<ac3::SubstreamDecoder>();
        decoder->setDrcScale(drc_scale_);
      }
      const ac3::PcmTarget target {out, program.channels, dependent.slots};
      if (!ac3::checkFrameCrc(dependent.frame) || !decoder->decode(dependent.frame, target)) {
        decoder->reset();
        for (int ch = 0; ch < dependent.header.channels(); ++ch) {
          if (dependent.slots[ch] < 0) continue;
          for (size_t i = dependent.slots[ch]; i < count; i += program.channels) out[i] = 0.0f;
        }
      }
    }
  }

  auto samplesToTimeBase(int64_t samples, uint32_t sample_rate) const -> int64_t {
    if (samples == 0 || sample_rate == 0 || time_base_.num <= 0 || time_base_.den <= 0) return 0;
    return samples * time_base_.den / (static_cast<int64_t>(sample_rate) * time_base_.num);
  }
};

const CodecDescriptor CODEC_AC3 = {
  .codec_id = OM_CODEC_AC3,
  .type = OM_MEDIA_AUDIO,
  .name = "ac3",
  .long_name = "ATSC A/52 AC-3 (Dolby Digital)",
  .vendor = "OpenMedia",
  .flags = NONE,
  .decoder_factory = [] { return std::make_unique<AC3Decoder>(); },
};

const CodecDescriptor CODEC_EAC3 = {
  .codec_id = OM_CODEC_EAC3,
  .type = OM_MEDIA_AUDIO,
  .name = "eac3",
  .long_name = "ATSC A/52 Annex E E-AC-3 (Dolby Digital Plus)",
  .vendor = "OpenMedia",
  .flags = NONE,
  .decoder_factory = [] { return std::make_unique<AC3Decoder>(); },
};

} // namespace openmedia
