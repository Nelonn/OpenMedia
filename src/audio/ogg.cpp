#include <ogg/ogg.h>
#include <array>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <openmedia/format_api.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/track.hpp>
#include <optional>
#include <span>
#include <string_view>
#include <util/demuxer_base.hpp>
#include <util/io_util.hpp>
#include <util/vorbis_comment.hpp>
#include <util/xiph.hpp>
#include <vector>

namespace openmedia {

namespace {

auto buildOpusTags(std::string_view vendor) -> std::vector<uint8_t> {
  std::vector<uint8_t> packet;
  packet.reserve(16 + vendor.size());
  packet.insert(packet.end(), {'O', 'p', 'u', 's', 'T', 'a', 'g', 's'});
  append_u32_le(packet, static_cast<uint32_t>(vendor.size()));
  packet.insert(packet.end(), vendor.begin(), vendor.end());
  append_u32_le(packet, 0); // user comment count
  return packet;
}

auto makePacket(const ogg_packet& op, int32_t stream_index) -> Packet {
  const auto size = static_cast<size_t>(op.bytes);
  Packet pkt;
  pkt.allocate(size);
  memcpy(pkt.bytes.data(), op.packet, size);
  pkt.stream_index = stream_index;
  pkt.pts = op.granulepos;
  pkt.dts = op.granulepos;
  return pkt;
}

} // namespace

class OggMuxer final : public BaseMuxer {
  ogg_stream_state stream_ = {};
  bool stream_initialized_ = false;
  int64_t last_granulepos_ = 0;
  int64_t packetno_ = 0;

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
    return OM_SUCCESS;
  }

  void close() override {
    if (stream_initialized_) {
      ogg_stream_clear(&stream_);
      stream_initialized_ = false;
    }
    last_granulepos_ = 0;
    packetno_ = 0;
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

    Track stored_track = track;
    stored_track.index = 0;
    if (stored_track.time_base.num == 0 || stored_track.time_base.den == 0) {
      const int32_t rate = stored_track.format.codec_id == OM_CODEC_OPUS ? 48000 : static_cast<int32_t>(stored_track.format.audio.sample_rate);
      stored_track.time_base = {1, rate};
    }
    tracks_.push_back(std::move(stored_track));

    if (writeHeaders(tracks_.front()) != OM_SUCCESS) {
      ogg_stream_clear(&stream_);
      stream_initialized_ = false;
      tracks_.clear();
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
      const OMError head_err = submitHeaderPacket(track.extradata, true);
      if (head_err != OM_SUCCESS) return head_err;
      return submitHeaderPacket(buildOpusTags("OpenMedia"), false);
    }

    const auto headers = splitVorbisExtradata(track.extradata);
    if (!headers) {
      return OM_CODEC_INVALID_PARAMS;
    }
    for (size_t i = 0; i < headers->size(); ++i) {
      const OMError err = submitHeaderPacket((*headers)[i], i == 0);
      if (err != OM_SUCCESS) return err;
    }
    return OM_SUCCESS;
  }

  auto submitHeaderPacket(std::span<const uint8_t> bytes, bool bos) -> OMError {
    ogg_packet op = {};
    op.packet = const_cast<unsigned char*>(bytes.data());
    op.bytes = static_cast<long>(bytes.size());
    op.b_o_s = bos ? 1 : 0;
    op.packetno = packetno_++;
    // A header may not share its page with audio data, so each one is flushed.
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
    if (duration <= 0 && packet.pts >= 0 && packet.dts >= 0) {
      duration = std::llabs(packet.pts - packet.dts);
    }
    if (duration < 0) {
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
  static constexpr size_t READ_CHUNK = 64 * 1024;

  struct Stream {
    ogg_stream_state state = {};
    int serial = 0;
    int32_t track_index = 0;
    int32_t packet_count = 0;
    bool header_complete = false;
  };

  ogg_sync_state sync_ = {};
  // libogg is handed &Stream::state by address, so the container must not
  // relocate its elements as further streams are discovered.
  std::deque<Stream> streams_;
  std::deque<Packet> buffered_packets_;
  size_t pending_headers_ = 0;

public:
  OggDemuxer() {
    ogg_sync_init(&sync_);
  }

  ~OggDemuxer() override {
    close();
    ogg_sync_clear(&sync_);
  }

  void close() override {
    BaseDemuxer::close();
    buffered_packets_.clear();
    for (Stream& stream : streams_) {
      ogg_stream_clear(&stream.state);
    }
    streams_.clear();
    pending_headers_ = 0;
    ogg_sync_reset(&sync_);
  }

  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    close();
    input_ = std::move(input);
    if (!input_ || !input_->isValid()) {
      return OM_IO_INVALID_STREAM;
    }

    while (tracks_.empty() || pending_headers_ > 0) {
      if (!readMoreAndProcess()) break;
    }

    return tracks_.empty() ? OM_FORMAT_PARSE_FAILED : OM_SUCCESS;
  }

  auto readPacket() -> Result<Packet, OMError> override {
    if (!buffered_packets_.empty()) {
      Packet pkt = std::move(buffered_packets_.front());
      buffered_packets_.pop_front();
      return Ok(std::move(pkt));
    }
    while (true) {
      for (Stream& stream : streams_) {
        ogg_packet op;
        if (ogg_stream_packetout(&stream.state, &op) == 1) {
          return Ok(makePacket(op, stream.track_index));
        }
      }
      if (!readMoreAndProcess()) return Err(OM_FORMAT_END_OF_FILE);
    }
  }

  auto seek(int32_t stream_idx, int64_t timestamp, SeekMode mode) -> OMError override {
    if (timestamp != 0) {
      return OM_FORMAT_PARSE_FAILED;
    }
    buffered_packets_.clear();
    input_->seek(0, Whence::BEG);
    ogg_sync_reset(&sync_);
    for (Stream& stream : streams_) {
      ogg_stream_reset(&stream.state);
    }
    return OM_SUCCESS;
  }

private:
  static auto parseIdentificationHeader(const ogg_packet& op, Track& track) -> bool {
    if (op.bytes >= 7 && memcmp(op.packet + 1, "vorbis", 6) == 0) {
      track.format.type = OM_MEDIA_AUDIO;
      track.format.codec_id = OM_CODEC_VORBIS;
      if (op.bytes >= 30) {
        track.format.audio.channels = op.packet[11];
        track.format.audio.sample_rate = load_u32_le(op.packet + 12);
        track.bitrate = load_u32_le(op.packet + 20);
        track.time_base = {1, static_cast<int32_t>(track.format.audio.sample_rate)};
      }
      return true;
    }
    if (op.bytes >= 8 && memcmp(op.packet, "OpusHead", 8) == 0) {
      track.format.type = OM_MEDIA_AUDIO;
      track.format.codec_id = OM_CODEC_OPUS;
      if (op.bytes >= 19) {
        track.format.audio.channels = op.packet[9];
        track.format.audio.sample_rate = load_u32_le(op.packet + 12);
        // Opus granule positions count 48 kHz samples whatever the input rate.
        track.time_base = {1, 48000};
      }
      return true;
    }
    return false;
  }

  void parseCommentHeader(const ogg_packet& op, Track& track) {
    if (op.bytes >= 7 && op.packet[0] == 3 && memcmp(op.packet + 1, "vorbis", 6) == 0) {
      parseVorbisComment({op.packet + 7, static_cast<size_t>(op.bytes - 7)}, metadata_);
    } else if (op.bytes >= 8 && memcmp(op.packet, "OpusTags", 8) == 0) {
      parseVorbisComment({op.packet + 8, static_cast<size_t>(op.bytes - 8)}, metadata_);
    } else {
      return;
    }
    track.metadata = metadata_;
  }

  void markHeadersComplete(Stream& stream) {
    if (stream.header_complete) return;
    stream.header_complete = true;
    --pending_headers_;
  }

  // The decoders take the headers as ordinary packets, so each one is queued
  // for delivery as well as parsed here.
  void processHeaders(Stream& stream) {
    Track& track = tracks_[stream.track_index];

    ogg_packet op;
    while (!stream.header_complete && ogg_stream_packetout(&stream.state, &op) == 1) {
      const int32_t count = stream.packet_count++;
      buffered_packets_.push_back(makePacket(op, stream.track_index));

      if (count == 0) {
        if (!parseIdentificationHeader(op, track)) markHeadersComplete(stream);
      } else if (count == 1) {
        parseCommentHeader(op, track);
        if (track.format.codec_id == OM_CODEC_OPUS) markHeadersComplete(stream);
      } else {
        markHeadersComplete(stream);
      }
    }
  }

  auto streamFor(int serial) -> Stream& {
    for (Stream& stream : streams_) {
      if (stream.serial == serial) return stream;
    }

    Stream& stream = streams_.emplace_back();
    ogg_stream_init(&stream.state, serial);
    stream.serial = serial;
    stream.track_index = static_cast<int32_t>(tracks_.size());
    ++pending_headers_;

    Track track;
    track.index = stream.track_index;
    track.id = serial;
    tracks_.push_back(std::move(track));
    return stream;
  }

  auto readMoreAndProcess() -> bool {
    char* buffer = ogg_sync_buffer(&sync_, static_cast<long>(READ_CHUNK));
    if (buffer == nullptr) return false;
    const size_t n = input_->read({reinterpret_cast<uint8_t*>(buffer), READ_CHUNK});
    if (n == 0) return false;
    ogg_sync_wrote(&sync_, static_cast<long>(n));

    ogg_page og;
    int ret;
    while ((ret = ogg_sync_pageout(&sync_, &og)) != 0) {
      if (ret < 0) continue; // hole in the stream, libogg resyncs on its own
      Stream& stream = streamFor(ogg_page_serialno(&og));
      ogg_stream_pagein(&stream.state, &og);
      if (!stream.header_complete) {
        processHeaders(stream);
      }
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
