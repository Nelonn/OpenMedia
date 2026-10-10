#include <ogg/ogg.h>
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <openmedia/format_api.hpp>
#include <openmedia/log.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/track.hpp>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <util/bit_reader.hpp>
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

auto makePacket(const ogg_packet& op, int32_t stream_index, int64_t duration) -> Packet {
  const auto size = static_cast<size_t>(op.bytes);
  Packet pkt;
  pkt.allocate(size);
  memcpy(pkt.bytes.data(), op.packet, size);
  pkt.stream_index = stream_index;
  pkt.duration = duration;
  return pkt;
}

// RFC 6716 section 3.1: the TOC byte's configuration number picks the frame
// length, its low two bits the frame count, and count code 3 moves that count
// into the byte behind it. Returns 48 kHz samples, the unit Opus granule
// positions are counted in.
auto opusPacketSamples(std::span<const uint8_t> bytes) -> int64_t {
  static constexpr int32_t SILK_SIZES[4] = {480, 960, 1920, 2880};
  static constexpr int32_t CELT_SIZES[4] = {120, 240, 480, 960};
  constexpr int64_t MAX_SAMPLES = 120 * 48; // a packet may not exceed 120 ms

  if (bytes.empty()) return 0;
  const uint32_t config = bytes[0] >> 3;
  const int32_t frame_samples = config < 12  ? SILK_SIZES[config & 3]
                                : config < 16 ? ((config & 1) != 0 ? 960 : 480)
                                              : CELT_SIZES[config & 3];

  int32_t frames = 1;
  switch (bytes[0] & 3) {
    case 1:
    case 2: frames = 2; break;
    case 3:
      if (bytes.size() < 2) return 0;
      frames = bytes[1] & 0x3F;
      break;
    default: break;
  }

  const int64_t samples = static_cast<int64_t>(frames) * frame_samples;
  return samples <= MAX_SAMPLES ? samples : 0;
}

// Which of the two block sizes each mode selects, and the width of the mode
// number in an audio packet.
struct VorbisModes {
  std::vector<uint8_t> blockflag;
  uint32_t index_bits = 0;
};

// Vorbis I section 4.2.4. The mode table closes the setup header, behind
// codebooks that cannot be skipped without decoding them, so it is read from
// the back instead. Reversing the bytes is all that takes: Vorbis packs its
// bits low end first, so an MSB-first read of the reversed bytes walks the
// stream backwards and still hands back each field the right way round.
auto parseVorbisModes(std::span<const uint8_t> setup) -> std::optional<VorbisModes> {
  constexpr uint32_t COUNT_BITS = 6;
  constexpr uint32_t MAX_MODES = 64;
  if (setup.size() < 8) return std::nullopt;

  const std::vector<uint8_t> reversed(setup.rbegin(), setup.rend());
  BitReader reader(reversed);
  // The framing bit is the last bit the encoder wrote, so at most a byte of
  // zero padding stands between it and the end of the packet.
  bool framed = false;
  for (uint32_t i = 0; i < 8 && !framed; ++i) framed = reader.readBit() != 0;
  if (!framed) return std::nullopt;

  // Nothing says where the table starts, so every position that could hold the
  // mode count is tried and the last one agreeing with the number of modes read
  // so far wins: an earlier agreement is just as likely to be codebook data that
  // happens to fit. ffmpeg's parser and liboggz settle it the same way.
  std::vector<uint8_t> blockflag;
  size_t count = 0;
  while (blockflag.size() < MAX_MODES) {
    const uint32_t mapping = reader.readBits(8);
    const uint32_t window_transform = reader.readBits(32);
    const uint32_t flag = reader.readBit();
    // Window and transform type are zero and the mapping is an index into at
    // most 64 mappings; anything else cannot be a mode.
    if (!reader.ok() || mapping > 63 || window_transform != 0) break;
    blockflag.push_back(static_cast<uint8_t>(flag));
    if (reader.bitsLeft() >= COUNT_BITS &&
        static_cast<size_t>(reader.peekBits(COUNT_BITS)) + 1 == blockflag.size()) {
      count = blockflag.size();
    }
  }
  if (count == 0) return std::nullopt;

  // The modes came out back to front, and only the first `count` of them belong
  // to the table.
  blockflag.resize(count);
  std::ranges::reverse(blockflag);

  VorbisModes modes;
  modes.blockflag = std::move(blockflag);
  // Vorbis I section 9.2.1: the mode number is ilog(mode_count - 1) bits wide.
  modes.index_bits =
      count > 1 ? 32 - static_cast<uint32_t>(std::countl_zero(static_cast<uint32_t>(count - 1))) : 0;
  return modes;
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
  // RFC 7845 section 13: an Opus decoder needs 80 ms of audio ahead of a seek
  // point before its output can be trusted.
  static constexpr int64_t OPUS_PREROLL = 80 * 48;
  // How far back the duration probe starts looking for the final page, and how
  // much further it reaches when a stream's last page is not in there.
  static constexpr int64_t TAIL_WINDOW = 64 * 1024;
  static constexpr int64_t TAIL_GROWTH = 16;

  struct Stream {
    ogg_stream_state state = {};
    int serial = 0;
    int32_t track_index = 0;
    int32_t header_packets = 0;
    bool header_complete = false;

    OMCodecId codec_id = OM_CODEC_NONE;
    // Granule positions to rewind past a seek target so that the decoder has
    // converged by the time it reaches it.
    int64_t preroll = 0;

    int32_t blocksize[2] = {0, 0};
    VorbisModes modes;
    int32_t previous_blocksize = 0;

    // Granule position the next packet begins at. Not trusted until a page has
    // pinned it, which is what lets a seek land without knowing where it came from.
    int64_t next_granule = 0;
    bool anchored = false;
    // Audio a seek asked to skip past but that shares a page with what it wants.
    int64_t discard_until = 0;
  };

  struct Page {
    int64_t start = 0;
    int64_t end = 0;
    int64_t granule = -1;
    int serial = 0;
    bool eos = false;
  };

  // A page boundary decoding can restart at, and the granule position the audio
  // after it begins at.
  struct Landing {
    int64_t offset = 0;
    int64_t granule = 0;
  };

  ogg_sync_state sync_ = {};
  // libogg is handed &Stream::state by address, so the container must not
  // relocate its elements as further streams are discovered.
  std::deque<Stream> streams_;
  std::deque<Packet> buffered_packets_;
  size_t pending_headers_ = 0;
  // Offset of the next byte ogg_sync_pageout will hand back. libogg reports no
  // position of its own, and a seek cannot be built without one.
  int64_t sync_offset_ = 0;
  int64_t data_offset_ = 0;
  bool headers_done_ = false;
  // The track a seek expressed in microseconds is measured against.
  int32_t reference_track_ = -1;

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
    sync_offset_ = 0;
    data_offset_ = 0;
    headers_done_ = false;
    reference_track_ = -1;
    ogg_sync_reset(&sync_);
  }

  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    close();
    input_ = std::move(input);
    if (!input_ || !input_->isValid()) {
      return OM_IO_INVALID_STREAM;
    }

    while (!headers_done_) {
      if (!advance()) break;
    }
    if (tracks_.empty()) {
      return OM_FORMAT_PARSE_FAILED;
    }

    for (const Stream& stream : streams_) {
      if (stream.codec_id == OM_CODEC_NONE) continue;
      reference_track_ = stream.track_index;
      break;
    }

    if (input_->canSeek()) {
      probeDuration();
      // The probe leaves the read position at the end of the file. The header
      // packets are already queued for delivery, so only that position and
      // libogg's page assembly need putting back.
      for (Stream& stream : streams_) {
        ogg_stream_reset(&stream.state);
      }
      if (!reposition(data_offset_)) return OM_IO_SEEK_FAILED;
    }
    return OM_SUCCESS;
  }

  auto readPacket() -> Result<Packet, OMError> override {
    while (buffered_packets_.empty()) {
      if (!advance()) return Err(OM_FORMAT_END_OF_FILE);
    }
    Packet pkt = std::move(buffered_packets_.front());
    buffered_packets_.pop_front();
    return Ok(std::move(pkt));
  }

  auto seek(int32_t stream_idx, int64_t timestamp, SeekMode mode) -> OMError override {
    if (tracks_.empty()) return OM_COMMON_NOT_INITIALIZED;
    Stream* stream = trackStream(stream_idx >= 0 ? stream_idx : reference_track_);
    if (stream == nullptr) return OM_FORMAT_STREAM_NOT_FOUND;

    if (timestamp <= 0) {
      // Rewinding to the very start replays the codec headers, which is how a
      // decoder set up from the packet stream rather than from extradata gets
      // configured again.
      return rewindTo(0) ? OM_SUCCESS : OM_IO_SEEK_FAILED;
    }
    // A codec this demuxer cannot time gives no granule scale to measure a
    // target against, and so nothing a bisection could compare.
    if (stream->codec_id == OM_CODEC_NONE) return OM_FORMAT_STREAM_NOT_FOUND;
    if (!input_->canSeek()) return OM_IO_SEEK_FAILED;

    const Rational time_base = tracks_[stream->track_index].time_base;
    // Streams of one file need not share a time base, so the request travels
    // between them in microseconds.
    const int64_t target_us =
        stream_idx < 0 ? timestamp : granuleToMicroseconds(timestamp, time_base);
    const int64_t target = stream_idx < 0 ? microsecondsToGranule(timestamp, time_base) : timestamp;

    // Every Ogg audio packet depends on the one before it, so there is no sync
    // point to round to: all modes but NEXT_SYNC land on the page boundary at or
    // before the target, backed off by the codec's pre-roll.
    const int64_t search = mode == SeekMode::NEXT_SYNC
                               ? target
                               : std::max<int64_t>(0, target - stream->preroll);
    const auto [before, after] = bisect(stream->serial, search);
    const Landing& landing = mode == SeekMode::NEXT_SYNC && after ? *after : before;

    if (!rewindTo(landing.offset)) return OM_IO_SEEK_FAILED;
    for (Stream& other : streams_) {
      if (other.codec_id == OM_CODEC_NONE) continue;
      const int64_t own = microsecondsToGranule(target_us, tracks_[other.track_index].time_base);
      other.discard_until = std::max<int64_t>(0, own - other.preroll);
    }

    log(OM_CATEGORY_DEMUXER, OM_LEVEL_INFO,
        "Ogg: seek to {} on track {} resumes at granule {} (offset {})",
        target, stream->track_index, landing.granule, landing.offset);
    return OM_SUCCESS;
  }

private:
  // ---- page reading ----------------------------------------------------------

  auto fill() -> bool {
    char* buffer = ogg_sync_buffer(&sync_, static_cast<long>(READ_CHUNK));
    if (buffer == nullptr) return false;
    const size_t n = input_->read({reinterpret_cast<uint8_t*>(buffer), READ_CHUNK});
    if (n == 0) return false;
    ogg_sync_wrote(&sync_, static_cast<long>(n));
    return true;
  }

  auto reposition(int64_t offset) -> bool {
    if (!input_->seek(offset, Whence::BEG)) return false;
    ogg_sync_reset(&sync_);
    sync_offset_ = offset;
    return true;
  }

  // The next page, as long as it begins before `limit`. `page` stays valid only
  // until the following call.
  auto nextPage(ogg_page& page, int64_t limit, Page& info) -> bool {
    for (;;) {
      const int ret = ogg_sync_pageout(&sync_, &page);
      if (ret < 0) {
        // A hole, which libogg resynchronises past on its own; the bytes it
        // threw away still have to be counted to keep the offsets exact.
        sync_offset_ += -ret;
        continue;
      }
      if (ret == 0) {
        if (sync_offset_ >= limit || !fill()) return false;
        continue;
      }
      info.start = sync_offset_;
      info.end = sync_offset_ + page.header_len + page.body_len;
      info.granule = ogg_page_granulepos(&page);
      info.serial = ogg_page_serialno(&page);
      info.eos = ogg_page_eos(&page) != 0;
      sync_offset_ = info.end;
      return info.start < limit;
    }
  }

  auto advance() -> bool {
    ogg_page page;
    Page info;
    if (!nextPage(page, INT64_MAX, info)) return false;

    // A logical stream is introduced by a beginning-of-stream page, and the spec
    // groups all of them at the head of the file. A serial that turns up without
    // one belongs to a chained stream, which this demuxer does not follow.
    const bool bos = ogg_page_bos(&page) != 0;
    Stream* stream = bos && !headers_done_ ? &streamFor(info.serial) : findStream(info.serial);
    if (stream == nullptr) return true;

    ogg_stream_pagein(&stream->state, &page);
    if (!stream->header_complete) {
      processHeaders(*stream);
    }
    // Every stream has its headers and the run of beginning-of-stream pages is
    // behind us, so the header region of the file ends with this page. Waiting
    // for that run to end matters when the first stream is a codec whose headers
    // this demuxer cannot count, such as a video track alongside the audio.
    if (!headers_done_ && !bos && pending_headers_ == 0) {
      headers_done_ = true;
      data_offset_ = info.end;
    }
    if (stream->header_complete) {
      drainPage(*stream, info);
    }
    return true;
  }

  // ---- streams ---------------------------------------------------------------

  auto findStream(int serial) -> Stream* {
    for (Stream& stream : streams_) {
      if (stream.serial == serial) return &stream;
    }
    return nullptr;
  }

  auto trackStream(int32_t track_index) -> Stream* {
    for (Stream& stream : streams_) {
      if (stream.track_index == track_index) return &stream;
    }
    return nullptr;
  }

  auto streamFor(int serial) -> Stream& {
    if (Stream* existing = findStream(serial)) return *existing;

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

  // ---- headers ---------------------------------------------------------------

  auto parseIdentificationHeader(const ogg_packet& op, Stream& stream, Track& track) -> bool {
    if (op.bytes >= 30 && memcmp(op.packet + 1, "vorbis", 6) == 0) {
      stream.codec_id = track.format.codec_id = OM_CODEC_VORBIS;
      track.format.type = OM_MEDIA_AUDIO;
      track.format.audio.channels = op.packet[11];
      track.format.audio.sample_rate = load_u32_le(op.packet + 12);
      track.bitrate = load_u32_le(op.packet + 20);
      track.time_base = {1, static_cast<int32_t>(track.format.audio.sample_rate)};
      stream.blocksize[0] = 1 << (op.packet[28] & 0xF);
      stream.blocksize[1] = 1 << (op.packet[28] >> 4);
      // One long block back is enough for the decoder to hold a lapped pair of
      // blocks by the time it reaches the seek target.
      stream.preroll = stream.blocksize[1];
      return true;
    }
    if (op.bytes >= 19 && memcmp(op.packet, "OpusHead", 8) == 0) {
      stream.codec_id = track.format.codec_id = OM_CODEC_OPUS;
      track.format.type = OM_MEDIA_AUDIO;
      track.format.audio.channels = op.packet[9];
      track.format.audio.sample_rate = load_u32_le(op.packet + 12);
      // Opus granule positions count 48 kHz samples whatever the input rate.
      track.time_base = {1, 48000};
      stream.preroll = load_u16_le(op.packet + 10) + OPUS_PREROLL;
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

  // The decoders take the headers as ordinary packets, so each one is queued for
  // delivery as well as parsed here.
  void processHeaders(Stream& stream) {
    Track& track = tracks_[stream.track_index];

    ogg_packet op;
    while (!stream.header_complete && ogg_stream_packetout(&stream.state, &op) == 1) {
      const int32_t count = stream.header_packets++;
      Packet& pkt = buffered_packets_.emplace_back(makePacket(op, stream.track_index, 0));
      pkt.pts = pkt.dts = 0;

      if (count == 0) {
        if (!parseIdentificationHeader(op, stream, track)) markHeadersComplete(stream);
      } else if (count == 1) {
        parseCommentHeader(op, track);
        if (stream.codec_id == OM_CODEC_OPUS) markHeadersComplete(stream);
      } else {
        if (stream.codec_id == OM_CODEC_VORBIS) {
          stream.modes = parseVorbisModes({op.packet, static_cast<size_t>(op.bytes)})
                             .value_or(VorbisModes {});
        }
        markHeadersComplete(stream);
      }
    }
  }

  // ---- timing ----------------------------------------------------------------

  auto vorbisPacketSamples(Stream& stream, std::span<const uint8_t> bytes) -> int64_t {
    if (bytes.empty() || (bytes[0] & 1) != 0 || stream.modes.blockflag.empty()) return 0;

    const uint32_t mode = (bytes[0] >> 1) & ((1u << stream.modes.index_bits) - 1u);
    if (mode >= stream.modes.blockflag.size()) return 0;

    const int32_t blocksize = stream.blocksize[stream.modes.blockflag[mode]];
    // Vorbis I section 1.3.2: a packet renders the overlap of its own block with
    // the previous one, so the first packet of a stream returns no audio at all -
    // which is also what the decoder does after a reset or a seek.
    const int64_t samples =
        stream.previous_blocksize != 0 ? (stream.previous_blocksize + blocksize) / 4 : 0;
    stream.previous_blocksize = blocksize;
    return samples;
  }

  auto packetSamples(Stream& stream, std::span<const uint8_t> bytes) -> int64_t {
    switch (stream.codec_id) {
      case OM_CODEC_OPUS: return opusPacketSamples(bytes);
      case OM_CODEC_VORBIS: return vorbisPacketSamples(stream, bytes);
      default: return 0;
    }
  }

  // A page's granule position is the one authority on where its packets sit: it
  // ends the last of them, so the rest are timed back from it. Re-basing on every
  // page is also what lets a seek land with exact timestamps without having to
  // know what the position was before it.
  void drainPage(Stream& stream, const Page& info) {
    std::vector<Packet> packets;
    int64_t total = 0;

    ogg_packet op;
    for (int ret; (ret = ogg_stream_packetout(&stream.state, &op)) != 0;) {
      if (ret < 0) continue; // a hole, which libogg already resynchronised past
      const std::span<const uint8_t> bytes {op.packet, static_cast<size_t>(op.bytes)};
      const int64_t duration = packetSamples(stream, bytes);
      packets.push_back(makePacket(op, stream.track_index, duration));
      total += duration;
    }
    if (packets.empty()) return;

    // An encoder that starts a stream partway into its first packet says so with
    // a first granule position short of the audio the page carries. Those samples
    // are not played, so the shortfall comes off the packets that hold them
    // rather than shifting everything behind them.
    int64_t shortfall = stream.anchored || info.granule < 0
                            ? 0
                            : std::max<int64_t>(0, total - info.granule);
    int64_t cursor = stream.anchored ? stream.next_granule
                                     : std::max<int64_t>(0, info.granule - total);
    for (Packet& pkt : packets) {
      const int64_t trim = std::min(shortfall, pkt.duration);
      pkt.duration -= trim;
      shortfall -= trim;
      pkt.pts = pkt.dts = cursor;
      cursor += pkt.duration;
    }

    if (info.granule < 0) {
      stream.next_granule = cursor;
    } else {
      // A stream that ends mid-packet says so with a final granule position short
      // of the audio its last page carries; the surplus is not to be played.
      if (info.eos && cursor > info.granule) {
        Packet& last = packets.back();
        last.duration = std::max<int64_t>(0, info.granule - last.pts);
      }
      stream.next_granule = info.granule;
      stream.anchored = true;
    }

    // A page can carry a second of audio, so a seek that stopped at its boundary
    // would hand back far more than the pre-roll. Now that the timestamps are
    // known, the part ahead of the target is dropped rather than delivered.
    auto begin = packets.begin();
    if (stream.discard_until > 0) {
      begin = std::ranges::find_if(packets, [&](const Packet& pkt) {
        return pkt.pts + pkt.duration > stream.discard_until;
      });
      if (begin != packets.end()) stream.discard_until = 0;
    }
    buffered_packets_.insert(buffered_packets_.end(), std::make_move_iterator(begin),
                             std::make_move_iterator(packets.end()));
  }

  // ---- seeking ---------------------------------------------------------------

  // Both conversions split at the second boundary so that a long file cannot
  // overflow the product.
  static auto microsecondsToGranule(int64_t us, Rational time_base) -> int64_t {
    const int64_t rate = granuleRate(time_base);
    return (us / 1000000) * rate + ((us % 1000000) * rate) / 1000000;
  }

  static auto granuleToMicroseconds(int64_t granule, Rational time_base) -> int64_t {
    const int64_t rate = granuleRate(time_base);
    return (granule / rate) * 1000000 + ((granule % rate) * 1000000) / rate;
  }

  static auto granuleRate(Rational time_base) -> int64_t {
    // Granule positions count samples, which is what every Ogg audio time base
    // denominates; 48 kHz, the Opus rate, stands in when there is no usable one.
    const int64_t rate = time_base.num > 0 ? time_base.den / time_base.num : 0;
    return rate > 0 ? rate : 48000;
  }

  auto rewindTo(int64_t offset) -> bool {
    buffered_packets_.clear();
    for (Stream& stream : streams_) {
      ogg_stream_reset(&stream.state);
      stream.previous_blocksize = 0;
      stream.next_granule = 0;
      stream.anchored = false;
      stream.discard_until = 0;
    }
    return reposition(offset);
  }

  // The last page of a logical stream carries its total sample count, so the
  // duration costs a read at the end of the file rather than a full scan.
  void probeDuration() {
    const int64_t size = input_->size();
    if (size <= data_offset_) return;

    for (int64_t window = TAIL_WINDOW;; window *= TAIL_GROWTH) {
      const int64_t offset = std::max(data_offset_, size - window);
      if (!reposition(offset)) return;

      bool found = false;
      ogg_page page;
      Page info;
      while (nextPage(page, size, info)) {
        Stream* stream = findStream(info.serial);
        if (stream == nullptr || info.granule < 0) continue;
        Track& track = tracks_[stream->track_index];
        track.start_time = 0;
        // Opus granule positions include the pre-skip samples and the decoder
        // returns them, so the timeline keeps them rather than trimming here.
        track.duration = info.granule;
        found = true;
      }
      if (found || offset <= data_offset_) return;
    }
  }

  // The first page at or after `offset` that times `serial`, which is the only
  // kind a bisection has anything to compare against a target.
  auto probePage(int64_t offset, int64_t limit, int serial, Page& out) -> bool {
    if (!reposition(offset)) return false;
    ogg_page page;
    while (nextPage(page, limit, out)) {
      if (out.serial == serial && out.granule >= 0) return true;
    }
    return false;
  }

  // Narrows the file down to the page boundaries bracketing `search`: the last
  // place decoding can start at or before it, and the first one past it. Each
  // round either raises the lower bound past the page it just read or drops the
  // upper bound to that page's start, so the range always shrinks.
  auto bisect(int serial, int64_t search) -> std::pair<Landing, std::optional<Landing>> {
    Landing before {data_offset_, 0};
    std::optional<Landing> after;

    int64_t lo = data_offset_;
    int64_t hi = input_->size();
    while (lo < hi) {
      const int64_t guess = lo + (hi - lo) / 2;
      Page page;
      if (!probePage(guess, hi, serial, page)) {
        hi = guess;
      } else if (page.granule <= search) {
        before = {page.end, page.granule};
        lo = page.end;
      } else {
        after = Landing {page.end, page.granule};
        hi = page.start;
      }
    }
    return {before, after};
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
