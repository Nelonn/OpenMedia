#include <mpegts.hpp>

#include <algorithm>
#include <cstring>
#include <deque>
#include <numeric>
#include <openmedia/codec_defs.h>
#include <openmedia/log.hpp>
#include <util/bit_reader.hpp>
#include <util/timescale.hpp>
#include <video/parser/h264_types.hpp>
#include <video/parser/h265_parser.hpp>

namespace openmedia {
namespace {

constexpr size_t TS_PACKET_SIZE = 188;
constexpr uint8_t TS_SYNC_BYTE = 0x47;

// Every timestamp in a transport stream is on the 90 kHz system clock, and that is
// the only timescale this reader ever reports.
constexpr uint32_t TS_TIMESCALE = 90000;

// PTS and DTS are 33 bits wide, so they wrap every 26.5 hours -- which live streams
// do reach, and a stream may be published with its clock already near the end.
constexpr int64_t TS_TIMESTAMP_MODULUS = INT64_C(1) << 33;

constexpr uint16_t PAT_PID = 0x0000;
constexpr uint16_t NULL_PID = 0x1FFF;

// Consecutive segments of one encode meet on the wire's clock to within a frame, and
// both figures compared here are read off that clock, so a step of a whole second is
// not drift but a discontinuity, a clock reset or wrap, or a skipped segment. There,
// timestamps are re-based onto the playlist's timeline -- the only thing both sides
// still agree on.
constexpr int64_t DISCONTINUITY_TICKS = TS_TIMESCALE;

// Every AAC frame is 1024 samples, which is what lets the frames of one PES packet be
// timed from the single timestamp in front of them.
constexpr int64_t AAC_FRAME_SAMPLES = 1024;

// ISO/IEC 13818-1 Table 2-34, the entries an HLS segment actually carries.
enum StreamType : uint8_t {
  STREAM_TYPE_H264 = 0x1B,
  STREAM_TYPE_HEVC = 0x24,
  STREAM_TYPE_AAC_ADTS = 0x0F,
};

constexpr uint32_t AAC_SAMPLE_RATES[] = {96000, 88200, 64000, 48000, 44100, 32000,
                                        24000, 22050, 16000, 12000, 11025, 8000, 7350};
constexpr uint8_t AAC_CHANNELS[] = {0, 1, 2, 3, 4, 5, 6, 8};

// A 33-bit timestamp, spread over five bytes around the marker bits.
auto readTimestamp(const uint8_t* bytes) -> int64_t {
  return (static_cast<int64_t>(bytes[0] & 0x0E) << 29) | (static_cast<int64_t>(bytes[1]) << 22) |
         (static_cast<int64_t>(bytes[2] & 0xFE) << 14) | (static_cast<int64_t>(bytes[3]) << 7) |
         (static_cast<int64_t>(bytes[4]) >> 1);
}

// Each Annex-B NAL unit of `bytes`, start code excluded. The zero byte of a four-byte
// start code is left on the end of the NAL before it, which no reader of a NAL header
// or an RBSP minds.
template <typename Fn>
void forEachNal(std::span<const uint8_t> bytes, Fn&& fn) {
  size_t start = 0;
  bool open = false;
  size_t i = 0;
  while (i + 2 < bytes.size()) {
    if (bytes[i] != 0 || bytes[i + 1] != 0 || bytes[i + 2] != 1) {
      ++i;
      continue;
    }
    if (open && i > start) fn(bytes.subspan(start, i - start));
    i += 3;
    start = i;
    open = true;
  }
  if (open && start < bytes.size()) fn(bytes.subspan(start));
}

void appendAnnexB(std::vector<uint8_t>& out, std::span<const uint8_t> nal) {
  static constexpr uint8_t START_CODE[4] = {0x00, 0x00, 0x00, 0x01};
  out.insert(out.end(), START_CODE, START_CODE + 4);
  out.insert(out.end(), nal.begin(), nal.end());
}

auto h264NalType(std::span<const uint8_t> nal) -> uint8_t {
  return nal.empty() ? 0 : static_cast<uint8_t>(nal[0] & 0x1F);
}

auto hevcNalType(std::span<const uint8_t> nal) -> uint8_t {
  return nal.empty() ? 0 : static_cast<uint8_t>((nal[0] >> 1) & 0x3F);
}

// 7.4.2.2: the picture is an IRAP picture, so the stream can be entered here.
auto isHevcIrap(uint8_t nal_type) -> bool { return nal_type >= 16 && nal_type <= 21; }

auto reduced(int64_t numerator, int64_t denominator) -> Rational {
  const int64_t divisor = std::gcd(numerator, denominator);
  if (divisor <= 0) return {};
  return {static_cast<int32_t>(numerator / divisor), static_cast<int32_t>(denominator / divisor)};
}

// PSI sections, which start at a pointer-delimited offset of the packet carrying the
// payload-unit-start flag and may run over several packets after it.
class SectionAssembler {
  std::vector<uint8_t> data_;
  size_t expected_ = 0;

public:
  void reset() {
    data_.clear();
    expected_ = 0;
  }

  // The section, once it is complete; nothing while it is still being assembled.
  auto feed(std::span<const uint8_t> payload, bool start) -> std::span<const uint8_t> {
    if (start) {
      if (payload.empty()) return {};
      const size_t pointer = payload[0];
      if (1 + pointer > payload.size()) return {};
      data_.assign(payload.begin() + 1 + pointer, payload.end());
      expected_ = 0;
    } else if (data_.empty()) {
      return {}; // the head of this section was never seen
    } else {
      data_.insert(data_.end(), payload.begin(), payload.end());
    }

    if (expected_ == 0) {
      if (data_.size() < 3) return {};
      // section_length counts the bytes after itself, including the CRC.
      expected_ = 3 + (((static_cast<size_t>(data_[1]) & 0x0F) << 8) | data_[2]);
    }
    if (data_.size() < expected_) return {};
    return std::span<const uint8_t>(data_.data(), expected_);
  }
};

// One elementary stream, its PES assembly state, and what the bitstream has said
// about the track it feeds.
struct ElementaryStream {
  uint16_t pid = NULL_PID;
  uint8_t stream_type = 0;
  int32_t track = -1;
  OMCodecId codec = OM_CODEC_NONE;
  OMMediaType type = OM_MEDIA_NONE;

  std::vector<uint8_t> pes;
  // PES_packet_length, or 0 for the unbounded form a video stream uses, which ends
  // only where the next packet or the segment does.
  size_t declared = 0;
  bool collecting = false;
  int32_t continuity = -1;

  // Set from the first frame and then left alone, since the demuxer publishes the
  // track list once.
  bool described = false;
  uint32_t sample_rate = 0;
  // Where the next audio frame falls when its PES packet carries no timestamp, which
  // is allowed as long as one came before.
  int64_t audio_cursor = -1;

  // Parsed but not readable yet, waiting on two things: a video frame lasts until the
  // frame after it, and no stream's packet can go before the others have one to
  // compare against -- or a segment read as it arrives hands out video ahead of the
  // audio belonging in front of it.
  std::deque<Packet> staged;
  size_t timed = 0;       // leading entries of `staged` whose duration is settled
  int64_t video_step = 0; // the last frame interval seen, for the frame ending a segment
};

class MpegTsReader final : public SegmentReader {
  std::vector<Track> tracks_;
  Dictionary metadata_;

  uint16_t pmt_pid_ = NULL_PID;
  SectionAssembler pat_;
  SectionAssembler pmt_;
  std::vector<ElementaryStream> streams_;

  // Ready to be read, in decode order. A segment arriving in pieces adds to this as
  // it goes, so it is drained from the front rather than indexed.
  std::deque<Packet> packets_;
  // How many this segment handed over. A segment read as it arrived has had its
  // packets taken already, so an empty queue at its end says nothing.
  size_t released_ = 0;

  // The playlist's timeline against the stream's own clock, which started wherever
  // the encoder's stood -- so the offset between them is the playlist's to supply.
  int64_t expected_start_ns_ = 0;
  int64_t base_ = 0;
  bool have_base_ = false;
  // Where the previous segment ended on the wire's clock, for telling drift from a
  // discontinuity, and the same figure accumulating for the segment in hand.
  int64_t previous_end_raw_ = 0;
  int64_t segment_end_raw_ = 0;
  // Whether this segment's packets are anchored onto the playlist's timeline, which
  // needs its earliest decode time -- not the first one parsed.
  bool base_settled_ = false;

  // The furthest the program's clock has been unwrapped to, over every stream of it.
  //
  // PTS and DTS of one program all count on the same 33-bit system clock, so its wrap
  // belongs to the program and not to any one elementary stream. Each stream crosses
  // it at its own moment -- video running a second ahead of the audio covering the
  // same instant is ordinary -- and unwrapping them against counters of their own put
  // the two a whole modulus apart. The segment's anchor is then taken as the earliest
  // of them, so one stream on the far side of the wrap moved every packet of the
  // other by 26.5 hours.
  int64_t clock_reference_ = INT64_MIN;

  // Whether a segment is open, and the tail of the last piece that did not add up to
  // a whole transport packet.
  bool segment_open_ = false;
  std::vector<uint8_t> carry_;

  RbspBuffer rbsp_;

public:
  auto openInit(std::span<const uint8_t> init) -> OMError override {
    // A transport stream has no initialization section, so a playlist giving one is
    // describing fragmented MP4 and these bytes are not it.
    return init.empty() ? OM_SUCCESS : OM_FORMAT_NOT_SUPPORTED;
  }

  auto tracks() const -> const std::vector<Track>& override { return tracks_; }
  auto metadata() const -> const Dictionary& override { return metadata_; }

  auto tracksDescribed() const -> bool override {
    if (tracks_.empty()) return false;
    // A program map names only a PID and a stream type. What a decoder needs --
    // parameter sets, picture size, AudioSpecificConfig -- is in the elementary
    // stream, so a track is described only once a frame of it has been read.
    for (const ElementaryStream& stream : streams_) {
      if (stream.track >= 0 && !stream.described) return false;
    }
    return true;
  }

  void expectSegmentStart(int64_t start_ns, bool discontinuity) override {
    expected_start_ns_ = start_ns;
    // A declared discontinuity is the playlist saying outright what settleBase()
    // would otherwise have to infer from a jump in the timestamps: nothing this
    // reader worked out about the clock carries into the segment coming.
    if (discontinuity) have_base_ = false;
  }

  auto pushSegment(std::vector<uint8_t> segment) -> OMError override {
    if (segment.empty()) return OM_COMMON_INVALID_ARGUMENT;
    if (!looksLikeMpegTs(segment)) return OM_FORMAT_INVALID_HEADER;

    startSegment();
    const std::span<const uint8_t> bytes(segment);
    size_t at = 0;
    while (at + TS_PACKET_SIZE <= bytes.size()) {
      if (bytes[at] != TS_SYNC_BYTE) {
        // Sync is lost, and the stride has to be found again: guessing it wrong
        // reads payload as packet headers, which is how a demuxer invents streams.
        at = findSync(bytes, at);
        continue;
      }
      handlePacket(bytes.subspan(at, TS_PACKET_SIZE));
      at += TS_PACKET_SIZE;
    }
    return finishSegment();
  }

  // --- A segment that is still arriving ---------------------------------------

  auto beginSegment() -> OMError override {
    startSegment();
    segment_open_ = true;
    return OM_SUCCESS;
  }

  auto feedSegment(std::span<const uint8_t> bytes) -> OMError override {
    if (!segment_open_) return OM_COMMON_NOT_INITIALIZED;
    carry_.insert(carry_.end(), bytes.begin(), bytes.end());
    consumeWholePackets(/*final=*/false);
    releaseStaged(/*final=*/false);
    return OM_SUCCESS;
  }

  auto endSegment() -> OMError override {
    if (!segment_open_) return OM_COMMON_NOT_INITIALIZED;
    consumeWholePackets(/*final=*/true);
    segment_open_ = false;
    carry_.clear();
    return finishSegment();
  }

  auto segmentOpen() const -> bool override { return segment_open_; }

  auto needsSegment() const -> bool override { return packets_.empty(); }

  auto readPacket() -> Result<Packet, OMError> override {
    if (packets_.empty()) return Err(OM_IO_NOT_ENOUGH_DATA);
    Packet packet = std::move(packets_.front());
    packets_.pop_front();
    return Ok(std::move(packet));
  }

  auto nextDecodeTimeNs() const -> int64_t override {
    if (packets_.empty()) return INT64_MAX;
    return scaleToNs(packets_.front().dts, TS_TIMESCALE);
  }

  auto remainingDurationNs() const -> int64_t override {
    if (packets_.empty()) return 0;
    const Packet& last = packets_.back();
    const int64_t end = scaleToNs(last.dts + last.duration, TS_TIMESCALE);
    const int64_t start = scaleToNs(packets_.front().dts, TS_TIMESCALE);
    return end > start ? end - start : 0;
  }

  auto timescaleOf(int32_t) const -> uint32_t override { return TS_TIMESCALE; }

  void flushSegments() override {
    startSegment();
    segment_open_ = false;
    carry_.clear();
    // What was half-assembled belonged to a position this reader has left, and the
    // clock at the new one is whatever the next segment's expectSegmentStart() says.
    for (ElementaryStream& stream : streams_) {
      stream.continuity = -1;
      stream.audio_cursor = -1;
    }
    clock_reference_ = INT64_MIN;
    pat_.reset();
    pmt_.reset();
    have_base_ = false;
  }

private:
  static auto findSync(std::span<const uint8_t> bytes, size_t from) -> size_t {
    for (size_t at = from; at < bytes.size(); ++at) {
      if (bytes[at] != TS_SYNC_BYTE) continue;
      const size_t next = at + TS_PACKET_SIZE;
      if (next >= bytes.size() || bytes[next] == TS_SYNC_BYTE) return at;
    }
    return bytes.size();
  }

  // Resets what belongs to a segment rather than the stream. What survives one --
  // program tables, track list, wrap count, clock offset -- is not touched here.
  void startSegment() {
    packets_.clear();
    released_ = 0;
    base_settled_ = false;
    carry_.clear();
    for (ElementaryStream& stream : streams_) {
      stream.staged.clear();
      stream.timed = 0;
      stream.video_step = 0;
      // A PES packet lies entirely inside one segment, so anything half-assembled
      // was cut off by the end of the last.
      stream.pes.clear();
      stream.collecting = false;
      stream.declared = 0;
    }
  }

  auto finishSegment() -> OMError {
    // Every segment is entered on its own, so a PES packet still open at the end of
    // one is as complete as it is going to get.
    for (ElementaryStream& stream : streams_) emitPes(stream);
    releaseStaged(/*final=*/true);
    previous_end_raw_ = segment_end_raw_;

    // No track means no program map was seen; no packet means nothing of this
    // reader's program was in these bytes. Either way the caller reads on.
    if (tracks_.empty() || released_ == 0) return OM_FORMAT_NO_STREAMS;
    return OM_SUCCESS;
  }

  // Every whole transport packet in `carry_`, keeping the rest for the next piece.
  void consumeWholePackets(bool final) {
    const std::span<const uint8_t> bytes(carry_);
    size_t at = 0;
    while (at + TS_PACKET_SIZE <= bytes.size()) {
      if (bytes[at] != TS_SYNC_BYTE) {
        const size_t sync = findSync(bytes, at);
        // findSync confirms a boundary by the sync byte a packet further on, and
        // past the end of what arrived it has to guess. Mid-segment it need not:
        // the confirming bytes are coming, so the candidate waits for them.
        at = sync;
        if (!final && sync + 2 * TS_PACKET_SIZE > bytes.size()) break;
        continue;
      }
      handlePacket(bytes.subspan(at, TS_PACKET_SIZE));
      at += TS_PACKET_SIZE;
    }
    carry_.erase(carry_.begin(), carry_.begin() + static_cast<ptrdiff_t>(std::min(at, carry_.size())));
  }

  auto streamOf(uint16_t pid) -> ElementaryStream* {
    for (ElementaryStream& stream : streams_) {
      if (stream.pid == pid) return &stream;
    }
    return nullptr;
  }

  void handlePacket(std::span<const uint8_t> packet) {
    if ((packet[1] & 0x80) != 0) return; // transport_error_indicator: the payload is junk
    const uint16_t pid = static_cast<uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
    if (pid == NULL_PID) return;

    // Scrambled at the transport layer, and parsing it anyway would read ciphertext
    // as PES headers.
    if ((packet[3] & 0xC0) != 0) return;

    const bool start = (packet[1] & 0x40) != 0;
    const uint8_t adaptation = (packet[3] >> 4) & 0x03;
    const uint8_t continuity = packet[3] & 0x0F;

    size_t payload_at = 4;
    if ((adaptation & 0x02) != 0) {
      payload_at = 5 + static_cast<size_t>(packet[4]);
      if (payload_at > packet.size()) return;
    }
    if ((adaptation & 0x01) == 0) return; // adaptation field only: a PCR, or padding
    const std::span<const uint8_t> payload = packet.subspan(payload_at);
    if (payload.empty()) return;

    if (pid == PAT_PID) {
      if (const auto section = pat_.feed(payload, start); !section.empty()) parsePat(section);
      return;
    }
    if (pid == pmt_pid_) {
      if (const auto section = pmt_.feed(payload, start); !section.empty()) parsePmt(section);
      return;
    }

    ElementaryStream* stream = streamOf(pid);
    if (!stream || stream->track < 0) return; // not ours, or of a kind not read

    // A break in the continuity counter means the PES packet being assembled has a
    // hole in it. Dropping it loses one frame; keeping it feeds a decoder a frame
    // missing its middle.
    const int32_t expected = stream->continuity < 0 ? continuity : (stream->continuity + 1) & 0x0F;
    stream->continuity = continuity;
    if (continuity != expected && stream->collecting) {
      stream->pes.clear();
      stream->collecting = false;
    }

    if (start) {
      emitPes(*stream);
      stream->collecting = true;
      stream->declared = 0;
      stream->pes.assign(payload.begin(), payload.end());
      if (stream->pes.size() >= 6) {
        const size_t length = (static_cast<size_t>(stream->pes[4]) << 8) | stream->pes[5];
        if (length != 0) stream->declared = 6 + length;
      }
    } else if (stream->collecting) {
      stream->pes.insert(stream->pes.end(), payload.begin(), payload.end());
    }

    if (stream->declared != 0 && stream->pes.size() >= stream->declared) {
      stream->pes.resize(stream->declared);
      emitPes(*stream);
    }
  }

  void parsePat(std::span<const uint8_t> section) {
    if (section.size() < 12 || section[0] != 0x00) return;
    const size_t end = section.size() - 4; // less the CRC
    for (size_t at = 8; at + 4 <= end; at += 4) {
      const uint16_t program = static_cast<uint16_t>((section[at] << 8) | section[at + 1]);
      if (program == 0) continue; // the network information table, not a program
      // The first program, and in a playlist's segment there is only ever one.
      pmt_pid_ = static_cast<uint16_t>(((section[at + 2] & 0x1F) << 8) | section[at + 3]);
      return;
    }
  }

  void parsePmt(std::span<const uint8_t> section) {
    if (section.size() < 16 || section[0] != 0x02) return;
    // The tracks are published once and cannot be renumbered afterwards, so the
    // first program map settles them; the copy in every later segment repeats it.
    if (!streams_.empty()) return;

    const size_t end = section.size() - 4;
    const size_t program_info = ((static_cast<size_t>(section[10]) & 0x0F) << 8) | section[11];
    size_t at = 12 + program_info;
    while (at + 5 <= end) {
      const uint8_t stream_type = section[at];
      const uint16_t pid = static_cast<uint16_t>(((section[at + 1] & 0x1F) << 8) | section[at + 2]);
      const size_t info_length =
          ((static_cast<size_t>(section[at + 3]) & 0x0F) << 8) | section[at + 4];
      at += 5 + info_length;
      addStream(stream_type, pid);
    }
  }

  void addStream(uint8_t stream_type, uint16_t pid) {
    ElementaryStream stream;
    stream.pid = pid;
    stream.stream_type = stream_type;

    switch (stream_type) {
      case STREAM_TYPE_H264:
        stream.codec = OM_CODEC_H264;
        stream.type = OM_MEDIA_VIDEO;
        break;
      case STREAM_TYPE_HEVC:
        stream.codec = OM_CODEC_HEVC;
        stream.type = OM_MEDIA_VIDEO;
        break;
      case STREAM_TYPE_AAC_ADTS:
        stream.codec = OM_CODEC_AAC;
        stream.type = OM_MEDIA_AUDIO;
        break;
      default:
        // Known of, but each needs framing of its own to be timed: MPEG audio and
        // AC-3 arrive as a run of frames under one timestamp, to be split the way
        // the AAC path below does. The PID is remembered so its packets are skipped
        // rather than resynchronised against.
        log(OM_CATEGORY_DEMUXER, OM_LEVEL_INFO,
            "MPEG-TS: stream type 0x{:02X} on PID {} is not read", stream_type, pid);
        streams_.push_back(std::move(stream));
        return;
    }

    Track track;
    track.index = static_cast<int32_t>(tracks_.size());
    track.id = static_cast<int32_t>(pid);
    track.format.type = stream.type;
    track.format.codec_id = stream.codec;
    track.time_base = {1, static_cast<int32_t>(TS_TIMESCALE)};
    stream.track = track.index;
    tracks_.push_back(std::move(track));
    streams_.push_back(std::move(stream));
  }

  // --- PES ------------------------------------------------------------------

  void emitPes(ElementaryStream& stream) {
    std::vector<uint8_t> pes = std::move(stream.pes);
    stream.pes.clear();
    stream.collecting = false;
    stream.declared = 0;
    if (pes.size() < 9 || stream.track < 0) return;
    if (pes[0] != 0x00 || pes[1] != 0x00 || pes[2] != 0x01) return;

    // Padding and private-2 streams carry no PES header flags, so the bytes a
    // header would occupy are payload; neither is media.
    const uint8_t stream_id = pes[3];
    if (stream_id == 0xBE || stream_id == 0xBF) return;

    const uint8_t flags = pes[7];
    const size_t header_length = pes[8];
    int64_t pts = -1;
    int64_t dts = -1;
    if ((flags & 0xC0) == 0xC0 && pes.size() >= 19) {
      pts = readTimestamp(&pes[9]);
      dts = readTimestamp(&pes[14]);
    } else if ((flags & 0x80) != 0 && pes.size() >= 14) {
      pts = readTimestamp(&pes[9]);
      dts = pts;
    }

    const size_t payload_at = 9 + header_length;
    if (payload_at >= pes.size()) return;
    const std::span<const uint8_t> payload(pes.data() + payload_at, pes.size() - payload_at);

    // Only decode times move the program's clock on: a presentation time runs ahead
    // of the decode time and back again whenever the stream has B-frames, so counting
    // wraps off that would invent one. The presentation time is then placed against
    // its own decode time, already unwrapped, which is the nearest point on the real
    // timeline it has.
    if (dts >= 0) {
      const int64_t raw_dts = dts;
      dts = unwrapClock(dts);
      pts = unwrapAgainst(pts, raw_dts, dts);
    }

    if (stream.type == OM_MEDIA_VIDEO) {
      emitVideoFrame(stream, payload, pts, dts);
    } else if (stream.codec == OM_CODEC_AAC) {
      emitAdtsFrames(stream, payload, dts);
    }
  }

  // A decode time on the timeline the program's clock would have had if its counter
  // were wider: the epoch that brings it nearest to everything already read.
  //
  // Nearest in both directions, not merely counting upwards. A step forward of more
  // than half the range is the counter wrapping rather than the clock leaping ahead;
  // a step back of more than half is a stream that has not crossed the wrap yet, or
  // two consecutive segments of one encode overlapping by a frame -- both of which
  // happen, and neither of which is time running backwards.
  auto unwrapClock(int64_t raw) -> int64_t {
    if (raw < 0) return -1;
    if (clock_reference_ == INT64_MIN) {
      clock_reference_ = raw;
      return raw;
    }
    const int64_t away = clock_reference_ - raw;
    // Rounded division, so the epoch chosen is the nearest rather than the one below.
    const int64_t epochs = away >= 0
                               ? (away + TS_TIMESTAMP_MODULUS / 2) / TS_TIMESTAMP_MODULUS
                               : -((-away + TS_TIMESTAMP_MODULUS / 2) / TS_TIMESTAMP_MODULUS);
    const int64_t out = raw + epochs * TS_TIMESTAMP_MODULUS;
    // The frontier, not the last value seen: a stream lagging the others must not
    // drag it back, or the next wrap is counted twice.
    clock_reference_ = std::max(clock_reference_, out);
    return out;
  }

  // One access unit's presentation time, put on the same unwrapped timeline as its
  // own decode time. The two are within a reorder delay of each other, so whichever
  // of the three candidate epochs lands nearest the decode time is the right one --
  // and an access unit straddling the wrap, with one of the pair past it and the
  // other not, comes out right without touching the wrap count.
  static auto unwrapAgainst(int64_t raw_pts, int64_t raw_dts, int64_t dts) -> int64_t {
    if (raw_pts < 0) return -1;
    const int64_t base = dts - raw_dts; // whole modulus multiples already applied
    int64_t best = raw_pts + base;
    for (const int64_t shift : {-TS_TIMESTAMP_MODULUS, TS_TIMESTAMP_MODULUS}) {
      const int64_t candidate = raw_pts + base + shift;
      if (std::abs(candidate - dts) < std::abs(best - dts)) best = candidate;
    }
    return best;
  }

  void emitVideoFrame(ElementaryStream& stream, std::span<const uint8_t> payload, int64_t pts,
                      int64_t dts) {
    if (dts < 0) return; // nothing places it in time, and nothing can be done with it

    const bool keyframe = describeVideo(stream, payload);

    Packet packet;
    // A video PES packet is one access unit, and its payload is already the Annex-B
    // byte stream a decoder takes. It is copied out because the bytes of one frame
    // arrive spread over as many transport packets as it took to carry them.
    packet.allocate(payload.size());
    std::memcpy(packet.bytes.data(), payload.data(), payload.size());
    packet.stream_index = stream.track;
    packet.pts = pts >= 0 ? pts : dts;
    packet.dts = dts;
    packet.is_keyframe = keyframe;
    stream.staged.push_back(std::move(packet));
  }

  // Fills in what only the bitstream knows -- parameter sets, picture size -- and
  // reports whether this access unit can be decoded from cold.
  auto describeVideo(ElementaryStream& stream, std::span<const uint8_t> payload) -> bool {
    Track& track = tracks_[static_cast<size_t>(stream.track)];
    const bool h264 = stream.codec == OM_CODEC_H264;

    bool keyframe = false;
    std::vector<uint8_t> parameter_sets;
    std::span<const uint8_t> sps_nal;

    forEachNal(payload, [&](std::span<const uint8_t> nal) {
      const uint8_t type = h264 ? h264NalType(nal) : hevcNalType(nal);
      if (h264) {
        if (type == h264::NAL_UNIT_TYPE_CODED_SLICE_IDR) keyframe = true;
        if (type == h264::NAL_UNIT_TYPE_SPS || type == h264::NAL_UNIT_TYPE_PPS) {
          if (type == h264::NAL_UNIT_TYPE_SPS && sps_nal.empty()) sps_nal = nal;
          appendAnnexB(parameter_sets, nal);
        }
        return;
      }
      if (isHevcIrap(type)) keyframe = true;
      if (type == 32 || type == 33 || type == 34) { // VPS, SPS, PPS
        if (type == 33 && sps_nal.empty()) sps_nal = nal;
        appendAnnexB(parameter_sets, nal);
      }
    });

    if (stream.described || parameter_sets.empty()) return keyframe;
    stream.described = true;

    // Annex-B, the same form the MP4 and Matroska demuxers hand over after
    // unpacking an avcC or hvcC: the decoders of this library take that one shape.
    track.extradata = std::move(parameter_sets);
    if (h264) {
      describeH264(sps_nal, track);
    } else {
      describeHevc(payload, track);
    }
    return keyframe;
  }

  void describeH264(std::span<const uint8_t> sps_nal, Track& track) {
    if (sps_nal.empty()) return;
    h264::Bitstream bitstream;
    bitstream.init(sps_nal.data(), sps_nal.size());
    h264::NALHeader header = {};
    if (!h264::read_nal_header(header, bitstream)) return;
    h264::SPS sps = {};
    if (!h264::read_sps(sps, bitstream, rbsp_)) return;

    uint32_t width = (static_cast<uint32_t>(sps.pic_width_in_mbs_minus1) + 1) * 16;
    uint32_t height = (static_cast<uint32_t>(sps.pic_height_in_map_units_minus1) + 1) * 16 *
                      (sps.frame_mbs_only_flag != 0 ? 1 : 2);

    // 7.4.2.1.1: the cropping offsets are counted in chroma samples, so what one
    // unit of them removes depends on the chroma format and on whether the stream
    // codes fields.
    if (sps.frame_cropping_flag != 0) {
      const uint32_t unit_x = (sps.chroma_format_idc == 1 || sps.chroma_format_idc == 2) ? 2 : 1;
      const uint32_t unit_y = (sps.chroma_format_idc == 1 ? 2u : 1u) *
                              (sps.frame_mbs_only_flag != 0 ? 1u : 2u);
      const uint32_t cropped_x =
          (static_cast<uint32_t>(sps.frame_crop_left_offset + sps.frame_crop_right_offset)) * unit_x;
      const uint32_t cropped_y =
          (static_cast<uint32_t>(sps.frame_crop_top_offset + sps.frame_crop_bottom_offset)) * unit_y;
      if (cropped_x < width) width -= cropped_x;
      if (cropped_y < height) height -= cropped_y;
    }

    track.format.video.width = width;
    track.format.video.height = height;
    track.format.profile = static_cast<OMProfile>(sps.profile_idc);
    track.format.level = sps.level_idc;
    if (sps.vui_parameters_present_flag != 0 && sps.vui.timing_info_present_flag != 0 &&
        sps.vui.num_units_in_tick > 0) {
      // E.2.1: the VUI counts field intervals, and a frame is two of them.
      track.format.video.framerate =
          reduced(sps.vui.time_scale, INT64_C(2) * sps.vui.num_units_in_tick);
    }
    if (sps.vui.sar_width > 0 && sps.vui.sar_height > 0) {
      track.format.video.sample_aspect_ratio = reduced(sps.vui.sar_width, sps.vui.sar_height);
    }
  }

  void describeHevc(std::span<const uint8_t> payload, Track& track) {
    // The HEVC parameter sets are read by running the access unit through the
    // parser, which is the only entry point that reaches its SPS reader.
    video_parser::H265AccessUnitParser parser;
    parser.parse(payload);
    if (!parser.hasSps()) return;
    for (int id = 0; id < 16; ++id) {
      const auto& sps = parser.sps(id);
      if (!sps.valid || sps.pic_width_in_luma_samples <= 0) continue;
      track.format.video.width = static_cast<uint32_t>(sps.pic_width_in_luma_samples);
      track.format.video.height = static_cast<uint32_t>(sps.pic_height_in_luma_samples);
      return;
    }
  }

  void emitAdtsFrames(ElementaryStream& stream, std::span<const uint8_t> payload, int64_t pts) {
    // One PES packet holds a run of ADTS frames under a single timestamp. Each frame
    // becomes a packet timed from the one before by its own sample count, which is
    // what keeps the audio track in step with its decode clock.
    int64_t frame_pts = pts >= 0 ? pts : stream.audio_cursor;
    if (frame_pts < 0) return; // nothing has ever placed this stream in time

    size_t at = 0;
    while (at + 7 <= payload.size()) {
      if (payload[at] != 0xFF || (payload[at + 1] & 0xF0) != 0xF0) {
        ++at; // not a frame header: step on and look for the next syncword
        continue;
      }
      const size_t frame_length = ((static_cast<size_t>(payload[at + 3]) & 0x03) << 11) |
                                  (static_cast<size_t>(payload[at + 4]) << 3) |
                                  (static_cast<size_t>(payload[at + 5]) >> 5);
      const size_t header_length = (payload[at + 1] & 0x01) == 0 ? 9 : 7; // the CRC adds two
      if (frame_length <= header_length || at + frame_length > payload.size()) break;

      describeAac(stream, payload.subspan(at, header_length));
      if (stream.sample_rate == 0) break;

      const int64_t blocks = static_cast<int64_t>(payload[at + 6] & 0x03) + 1;
      const int64_t duration = rescaleTime(blocks * AAC_FRAME_SAMPLES, stream.sample_rate,
                                           TS_TIMESCALE);

      Packet packet;
      // The ADTS header is dropped: the track carries an AudioSpecificConfig, which
      // says everything the header repeats, and a decoder configured from one
      // expects the raw blocks after it.
      const size_t raw_size = frame_length - header_length;
      packet.allocate(raw_size);
      std::memcpy(packet.bytes.data(), payload.data() + at + header_length, raw_size);
      packet.stream_index = stream.track;
      packet.pts = frame_pts;
      packet.dts = frame_pts;
      packet.duration = duration;
      packet.is_keyframe = true; // every AAC frame is decodable on its own
      stream.staged.push_back(std::move(packet));

      frame_pts += duration;
      at += frame_length;
    }
    stream.audio_cursor = frame_pts;
  }

  void describeAac(ElementaryStream& stream, std::span<const uint8_t> header) {
    const uint32_t profile = (header[2] >> 6) & 0x03;
    const uint32_t rate_index = (header[2] >> 2) & 0x0F;
    const uint32_t channel_config = ((header[2] & 0x01) << 2) | ((header[3] >> 6) & 0x03);
    if (rate_index >= std::size(AAC_SAMPLE_RATES)) return;

    stream.sample_rate = AAC_SAMPLE_RATES[rate_index];
    if (stream.described) return;
    stream.described = true;

    Track& track = tracks_[static_cast<size_t>(stream.track)];
    track.format.audio.sample_rate = stream.sample_rate;
    track.format.audio.channels =
        channel_config < std::size(AAC_CHANNELS) ? AAC_CHANNELS[channel_config] : 0;
    // ADTS writes the object type one less than it is.
    const uint32_t object_type = profile + 1;
    track.format.profile = static_cast<OMProfile>(object_type);
    // An AudioSpecificConfig built from the same fields, because that is what the
    // decoders of this library are configured from -- the ADTS headers themselves
    // are dropped along with the framing they describe.
    track.extradata = {
        static_cast<uint8_t>((object_type << 3) | (rate_index >> 1)),
        static_cast<uint8_t>(((rate_index & 0x01) << 7) | (channel_config << 3)),
    };
  }

  // --- Timing ---------------------------------------------------------------

  // A video frame's duration is the step to the next in decode order, so one frame
  // of lookahead is staged before the frame in front of it can go. `timed` counts the
  // leading staged frames whose duration is settled -- all of them once a segment
  // ends.
  void assignDurations(ElementaryStream& stream, bool final) {
    if (stream.type != OM_MEDIA_VIDEO) {
      // An ADTS frame's duration comes from its own sample count, settled where it
      // was parsed.
      stream.timed = stream.staged.size();
      return;
    }
    for (size_t i = stream.timed; i + 1 < stream.staged.size(); ++i) {
      stream.video_step = stream.staged[i + 1].dts - stream.staged[i].dts;
      stream.staged[i].duration = stream.video_step;
    }
    stream.timed = stream.staged.empty() ? 0 : stream.staged.size() - 1;

    if (final && !stream.staged.empty()) {
      // The last frame of a segment is followed by the first of the next one, which
      // is not in hand; it lasts as long as its predecessor did.
      stream.staged.back().duration = stream.video_step;
      stream.timed = stream.staged.size();
    }

    if (stream.video_step > 0 && stream.track >= 0) {
      Track& track = tracks_[static_cast<size_t>(stream.track)];
      if (track.format.video.framerate.num == 0) {
        track.format.video.framerate = reduced(TS_TIMESCALE, stream.video_step);
      }
    }
  }

  // Whether every track has a packet staged, which is what it takes to know which
  // comes first. A segment carrying nothing of one of its tracks never gets there and
  // is read out in one go when it ends.
  auto everyTrackStaged() const -> bool {
    for (const ElementaryStream& stream : streams_) {
      if (stream.track >= 0 && stream.staged.empty()) return false;
    }
    return true;
  }

  // Moves the segment's timestamps onto the playlist's timeline, so the nth second of
  // the presentation is the nth second of a packet whatever clock the encoder ran.
  void settleBase(int64_t first_raw) {
    base_settled_ = true;
    segment_end_raw_ = first_raw;

    const bool continues = have_base_ &&
                           std::abs(first_raw - previous_end_raw_) <= DISCONTINUITY_TICKS;
    if (continues) return;

    // Either the first segment read, or the clock jumped -- a discontinuity, a wrap,
    // or a seek into a clock this reader has not followed. The playlist says where
    // the segment belongs; the stream only says how its samples are spaced.
    //
    // Anchored on the earliest decode time, not presentation time, so no timestamp
    // comes out negative and each segment's packets fall inside the span the playlist
    // gave it. With B-frames the first picture then appears a reorder delay late --
    // the same at every point of the stream, so it does not accumulate.
    base_ = first_raw - nsToScale(expected_start_ns_, TS_TIMESCALE);
    have_base_ = true;
  }

  // Every staged packet whose turn has come, earliest decode time first and only
  // while the order is beyond doubt. Reading a segment as it arrives settles that by
  // waiting: a packet goes only once every track has one to compare against, so
  // nothing goes out ahead of a packet that belongs in front of it.
  void releaseStaged(bool final) {
    for (ElementaryStream& stream : streams_) assignDurations(stream, final);

    if (!base_settled_) {
      // The anchor is the segment's earliest decode time, and which track owns that
      // is not knowable until each has spoken.
      if (!final && !everyTrackStaged()) return;
      int64_t first_raw = INT64_MAX;
      for (const ElementaryStream& stream : streams_) {
        if (stream.track < 0 || stream.staged.empty()) continue;
        first_raw = std::min(first_raw, stream.staged.front().dts);
      }
      if (first_raw == INT64_MAX) return; // nothing timed has been parsed yet
      settleBase(first_raw);
    }

    for (;;) {
      ElementaryStream* earliest = nullptr;
      bool comparable = true;
      for (ElementaryStream& stream : streams_) {
        if (stream.track < 0) continue;
        if (stream.staged.empty()) {
          // Nothing to compare against, so nothing comes first -- unless the
          // segment has ended and nothing more is coming.
          if (!final) comparable = false;
          continue;
        }
        if (!earliest || stream.staged.front().dts < earliest->staged.front().dts) {
          earliest = &stream;
        }
      }
      if (!comparable || !earliest || earliest->timed == 0) break;

      Packet packet = std::move(earliest->staged.front());
      earliest->staged.pop_front();
      --earliest->timed;

      segment_end_raw_ = std::max(segment_end_raw_, packet.dts + packet.duration);
      packet.pts -= base_;
      packet.dts -= base_;
      packets_.push_back(std::move(packet));
      ++released_;
    }
  }
};

} // namespace

auto looksLikeMpegTs(std::span<const uint8_t> bytes) -> bool {
  if (bytes.size() < TS_PACKET_SIZE) return false;
  if (bytes[0] != TS_SYNC_BYTE) return false;
  // One sync byte is a coincidence a `moov` can produce; three a packet apart are the
  // stride of a transport stream.
  for (size_t at = TS_PACKET_SIZE; at < bytes.size() && at <= 2 * TS_PACKET_SIZE;
       at += TS_PACKET_SIZE) {
    if (bytes[at] != TS_SYNC_BYTE) return false;
  }
  return true;
}

auto createMpegTsReader() -> std::unique_ptr<SegmentReader> {
  return std::make_unique<MpegTsReader>();
}

} // namespace openmedia
