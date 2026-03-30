#include <array>
#include <cassert>
#include <cstring>
#include <functional>
#include <future>
#include <openmedia/format_api.hpp>
#include <openmedia/packet.hpp>
#include <util/demuxer_base.hpp>
#include <util/io_util.hpp>
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

struct FLACPicture {
  std::vector<uint8_t> cover_art;
  uint32_t width = 0;
  uint32_t height = 0;
};

static auto crc8(const uint8_t* data, size_t len) -> uint8_t {
  static constexpr auto s_table = []() {
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
  for (size_t i = 0; i < len; i++) crc = s_table[crc ^ data[i]];
  return crc;
}

static auto crc16(const uint8_t* data, size_t len) -> uint16_t {
  static constexpr auto s_table = []() {
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
  for (size_t i = 0; i < len; i++)
    crc = static_cast<uint16_t>((crc << 8) ^ s_table[(crc >> 8) ^ data[i]]);
  return crc;
}

static auto decodeUtf8ExtraBytes(uint8_t b) -> int {
  if ((b & 0x80) == 0x00)
    return 0;
  else if ((b & 0xE0) == 0xC0)
    return 1;
  else if ((b & 0xF0) == 0xE0)
    return 2;
  else if ((b & 0xF8) == 0xF0)
    return 3;
  else if ((b & 0xFC) == 0xF8)
    return 4;
  else if ((b & 0xFE) == 0xFC)
    return 5;
  else if (b == 0xFE)
    return 6;
  return -1;
}

static auto decodeUtf8Number(const uint8_t* data, size_t& pos, size_t max_len) -> uint64_t {
  if (pos >= max_len) return 0;
  uint8_t b = data[pos++];
  int extra = decodeUtf8ExtraBytes(b);
  if (extra < 0 || pos + extra > max_len) return 0;

  uint64_t v = 0;
  if (extra == 0)
    v = b;
  else if (extra == 1)
    v = b & 0x1F;
  else if (extra == 2)
    v = b & 0x0F;
  else if (extra == 3)
    v = b & 0x07;
  else if (extra == 4)
    v = b & 0x03;
  else if (extra == 5)
    v = b & 0x01;

  for (int i = 0; i < extra; i++)
    v = (v << 6) | (data[pos++] & 0x3F);
  return v;
}

struct FLACSeekPoint {
  uint64_t sample_number;
  uint64_t stream_offset;
  uint16_t num_samples;
};

static auto parseStreamInfo(const std::vector<uint8_t>& body, FLACStreamInfo& stream) -> bool {
  if (body.size() != 34) return false;
  const uint8_t* data = body.data();

  stream.min_blocksize = load_u16_be(data + 0);
  stream.max_blocksize = load_u16_be(data + 2);
  stream.min_framesize = read_u24_be(data + 4);
  stream.max_framesize = read_u24_be(data + 7);

  uint64_t packed = 0;
  for (int i = 10; i < 18; ++i) {
    packed = (packed << 8) | data[i];
  }

  stream.sample_rate = static_cast<uint32_t>((packed >> 44) & 0xFFFFF);
  stream.channels = static_cast<uint32_t>(((packed >> 41) & 0x7) + 1);
  stream.bits_per_sample = static_cast<uint32_t>(((packed >> 36) & 0x1F) + 1);
  stream.total_samples = packed & 0xFFFFFFFFF;

  std::memcpy(stream.md5sum, data + 18, 16);
  return true;
}

static void parseSeektable(const std::vector<uint8_t>& body,
                           std::vector<FLACSeekPoint>& seek_table) {
  const size_t point_size = 18;
  const size_t num_points = body.size() / point_size;
  seek_table.reserve(seek_table.size() + num_points);
  for (size_t i = 0; i < num_points; i++) {
    const uint8_t* p = body.data() + i * point_size;
    uint64_t sample_number = load_u64_be(p);
    if (sample_number == UINT64_MAX) continue;
    uint64_t stream_offset = load_u64_be(p + 8);
    uint16_t num_samples = load_u16_be(p + 16);
    seek_table.push_back({sample_number, stream_offset, num_samples});
  }
}

static void parsePicture(const std::vector<uint8_t>& body, FLACPicture& picture) {
  if (body.size() < 8) return;
  size_t pos = 0;

  auto read_be32 = [&]() -> uint32_t {
    if (pos + 4 > body.size()) return 0;
    uint32_t v = load_u32_be(body.data() + pos);
    pos += 4;
    return v;
  };

  uint32_t pic_type = read_be32();
  uint32_t mime_len = read_be32();
  pos += mime_len;
  if (pos + 4 > body.size()) return;
  uint32_t desc_len = read_be32();
  pos += desc_len;
  if (pos + 20 > body.size()) return;
  uint32_t width = read_be32();
  uint32_t height = read_be32();
  /* color_depth */ read_be32();
  /* color_count */ read_be32();
  uint32_t data_len = read_be32();
  if (pos + data_len > body.size()) return;

  if (pic_type == 3 && data_len > 0) {
    picture.cover_art.assign(body.data() + pos, body.data() + pos + data_len);
    picture.width = width;
    picture.height = height;
  }
}

static void parseVorbisComment(const std::vector<uint8_t>& body) {
  if (body.size() < 4) return;
}

struct FLACFrameInfo {
  int64_t number = 0;
  bool is_sample_number = false;
  int block_size = 0;
  int channel_assignment = 0;
};

static auto parseHeaderLen(const uint8_t* data, size_t avail) -> int32_t {
  if (avail < 6) return -1;
  if (data[0] != 0xFF || (data[1] & 0xFE) != 0xF8) return -1;

  size_t pos = 4; // sync(2) + block_size/sr hints(1) + ch/bps/reserved(1)

  if (pos >= avail) return -1;
  int extra = decodeUtf8ExtraBytes(data[pos++]);
  if (extra < 0) return -1;
  pos += static_cast<size_t>(extra);

  uint8_t bs_hint = (data[2] >> 4) & 0x0F;
  if (bs_hint == 0x06) {
    pos += 1;
  } else if (bs_hint == 0x07) {
    pos += 2;
  }

  uint8_t sr_hint = data[2] & 0x0F;
  if (sr_hint == 0x0C) {
    pos += 1;
  } else if (sr_hint == 0x0D || sr_hint == 0x0E) {
    pos += 2;
  }

  pos += 1; // CRC-8

  if (pos > avail) return -1;
  return static_cast<int32_t>(pos);
}

class FLACDemuxer final : public BaseDemuxer {
  int64_t audio_data_offset_ = 0;
  int64_t current_sample_pos_ = 0;
  int64_t current_frame_index_ = 0;
  bool is_sequential_ = true;

  FLACStreamInfo stream_info_ = {};

  std::vector<FLACSeekPoint> seek_points_;
  FLACPicture cover_art_;

  std::vector<uint8_t> read_buf_;
  int64_t read_buf_origin_ = 0;

public:
  auto open(std::unique_ptr<InputStream> input) -> OMError override {
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
        size_t tag_size = ((size_t) (id3hdr[2] & 0x7F) << 21) |
                          ((size_t) (id3hdr[3] & 0x7F) << 14) |
                          ((size_t) (id3hdr[4] & 0x7F) << 7) |
                          (size_t) (id3hdr[5] & 0x7F);
        tag_size += 10;
        if (id3hdr[1] & 0x10) tag_size += 10;
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

    std::vector<uint8_t> extradata(marker, marker + 4);

    bool found_streaminfo = false;

    while (true) {
      uint8_t block_header[4];
      if (readExact(block_header, 4) != 4) {
        return OM_IO_NOT_ENOUGH_DATA;
      }

      bool is_last = (block_header[0] & 0x80) != 0;
      auto block_type = static_cast<FLACMetadataType>(block_header[0] & 0x7F);
      uint32_t body_len =
          (static_cast<uint32_t>(block_header[1]) << 16) |
          (static_cast<uint32_t>(block_header[2]) << 8) |
          static_cast<uint32_t>(block_header[3]);

      extradata.insert(extradata.end(), block_header, block_header + 4);

      std::vector<uint8_t> body(body_len);
      if (body_len > 0) {
        if (readExact(body.data(), body_len) != body_len) {
          return OM_IO_NOT_ENOUGH_DATA;
        }
        extradata.insert(extradata.end(), body.begin(), body.end());
      }

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
          parseVorbisComment(body);
          break;
        case FLACMetadataType::PICTURE:
          parsePicture(body, cover_art_);
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

    tracks_.push_back(track);
    return OM_SUCCESS;
  }

  auto readPacket() -> Result<Packet, OMError> override {
    return scanAndDeliverFrame();
  }

  // -----------------------------------------------------------------------
  // seek()
  //
  // timestamp_ns is in the stream's time_base units (samples), not
  // nanoseconds — the name is inherited from the base class interface.
  //
  // Strategy:
  //   1. Convert the target timestamp to a sample number.
  //   2. If a seek table exists, jump to the best seek point whose
  //      sample_number <= target (stream_offset is relative to
  //      audio_data_offset_).
  //   3. If no seek table (or the best point is sample 0), seek the
  //      underlying stream to audio_data_offset_.
  //   4. Reset the read buffer and its origin to match the new stream
  //      position.
  //   5. Call seekScanSync() to advance frame-by-frame until
  //      current_sample_pos_ reaches the frame that contains the target.
  // -----------------------------------------------------------------------
  auto seek(int64_t timestamp_us, SeekMode mode) -> OMError override {
    is_sequential_ = false;

    // Convert nanoseconds -> samples.
    const uint64_t sr = stream_info_.sample_rate;
    const uint64_t ts = timestamp_us < 0 ? 0 : static_cast<uint64_t>(timestamp_us);
    const uint64_t seconds = ts / 1'000'000;
    const uint64_t micros = ts % 1'000'000;
    const uint64_t samples_from_seconds = seconds * sr;
    const uint64_t samples_from_micros = (micros * sr) / 1'000'000;
    const int64_t target_sample = static_cast<int64_t>(samples_from_seconds + samples_from_micros);

    // Always discard buffered data — it belongs to the old position.
    read_buf_.clear();
    read_buf_origin_ = 0;

    if (target_sample <= 0) {
      current_sample_pos_ = 0;
      current_frame_index_ = 0;
      if (!input_->seek(audio_data_offset_, Whence::BEG)) {
        return OM_IO_SEEK_FAILED;
      }
      read_buf_origin_ = audio_data_offset_;
      return OM_SUCCESS;
    }

    // ---- Use seek table if available ------------------------------------
    int64_t seek_file_pos = audio_data_offset_; // absolute file position
    int64_t seek_sample_pos = 0;                // sample at that position

    if (!seek_points_.empty()) {
      // Walk forward while the next point is still <= target.
      // seek_points_ is ordered by sample_number ascending.
      for (const auto& sp : seek_points_) {
        if (static_cast<int64_t>(sp.sample_number) <= target_sample) {
          seek_file_pos = audio_data_offset_ +
                          static_cast<int64_t>(sp.stream_offset);
          seek_sample_pos = static_cast<int64_t>(sp.sample_number);
        } else {
          break;
        }
      }
    }

    // ---- Position the underlying stream --------------------------------
    if (!input_->seek(seek_file_pos, Whence::BEG)) {
      return OM_IO_SEEK_FAILED;
    }
    read_buf_origin_ = seek_file_pos;
    current_sample_pos_ = seek_sample_pos;
    current_frame_index_ = 0; // will be corrected by seekScanSync

    // ---- Scan forward frame-by-frame to the target ---------------------
    //
    // seekScanSync() advances current_sample_pos_ by each frame's
    // block_size and stops as soon as the *next* frame would overshoot
    // target_sample, i.e. the current frame is the one containing the
    // target.  The read buffer is left pointing at the start of that
    // frame so scanAndDeliverFrame() can emit it immediately.
    seekScanSync(target_sample);

    return OM_SUCCESS;
  }

private:

  // =======================================================================
  // ensureBytes — grow read_buf_ until it holds at least `needed` bytes.
  //
  // IMPORTANT: because read_buf_ may reallocate its internal storage when
  // it grows, callers that hold a MemoryBitReader over read_buf_.data()
  // MUST call br.repoint(read_buf_.data(), read_buf_.size()) after every
  // call to ensureBytes().  The helper ensureAndRepoint() below does this
  // automatically.
  // =======================================================================
  auto ensureBytes(size_t needed) -> bool {
    while (read_buf_.size() < needed) {
      uint8_t tmp[8192];
      size_t n = input_->read(tmp);
      if (n == 0) return false;
      read_buf_.insert(read_buf_.end(), tmp, tmp + n);
    }
    return true;
  }

  // Ensure + repoint in one call.  Returns false on EOF before `needed`.
  auto ensureAndRepoint(size_t needed, MemoryBitReader& br) -> bool {
    if (read_buf_.size() >= needed) {
      // Buffer is already large enough; still repoint in case a previous
      // ensureBytes caused a reallocation that the caller hasn't seen yet.
      br.repoint(read_buf_.data(), read_buf_.size());
      return true;
    }
    bool ok = ensureBytes(needed);
    // Always repoint — the buffer may have reallocated even on failure.
    br.repoint(read_buf_.data(), read_buf_.size());
    return ok;
  }

  auto scanAndDeliverFrame() -> Result<Packet, OMError> {
    // ------------------------------------------------------------------
    // Phase 1: locate a sync word and validate the frame-header CRC-8.
    // ------------------------------------------------------------------
    size_t scan_pos = 0;

    while (true) {
      if (!ensureBytes(scan_pos + 2)) return Err(OM_FORMAT_END_OF_FILE);

      if (read_buf_[scan_pos] != 0xFF ||
          (read_buf_[scan_pos + 1] & 0xFE) != 0xF8) {
        ++scan_pos;
        continue;
      }

      // Candidate sync at scan_pos.  Need enough bytes for the maximum
      // frame header (16 bytes).
      if (!ensureBytes(scan_pos + 16)) {
        if (!ensureBytes(scan_pos + 4)) return Err(OM_FORMAT_END_OF_FILE);
      }

      size_t avail = read_buf_.size() - scan_pos;
      int hdr_len = parseHeaderLen(read_buf_.data() + scan_pos, avail);
      if (hdr_len < 0) {
        ++scan_pos;
        continue;
      }

      uint8_t expected_crc = crc8(read_buf_.data() + scan_pos,
                                  static_cast<size_t>(hdr_len - 1));
      if (expected_crc != read_buf_[scan_pos + hdr_len - 1]) {
        ++scan_pos;
        continue;
      }

      break; // valid header found at scan_pos
    }

    size_t frame_start_in_buf = scan_pos;

    // ------------------------------------------------------------------
    // Phase 2: parse the frame header fields.
    // ------------------------------------------------------------------
    FLACFrameInfo info;
    {
      const uint8_t* hdr = read_buf_.data() + frame_start_in_buf;
      size_t avail = read_buf_.size() - frame_start_in_buf;
      if (!parseFrameHeaderFields(hdr, avail, info)) return Err(OM_FORMAT_PARSE_FAILED);
    }

    int ch_assignment = info.channel_assignment;
    bool has_side = (ch_assignment >= 8 && ch_assignment <= 10);

    // ------------------------------------------------------------------
    // Phase 3: bit-accurate subframe consumption.
    //
    // Key invariant: MemoryBitReader always points at read_buf_.data()
    // and preserves its bit position across buffer growth via repoint().
    // We never call br.init() again after the initial setup — only
    // repoint() is used when the buffer grows.
    // ------------------------------------------------------------------
    int hdr_len = parseHeaderLen(read_buf_.data() + frame_start_in_buf,
                                 read_buf_.size() - frame_start_in_buf);
    if (hdr_len < 0) return Err(OM_FORMAT_PARSE_FAILED);

    size_t subframe_start_byte = frame_start_in_buf + static_cast<size_t>(hdr_len);

    // Ensure we have a reasonable initial window before starting.
    if (!ensureBytes(subframe_start_byte + 64)) {
      if (read_buf_.size() <= subframe_start_byte) return Err(OM_FORMAT_PARSE_FAILED);
    }

    MemoryBitReader br;
    br.init(read_buf_.data(), read_buf_.size(), subframe_start_byte);

    // ensureAndRepoint wrapper that updates br after any buffer growth.
    auto ensure = [&](size_t needed) -> bool {
      return ensureAndRepoint(needed, br);
    };

    int rc = consumeSubframes(br, ensure,
                              stream_info_.channels, info.block_size,
                              stream_info_.bits_per_sample, has_side, ch_assignment);
    if (rc < 0) {
      // False positive — step over this sync byte and retry.
      read_buf_.erase(read_buf_.begin(),
                      read_buf_.begin() + frame_start_in_buf + 1);
      return scanAndDeliverFrame();
    }

    // ------------------------------------------------------------------
    // Phase 4: align to byte boundary, then consume the 2-byte CRC-16.
    // ------------------------------------------------------------------
    // br.bitPos() is the exact bit position after the last subframe bit.
    // Align to the next byte boundary.
    br.alignToByte();
    size_t aligned_byte = br.bytePos();         // first byte after subframe data
    size_t frame_end_in_buf = aligned_byte + 2; // +2 for CRC-16

    if (!ensure(frame_end_in_buf)) {
      if (read_buf_.size() < aligned_byte) return Err(OM_FORMAT_PARSE_FAILED);
      frame_end_in_buf = read_buf_.size();
    }

    // ------------------------------------------------------------------
    // Phase 4b: verify CRC-16.
    // ------------------------------------------------------------------
    if (frame_end_in_buf >= frame_start_in_buf + 2) {
      size_t crc_data_len = frame_end_in_buf - frame_start_in_buf - 2;
      uint16_t computed = crc16(read_buf_.data() + frame_start_in_buf, crc_data_len);
      const uint8_t* crc_bytes = read_buf_.data() + frame_start_in_buf + crc_data_len;
      uint16_t stored = static_cast<uint16_t>(
          (static_cast<uint16_t>(crc_bytes[0]) << 8) | crc_bytes[1]);

      if (computed != stored) {
        read_buf_.erase(read_buf_.begin(),
                        read_buf_.begin() + frame_start_in_buf + 1);
        return scanAndDeliverFrame();
      }
    }

    // ------------------------------------------------------------------
    // Phase 5: emit the packet.
    // ------------------------------------------------------------------
    size_t frame_size = frame_end_in_buf - frame_start_in_buf;

    Packet pkt;
    pkt.allocate(frame_size);
    std::memcpy(pkt.bytes.data(), read_buf_.data() + frame_start_in_buf, frame_size);

    pkt.stream_index = 0;
    pkt.pos = read_buf_origin_ + static_cast<int64_t>(frame_start_in_buf);
    pkt.pts = pkt.dts = current_sample_pos_;
    pkt.duration = info.block_size;

    current_sample_pos_ += info.block_size;
    current_frame_index_ += 1;

    read_buf_.erase(read_buf_.begin(),
                    read_buf_.begin() + frame_end_in_buf);
    read_buf_origin_ += static_cast<int64_t>(frame_end_in_buf);

    return Ok(std::move(pkt));
  }

  // =======================================================================
  // Bit-accurate subframe consumer (RFC 9639 §10.2 – §10.2.4).
  //
  // CONTRACT: `ensure(n)` must call br.repoint() internally so that br's
  // data pointer stays valid after any buffer reallocation.  The caller
  // (scanAndDeliverFrame) sets this up via the lambda above.
  //
  // We NEVER call br.init() here — only the `ensure` callback may change
  // the buffer and it always repoints br before returning.
  //
  // Returns 0 on success, -1 on parse error.
  // =======================================================================
  auto consumeSubframes(MemoryBitReader& br,
                        const std::function<bool(size_t)>& ensure,
                        int channels, int block_size,
                        int sample_bits, bool has_side, int ch_assignment) -> int {
    for (int ch = 0; ch < channels; ch++) {
      int bps = sample_bits;
      if (has_side) {
        bool is_side =
            (ch_assignment == 8 && ch == 1) ||
            (ch_assignment == 9 && ch == 0) ||
            (ch_assignment == 10 && ch == 1);
        if (is_side) bps++;
      }

      // ---- Subframe header (RFC 9639 §10.2) -------------------------
      // Need at least 1 byte for the subframe header.
      // We are always byte-aligned at this point (the frame header
      // ends on a byte boundary, and each subframe ends on a bit
      // boundary that we align after the last subframe).
      if (!ensure(br.bytePos() + 2)) return -1;

      uint32_t sf_hdr = br.readBits(8);
      if (sf_hdr & 0x80) return -1; // reserved bit must be zero

      int type = static_cast<int>((sf_hdr >> 1) & 0x3F);

      int wasted_bits = 0;
      if (sf_hdr & 1) {
        // Unary-coded wasted bits: consume 0-bits until stop bit 1.
        // The number of wasted bits = zero_count + 1.
        int k = 0;
        while (true) {
          // Grow the buffer if we are near its edge mid-unary.
          if (br.bytePos() + 2 > read_buf_.size()) {
            if (!ensure(br.bytePos() + 64)) return -1;
          }
          if (br.readBits(1) != 0) break;
          if (++k > 30) return -1;
        }
        wasted_bits = k + 1;
      }
      bps -= wasted_bits;
      if (bps <= 0) return -1;

      // ---- Subframe data (RFC 9639 §10.2.1 – §10.2.4) ---------------
      if (type == 0) {
        // SUBFRAME_CONSTANT
        size_t need = br.bytePos() + static_cast<size_t>((bps + 7) / 8) + 1;
        if (!ensure(need)) return -1;
        br.skipBits(bps);

      } else if (type == 1) {
        // SUBFRAME_VERBATIM
        int64_t verbatim_bits = static_cast<int64_t>(bps) * block_size;
        size_t verbatim_bytes = static_cast<size_t>((verbatim_bits + 7) / 8);
        if (!ensure(br.bytePos() + verbatim_bytes + 1)) return -1;
        br.skipBits(verbatim_bits);

      } else if (type >= 8 && type <= 12) {
        // SUBFRAME_FIXED
        int order = type - 8;
        if (order > block_size) return -1;
        int64_t warmup_bits = static_cast<int64_t>(bps) * order;
        if (!ensure(br.bytePos() + static_cast<size_t>((warmup_bits + 7) / 8) + 16))
          return -1;
        br.skipBits(warmup_bits);
        if (consumeResidual(br, ensure, block_size, order) < 0) return -1;

      } else if (type >= 32 && type <= 63) {
        // SUBFRAME_LPC
        int order = type - 31;
        if (order > block_size) return -1;
        int64_t warmup_bits = static_cast<int64_t>(bps) * order;
        if (!ensure(br.bytePos() + static_cast<size_t>((warmup_bits + 7) / 8) + 32))
          return -1;
        br.skipBits(warmup_bits);
        int qlp_prec = static_cast<int>(br.readBits(4)) + 1;
        br.skipBits(5); // qlp_shift
        int64_t coeff_bits = static_cast<int64_t>(qlp_prec) * order;
        if (!ensure(br.bytePos() + static_cast<size_t>((coeff_bits + 7) / 8) + 16))
          return -1;
        br.skipBits(coeff_bits);
        if (consumeResidual(br, ensure, block_size, order) < 0) return -1;

      } else {
        return -1; // reserved subframe type
      }
    }
    return 0;
  }

  // =======================================================================
  // Rice-coded residual consumer (RFC 9639 §10.2.5).
  // Returns 0 on success, -1 on error.
  // =======================================================================
  auto consumeResidual(MemoryBitReader& br,
                       const std::function<bool(size_t)>& ensure,
                       int block_size, int predictor_order) -> int {
    if (!ensure(br.bytePos() + 2)) return -1;

    int coding_method = static_cast<int>(br.readBits(2));
    if (coding_method > 1) return -1;
    int partition_order = static_cast<int>(br.readBits(4));
    int num_partitions = 1 << partition_order;
    int rice_param_bits = (coding_method == 0) ? 4 : 5;
    int escape_value = (coding_method == 0) ? 15 : 31;

    for (int p = 0; p < num_partitions; p++) {
      if (!ensure(br.bytePos() + 4)) return -1;

      int rice_param = static_cast<int>(br.readBits(rice_param_bits));

      int samples_in_partition;
      if (partition_order == 0) {
        samples_in_partition = block_size - predictor_order;
      } else if (p == 0) {
        samples_in_partition = (block_size >> partition_order) - predictor_order;
      } else {
        samples_in_partition = block_size >> partition_order;
      }
      if (samples_in_partition < 0) return -1;

      if (rice_param == escape_value) {
        // Escaped: 5 bits of raw_bits, then raw_bits per sample.
        if (!ensure(br.bytePos() + 2)) return -1;
        int raw_bits = static_cast<int>(br.readBits(5));
        int64_t total_bits = static_cast<int64_t>(raw_bits) * samples_in_partition;
        size_t need_bytes = br.bytePos() + static_cast<size_t>((total_bits + 7) / 8) + 1;
        if (!ensure(need_bytes)) return -1;
        br.skipBits(total_bits);

      } else {
        // Standard Rice: unary quotient (stop-bit = 1) + rice_param remainder bits.
        for (int s = 0; s < samples_in_partition; s++) {
          // Grow buffer on demand while reading the unary prefix.
          int zeros = 0;
          while (true) {
            if (br.bytePos() + 8 > read_buf_.size()) {
              if (!ensure(br.bytePos() + 4096)) return -1;
            }
            if (br.readBits(1) != 0) break;
            if (++zeros > 65536) return -1;
          }
          if (rice_param > 0) {
            if (br.bytePos() + 8 > read_buf_.size()) {
              if (!ensure(br.bytePos() + 4096)) return -1;
            }
            br.skipBits(rice_param);
          }
        }
      }
    }
    return 0;
  }

  auto parseFrameHeaderFields(const uint8_t* data, size_t avail,
                              FLACFrameInfo& info) const -> bool {
    int hdr_len = parseHeaderLen(data, avail);
    if (hdr_len < 0) return false;

    info.is_sample_number = (data[1] & 0x01) != 0;
    info.channel_assignment = (data[3] >> 4) & 0x0F;
    info.block_size = static_cast<int>(decodeBlockSizeFromHeader(data, avail));
    if (info.block_size <= 0) return false;

    size_t pos = 4;
    info.number = static_cast<int64_t>(decodeUtf8Number(data, pos, avail));
    return true;
  }

  auto decodeBlockSizeFromHeader(const uint8_t* data, size_t avail) const -> int64_t {
    if (avail < 3) return 0;
    uint8_t hint = (data[2] >> 4) & 0x0F;
    switch (hint) {
      case 0x00: return stream_info_.min_blocksize;
      case 0x01: return 192;
      case 0x02: return 576;
      case 0x03: return 1152;
      case 0x04: return 2304;
      case 0x05: return 4608;
      case 0x08: return 256;
      case 0x09: return 512;
      case 0x0A: return 1024;
      case 0x0B: return 2048;
      case 0x0C: return 4096;
      case 0x0D: return 8192;
      case 0x0E: return 16384;
      case 0x0F: return 32768;
      default: break;
    }
    size_t pos = 4;
    if (pos >= avail) return 0;
    int extra = decodeUtf8ExtraBytes(data[pos++]);
    if (extra < 0) return 0;
    pos += static_cast<size_t>(extra);

    if (hint == 0x06 && pos < avail)
      return static_cast<int64_t>(data[pos]) + 1;
    if (hint == 0x07 && pos + 1 < avail)
      return (static_cast<int64_t>(data[pos]) << 8 | data[pos + 1]) + 1;
    return 0;
  }

  auto readExact(void* dst, size_t n) -> size_t {
    return input_->read(std::span(static_cast<uint8_t*>(dst), n));
  }

  // -----------------------------------------------------------------------
  // seekScanSync — advance frame-by-frame until the frame that *contains*
  // target_sample is at the front of read_buf_.
  //
  // We stop when advancing by one more frame would push current_sample_pos_
  // past target_sample, meaning the current frame is the right one to
  // deliver first.
  //
  // All positions are in samples (same unit as current_sample_pos_ and
  // target_sample).  read_buf_origin_ is kept in sync with every
  // consume/trim of read_buf_.
  //
  // Bug fixes vs. original:
  //   - Comparison is now sample vs. sample (was sample vs. ns).
  //   - After a valid frame is found and consumed, the entire frame is
  //     erased from read_buf_ (not just 2 bytes), so we advance correctly.
  //   - current_frame_index_ is incremented for each consumed frame.
  // -----------------------------------------------------------------------
  void seekScanSync(int64_t target_sample) {
    while (current_sample_pos_ < target_sample) {

      // Read ahead in large chunks, not 2 bytes at a time.
      constexpr size_t K_CHUNK = 65536;
      if (read_buf_.size() < 16) {
        if (!ensureBytes(K_CHUNK)) return;
      }

      // Scan for sync word.
      size_t sync_pos = read_buf_.size(); // sentinel = not found
      for (size_t i = 0; i + 1 < read_buf_.size(); i++) {
        if (read_buf_[i] == 0xFF && (read_buf_[i + 1] & 0xFE) == 0xF8) {
          sync_pos = i;
          break;
        }
      }

      if (sync_pos == read_buf_.size()) {
        // No sync found — discard all but last byte, fetch more.
        size_t keep = read_buf_.empty() ? 0 : 1;
        read_buf_origin_ += static_cast<int64_t>(read_buf_.size() - keep);
        if (keep)
          read_buf_ = {read_buf_.back()};
        else
          read_buf_.clear();
        if (!ensureBytes(K_CHUNK)) return;
        continue;
      }

      // Discard bytes before sync.
      if (sync_pos > 0) {
        read_buf_origin_ += static_cast<int64_t>(sync_pos);
        read_buf_.erase(read_buf_.begin(), read_buf_.begin() + sync_pos);
      }

      // Need full header to validate.
      if (!ensureBytes(16)) return;

      int hdr_len = parseHeaderLen(read_buf_.data(), read_buf_.size());
      if (hdr_len < 0) {
        // False sync — skip this byte.
        read_buf_origin_++;
        read_buf_.erase(read_buf_.begin(), read_buf_.begin() + 1);
        continue;
      }

      if (!ensureBytes(static_cast<size_t>(hdr_len))) return;
      uint8_t expected_crc = crc8(read_buf_.data(), static_cast<size_t>(hdr_len - 1));
      if (expected_crc != read_buf_[static_cast<size_t>(hdr_len - 1)]) {
        // CRC-8 mismatch — false sync, skip one byte.
        read_buf_origin_++;
        read_buf_.erase(read_buf_.begin(), read_buf_.begin() + 1);
        continue;
      }

      int64_t block_size = decodeBlockSizeFromHeader(read_buf_.data(), read_buf_.size());
      if (block_size <= 0) {
        read_buf_origin_++;
        read_buf_.erase(read_buf_.begin(), read_buf_.begin() + 1);
        continue;
      }

      // Is this the frame containing target_sample?
      if (current_sample_pos_ + block_size > target_sample) {
        break; // Leave this frame at front of read_buf_ for delivery.
      }

      // This frame is before the target. Skip the whole frame.
      // Use min_framesize as a fast-forward hint if available.
      uint32_t min_fs = stream_info_.min_framesize;
      if (min_fs > 0 && static_cast<size_t>(hdr_len) + min_fs <= read_buf_.size()) {
        // Skip at least min_framesize bytes from the frame start,
        // past the header, so we don't re-match this sync word.
        size_t skip = static_cast<size_t>(hdr_len) + min_fs;
        // But don't skip past a potential next sync — just advance past
        // the header so the scan loop can't re-find this sync word.
        // Actually: skip hdr_len so we land INSIDE the frame body,
        // guaranteeing the next scan won't re-match this sync.
        read_buf_origin_ += static_cast<int64_t>(skip);
        read_buf_.erase(read_buf_.begin(), read_buf_.begin() + skip);
      } else {
        // Skip just past the header to enter the frame body.
        read_buf_origin_ += static_cast<int64_t>(hdr_len);
        read_buf_.erase(read_buf_.begin(), read_buf_.begin() + hdr_len);
      }

      current_sample_pos_ += block_size;
      current_frame_index_ += 1;
    }
  }
};

const FormatDescriptor FORMAT_FLAC = {
    .container_id = OM_CONTAINER_FLAC,
    .name = "flac",
    .long_name = "FLAC (Free Lossless Audio Codec)",
    .demuxer_factory = [] { return std::make_unique<FLACDemuxer>(); },
    .muxer_factory = {},
};

} // namespace openmedia
