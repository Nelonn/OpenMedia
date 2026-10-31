#include <codecs.hpp>
#include <openmedia/audio.hpp>
#include <minimp3.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace openmedia {

class MP3Decoder final : public Decoder {
  mp3dec_t decoder_ = {};
  AudioFormat output_format_;

public:
  MP3Decoder() {
    mp3dec_init(&decoder_);
  }

  ~MP3Decoder() override = default;

  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_MP3) {
      return OM_CODEC_INVALID_PARAMS;
    }
    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    DecodingInfo info;
    info.media_type = OM_MEDIA_AUDIO;
    info.audio_format = output_format_;
    return info;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    std::vector<Frame> frames;

    mp3dec_frame_info_t info;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

    const uint8_t* data = packet.bytes.data();
    size_t size = packet.bytes.size();

    while (size > 0) {
      int samples = mp3dec_decode_frame(&decoder_, data, static_cast<int>(size), pcm, &info);
      if (info.frame_bytes > 0) {
        output_format_.sample_format = OM_SAMPLE_S16;
        output_format_.sample_rate = static_cast<uint32_t>(info.hz);
        output_format_.channels = static_cast<uint32_t>(info.channels);

        AudioSamples samples_fmt(output_format_, static_cast<uint32_t>(samples));
        std::memcpy(samples_fmt.planes.data[0], pcm, static_cast<size_t>(samples) * info.channels * 2);

        Frame frame;
        frame.pts = packet.pts;
        frame.dts = packet.dts;
        frame.data = std::move(samples_fmt);
        frames.push_back(std::move(frame));

        data += info.frame_bytes;
        size -= info.frame_bytes;
      } else {
        break;
      }
    }

    return Ok(std::move(frames));
  }

  void flush() override {
    mp3dec_init(&decoder_);
  }
};

const CodecDescriptor CODEC_MP3 = {
    .codec_id = OM_CODEC_MP3,
    .type = OM_MEDIA_AUDIO,
    .name = "mp3",
    .long_name = "MP3",
    .vendor = "minimp3",
    .flags = NONE,
    .decoder_factory = [] { return std::make_unique<MP3Decoder>(); },
};

} // namespace openmedia
