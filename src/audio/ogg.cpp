#include <ogg/ogg.h>
#include <cstdlib>
#include <cstring>
#include <future>
#include <map>
#include <openmedia/format_api.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/track.hpp>
#include <span>
#include <string_view>
#include <util/demuxer_base.hpp>
#include <util/io_util.hpp>
#include <vector>

namespace openmedia {

namespace {

void appendOpusTags(std::vector<uint8_t>& packet, std::string_view vendor) {
  packet.clear();
  packet.reserve(16 + vendor.size());
  packet.insert(packet.end(), {'O', 'p', 'u', 's', 'T', 'a', 'g', 's'});
  packet.push_back(static_cast<uint8_t>(vendor.size() & 0xFF));
  packet.push_back(static_cast<uint8_t>((vendor.size() >> 8) & 0xFF));
  packet.push_back(static_cast<uint8_t>((vendor.size() >> 16) & 0xFF));
  packet.push_back(static_cast<uint8_t>((vendor.size() >> 24) & 0xFF));
  packet.insert(packet.end(), vendor.begin(), vendor.end());
  packet.insert(packet.end(), {0, 0, 0, 0});
}

auto splitVorbisExtradata(std::span<const uint8_t> extradata) -> std::vector<std::vector<uint8_t>> {
  std::vector<std::vector<uint8_t>> packets;
  if (extradata.size() < 3 || extradata[0] != 2) return packets;

  size_t offset = 1;
  size_t sizes[2] = {0, 0};
  for (size_t i = 0; i < 2; ++i) {
    size_t size = 0;
    while (offset < extradata.size()) {
      const uint8_t value = extradata[offset++];
      size += value;
      if (value < 255) break;
    }
    sizes[i] = size;
  }

  if (offset + sizes[0] + sizes[1] > extradata.size()) return {};
  const size_t size3 = extradata.size() - offset - sizes[0] - sizes[1];

  packets.resize(3);
  packets[0].assign(extradata.begin() + static_cast<std::ptrdiff_t>(offset), extradata.begin() + static_cast<std::ptrdiff_t>(offset + sizes[0]));
  offset += sizes[0];
  packets[1].assign(extradata.begin() + static_cast<std::ptrdiff_t>(offset), extradata.begin() + static_cast<std::ptrdiff_t>(offset + sizes[1]));
  offset += sizes[1];
  packets[2].assign(extradata.begin() + static_cast<std::ptrdiff_t>(offset), extradata.begin() + static_cast<std::ptrdiff_t>(offset + size3));
  return packets;
}

}

class OggMuxer final : public BaseMuxer {
  ogg_stream_state stream_ = {};
  bool stream_initialized_ = false;
  int64_t last_granulepos_ = 0;
  int64_t packetno_ = 0;
  OMCodecId codec_id_ = OM_CODEC_NONE;

public:
  ~OggMuxer() override { close(); }

  auto open(std::unique_ptr<OutputStream> output) -> OMError override {
    if (!output || !output->isValid()) {
      return OM_IO_INVALID_STREAM;
    }
    output_ = std::move(output);
    opened_ = true;
    finalized_ = false;
    tracks_.clear();
    last_granulepos_ = 0;
    packetno_ = 0;
    codec_id_ = OM_CODEC_NONE;
    return OM_SUCCESS;
  }

  void close() override {
    if (stream_initialized_) {
      ogg_stream_clear(&stream_);
      stream_initialized_ = false;
    }
    last_granulepos_ = 0;
    packetno_ = 0;
    codec_id_ = OM_CODEC_NONE;
    BaseMuxer::close();
  }

  auto addTrack(const Track& track) -> int32_t override {
    if (!opened_ || finalized_ || stream_initialized_ || !tracks_.empty()) {
      return -1;
    }
    if (track.format.type != OM_MEDIA_AUDIO) {
      return -1;
    }
    if (track.format.codec_id != OM_CODEC_OPUS && track.format.codec_id != OM_CODEC_VORBIS) {
      return -1;
    }

    const int serial = track.id > 0 ? track.id : 1;
    if (ogg_stream_init(&stream_, serial) != 0) {
      return -1;
    }
    stream_initialized_ = true;
    codec_id_ = track.format.codec_id;

    Track stored_track = track;
    stored_track.index = 0;
    if (stored_track.time_base.num == 0 || stored_track.time_base.den == 0) {
      const int32_t rate = stored_track.format.codec_id == OM_CODEC_OPUS ? 48000 : static_cast<int32_t>(stored_track.format.audio.sample_rate);
      stored_track.time_base = {1, rate};
    }
    tracks_.push_back(std::move(stored_track));

    const OMError header_err = writeHeaders(tracks_.front());
    if (header_err != OM_SUCCESS) {
      ogg_stream_clear(&stream_);
      stream_initialized_ = false;
      tracks_.clear();
      codec_id_ = OM_CODEC_NONE;
      return -1;
    }
    return 0;
  }

  auto writePacket(const Packet& packet) -> OMError override {
    if (!opened_ || finalized_ || !stream_initialized_ || tracks_.empty()) {
      return OM_COMMON_NOT_INITIALIZED;
    }
    if (packet.stream_index != 0) {
      return OM_FORMAT_STREAM_NOT_FOUND;
    }

    ogg_packet op = {};
    op.packet = const_cast<unsigned char*>(packet.bytes.data());
    op.bytes = static_cast<long>(packet.bytes.size());
    op.b_o_s = 0;
    op.e_o_s = 0;
    op.packetno = packetno_++;
    op.granulepos = updateGranulePosition(packet);

    if (ogg_stream_packetin(&stream_, &op) != 0) {
      return OM_FORMAT_MUXING_FAILED;
    }
    return flushPages(false);
  }

  auto finalize() -> OMError override {
    if (!opened_ || finalized_) {
      return OM_SUCCESS;
    }
    if (!stream_initialized_) {
      return OM_COMMON_NOT_INITIALIZED;
    }

    ogg_packet eos = {};
    eos.packet = nullptr;
    eos.bytes = 0;
    eos.b_o_s = 0;
    eos.e_o_s = 1;
    eos.packetno = packetno_++;
    eos.granulepos = last_granulepos_;
    if (ogg_stream_packetin(&stream_, &eos) != 0) {
      return OM_FORMAT_MUXING_FAILED;
    }
    const OMError err = flushPages(true);
    if (err != OM_SUCCESS) return err;
    if (!output_->flush()) return OM_IO_WRITE_FAILED;
    finalized_ = true;
    return OM_SUCCESS;
  }

private:
  auto writeHeaders(const Track& track) -> OMError {
    if (track.format.codec_id == OM_CODEC_OPUS) {
      if (track.extradata.size() < 8 || std::memcmp(track.extradata.data(), "OpusHead", 8) != 0) {
        return OM_CODEC_INVALID_PARAMS;
      }

      std::vector<uint8_t> tags;
      appendOpusTags(tags, "OpenMedia");
      const OMError head_err = submitHeaderPacket(track.extradata, true, false, 0);
      if (head_err != OM_SUCCESS) return head_err;
      return submitHeaderPacket(tags, false, false, 0);
    }

    auto headers = splitVorbisExtradata(track.extradata);
    if (headers.size() != 3) {
      return OM_CODEC_INVALID_PARAMS;
    }
    for (size_t i = 0; i < headers.size(); ++i) {
      const OMError err = submitHeaderPacket(headers[i], i == 0, false, 0);
      if (err != OM_SUCCESS) return err;
    }
    return OM_SUCCESS;
  }

  auto submitHeaderPacket(std::span<const uint8_t> bytes, bool bos, bool eos, int64_t granulepos) -> OMError {
    ogg_packet op = {};
    op.packet = const_cast<unsigned char*>(bytes.data());
    op.bytes = static_cast<long>(bytes.size());
    op.b_o_s = bos ? 1 : 0;
    op.e_o_s = eos ? 1 : 0;
    op.packetno = packetno_++;
    op.granulepos = granulepos;
    if (ogg_stream_packetin(&stream_, &op) != 0) {
      return OM_FORMAT_MUXING_FAILED;
    }
    return flushPages(true);
  }

  auto flushPages(bool force) -> OMError {
    ogg_page page = {};
    while ((force ? ogg_stream_flush(&stream_, &page) : ogg_stream_pageout(&stream_, &page)) != 0) {
      if (output_->write({page.header, static_cast<size_t>(page.header_len)}) != static_cast<size_t>(page.header_len) ||
          output_->write({page.body, static_cast<size_t>(page.body_len)}) != static_cast<size_t>(page.body_len)) {
        return OM_IO_WRITE_FAILED;
      }
    }
    return OM_SUCCESS;
  }

  auto updateGranulePosition(const Packet& packet) -> int64_t {
    int64_t duration = packet.duration;
    if (duration <= 0 && packet.pts >= 0 && packet.dts >= 0 && packet.pts != packet.dts) {
      duration = std::llabs(packet.pts - packet.dts);
    }
    if (duration <= 0) {
      duration = 0;
    }

    if (packet.pts >= 0) {
      last_granulepos_ = packet.pts + duration;
    } else {
      last_granulepos_ += duration;
    }
    return last_granulepos_;
  }
};

class OggDemuxer final : public BaseDemuxer {
  ogg_sync_state sync_ = {};
  std::map<int, ogg_stream_state> streams_state_;
  std::map<int, int> stream_id_to_index_;
  std::map<int, bool> streams_header_complete_;

public:
  OggDemuxer() {
    ogg_sync_init(&sync_);
  }

  ~OggDemuxer() override {
    for (auto& pair : streams_state_) {
      ogg_stream_clear(&pair.second);
    }
    ogg_sync_clear(&sync_);
  }

  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    input_ = std::move(input);
    if (!input_ || !input_->isValid()) {
      return OM_IO_INVALID_STREAM;
    }

    while (tracks_.empty() || !all_headers_read()) {
      if (!read_more_and_process()) break;
      if (input_->isEOF() && tracks_.empty()) break;
    }

    return tracks_.empty() ? OM_FORMAT_PARSE_FAILED : OM_SUCCESS;
  }

  auto readPacket() -> Result<Packet, OMError> override {
    while (true) {
      for (auto& pair : streams_state_) {
        ogg_packet op;
        if (ogg_stream_packetout(&pair.second, &op) == 1) {
          Packet pkt;
          pkt.allocate(op.bytes);
          memcpy(pkt.bytes.data(), op.packet, op.bytes);
          pkt.stream_index = stream_id_to_index_[pair.first];
          pkt.pts = op.granulepos;
          pkt.dts = pkt.pts;
          return Ok(std::move(pkt));
        }
      }
      if (!read_more_and_process()) return Err(OM_FORMAT_PARSE_FAILED);
    }
  }

  auto seek(int32_t stream_idx, int64_t timestamp, SeekMode mode) -> OMError override {
    if (timestamp == 0) {
      input_->seek(0, Whence::BEG);
      ogg_sync_reset(&sync_);
      for (auto& pair : streams_state_) {
        ogg_stream_reset(&pair.second);
      }
      return OM_SUCCESS;
    }
    return OM_FORMAT_PARSE_FAILED;
  }

private:
  auto read_more_and_process() -> bool {
    char* buffer = ogg_sync_buffer(&sync_, 8192);
    size_t n = input_->read({reinterpret_cast<uint8_t*>(buffer), 8192});
    if (n == 0) return false;

    ogg_sync_wrote(&sync_, n);

    ogg_page og;
    while (ogg_sync_pageout(&sync_, &og) == 1) {
      int serial = ogg_page_serialno(&og);
      if (!streams_state_.contains(serial)) {
        ogg_stream_init(&streams_state_[serial], serial);

        Track track;
        track.index = static_cast<int32_t>(tracks_.size());
        track.id = serial;
        stream_id_to_index_[serial] = track.index;

        ogg_stream_pagein(&streams_state_[serial], &og);

        ogg_packet op;
        if (ogg_stream_packetpeek(&streams_state_[serial], &op) == 1) {
          if (op.bytes >= 7 && memcmp(op.packet + 1, "vorbis", 6) == 0) {
            track.format.type = OM_MEDIA_AUDIO;
            track.format.codec_id = OM_CODEC_VORBIS;
            if (op.bytes >= 30) {
              track.format.audio.channels = op.packet[11];
              track.format.audio.sample_rate = load_u32_le(op.packet + 12);
              track.bitrate = load_u32_le(op.packet + 20);
              track.time_base = {1, static_cast<int32_t>(track.format.audio.sample_rate)};
            }
          } else if (op.bytes >= 8 && memcmp(op.packet, "OpusHead", 8) == 0) {
            track.format.type = OM_MEDIA_AUDIO;
            track.format.codec_id = OM_CODEC_OPUS;
            if (op.bytes >= 19) {
              track.format.audio.channels = op.packet[9];
              track.format.audio.sample_rate = load_u32_le(op.packet + 12);
              track.time_base = {1, 48000};
            }
          }
        }
        tracks_.push_back(track);
        streams_header_complete_[serial] = true;
      } else {
        ogg_stream_pagein(&streams_state_[serial], &og);
      }
    }
    return true;
  }

  auto all_headers_read() const -> bool {
    if (tracks_.empty()) return false;
    for (const auto& stream : tracks_) {
      if (!streams_header_complete_.contains(stream.id)) return false;
    }
    return true;
  }
};

const FormatDescriptor FORMAT_OGG = {
    .container_id = OM_CONTAINER_OGG,
    .name = "ogg",
    .long_name = "Ogg",
    .demuxer_factory = [] { return std::make_unique<OggDemuxer>(); },
    .muxer_factory = [] { return std::make_unique<OggMuxer>(); },
};

} // namespace openmedia
