#include <algorithm>
#include <cstring>
#include <future>
#include <openmedia/format_api.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/track.hpp>
#include <util/demuxer_base.hpp>
#include <util/io_util.hpp>

namespace openmedia {

class WAVDemuxer final : public BaseDemuxer {
  uint32_t bits_per_sample_ = 0;
  int64_t data_offset_;
  uint32_t data_size_;

public:
  WAVDemuxer()
      : data_offset_(0), data_size_(0) {}

  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    input_ = std::move(input);
    if (!input_ || !input_->isValid()) {
      return OM_IO_INVALID_STREAM;
    }

    uint8_t riff_header[12];
    if (input_->read(riff_header) < 12) {
      return OM_IO_NOT_ENOUGH_DATA;
    }
    if (memcmp(riff_header, "RIFF", 4) != 0 || memcmp(riff_header + 8, "WAVE", 4) != 0) {
      return OM_FORMAT_PARSE_FAILED;
    }

    uint32_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t audio_format = 0;
    bool found_fmt = false;

    int64_t chunk_pos = 12;
    while (chunk_pos + 8 <= static_cast<int64_t>(input_->size())) {
      if (!input_->seek(chunk_pos, Whence::BEG)) break;

      uint8_t chunk_header[8];
      if (input_->read(chunk_header) < 8) break;

      uint32_t chunk_size =
          chunk_header[4] | (chunk_header[5] << 8) | (chunk_header[6] << 16) | (chunk_header[7] << 24);

      if (memcmp(chunk_header, "fmt ", 4) == 0) {
        if (chunk_size < 16) {
          return OM_FORMAT_PARSE_FAILED;
        }
        uint8_t fmt[16];
        if (input_->read(fmt) < 16) {
          return OM_IO_NOT_ENOUGH_DATA;
        }

        audio_format = fmt[0] | (fmt[1] << 8);
        channels = fmt[2] | (fmt[3] << 8);
        sample_rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (fmt[7] << 24);
        bits_per_sample_ = fmt[14] | (fmt[15] << 8);
        found_fmt = true;

      } else if (memcmp(chunk_header, "data", 4) == 0) {
        if (!found_fmt) {
          return OM_FORMAT_PARSE_FAILED;
        }
        data_size_ = chunk_size;
        data_offset_ = chunk_pos + 8;
        break;
      }

      chunk_pos += 8 + chunk_size;
      if (chunk_size & 1) chunk_pos++; // RIFF word-alignment
    }

    if (!found_fmt || data_size_ == 0 || channels == 0) {
      return OM_FORMAT_PARSE_FAILED;
    }

    Track track;
    track.index = 0;
    track.format.type = OM_MEDIA_AUDIO;
    track.format.audio.channels = channels;
    track.format.audio.sample_rate = sample_rate;

    if (audio_format == 3) {
      // IEEE float
      if (bits_per_sample_ == 32) {
        track.format.codec_id = OM_CODEC_PCM_F32LE;
      } else if (bits_per_sample_ == 64) {
        track.format.codec_id = OM_CODEC_PCM_F64LE;
      } else {
        return OM_FORMAT_PARSE_FAILED;
      }
    } else if (audio_format == 1) {
      // PCM integer
      if (bits_per_sample_ == 8) {
        track.format.codec_id = OM_CODEC_PCM_U8;
      } else if (bits_per_sample_ == 16) {
        track.format.codec_id = OM_CODEC_PCM_S16LE;
      } else if (bits_per_sample_ == 24 || bits_per_sample_ == 32) {
        track.format.codec_id = OM_CODEC_PCM_S32LE;
      } else {
        return OM_FORMAT_PARSE_FAILED;
      }
    } else {
      return OM_FORMAT_PARSE_FAILED;
    }

    track.time_base = {1, static_cast<int>(sample_rate)};
    track.duration = data_size_ / (channels * (bits_per_sample_ / 8));
    tracks_.push_back(track);

    input_->seek(data_offset_, Whence::BEG);
    return OM_SUCCESS;
  }

  auto readPacket() -> Result<Packet, OMError> override {
    int64_t current_pos = input_->tell();
    int64_t end_pos = data_offset_ + static_cast<int64_t>(data_size_);
    if (current_pos >= end_pos) return Err(OM_FORMAT_END_OF_FILE);

    size_t to_read = std::min<size_t>(4096, static_cast<size_t>(end_pos - current_pos));
    if (to_read == 0) {
      return Err(OM_FORMAT_PARSE_FAILED);
    }

    Packet pkt;
    pkt.allocate(to_read);
    pkt.stream_index = 0;
    pkt.pos = current_pos;

    int64_t offset_in_data = current_pos - data_offset_;
    pkt.pts = offset_in_data / (tracks_[0].format.audio.channels * (bits_per_sample_ / 8));
    pkt.dts = pkt.pts;

    size_t n = input_->read(pkt.bytes);
    if (n == 0) return Err(OM_FORMAT_END_OF_FILE);
    pkt.bytes = pkt.bytes.subspan(0, n);

    return Ok(std::move(pkt));
  }

  auto seek(int32_t stream_idx, int64_t timestamp, SeekMode mode) -> OMError override {
    if (tracks_.empty()) {
      return OM_COMMON_NOT_INITIALIZED;
    }
    const auto& fmt = tracks_[0].format.audio;
    // If stream_idx < 0, timestamp is in microseconds; otherwise it's in track time base.
    int64_t frame_index;
    if (stream_idx < 0) {
      // timestamp is in microseconds
      frame_index = (timestamp * fmt.sample_rate) / 1'000'000;
    } else {
      // timestamp is already in track time base (samples)
      frame_index = timestamp;
    }
    int64_t block_align = fmt.channels * (bits_per_sample_ == 24 ? (32 / 8) : (bits_per_sample_ / 8));
    int64_t byte_offset = frame_index * block_align;
    return input_->seek(data_offset_ + byte_offset, Whence::BEG) ? OM_SUCCESS : OM_IO_SEEK_FAILED;
  }
};

auto create_wav_demuxer() -> std::unique_ptr<Demuxer> {
  return std::make_unique<WAVDemuxer>();
}

const FormatDescriptor FORMAT_WAV = {
    .container_id = OM_CONTAINER_WAV,
    .name = "wav",
    .long_name = "WAV / RIFF WAVE",
    .demuxer_factory = [] { return create_wav_demuxer(); },
    .muxer_factory = {},
};

} // namespace openmedia
