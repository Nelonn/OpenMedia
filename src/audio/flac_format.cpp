#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <openmedia/format_api.hpp>
#include <openmedia/packet.hpp>
#include <util/bit_reader.hpp>
#include <util/bit_writer.hpp>
#include <util/byte_reader.hpp>
#include <util/byte_writer.hpp>
#include <util/demuxer_base.hpp>
#include <util/id3_parser.hpp>
#include <util/io_util.hpp>
#include <util/vorbis_comment.hpp>
#include <vector>

namespace openmedia {

enum class FLACMetadataType : uint8_t {
  STREAMINFO = 0,
  PADDING = 1,
  APPLICATION = 2,
  SEEKTABLE = 3,
  VORBIS_COMMENT = 4,
  CUESHEET = 5,
  PICTURE = 6,
  UNDEFINED = 7,
};

struct FLACStreamInfo {
  uint32_t min_blocksize;
  uint32_t max_blocksize;
  uint32_t min_framesize;
  uint32_t max_framesize;
  uint32_t sample_rate;
  uint32_t channels;
  uint32_t bits_per_sample;
  uint64_t total_samples;
  uint8_t md5sum[16];
};

// The cover art bytes themselves live in the demuxer's metadata dictionary,
// which is where both the image track and the cover-art packet read them from.
struct FLACPicture {
  OMCodecId codec_id = OM_CODEC_NONE;
  uint32_t width = 0;
  uint32_t height = 0;
};

struct FLACSeekPoint {
  uint64_t sample_number;
  uint64_t stream_offset;
  uint16_t num_samples;
};

struct FLACFrameHeader {
  int32_t size = 0;       // header bytes, including the trailing CRC-8
  int32_t block_size = 0; // samples per channel
  int32_t channel_assignment = 0;
  int64_t coded_number = 0;
  bool variable_block_size = false;
};

// RFC 9639 §9.1.5: a variable-block-size stream codes the sample number in the
// frame header, a fixed-block-size one codes the frame number instead.
static auto frameStartSample(const FLACFrameHeader& header) -> int64_t {
  return header.variable_block_size ? header.coded_number
                                    : header.coded_number * header.block_size;
}

static auto crc8(const uint8_t* data, size_t len) -> uint8_t {
  static constexpr auto S_TABLE = []() {
    std::array<uint8_t, 256> t {};
    for (int i = 0; i < 256; i++) {
      uint8_t crc = static_cast<uint8_t>(i);
      for (int j = 0; j < 8; j++)
        crc = (crc & 0x80) ? ((crc << 1) ^ 0x07) : (crc << 1);
      t[i] = crc;
    }
    return t;
  }();
  uint8_t crc = 0;
  for (size_t i = 0; i < len; i++) {
    crc = S_TABLE[crc ^ data[i]];
  }
  return crc;
}

static auto crc16(const uint8_t* data, size_t len) -> uint16_t {
  static constexpr auto S_TABLE = []() {
    std::array<uint16_t, 256> t {};
    for (int i = 0; i < 256; i++) {
      uint16_t crc = static_cast<uint16_t>(i << 8);
      for (int j = 0; j < 8; j++)
        crc = (crc & 0x8000) ? ((crc << 1) ^ 0x8005) : (crc << 1);
      t[i] = crc;
    }
    return t;
  }();
  uint16_t crc = 0;
  for (size_t i = 0; i < len; i++) {
    crc = static_cast<uint16_t>((crc << 8) ^ S_TABLE[(crc >> 8) ^ data[i]]);
  }
  return crc;
}

// A UTF-8 lead byte with n leading 1 bits is followed by n-1 continuation
// bytes; anything else is not a valid lead byte.
static auto decodeUtf8ExtraBytes(uint8_t b) -> int {
  if (b < 0x80) return 0;
  const int ones = std::countl_one(b);
  return (ones >= 2 && ones <= 7) ? ones - 1 : -1;
}

// RFC 9639 §9.1.1. Codes 0, 6 and 7 are not in the table: 0 means "the
// STREAMINFO minimum" and 6/7 carry the value in the header itself.
static constexpr int32_t BLOCK_SIZE_CODES[16] = {
    0, 192, 576, 1152, 2304, 4608, 0, 0,
    256, 512, 1024, 2048, 4096, 8192, 16384, 32768};

// RFC 9639 §9.1.2. Zero entries name no rate: code 0 defers to STREAMINFO,
// codes 12-14 carry the value in the header and 15 is invalid.
static constexpr uint32_t SAMPLE_RATE_CODES[16] = {
    0, 88200, 176400, 192000, 8000, 16000, 22050, 24000,
    32000, 44100, 48000, 96000, 0, 0, 0, 0};

// RFC 9639 §9.1.4. Code 0 defers to STREAMINFO and code 3 is reserved.
static constexpr uint8_t BIT_DEPTH_CODES[8] = {0, 8, 12, 0, 16, 20, 24, 32};

static auto parseStreamInfo(std::span<const uint8_t> body, FLACStreamInfo& stream) -> bool {
  if (body.size() != 34) return false;

  ByteReader r(body);
  stream.min_blocksize = r.u16be();
  stream.max_blocksize = r.u16be();
  stream.min_framesize = r.u24be();
  stream.max_framesize = r.u24be();

  BitReader br(r.bytes(8));
  stream.sample_rate = br.readBits(20);
  stream.channels = br.readBits(3) + 1;
  stream.bits_per_sample = br.readBits(5) + 1;
  stream.total_samples = br.readBits64(36);

  const auto md5 = r.bytes(sizeof(stream.md5sum));
  memcpy(stream.md5sum, md5.data(), md5.size());
  return r.ok();
}

static void parseSeektable(std::span<const uint8_t> body,
                           std::vector<FLACSeekPoint>& seek_table) {
  constexpr size_t POINT_SIZE = 18;
  ByteReader r(body);
  seek_table.reserve(seek_table.size() + body.size() / POINT_SIZE);
  while (r.canRead(POINT_SIZE)) {
    const uint64_t sample_number = r.u64be();
    const uint64_t stream_offset = r.u64be();
    const uint16_t num_samples = r.u16be();
    if (sample_number == UINT64_MAX) continue; // placeholder point
    seek_table.push_back({sample_number, stream_offset, num_samples});
  }
}

static auto pictureCodecFromMime(std::string_view mime) -> OMCodecId {
  if (mime == "image/jpeg" || mime == "image/jpg") return OM_CODEC_JPEG;
  if (mime == "image/png") return OM_CODEC_PNG;
  return OM_CODEC_NONE;
}

// METADATA_BLOCK_PICTURE, RFC 9639 §8.8. Keeps the front cover (type 3) or,
// failing that, the first supported picture.
static void parsePicture(std::span<const uint8_t> body, FLACPicture& picture,
                         Dictionary& metadata) {
  ByteReader r(body);
  const uint32_t picture_type = r.u32be();
  const std::string_view mime = r.str(r.u32be());
  r.skip(r.u32be()); // description
  const uint32_t width = r.u32be();
  const uint32_t height = r.u32be();
  r.skip(8); // color depth, number of indexed colors
  const auto data = r.bytes(r.u32be());
  if (!r.ok() || data.empty()) return;

  const OMCodecId codec_id = pictureCodecFromMime(mime);
  if (codec_id == OM_CODEC_NONE) return;
  if (picture_type != 3 && picture.codec_id != OM_CODEC_NONE) return;

  picture.codec_id = codec_id;
  picture.width = width;
  picture.height = height;
  metadata.setBinary(COVER_ART, data);
  metadata.setString(COVER_ART_MIME,
                     codec_id == OM_CODEC_PNG ? std::string_view("image/png")
                                              : std::string_view("image/jpeg"));
}

class FLACDemuxer final : public BaseDemuxer {
  static constexpr size_t READ_CHUNK = 64 * 1024;
  static constexpr size_t MIN_HEADER_SIZE = 6;
  static constexpr size_t MAX_HEADER_SIZE = 16;
  static constexpr size_t RESIDUAL_LOOKAHEAD = 4096;
  // Where bisecting stops paying off and a forward scan is cheaper.
  static constexpr int64_t SEARCH_GRANULARITY = 64 * 1024;
  static constexpr size_t NO_SYNC = static_cast<size_t>(-1);

  int64_t audio_data_offset_ = 0;
  int64_t current_sample_pos_ = 0;

  FLACStreamInfo stream_info_ = {};

  std::vector<FLACSeekPoint> seek_points_;
  FLACPicture cover_art_;
  bool cover_art_sent_ = false;
  int32_t cover_art_track_index_ = -1;

  // read_buf_ is sized by hand so refills land straight in it: its size() is
  // the capacity, and [read_begin_, read_end_) is the data read from the input
  // but not yet delivered.
  std::vector<uint8_t> read_buf_;
  size_t read_begin_ = 0;
  size_t read_end_ = 0;
  int64_t read_origin_ = 0; // file offset of read_buf_[read_begin_]

public:
  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    cover_art_sent_ = false;
    cover_art_track_index_ = -1;
    input_ = std::move(input);
    if (!input_ || !input_->isValid()) {
      return OM_IO_INVALID_STREAM;
    }

    uint8_t marker[4];
    if (readExact(marker, 4) != 4) {
      return OM_IO_NOT_ENOUGH_DATA;
    }

    if (marker[0] == 'I' && marker[1] == 'D' && marker[2] == '3') {
      uint8_t id3hdr[6];
      if (readExact(id3hdr, 6) != 6) {
        return OM_IO_NOT_ENOUGH_DATA;
      }
      if (!(id3hdr[2] & 0x80) && !(id3hdr[3] & 0x80) && !(id3hdr[4] & 0x80) && !(id3hdr[5] & 0x80)) {
        size_t tag_size = (static_cast<size_t>(id3hdr[2] & 0x7F) << 21) |
                          (static_cast<size_t>(id3hdr[3] & 0x7F) << 14) |
                          (static_cast<size_t>(id3hdr[4] & 0x7F) << 7) |
                          static_cast<size_t>(id3hdr[5] & 0x7F);
        tag_size += 10;
        if (id3hdr[1] & 0x10) tag_size += 10;
        std::vector<uint8_t> id3_data(tag_size);
        memcpy(id3_data.data(), marker, 4);
        memcpy(id3_data.data() + 4, id3hdr, 6);
        if (tag_size > 10) {
          if (readExact(id3_data.data() + 10, tag_size - 10) != tag_size - 10) {
            return OM_IO_NOT_ENOUGH_DATA;
          }
        }
        parseId3v2(id3_data, metadata_);
        if (!input_->seek(static_cast<int64_t>(tag_size), Whence::BEG)) {
          return OM_IO_SEEK_FAILED;
        }
        if (readExact(marker, 4) != 4) {
          return OM_IO_NOT_ENOUGH_DATA;
        }
      }
    }

    if (std::memcmp(marker, "fLaC", 4) != 0) {
      return OM_FORMAT_PARSE_FAILED;
    }

    // Metadata blocks are read straight into the extradata that the decoder
    // will be handed, and parsed in place; a PICTURE block can be megabytes,
    // so staging each one in its own vector first is worth avoiding.
    std::vector<uint8_t> extradata(marker, marker + 4);

    bool found_streaminfo = false;

    while (true) {
      const size_t header_at = extradata.size();
      extradata.resize(header_at + 4);
      if (readExact(extradata.data() + header_at, 4) != 4) {
        return OM_IO_NOT_ENOUGH_DATA;
      }

      const bool is_last = (extradata[header_at] & 0x80) != 0;
      const auto block_type = static_cast<FLACMetadataType>(extradata[header_at] & 0x7F);
      const uint32_t body_len = load_u24_be(extradata.data() + header_at + 1);

      const size_t body_at = extradata.size();
      extradata.resize(body_at + body_len);
      if (body_len > 0 && readExact(extradata.data() + body_at, body_len) != body_len) {
        return OM_IO_NOT_ENOUGH_DATA;
      }
      const std::span<const uint8_t> body(extradata.data() + body_at, body_len);

      switch (block_type) {
        case FLACMetadataType::STREAMINFO:
          if (!parseStreamInfo(body, stream_info_)) {
            return OM_FORMAT_PARSE_FAILED;
          }
          found_streaminfo = true;
          break;
        case FLACMetadataType::SEEKTABLE:
          parseSeektable(body, seek_points_);
          break;
        case FLACMetadataType::VORBIS_COMMENT:
          parseVorbisComment(body, metadata_);
          break;
        case FLACMetadataType::PICTURE:
          parsePicture(body, cover_art_, metadata_);
          break;
        default:
          break;
      }

      if (is_last) {
        audio_data_offset_ = input_->tell();
        break;
      }
    }

    if (!found_streaminfo) {
      return OM_FORMAT_PARSE_FAILED;
    }

    resetBuffer(audio_data_offset_);
    current_sample_pos_ = 0;

    Track track;
    track.index = 0;
    track.format.type = OM_MEDIA_AUDIO;
    track.format.codec_id = OM_CODEC_FLAC;
    track.format.audio.bit_depth = stream_info_.bits_per_sample;
    track.format.audio.sample_rate = stream_info_.sample_rate;
    track.format.audio.channels = stream_info_.channels;
    track.time_base = {1, static_cast<int>(stream_info_.sample_rate)};
    track.duration = stream_info_.total_samples;
    track.extradata = std::move(extradata);
    track.metadata = metadata_;

    tracks_.push_back(std::move(track));

    if (cover_art_.codec_id != OM_CODEC_NONE) {
      Track image_track;
      image_track.index = static_cast<int32_t>(tracks_.size());
      image_track.format.type = OM_MEDIA_IMAGE;
      image_track.format.codec_id = cover_art_.codec_id;
      image_track.format.video.width = cover_art_.width;
      image_track.format.video.height = cover_art_.height;
      image_track.disposition = OM_DISPOSITION_COVER;
      image_track.time_base = {1, 1};
      image_track.duration = 1;
      image_track.nb_frames = 1;
      cover_art_track_index_ = image_track.index;
      tracks_.push_back(std::move(image_track));
    }

    return OM_SUCCESS;
  }

  void close() override {
    BaseDemuxer::close();
    cover_art_sent_ = false;
    cover_art_track_index_ = -1;
    cover_art_ = {};
    read_buf_.clear();
    read_buf_.shrink_to_fit();
    resetBuffer(0);
    audio_data_offset_ = 0;
    current_sample_pos_ = 0;
    seek_points_.clear();
    stream_info_ = {};
  }

  auto readPacket() -> Result<Packet, OMError> override {
    if (!cover_art_sent_ && cover_art_.codec_id != OM_CODEC_NONE) {
      cover_art_sent_ = true;
      if (auto data = metadata_.getBinary(COVER_ART)) {
        Packet pkt;
        pkt.allocate(data->size());
        memcpy(pkt.bytes.data(), data->data(), data->size());
        pkt.stream_index = cover_art_track_index_ >= 0 ? cover_art_track_index_ : 1;
        pkt.pos = 0;
        pkt.pts = 0;
        pkt.dts = 0;
        pkt.duration = 1;
        pkt.is_keyframe = true;
        return Ok(std::move(pkt));
      }
    }
    return scanAndDeliverFrame();
  }

  // `timestamp` is in microseconds when stream_idx < 0, otherwise in the
  // track's time base, which for FLAC is samples.
  auto seek(int32_t stream_idx, int64_t timestamp, SeekMode /*mode*/) -> OMError override {
    if (timestamp > 0) {
      cover_art_sent_ = true;
    }

    int64_t target_sample;
    if (stream_idx < 0) {
      const uint64_t sr = stream_info_.sample_rate;
      const uint64_t ts = timestamp < 0 ? 0 : static_cast<uint64_t>(timestamp);
      // Split at the second boundary so long files cannot overflow the product.
      target_sample = static_cast<int64_t>((ts / 1'000'000) * sr +
                                           ((ts % 1'000'000) * sr) / 1'000'000);
    } else {
      target_sample = timestamp;
    }

    if (target_sample <= 0) {
      if (!input_->seek(audio_data_offset_, Whence::BEG)) {
        return OM_IO_SEEK_FAILED;
      }
      resetBuffer(audio_data_offset_);
      current_sample_pos_ = 0;
      return OM_SUCCESS;
    }

    // Seek points are ordered by sample number, so the pair around the target
    // brackets the region the frame can be in. Most FLAC files carry no seek
    // table at all, in which case the bracket is the whole audio region.
    int64_t lo_pos = audio_data_offset_;
    int64_t lo_sample = 0;
    int64_t hi_pos = input_->size();
    int64_t hi_sample = static_cast<int64_t>(stream_info_.total_samples);
    const auto after = std::upper_bound(
        seek_points_.begin(), seek_points_.end(), static_cast<uint64_t>(target_sample),
        [](uint64_t target, const FLACSeekPoint& sp) { return target < sp.sample_number; });
    if (after != seek_points_.begin()) {
      const auto& sp = *(after - 1);
      lo_pos = audio_data_offset_ + static_cast<int64_t>(sp.stream_offset);
      lo_sample = static_cast<int64_t>(sp.sample_number);
    }
    if (after != seek_points_.end()) {
      hi_pos = audio_data_offset_ + static_cast<int64_t>(after->stream_offset);
      hi_sample = static_cast<int64_t>(after->sample_number);
    }

    if (hi_pos > lo_pos && input_->canSeek()) {
      narrowBySearch(target_sample, lo_pos, lo_sample, hi_pos, hi_sample);
    }

    if (!input_->seek(lo_pos, Whence::BEG)) {
      return OM_IO_SEEK_FAILED;
    }
    resetBuffer(lo_pos);
    current_sample_pos_ = lo_sample;

    seekScanSync(target_sample);

    return OM_SUCCESS;
  }

private:
  auto avail() const -> size_t { return read_end_ - read_begin_; }
  auto cur() const -> const uint8_t* { return read_buf_.data() + read_begin_; }

  void resetBuffer(int64_t origin) {
    read_begin_ = 0;
    read_end_ = 0;
    read_origin_ = origin;
  }

  // Makes at least `needed` undelivered bytes available at cur(). The buffer
  // may be compacted, so any pointer or BitReader over it must be re-derived;
  // offsets relative to cur() stay valid. False at end of input.
  auto ensureBytes(size_t needed) -> bool {
    if (avail() >= needed) return true;

    if (read_begin_ > 0) {
      const size_t keep = avail();
      if (keep > 0) {
        memmove(read_buf_.data(), read_buf_.data() + read_begin_, keep);
      }
      read_begin_ = 0;
      read_end_ = keep;
    }
    if (read_buf_.size() < needed + READ_CHUNK) {
      read_buf_.resize(needed + READ_CHUNK);
    }

    while (avail() < needed) {
      const size_t n = input_->read(
          std::span(read_buf_.data() + read_end_, read_buf_.size() - read_end_));
      if (n == 0) return false;
      read_end_ += n;
    }
    return true;
  }

  // ensureBytes() + re-point `br` at the buffer it may have moved.
  auto ensureRebind(size_t needed, BitReader& br) -> bool {
    const bool ok = ensureBytes(needed);
    br.rebind(std::span(cur(), avail()));
    return ok;
  }

  void consume(size_t n) {
    read_begin_ += n;
    read_origin_ += static_cast<int64_t>(n);
    if (read_begin_ == read_end_) {
      read_begin_ = 0;
      read_end_ = 0;
    }
  }

  // Offset of the next frame sync word, or NO_SYNC. A sync takes two bytes, so
  // a trailing 0xFF is never reported as one.
  static auto findSync(const uint8_t* p, size_t n) -> size_t {
    for (size_t i = 0; i + 1 < n;) {
      const auto* hit = static_cast<const uint8_t*>(memchr(p + i, 0xFF, n - 1 - i));
      if (!hit) break;
      i = static_cast<size_t>(hit - p);
      if ((p[i + 1] & 0xFE) == 0xF8) return i;
      ++i;
    }
    return NO_SYNC;
  }

  // RFC 9639 §9.1. Accepts only a header that fits in `size` and whose CRC-8
  // matches, which is what tells a real frame from audio data that happens to
  // contain the sync word.
  auto parseFrameHeader(const uint8_t* d, size_t size, FLACFrameHeader& out) const -> bool {
    if (size < MIN_HEADER_SIZE) return false;
    if (d[0] != 0xFF || (d[1] & 0xFE) != 0xF8) return false;

    const uint8_t bs_code = (d[2] >> 4) & 0x0F;
    const uint8_t sr_code = d[2] & 0x0F;

    // Cross-check the header against STREAMINFO. Audio data is full of byte
    // pairs that look like a sync word and the CRC-8 lets one in 256 of them
    // through; these fields are what make a header found at an arbitrary file
    // offset trustworthy enough for a seek probe to act on.
    const uint8_t channel_code = (d[3] >> 4) & 0x0F;
    if (channel_code > 10) return false; // reserved
    if ((channel_code < 8 ? channel_code + 1u : 2u) != stream_info_.channels) return false;

    const uint8_t depth_code = (d[3] >> 1) & 0x07;
    if (depth_code == 3) return false;  // reserved
    if ((d[3] & 0x01) != 0) return false; // reserved bit
    if (depth_code != 0 && BIT_DEPTH_CODES[depth_code] != stream_info_.bits_per_sample) return false;

    if (sr_code == 0x0F) return false; // invalid
    if (SAMPLE_RATE_CODES[sr_code] != 0 && SAMPLE_RATE_CODES[sr_code] != stream_info_.sample_rate) return false;

    // sync(2) + block size/sample rate codes(1) + channels/bps(1), then the
    // UTF-8 coded frame or sample number.
    const uint8_t lead = d[4];
    const int extra = decodeUtf8ExtraBytes(lead);
    if (extra < 0) return false;
    size_t pos = 5 + static_cast<size_t>(extra);
    if (pos > size) return false;

    uint64_t number = extra == 0 ? lead : (lead & (0x3Fu >> extra));
    for (int i = 0; i < extra; i++) {
      number = (number << 6) | (d[5 + i] & 0x3F);
    }

    int64_t block_size = BLOCK_SIZE_CODES[bs_code];
    if (bs_code == 0x00) {
      block_size = stream_info_.min_blocksize;
    } else if (bs_code == 0x06) {
      if (pos + 1 > size) return false;
      block_size = static_cast<int64_t>(d[pos]) + 1;
      pos += 1;
    } else if (bs_code == 0x07) {
      if (pos + 2 > size) return false;
      block_size = static_cast<int64_t>(load_u16_be(d + pos)) + 1;
      pos += 2;
    }
    if (block_size <= 0) return false;

    if (sr_code == 0x0C) {
      pos += 1;
    } else if (sr_code == 0x0D || sr_code == 0x0E) {
      pos += 2;
    }

    pos += 1; // CRC-8
    if (pos > size) return false;
    if (crc8(d, pos - 1) != d[pos - 1]) return false;

    out.size = static_cast<int32_t>(pos);
    out.block_size = static_cast<int32_t>(block_size);
    out.channel_assignment = channel_code;
    out.coded_number = static_cast<int64_t>(number);
    out.variable_block_size = (d[1] & 0x01) != 0;
    return true;
  }

  // Consumes everything up to the next valid frame header and leaves that
  // header at cur(). False at end of input.
  auto findNextFrameHeader(FLACFrameHeader& header) -> bool {
    for (;;) {
      // A short tail near the end of the file can still hold a last frame, so
      // a failed top-up is not fatal by itself.
      ensureBytes(MAX_HEADER_SIZE);
      if (avail() < MIN_HEADER_SIZE) return false;

      const size_t hit = findSync(cur(), avail());
      if (hit == NO_SYNC) {
        // Only the trailing byte can still be the first half of a sync word.
        consume(avail() - 1);
        if (!ensureBytes(avail() + READ_CHUNK)) return false;
        continue;
      }

      consume(hit);
      ensureBytes(MAX_HEADER_SIZE);
      if (parseFrameHeader(cur(), avail(), header)) return true;
      consume(1);
    }
  }

  // Leaves a whole frame at cur() and reports its header and byte length.
  // A frame carries no length, so the end is found by walking the subframe bits
  // and confirming the trailing CRC-16 -- which is also what makes a hit
  // trustworthy when the search started at an arbitrary file offset.
  auto locateFrame(FLACFrameHeader& header, size_t& frame_size) -> bool {
    for (;;) {
      if (!findNextFrameHeader(header)) return false;

      BitReader br(std::span(cur(), avail()));
      br.skipBits(static_cast<size_t>(header.size) * 8);

      if (!consumeSubframes(br, header) || !br.ok()) {
        consume(1); // false sync inside audio data
        continue;
      }

      br.alignToByte();
      size_t size = br.bytePosition() + 2; // + CRC-16
      if (!ensureRebind(size, br)) {
        if (avail() < br.bytePosition()) return false;
        size = avail(); // truncated last frame
      }

      if (size >= 2) {
        const size_t crc_len = size - 2;
        if (crc16(cur(), crc_len) != load_u16_be(cur() + crc_len)) {
          consume(1);
          continue;
        }
      }

      frame_size = size;
      return true;
    }
  }

  auto scanAndDeliverFrame() -> Result<Packet, OMError> {
    FLACFrameHeader header = {};
    size_t frame_size = 0;
    if (!locateFrame(header, frame_size)) return Err(OM_FORMAT_END_OF_FILE);

    Packet pkt;
    pkt.allocate(frame_size);
    memcpy(pkt.bytes.data(), cur(), frame_size);

    // The frame passed its CRC-16, so its own header is a better timestamp
    // than a running total: a seek lands exactly, and a skipped or corrupt
    // frame cannot shift everything that follows it.
    current_sample_pos_ = frameStartSample(header);

    pkt.stream_index = 0;
    pkt.pos = read_origin_;
    pkt.pts = pkt.dts = current_sample_pos_;
    pkt.duration = header.block_size;

    current_sample_pos_ += header.block_size;
    consume(frame_size);

    return Ok(std::move(pkt));
  }

  // RFC 9639 §10.2 - §10.2.4. Steps over the subframe bits without decoding
  // them; the demuxer only needs to know where the frame ends.
  //
  // Every top-up goes through ensureRebind(): read_buf_ can move under `br`,
  // and bit positions are relative to cur(), which survives that.
  auto consumeSubframes(BitReader& br, const FLACFrameHeader& header) -> bool {
    const int channels = static_cast<int>(stream_info_.channels);
    const int block_size = header.block_size;
    const int assignment = header.channel_assignment;
    const bool has_side = assignment >= 8 && assignment <= 10;

    for (int ch = 0; ch < channels; ch++) {
      int bps = static_cast<int>(stream_info_.bits_per_sample);
      if (has_side) {
        const bool is_side = (assignment == 8 && ch == 1) ||
                             (assignment == 9 && ch == 0) ||
                             (assignment == 10 && ch == 1);
        if (is_side) bps++;
      }

      if (!ensureRebind(br.bytePosition() + 2, br)) return false;

      const uint32_t sf_header = br.readBits(8);
      if (sf_header & 0x80) return false; // reserved bit must be zero

      const int type = static_cast<int>((sf_header >> 1) & 0x3F);

      if (sf_header & 1) {
        // Wasted bits are unary-coded: k zeros then a stop bit means k+1.
        int k = 0;
        for (;;) {
          if (br.bytePosition() + 2 > avail() &&
              !ensureRebind(br.bytePosition() + 64, br)) {
            return false;
          }
          if (br.readBits(1) != 0) break;
          if (++k > 30) return false;
        }
        bps -= k + 1;
      }
      if (bps <= 0) return false;

      if (type == 0) { // SUBFRAME_CONSTANT
        if (!ensureRebind(br.bytePosition() + static_cast<size_t>((bps + 7) / 8) + 1, br)) return false;
        br.skipBits(static_cast<size_t>(bps));

      } else if (type == 1) { // SUBFRAME_VERBATIM
        const int64_t verbatim_bits = static_cast<int64_t>(bps) * block_size;
        const size_t verbatim_bytes = static_cast<size_t>((verbatim_bits + 7) / 8);
        if (!ensureRebind(br.bytePosition() + verbatim_bytes + 1, br)) return false;
        br.skipBits(static_cast<size_t>(verbatim_bits));

      } else if (type >= 8 && type <= 12) { // SUBFRAME_FIXED
        const int order = type - 8;
        if (order > block_size) return false;
        const int64_t warmup_bits = static_cast<int64_t>(bps) * order;
        if (!ensureRebind(br.bytePosition() + static_cast<size_t>((warmup_bits + 7) / 8) + 16, br)) return false;
        br.skipBits(static_cast<size_t>(warmup_bits));
        if (!consumeResidual(br, block_size, order)) return false;

      } else if (type >= 32 && type <= 63) { // SUBFRAME_LPC
        const int order = type - 31;
        if (order > block_size) return false;
        const int64_t warmup_bits = static_cast<int64_t>(bps) * order;
        if (!ensureRebind(br.bytePosition() + static_cast<size_t>((warmup_bits + 7) / 8) + 32, br)) return false;
        br.skipBits(static_cast<size_t>(warmup_bits));
        const int qlp_precision = static_cast<int>(br.readBits(4)) + 1;
        br.skipBits(5); // qlp_shift
        const int64_t coeff_bits = static_cast<int64_t>(qlp_precision) * order;
        if (!ensureRebind(br.bytePosition() + static_cast<size_t>((coeff_bits + 7) / 8) + 16, br)) return false;
        br.skipBits(static_cast<size_t>(coeff_bits));
        if (!consumeResidual(br, block_size, order)) return false;

      } else {
        return false; // reserved subframe type
      }
    }
    return true;
  }

  // Rice-coded residual, RFC 9639 §10.2.5.
  auto consumeResidual(BitReader& br, int block_size, int predictor_order) -> bool {
    if (!ensureRebind(br.bytePosition() + 2, br)) return false;

    const int coding_method = static_cast<int>(br.readBits(2));
    if (coding_method > 1) return false;
    const int partition_order = static_cast<int>(br.readBits(4));
    const int num_partitions = 1 << partition_order;
    const uint32_t rice_param_bits = (coding_method == 0) ? 4 : 5;
    const int escape_value = (coding_method == 0) ? 15 : 31;

    for (int p = 0; p < num_partitions; p++) {
      if (!ensureRebind(br.bytePosition() + 4, br)) return false;

      const int rice_param = static_cast<int>(br.readBits(rice_param_bits));

      int samples_in_partition;
      if (partition_order == 0) {
        samples_in_partition = block_size - predictor_order;
      } else if (p == 0) {
        samples_in_partition = (block_size >> partition_order) - predictor_order;
      } else {
        samples_in_partition = block_size >> partition_order;
      }
      if (samples_in_partition < 0) return false;

      if (rice_param == escape_value) {
        // Escaped: 5 bits of raw_bits, then raw_bits per sample.
        if (!ensureRebind(br.bytePosition() + 2, br)) return false;
        const int raw_bits = static_cast<int>(br.readBits(5));
        const int64_t total_bits = static_cast<int64_t>(raw_bits) * samples_in_partition;
        const size_t need = br.bytePosition() + static_cast<size_t>((total_bits + 7) / 8) + 1;
        if (!ensureRebind(need, br)) return false;
        br.skipBits(static_cast<size_t>(total_bits));
        continue;
      }

      // Unary quotient terminated by a 1 bit, then rice_param remainder bits.
      // Scanning the quotient 32 bits at a time needs a margin: one sample can
      // consume 32 + 1 + 30 bits, and bytePosition() rounds down, so 16 bytes
      // in reserve is the smallest that always covers it.
      for (int s = 0; s < samples_in_partition; s++) {
        uint32_t zeros = 0;
        for (;;) {
          if (br.bytePosition() + 16 > avail() &&
              !ensureRebind(br.bytePosition() + RESIDUAL_LOOKAHEAD, br)) {
            return false;
          }
          const auto lead = static_cast<uint32_t>(std::countl_zero(br.peekBits(32)));
          if (lead < 32) {
            br.skipBits(lead + 1);
            break;
          }
          // peekBits() zero-pads past the end, so a whole word of zeros at the
          // tail is padding rather than a quotient.
          if (br.bitsLeft() < 32) return false;
          br.skipBits(32);
          zeros += 32;
          if (zeros > 65536) return false;
        }
        br.skipBits(static_cast<size_t>(rice_param));
      }
      if (!br.ok()) return false;
    }
    return true;
  }

  auto readExact(void* dst, size_t n) -> size_t {
    return input_->read(std::span(static_cast<uint8_t*>(dst), n));
  }

  // Narrows [lo_pos, hi_pos) to the last frame starting at or before
  // target_sample, by probing file positions and reading the sample number out
  // of the first frame header at each one.
  //
  // Without this a file with no seek table can only be seeked by scanning every
  // frame from the start, which on a long hi-res stream means reading hundreds
  // of megabytes to jump to the middle.
  void narrowBySearch(int64_t target_sample, int64_t& lo_pos, int64_t& lo_sample,
                      int64_t hi_pos, int64_t hi_sample) {
    const int64_t granularity =
        std::max<int64_t>(SEARCH_GRANULARITY, 4 * static_cast<int64_t>(stream_info_.max_framesize));

    for (int probe = 0; hi_pos - lo_pos > granularity; probe++) {
      // A FLAC stream's bitrate is steady enough over a long window that
      // interpolating on the sample number usually lands within a frame or two,
      // where bisecting the same range would take a dozen reads. Every other
      // probe bisects anyway, so a stream that defeats the estimate still
      // converges in a bounded number of steps.
      int64_t guess = lo_pos + (hi_pos - lo_pos) / 2;
      if (probe % 2 == 0 && hi_sample > lo_sample && target_sample > lo_sample) {
        const double ratio = static_cast<double>(target_sample - lo_sample) /
                             static_cast<double>(hi_sample - lo_sample);
        guess = lo_pos + static_cast<int64_t>(static_cast<double>(hi_pos - lo_pos) * ratio);
        guess = std::clamp(guess, lo_pos + 1, hi_pos - 1);
      }

      if (!input_->seek(guess, Whence::BEG)) return;
      resetBuffer(guess);

      FLACFrameHeader header = {};
      if (!findNextFrameHeader(header)) {
        hi_pos = guess; // nothing parseable beyond here
        continue;
      }

      const int64_t sample = frameStartSample(header);
      if (sample <= target_sample) {
        lo_pos = read_origin_; // the header's own offset, not the probe point
        lo_sample = sample;
      } else {
        hi_pos = guess;
        hi_sample = sample;
      }
    }
  }

  // Advances frame by frame until the frame holding target_sample is at cur(),
  // ready for the next readPacket(). It stops one frame short on purpose: the
  // frame whose range covers the target is the one that has to be delivered.
  void seekScanSync(int64_t target_sample) {
    FLACFrameHeader header = {};

    while (current_sample_pos_ < target_sample) {
      if (!findNextFrameHeader(header)) return;

      current_sample_pos_ = frameStartSample(header);
      if (current_sample_pos_ + header.block_size > target_sample) return;

      // The body is stepped over rather than parsed. Every frame is at least
      // min_framesize bytes long, so skipping that far can never cross the next
      // sync word; the header size alone is the fallback that still guarantees
      // forward progress.
      size_t skip = static_cast<size_t>(header.size);
      const uint32_t min_framesize = stream_info_.min_framesize;
      if (min_framesize > skip && min_framesize <= READ_CHUNK) {
        skip = min_framesize;
      }
      if (!ensureBytes(skip)) return;
      consume(skip);

      current_sample_pos_ += header.block_size;
    }
  }
};

class FLACMuxer final : public BaseMuxer {
  int32_t audio_track_index_ = -1;
  int32_t image_track_index_ = -1;
  bool header_written_ = false;
  std::vector<uint8_t> cover_art_data_;
  OMCodecId cover_art_codec_ = OM_CODEC_NONE;
  uint32_t cover_art_width_ = 0;
  uint32_t cover_art_height_ = 0;
  uint64_t total_audio_samples_ = 0;
  int64_t streaminfo_offset_ = -1;
  std::vector<uint8_t> streaminfo_bytes_;
  uint32_t min_blocksize_ = 0xFFFF;
  uint32_t max_blocksize_ = 0;
  uint32_t min_framesize_ = 0xFFFFFF;
  uint32_t max_framesize_ = 0;

public:
  FLACMuxer() = default;
  ~FLACMuxer() override = default;

  auto open(std::unique_ptr<OutputStream> output) -> OMError override {
    output_ = std::move(output);
    if (!output_ || !output_->isValid()) {
      return OM_IO_INVALID_STREAM;
    }
    opened_ = true;
    finalized_ = false;
    resetState();
    tracks_.clear();
    return OM_SUCCESS;
  }

  void close() override {
    BaseMuxer::close();
    resetState();
  }

  auto addTrack(const Track& track) -> int32_t override {
    if (finalized_) return -1;

    Track stored = track;
    int32_t idx = static_cast<int32_t>(tracks_.size());
    stored.index = idx;

    if (stored.format.type == OM_MEDIA_AUDIO) {
      if (stored.format.codec_id != OM_CODEC_FLAC) {
        return -1;
      }
      if (audio_track_index_ >= 0) {
        return -1;
      }
      audio_track_index_ = idx;
      tracks_.push_back(std::move(stored));
      return idx;
    }

    if (stored.format.type == OM_MEDIA_IMAGE || stored.isCover()) {
      if (image_track_index_ >= 0) {
        return -1;
      }
      image_track_index_ = idx;
      cover_art_codec_ = stored.format.codec_id;
      cover_art_width_ = stored.format.video.width;
      cover_art_height_ = stored.format.video.height;
      if (!stored.extradata.empty()) {
        cover_art_data_ = stored.extradata;
      }
      tracks_.push_back(std::move(stored));
      return idx;
    }

    return -1;
  }

  auto writePacket(const Packet& packet) -> OMError override {
    if (!opened_ || finalized_ || tracks_.empty() || audio_track_index_ < 0) {
      return OM_COMMON_NOT_INITIALIZED;
    }

    if (image_track_index_ >= 0 && packet.stream_index == image_track_index_) {
      if (header_written_) {
        return OM_FORMAT_MUXING_FAILED;
      }
      cover_art_data_.assign(packet.bytes.begin(), packet.bytes.end());
      return OM_SUCCESS;
    }

    if (packet.stream_index != audio_track_index_) {
      return OM_FORMAT_STREAM_NOT_FOUND;
    }

    if (!header_written_) {
      auto err = writeHeader();
      if (err != OM_SUCCESS) return err;
    }

    if (packet.duration > 0) {
      total_audio_samples_ += static_cast<uint64_t>(packet.duration);
      uint32_t bs = static_cast<uint32_t>(packet.duration);
      if (bs < min_blocksize_) min_blocksize_ = bs;
      if (bs > max_blocksize_) max_blocksize_ = bs;
    }

    uint32_t fs = static_cast<uint32_t>(packet.bytes.size());
    if (fs < min_framesize_) min_framesize_ = fs;
    if (fs > max_framesize_) max_framesize_ = fs;

    size_t written = output_->write(packet.bytes);
    if (written != packet.bytes.size()) {
      return OM_IO_WRITE_FAILED;
    }

    return OM_SUCCESS;
  }

  auto finalize() -> OMError override {
    if (!opened_ || finalized_) {
      return OM_SUCCESS;
    }
    if (!header_written_) {
      auto err = writeHeader();
      if (err != OM_SUCCESS) return err;
    }

    if (streaminfo_offset_ >= 0 && streaminfo_bytes_.size() == 34 && output_->canSeek()) {
      uint8_t* si = streaminfo_bytes_.data();
      if (min_blocksize_ != 0xFFFF) {
        store_u16_be(si + 0, static_cast<uint16_t>(min_blocksize_));
        store_u16_be(si + 2, static_cast<uint16_t>(max_blocksize_));
      }
      if (min_framesize_ != 0xFFFFFF) {
        store_u24_be(si + 4, min_framesize_);
        store_u24_be(si + 7, max_framesize_);
      }
      if (total_audio_samples_ > 0) {
        constexpr uint64_t K_TOTAL_SAMPLES_MASK = 0xFFFFFFFFFull; // low 36 bits
        const uint64_t packed = (load_u64_be(si + 10) & ~K_TOTAL_SAMPLES_MASK) |
                                (total_audio_samples_ & K_TOTAL_SAMPLES_MASK);
        store_u64_be(si + 10, packed);
      }
      if (output_->seek(streaminfo_offset_, Whence::BEG)) {
        output_->write(streaminfo_bytes_);
        output_->seek(0, Whence::END);
      }
    }

    if (!output_->flush()) {
      return OM_IO_WRITE_FAILED;
    }

    finalized_ = true;
    return OM_SUCCESS;
  }

private:
  void resetState() {
    audio_track_index_ = -1;
    image_track_index_ = -1;
    header_written_ = false;
    cover_art_data_.clear();
    cover_art_codec_ = OM_CODEC_NONE;
    cover_art_width_ = 0;
    cover_art_height_ = 0;
    total_audio_samples_ = 0;
    streaminfo_offset_ = -1;
    streaminfo_bytes_.clear();
    min_blocksize_ = 0xFFFF;
    max_blocksize_ = 0;
    min_framesize_ = 0xFFFFFF;
    max_framesize_ = 0;
  }

  auto writeHeader() -> OMError {
    if (audio_track_index_ < 0) {
      return OM_COMMON_NOT_INITIALIZED;
    }
    const auto& track = tracks_[audio_track_index_];
    const bool has_picture = !cover_art_data_.empty() && pictureBodySize() <= K_MAX_BLOCK_SIZE;

    std::vector<uint8_t> header_bytes;

    if (track.extradata.size() >= 4 && std::memcmp(track.extradata.data(), "fLaC", 4) == 0) {
      header_bytes.assign(track.extradata.begin(), track.extradata.end());
      if (header_bytes.size() >= 42) {
        streaminfo_offset_ = 8;
        streaminfo_bytes_.assign(header_bytes.begin() + 8, header_bytes.begin() + 42);
      }
      if (has_picture) {
        bool found_picture = false;
        size_t pos = 4;
        size_t last_hdr_pos = 4;
        while (pos + 4 <= header_bytes.size()) {
          last_hdr_pos = pos;
          const bool is_last = (header_bytes[pos] & 0x80) != 0;
          const auto type = static_cast<FLACMetadataType>(header_bytes[pos] & 0x7F);
          if (type == FLACMetadataType::PICTURE) {
            found_picture = true;
          }
          pos += 4 + load_u24_be(header_bytes.data() + pos + 1);
          if (is_last) break;
        }
        if (!found_picture && last_hdr_pos + 4 <= header_bytes.size()) {
          header_bytes[last_hdr_pos] &= 0x7F;
          appendPictureBlock(header_bytes, true);
        }
      }
    } else {
      streaminfo_bytes_.assign(34, 0);
      if (track.extradata.size() == 34) {
        memcpy(streaminfo_bytes_.data(), track.extradata.data(), 34);
      } else {
        const uint32_t sr = track.format.audio.sample_rate ? track.format.audio.sample_rate : 44100;
        const uint32_t ch = track.format.audio.channels ? track.format.audio.channels : 2;
        const uint32_t bps = track.format.audio.bit_depth ? track.format.audio.bit_depth : 16;
        const uint64_t dur = track.duration > 0 ? static_cast<uint64_t>(track.duration) : 0;

        // sample_rate(20) | channels-1(3) | bits_per_sample-1(5) | total_samples(36)
        SpanBitWriter(std::span(streaminfo_bytes_).subspan(10, 8))
            .bits(sr & 0xFFFFF, 20)
            .bits((ch - 1) & 0x7, 3)
            .bits((bps - 1) & 0x1F, 5)
            .bits64(dur & 0xFFFFFFFFF, 36)
            .flush();
      }

      ByteWriter w(header_bytes);
      w.str("fLaC")
       .u8((has_picture ? 0x00 : 0x80) | static_cast<uint8_t>(FLACMetadataType::STREAMINFO))
       .u24be(34);
      streaminfo_offset_ = static_cast<int64_t>(w.size());
      w.bytes(streaminfo_bytes_);

      if (has_picture) {
        appendPictureBlock(header_bytes, true);
      }
    }

    size_t written = output_->write(header_bytes);
    if (written != header_bytes.size()) {
      return OM_IO_WRITE_FAILED;
    }

    header_written_ = true;
    return OM_SUCCESS;
  }

  static constexpr uint32_t K_MAX_BLOCK_SIZE = 0xFFFFFF; // 24-bit metadata block length

  auto pictureMime() const -> std::string_view {
    const bool is_png = cover_art_codec_ == OM_CODEC_PNG ||
                        (cover_art_data_.size() >= 8 && cover_art_data_[0] == 0x89 && cover_art_data_[1] == 'P');
    return is_png ? "image/png" : "image/jpeg";
  }

  auto pictureBodySize() const -> size_t {
    // 8 u32 fields + MIME string + picture data (description is empty).
    return 8 * 4 + pictureMime().size() + cover_art_data_.size();
  }

  // METADATA_BLOCK_PICTURE, RFC 9639 §8.8. Caller guarantees it fits K_MAX_BLOCK_SIZE.
  void appendPictureBlock(std::vector<uint8_t>& out, bool is_last) const {
    const std::string_view mime = pictureMime();
    ByteWriter w(out);
    w.u8((is_last ? 0x80 : 0x00) | static_cast<uint8_t>(FLACMetadataType::PICTURE))
     .u24be(static_cast<uint32_t>(pictureBodySize()))
     .u32be(3) // picture type: front cover
     .u32be(static_cast<uint32_t>(mime.size()))
     .str(mime)
     .u32be(0) // description length
     .u32be(cover_art_width_)
     .u32be(cover_art_height_)
     .u32be(24) // color depth
     .u32be(0)  // number of indexed colors
     .u32be(static_cast<uint32_t>(cover_art_data_.size()))
     .bytes(cover_art_data_);
  }
};

const FormatDescriptor FORMAT_FLAC = {
    .container_id = OM_CONTAINER_FLAC,
    .name = "flac",
    .long_name = "FLAC (Free Lossless Audio Codec)",
    .demuxer_factory = [] { return std::make_unique<FLACDemuxer>(); },
    .muxer_factory = [] { return std::make_unique<FLACMuxer>(); },
};

} // namespace openmedia
