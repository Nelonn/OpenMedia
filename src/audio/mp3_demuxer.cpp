#include <cstring>
#include <future>
#include <openmedia/format_api.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/track.hpp>
#include <util/demuxer_base.hpp>
#include <util/id3_parser.hpp>
#include <util/io_util.hpp>
#include <vector>

#define MINIMP3_IMPLEMENTATION
#include <minimp3.h>

namespace openmedia {

static constexpr int XING_OFFSET_STEREO = 4 + 32;
static constexpr int XING_OFFSET_MONO = 4 + 17;

static auto samplesPerFrame(const mp3dec_frame_info_t& info) -> int {
  if (info.layer == 3 && info.hz < 32000) return 576;
  return 1152;
}

static auto parseVbrHeader(const uint8_t* frame_start, size_t frame_len,
                           const mp3dec_frame_info_t& info) -> int64_t {
  if (frame_len < 180) return 0;

  int xing_off = (info.channels > 1) ? XING_OFFSET_STEREO : XING_OFFSET_MONO;
  const uint8_t* xing = frame_start + xing_off;

  if (frame_len > static_cast<size_t>(xing_off + 12) &&
      (memcmp(xing, "Xing", 4) == 0 || memcmp(xing, "Info", 4) == 0)) {
    uint32_t flags = (xing[4] << 24) | (xing[5] << 16) | (xing[6] << 8) | xing[7];
    if (flags & 0x1) {
      uint32_t total_frames = (xing[8] << 24) | (xing[9] << 16) | (xing[10] << 8) | xing[11];
      if (total_frames > 0)
        return static_cast<int64_t>(total_frames) * samplesPerFrame(info);
    }
  }

  const uint8_t* vbri = frame_start + 4 + 32;
  if (frame_len > static_cast<size_t>(4 + 32 + 18) && memcmp(vbri, "VBRI", 4) == 0) {
    uint32_t total_frames = (vbri[14] << 24) | (vbri[15] << 16) | (vbri[16] << 8) | vbri[17];
    if (total_frames > 0)
      return static_cast<int64_t>(total_frames) * samplesPerFrame(info);
  }

  return 0;
}

static auto estimateDurationFromBitrate(InputStream* input, const mp3dec_frame_info_t& info) -> int64_t {
  if (info.bitrate_kbps <= 0) return 0;
  int64_t file_size = input->size();
  if (file_size <= 0) return 0;

  double seconds = static_cast<double>(file_size) / (info.bitrate_kbps * 125.0);
  return static_cast<int64_t>(seconds * info.hz);
}

class MP3Demuxer final : public BaseDemuxer {
  mp3dec_t decoder_ = {};
  int64_t pts_counter_ = 0;
  int64_t audio_data_offset_ = 0;

public:
  MP3Demuxer() {
    mp3dec_init(&decoder_);
  }

  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    input_ = std::move(input);
    if (!input_ || !input_->isValid()) {
      return OM_IO_INVALID_STREAM;
    }

    uint8_t id3hdr[10];
    int64_t audio_start_pos = 0;
    if (readExact(*input_, id3hdr) == 10 && std::memcmp(id3hdr, "ID3", 3) == 0 &&
        id3hdr[3] <= 4 && !(id3hdr[6] & 0x80) && !(id3hdr[7] & 0x80) &&
        !(id3hdr[8] & 0x80) && !(id3hdr[9] & 0x80)) {
      size_t tag_size = (static_cast<size_t>(id3hdr[6] & 0x7F) << 21) |
                        (static_cast<size_t>(id3hdr[7] & 0x7F) << 14) |
                        (static_cast<size_t>(id3hdr[8] & 0x7F) << 7) |
                        static_cast<size_t>(id3hdr[9] & 0x7F);
      tag_size += 10;
      if (id3hdr[5] & 0x10) tag_size += 10;
      std::vector<uint8_t> tag_bytes(tag_size);
      memcpy(tag_bytes.data(), id3hdr, 10);
      if (tag_size > 10) {
        readExact(*input_, std::span<uint8_t>(tag_bytes.data() + 10, tag_size - 10));
      }
      parseId3v2(tag_bytes, metadata_);
      audio_start_pos = static_cast<int64_t>(tag_size);
    }
    input_->seek(audio_start_pos, Whence::BEG);

    if (input_->canSeek() && input_->size() >= 128) {
      int64_t saved_pos = input_->tell();
      if (input_->seek(input_->size() - 128, Whence::BEG)) {
        uint8_t id3v1[128];
        if (readExact(*input_, id3v1) == 128) {
          parseId3v1(id3v1, metadata_);
        }
      }
      input_->seek(saved_pos, Whence::BEG);
    }

    uint8_t buffer[16384];
    size_t n = input_->read(buffer);
    if (n < 4) {
      return OM_IO_NOT_ENOUGH_DATA;
    }

    mp3dec_frame_info_t info;
    size_t frame_offset = 0;

    mp3dec_decode_frame(&decoder_, buffer, n, nullptr, &info);

    if (info.frame_bytes == 0) {
      for (size_t i = 0; i < n - 4; ++i) {
        if (buffer[i] == 0xFF && (buffer[i + 1] & 0xE0) == 0xE0) {
          mp3dec_decode_frame(&decoder_, buffer + i, n - i, nullptr, &info);
          if (info.frame_bytes > 0) {
            frame_offset = i;
            input_->seek(audio_start_pos + static_cast<int64_t>(i), Whence::BEG);
            break;
          }
        }
      }
    } else {
      input_->seek(audio_start_pos, Whence::BEG);
    }

    if (info.frame_bytes == 0) {
      return OM_FORMAT_PARSE_FAILED;
    }

    audio_data_offset_ = audio_start_pos + static_cast<int64_t>(frame_offset);

    int64_t duration = parseVbrHeader(buffer + frame_offset, n - frame_offset, info);
    if (duration == 0) {
      duration = estimateDurationFromBitrate(input_.get(), info);
    }

    Track track;
    track.index = 0;
    track.format.type = OM_MEDIA_AUDIO;
    track.format.codec_id = OM_CODEC_MP3;
    track.format.audio.sample_rate = static_cast<uint32_t>(info.hz);
    track.format.audio.channels = static_cast<uint32_t>(info.channels);
    track.bitrate = info.bitrate_kbps * 1000;
    track.time_base = {1, info.hz};
    track.duration = duration;
    track.metadata = metadata_;

    tracks_.push_back(track);

    return OM_SUCCESS;
  }

  void close() override {
    BaseDemuxer::close();
    pts_counter_ = 0;
    audio_data_offset_ = 0;
    mp3dec_init(&decoder_);
  }

  auto readPacket() -> Result<Packet, OMError> override {
    uint8_t buffer[16384];
    int64_t pos = input_->tell();
    size_t n = input_->read(buffer);
    if (n < 4) return Err(OM_FORMAT_END_OF_FILE);

    mp3dec_frame_info_t info;
    int frame_samples = mp3dec_decode_frame(&decoder_, buffer, n, nullptr, &info);

    if (info.frame_bytes == 0) {
      for (size_t i = 1; i < n - 4; ++i) {
        if (buffer[i] == 0xFF && (buffer[i + 1] & 0xE0) == 0xE0) {
          input_->seek(pos + i, Whence::BEG);
          return Err(OM_FORMAT_PARSE_FAILED);
        }
      }
      return Err(OM_FORMAT_PARSE_FAILED);
    }

    Packet pkt;
    pkt.allocate(info.frame_bytes);
    memcpy(pkt.bytes.data(), buffer, info.frame_bytes);
    pkt.stream_index = 0;
    pkt.pos = pos;
    pkt.pts = pts_counter_;
    pkt.dts = pkt.pts;
    pkt.duration = frame_samples;

    pts_counter_ += frame_samples;

    input_->seek(pos + info.frame_bytes, Whence::BEG);
    return Ok(std::move(pkt));
  }

  auto seek(int32_t stream_idx, int64_t timestamp, SeekMode mode) -> OMError override {
    if (tracks_.empty()) {
      return OM_COMMON_NOT_INITIALIZED;
    }

    if (timestamp < 0) {
      return OM_COMMON_INVALID_ARGUMENT;
    }

    if (timestamp == 0) {
      pts_counter_ = 0;
      return input_->seek(audio_data_offset_, Whence::BEG) ? OM_SUCCESS : OM_IO_SEEK_FAILED;
    }

    const Track& track = tracks_[0];
    const int64_t sample_rate = static_cast<int64_t>(track.format.audio.sample_rate);
    if (sample_rate <= 0) {
      return OM_COMMON_INVALID_ARGUMENT;
    }

    int64_t target_sample;
    if (stream_idx < 0) {
      const double target_sample_d = static_cast<double>(timestamp) * static_cast<double>(sample_rate) / 1.0e6;
      target_sample = static_cast<int64_t>(target_sample_d);
    } else {
      target_sample = timestamp;
    }

    if (track.duration > 0 && target_sample >= track.duration) {
      return OM_COMMON_INVALID_ARGUMENT;
    }

    const int bitrate_kbps = track.bitrate / 1000;

    int64_t byte_offset = audio_data_offset_;
    if (bitrate_kbps > 0) {
      byte_offset += static_cast<int64_t>(
          static_cast<double>(target_sample) * static_cast<double>(bitrate_kbps) * 125.0 / static_cast<double>(sample_rate));
    }

    if (!input_->seek(byte_offset, Whence::BEG)) {
      return OM_IO_SEEK_FAILED;
    }

    // Read a chunk and scan forward for the next valid MP3 sync word.
    // A false sync is rejected when mp3dec_decode_frame returns frame_bytes == 0.
    uint8_t sync_buf[16384];
    const size_t sync_n = input_->read(sync_buf);
    if (sync_n < 4) {
      return OM_IO_NOT_ENOUGH_DATA;
    }

    mp3dec_frame_info_t info;
    size_t sync_off = 0;
    bool found = false;

    for (size_t i = 0; i + 4 <= sync_n; ++i) {
      if (sync_buf[i] == 0xFF && (sync_buf[i + 1] & 0xE0) == 0xE0) {
        mp3dec_decode_frame(&decoder_, sync_buf + i, sync_n - i, nullptr, &info);
        if (info.frame_bytes > 0) {
          sync_off = i;
          found = true;
          break;
        }
      }
    }

    if (!found) {
      return OM_FORMAT_PARSE_FAILED;
    }

    // Position the stream at the confirmed frame boundary.
    const int64_t final_pos = byte_offset + static_cast<int64_t>(sync_off);
    if (!input_->seek(final_pos, Whence::BEG)) {
      return OM_IO_SEEK_FAILED;
    }

    // Derive pts_counter_ from the actual landing position rather than the
    // requested timestamp, so subsequent packets are time-stamped consistently
    // with where we actually landed.
    if (bitrate_kbps > 0) {
      pts_counter_ = static_cast<int64_t>(
          static_cast<double>(final_pos) / (static_cast<double>(bitrate_kbps) * 125.0) * static_cast<double>(sample_rate));
    } else {
      // No bitrate info; best-effort fallback.
      pts_counter_ = target_sample;
    }

    return OM_SUCCESS;
  }
};

const FormatDescriptor FORMAT_MP3 = {
    .container_id = OM_CONTAINER_MP3,
    .name = "mp3",
    .long_name = "MPEG Audio Layer III",
    .demuxer_factory = [] { return std::make_unique<MP3Demuxer>(); },
    .muxer_factory = {},
};

} // namespace openmedia
