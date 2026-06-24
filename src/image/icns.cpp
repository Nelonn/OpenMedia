#include <algorithm>
#include <array>
#include <cstring>
#include <openmedia/format_api.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/track.hpp>
#include <util/demuxer_base.hpp>
#include <util/io_util.hpp>

namespace openmedia {

static constexpr auto FOURCC(char a, char b, char c, char d) -> uint32_t {
  return (static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24) | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 8) | static_cast<uint32_t>(static_cast<uint8_t>(d));
}

// PNG / JPEG2000 icon types (modern, variable size, passthrough)
static constexpr uint32_t OST_IC07 = FOURCC('i', 'c', '0', '7');       // 128x128
static constexpr uint32_t OST_IC08 = FOURCC('i', 'c', '0', '8');       // 256x256
static constexpr uint32_t OST_IC09 = FOURCC('i', 'c', '0', '9');       // 512x512
static constexpr uint32_t OST_IC10 = FOURCC('i', 'c', '1', '0');       // 1024x1024 (or 512@2x)
static constexpr uint32_t OST_IC11 = FOURCC('i', 'c', '1', '1');       // 32x32 (16@2x)
static constexpr uint32_t OST_IC12 = FOURCC('i', 'c', '1', '2');       // 64x64 (32@2x)
static constexpr uint32_t OST_IC13 = FOURCC('i', 'c', '1', '3');       // 256x256 (128@2x)
static constexpr uint32_t OST_IC14 = FOURCC('i', 'c', '1', '4');       // 512x512 (256@2x)
static constexpr uint32_t OST_ICP4 = FOURCC('i', 'c', 'p', '4');       // 16x16
static constexpr uint32_t OST_ICP5 = FOURCC('i', 'c', 'p', '5');       // 32x32
static constexpr uint32_t OST_ICP6 = FOURCC('i', 'c', 'p', '6');       // 48x48
static constexpr uint32_t OST_ICSB_UPPER = FOURCC('i', 'c', 's', 'B'); // 36x36 retina
static constexpr uint32_t OST_SB24_UPPER = FOURCC('S', 'B', '2', '4'); // 48x48 retina

// ARGB types (ic04/ic05/icsb): 'ARGB' header + PackBits per channel
static constexpr uint32_t OST_IC04 = FOURCC('i', 'c', '0', '4'); // 16x16 ARGB
static constexpr uint32_t OST_IC05 = FOURCC('i', 'c', '0', '5'); // 32x32 ARGB
static constexpr uint32_t OST_ICSB = FOURCC('i', 'c', 's', 'b'); // 18x18 ARGB

// 24-bit RGB types: three PackBits-compressed channels (+ separate mask)
static constexpr uint32_t OST_IS32 = FOURCC('i', 's', '3', '2'); // 16x16
static constexpr uint32_t OST_IL32 = FOURCC('i', 'l', '3', '2'); // 32x32
static constexpr uint32_t OST_IH32 = FOURCC('i', 'h', '3', '2'); // 48x48
static constexpr uint32_t OST_IT32 = FOURCC('i', 't', '3', '2'); // 128x128

// 8-bit alpha mask types (paired with RGB types)
static constexpr uint32_t OST_S8MK = FOURCC('s', '8', 'm', 'k'); // 16x16 mask
static constexpr uint32_t OST_L8MK = FOURCC('l', '8', 'm', 'k'); // 32x32 mask
static constexpr uint32_t OST_H8MK = FOURCC('h', '8', 'm', 'k'); // 48x48 mask
static constexpr uint32_t OST_T8MK = FOURCC('t', '8', 'm', 'k'); // 128x128 mask

// 8-bit indexed (256-colour Mac palette)
static constexpr uint32_t OST_ICS8 = FOURCC('i', 'c', 's', '8'); // 16x16
static constexpr uint32_t OST_ICL8 = FOURCC('i', 'c', 'l', '8'); // 32x32
static constexpr uint32_t OST_ICH8 = FOURCC('i', 'c', 'h', '8'); // 48x48

// 4-bit indexed (16-colour Mac palette)
static constexpr uint32_t OST_ICS4 = FOURCC('i', 'c', 's', '4'); // 16x16
static constexpr uint32_t OST_ICL4 = FOURCC('i', 'c', 'l', '4'); // 32x32
static constexpr uint32_t OST_ICH4 = FOURCC('i', 'c', 'h', '4'); // 48x48
static constexpr uint32_t OST_ICM4 = FOURCC('i', 'c', 'm', '4'); // 16x12
static constexpr uint32_t OST_ICM8 = FOURCC('i', 'c', 'm', '8'); // 16x12

// 1-bit mono (+ mask packed together)
static constexpr uint32_t OST_ICN_HASH = FOURCC('I', 'C', 'N', '#'); // 32x32
static constexpr uint32_t OST_ICS_HASH = FOURCC('i', 'c', 's', '#'); // 16x16
static constexpr uint32_t OST_ICH_HASH = FOURCC('i', 'c', 'h', '#'); // 48x48
static constexpr uint32_t OST_ICM_HASH = FOURCC('i', 'c', 'm', '#'); // 16x12
static constexpr uint32_t OST_ICON = FOURCC('I', 'C', 'O', 'N');     // 32x32 no mask

// Non-image metadata types (skip)
static constexpr uint32_t OST_TOC = FOURCC('T', 'O', 'C', ' ');
static constexpr uint32_t OST_ICNV = FOURCC('i', 'c', 'n', 'V');
static constexpr uint32_t OST_NAME = FOURCC('n', 'a', 'm', 'e');
static constexpr uint32_t OST_INFO = FOURCC('i', 'n', 'f', 'o');
static constexpr uint32_t OST_SBTP = FOURCC('s', 'b', 't', 'p');
static constexpr uint32_t OST_SLCT = FOURCC('s', 'l', 'c', 't');

// PNG magic
static constexpr uint8_t PNG_SIG[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};

// 4-bit Mac palette (16 entries, RGB)
static constexpr uint8_t MAC_PALETTE_4[16][3] = {
    {0xFF, 0xFF, 0xFF},
    {0xFC, 0xF3, 0x05},
    {0xFF, 0x64, 0x02},
    {0xDD, 0x08, 0x06},
    {0xF2, 0x08, 0x84},
    {0x46, 0x00, 0xA5},
    {0x00, 0x00, 0xD4},
    {0x02, 0xAB, 0xEA},
    {0x1F, 0xB7, 0x14},
    {0x00, 0x64, 0x11},
    {0x56, 0x2C, 0x05},
    {0x90, 0x71, 0x3A},
    {0xC0, 0xC0, 0xC0},
    {0x80, 0x80, 0x80},
    {0x40, 0x40, 0x40},
    {0x00, 0x00, 0x00},
};

// 8-bit Mac palette — first 256 entries of the standard Mac System Palette.
// Only the first few and last few are distinctive; the bulk is a 6x6x6 cube.
// This is a compact generator rather than a 256-entry table.
static void mac8bppColor(uint8_t idx, uint8_t& r, uint8_t& g, uint8_t& b) {
  // The Mac 256-color system palette is defined by the original Quickdraw
  // color manager.  Indices 0–215 are a 6x6x6 RGB cube; 216–255 are grays
  // and special colors.  The exact mapping matches libicns / Apple sources.
  static constexpr uint8_t LEVELS[6] = {0xFF, 0xCC, 0x99, 0x66, 0x33, 0x00};
  if (idx <= 215) {
    uint8_t i = idx;
    b = LEVELS[i % 6];
    i /= 6;
    g = LEVELS[i % 6];
    i /= 6;
    r = LEVELS[i % 6];
  } else {
    // 216–255: 10-step gray ramp from white to black
    uint8_t step = idx - 216; // 0..39
    uint8_t v = static_cast<uint8_t>(255 - step * (255 / 39));
    r = g = b = v;
  }
}

// Decompress one channel of PackBits-encoded data.
// Returns false if the output buffer is not exactly filled.
static auto packbitsDecompress(
    const uint8_t* src, size_t src_len,
    uint8_t* dst, size_t dst_len) -> bool {
  size_t si = 0, di = 0;
  while (si < src_len && di < dst_len) {
    uint8_t n = src[si++];
    if (n < 0x80) {
      // Literal run: copy (n+1) bytes verbatim
      size_t count = static_cast<size_t>(n) + 1;
      if (si + count > src_len) return false;
      if (di + count > dst_len) count = dst_len - di; // clamp
      memcpy(dst + di, src + si, count);
      si += (static_cast<size_t>(n) + 1); // always advance full run in source
      di += count;
    } else {
      // Repeat run: repeat next byte (n - 0x80 + 3) times
      size_t count = static_cast<size_t>(n) - 0x80 + 3;
      if (si >= src_len) return false;
      uint8_t val = src[si++];
      size_t to_write = std::min(count, dst_len - di);
      memset(dst + di, val, to_write);
      di += count; // track even if clamped, for dst_len check
    }
  }
  return (di >= dst_len);
}

static auto isPNG(const uint8_t* data, size_t len) -> bool {
  return len >= 8 && memcmp(data, PNG_SIG, 8) == 0;
}

static auto isJP2(const uint8_t* data, size_t len) -> bool {
  static constexpr uint8_t JP2_SIG[12] = {
      0x00, 0x00, 0x00, 0x0C, 0x6A, 0x50, 0x20, 0x20, 0x0D, 0x0A, 0x87, 0x0A};
  return len >= 12 && memcmp(data, JP2_SIG, 12) == 0;
}

struct ElementInfo {
  uint32_t type = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  // higher = better when choosing the preferred element
  int priority = 0;
};

static auto elementInfo(uint32_t type) -> ElementInfo {
  switch (type) {
    // PNG/JPEG2000 – modern, lossless, highest quality
    case OST_IC10: return {type, 1024, 1024, 1000};
    case OST_IC09: return {type, 512, 512, 990};
    case OST_IC14: return {type, 512, 512, 985};
    case OST_IC08: return {type, 256, 256, 980};
    case OST_IC13: return {type, 256, 256, 975};
    case OST_IC07: return {type, 128, 128, 970};
    case OST_IC12: return {type, 64, 64, 960};
    case OST_ICP6: return {type, 48, 48, 950};
    case OST_IC11: return {type, 32, 32, 940};
    case OST_ICSB_UPPER: return {type, 36, 36, 935};
    case OST_ICP5: return {type, 32, 32, 930};
    case OST_SB24_UPPER: return {type, 48, 48, 925};
    case OST_ICP4: return {type, 16, 16, 920};
    // ARGB (PackBits)
    case OST_IC05: return {type, 32, 32, 810};
    case OST_ICSB: return {type, 18, 18, 800};
    case OST_IC04: return {type, 16, 16, 790};
    // 24-bit RGB (PackBits + separate mask)
    case OST_IT32: return {type, 128, 128, 700};
    case OST_IH32: return {type, 48, 48, 690};
    case OST_IL32: return {type, 32, 32, 680};
    case OST_IS32: return {type, 16, 16, 670};
    // 8-bit indexed
    case OST_ICH8: return {type, 48, 48, 500};
    case OST_ICL8: return {type, 32, 32, 490};
    case OST_ICS8: return {type, 16, 16, 480};
    case OST_ICM8: return {type, 16, 12, 470};
    // 4-bit indexed
    case OST_ICH4: return {type, 48, 48, 300};
    case OST_ICL4: return {type, 32, 32, 290};
    case OST_ICS4: return {type, 16, 16, 280};
    case OST_ICM4: return {type, 16, 12, 270};
    // 1-bit mono
    case OST_ICH_HASH: return {type, 48, 48, 100};
    case OST_ICN_HASH: return {type, 32, 32, 90};
    case OST_ICS_HASH: return {type, 16, 16, 80};
    case OST_ICM_HASH: return {type, 16, 12, 70};
    case OST_ICON: return {type, 32, 32, 60};
    default: return {type, 0, 0, -1}; // unknown / skip
  }
}

// Return the paired alpha-mask OSType for a given RGB OSType, or 0.
static auto maskTypeFor(uint32_t rgb_type) -> uint32_t {
  switch (rgb_type) {
    case OST_IS32: return OST_S8MK;
    case OST_IL32: return OST_L8MK;
    case OST_IH32: return OST_H8MK;
    case OST_IT32: return OST_T8MK;
    default: return 0;
  }
}

struct ParsedElement {
  uint32_t type = 0;
  uint64_t data_offset = 0; // offset of payload (after 8-byte element header)
  uint32_t data_len = 0;    // payload length (element total_len - 8)
};

class ICNSDemuxer final : public BaseDemuxer {
  // All parsed elements from the file (includes metadata chunks)
  std::vector<ParsedElement> all_elements_;

  // Subset of all_elements_ that are actual image-bearing entries
  // (priority >= 0), sorted by descending priority.  Track index N
  // corresponds to image_elements_[N].
  std::vector<ParsedElement> image_elements_;

  // Index of the next track whose packet has not yet been emitted.
  int next_track_idx_ = 0;

public:
  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    input_ = std::move(input);
    if (!input_ || !input_->isValid()) return OM_IO_INVALID_STREAM;

    uint8_t header[8];
    if (input_->read(header) < 8) return OM_IO_NOT_ENOUGH_DATA;

    if (header[0] != 'i' || header[1] != 'c' ||
        header[2] != 'n' || header[3] != 's') {
      return OM_FORMAT_PARSE_FAILED;
    }

    uint32_t file_size = load_u32_be(header + 4);
    if (file_size < 8) return OM_FORMAT_PARSE_FAILED;

    OMError scan_err = scanElements(file_size);
    if (scan_err != OM_SUCCESS) return scan_err;

    // Collect image-bearing elements and sort best-first so that
    // track 0 is always the highest-quality variant.
    for (const auto& pe : all_elements_) {
      ElementInfo info = elementInfo(pe.type);
      if (info.priority >= 0) {
        image_elements_.push_back(pe);
      }
    }

    if (image_elements_.empty()) return OM_FORMAT_PARSE_FAILED;

    std::stable_sort(
        image_elements_.begin(), image_elements_.end(),
        [](const ParsedElement& a, const ParsedElement& b) {
          return elementInfo(a.type).priority > elementInfo(b.type).priority;
        });

    for (size_t i = 0; i < image_elements_.size(); ++i) {
      const ParsedElement& pe = image_elements_[i];
      ElementInfo info = elementInfo(pe.type);

      Track track;
      track.index = static_cast<int32_t>(i);
      track.format.type = OM_MEDIA_IMAGE;
      track.format.codec_id = codecForElement(pe);
      track.time_base = {1, 1};
      track.duration = 1;
      track.format.image.width = info.width;
      track.format.image.height = info.height;
      tracks_.push_back(track);
    }

    return OM_SUCCESS;
  }

  auto readPacket() -> Result<Packet, OMError> override {
    if (next_track_idx_ >= static_cast<int>(image_elements_.size())) {
      return Err(OM_FORMAT_END_OF_FILE);
    }

    const int track_idx = next_track_idx_++;
    const ParsedElement& el = image_elements_[static_cast<size_t>(track_idx)];

    // Read raw element payload
    std::vector<uint8_t> raw(el.data_len);
    input_->seek(static_cast<int64_t>(el.data_offset), Whence::BEG);
    size_t bytes_read = input_->read({raw.data(), raw.size()});
    raw.resize(bytes_read);

    // Decode/convert based on element type
    std::vector<uint8_t> decoded;
    OMError err = decodeElement(el, raw, decoded);
    if (err != OM_SUCCESS) return Err(err);

    Packet pkt;
    pkt.allocate(decoded.size());
    memcpy(pkt.bytes.data(), decoded.data(), decoded.size());
    pkt.stream_index = track_idx;
    pkt.pos = static_cast<int64_t>(el.data_offset);
    pkt.pts = 0;
    pkt.dts = 0;
    pkt.is_keyframe = true;

    return Ok(std::move(pkt));
  }

  auto seek(int32_t /*stream_idx*/, int64_t timestamp, SeekMode /*mode*/)
      -> OMError override {
    // ICNS holds still images; the only meaningful seek is a rewind.
    if (timestamp <= 0) next_track_idx_ = 0;
    return OM_SUCCESS;
  }

private:
  auto scanElements(uint32_t file_size) -> OMError {
    uint64_t pos = 8; // already consumed the 8-byte file header
    while (pos + 8 <= file_size) {
      uint8_t elem_hdr[8];
      input_->seek(static_cast<int64_t>(pos), Whence::BEG);
      if (input_->read(elem_hdr) < 8) break;

      uint32_t type = load_u32_be(elem_hdr);
      uint32_t elem_len = load_u32_be(elem_hdr + 4);
      if (elem_len < 8) break; // corrupt

      ParsedElement pe;
      pe.type = type;
      pe.data_offset = pos + 8;
      pe.data_len = elem_len - 8;
      all_elements_.push_back(pe);

      pos += elem_len;
    }
    return all_elements_.empty() ? OM_FORMAT_PARSE_FAILED : OM_SUCCESS;
  }

  auto codecForElement(const ParsedElement& el) const -> OMCodecId {
    // Peek at the payload to distinguish PNG from JPEG2000/raw
    uint8_t peek[12] = {};
    input_->seek(static_cast<int64_t>(el.data_offset), Whence::BEG);
    input_->read({peek, sizeof(peek)});

    if (isPNG(peek, sizeof(peek))) return OM_CODEC_PNG;
    if (isJP2(peek, sizeof(peek))) return OM_CODEC_JPEG2000;

    // Everything else we will decode to raw RGBA
    return OM_CODEC_RAW_VIDEO;
  }

  // Find a sibling element by OSType
  auto findElement(uint32_t type) const -> const ParsedElement* {
    for (const auto& e : all_elements_) {
      if (e.type == type) return &e;
    }
    return nullptr;
  }

  auto readElementData(const ParsedElement& pe) const -> std::vector<uint8_t> {
    std::vector<uint8_t> buf(pe.data_len);
    input_->seek(static_cast<int64_t>(pe.data_offset), Whence::BEG);
    size_t n = input_->read({buf.data(), buf.size()});
    buf.resize(n);
    return buf;
  }

  // Write RGBA pixel at offset (x,y) into a flat RGBA buffer
  static void setPixel(std::span<uint8_t> rgba, uint32_t w,
                       uint32_t x, uint32_t y,
                       uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    size_t off = (size_t(y) * w + x) * 4;
    rgba[off + 0] = r;
    rgba[off + 1] = g;
    rgba[off + 2] = b;
    rgba[off + 3] = a;
  }

  // PackBits decompression wrapper
  static auto decompressChannel(const uint8_t* src, size_t src_avail,
                                uint8_t* dst, size_t dst_len) -> size_t {
    size_t si = 0, di = 0;
    while (si < src_avail && di < dst_len) {
      uint8_t n = src[si++];
      if (n < 0x80) {
        size_t count = size_t(n) + 1;
        if (si + count > src_avail) return 0;
        size_t to_copy = std::min(count, dst_len - di);
        memcpy(dst + di, src + si, to_copy);
        si += count;
        di += count;
      } else {
        size_t count = size_t(n) - 0x80 + 3;
        if (si >= src_avail) return 0;
        uint8_t val = src[si++];
        size_t to_write = std::min(count, dst_len - di);
        memset(dst + di, val, to_write);
        di += to_write;
      }
    }
    return si;
  }

  // Decoding dispatch
  auto decodeElement(const ParsedElement& el,
                     std::vector<uint8_t>& raw,
                     std::vector<uint8_t>& out) const -> OMError {
    // PNG / JPEG2000 passthrough
    if (isPNG(raw.data(), raw.size()) ||
        isJP2(raw.data(), raw.size())) {
      out = std::move(raw);
      return OM_SUCCESS;
    }

    switch (el.type) {
      case OST_ICP4:
      case OST_ICP5:
      case OST_ICP6:
      case OST_IC07:
      case OST_IC08:
      case OST_IC09:
      case OST_IC10:
      case OST_IC11:
      case OST_IC12:
      case OST_IC13:
      case OST_IC14:
      case OST_ICSB_UPPER:
      case OST_SB24_UPPER:
        // Should have been caught as PNG/JP2 above; pass through opaquely.
        out = std::move(raw);
        return OM_SUCCESS;

      case OST_IC04:
      case OST_IC05:
      case OST_ICSB:
        return decodeARGB(el, raw, out);

      case OST_IS32:
      case OST_IL32:
      case OST_IH32:
      case OST_IT32:
        return decodeRGB32(el, raw, out);

      case OST_ICS8:
      case OST_ICL8:
      case OST_ICH8:
      case OST_ICM8:
        return decode8bit(el, raw, out);

      case OST_ICS4:
      case OST_ICL4:
      case OST_ICH4:
      case OST_ICM4:
        return decode4bit(el, raw, out);

      case OST_ICN_HASH:
      case OST_ICS_HASH:
      case OST_ICH_HASH:
      case OST_ICM_HASH:
        return decode1bit(el, raw, out, /*has_mask=*/true);

      case OST_ICON:
        return decode1bit(el, raw, out, /*has_mask=*/false);

      default:
        out = std::move(raw);
        return OM_SUCCESS;
    }
  }

  // ARGB decoder (ic04, ic05, icsb)
  static auto decodeARGB(const ParsedElement& el,
                  std::span<const uint8_t> raw,
                  std::vector<uint8_t>& out) -> OMError {
    ElementInfo info = elementInfo(el.type);
    uint32_t w = info.width;
    uint32_t h = info.height;
    size_t n_pixels = static_cast<size_t>(w) * h;

    if (raw.size() < 4 || raw[0] != 'A' || raw[1] != 'R' ||
        raw[2] != 'G' || raw[3] != 'B') {
      return OM_FORMAT_PARSE_FAILED;
    }

    std::vector<uint8_t> ch_buf(n_pixels * 4);
    uint8_t* ch_a = ch_buf.data();
    uint8_t* ch_r = ch_buf.data() + n_pixels;
    uint8_t* ch_g = ch_buf.data() + n_pixels * 2;
    uint8_t* ch_b = ch_buf.data() + n_pixels * 3;

    const uint8_t* src = raw.data() + 4;
    size_t avail = raw.size() - 4;
    size_t consumed = 0;

    consumed = decompressChannel(src, avail, ch_a, n_pixels);
    if (!consumed) return OM_FORMAT_PARSE_FAILED;
    src += consumed; avail -= consumed;

    consumed = decompressChannel(src, avail, ch_r, n_pixels);
    if (!consumed) return OM_FORMAT_PARSE_FAILED;
    src += consumed; avail -= consumed;

    consumed = decompressChannel(src, avail, ch_g, n_pixels);
    if (!consumed) return OM_FORMAT_PARSE_FAILED;
    src += consumed; avail -= consumed;

    // Blue is last; tolerate partial decompression (known macOS 11 bug).
    decompressChannel(src, avail, ch_b, n_pixels);

    out.resize(n_pixels * 4);
    for (size_t i = 0; i < n_pixels; ++i) {
      out[i * 4 + 0] = ch_r[i];
      out[i * 4 + 1] = ch_g[i];
      out[i * 4 + 2] = ch_b[i];
      out[i * 4 + 3] = ch_a[i];
    }
    return OM_SUCCESS;
  }

  // 24-bit RGB decoder (is32, il32, ih32, it32 + 8mk mask)
  auto decodeRGB32(const ParsedElement& el,
                   std::span<const uint8_t> raw,
                   std::vector<uint8_t>& out) const -> OMError {
    ElementInfo info = elementInfo(el.type);
    uint32_t w = info.width;
    uint32_t h = info.height;
    size_t n_pixels = static_cast<size_t>(w) * h;

    const uint8_t* src = raw.data();
    size_t src_len = raw.size();

    // it32 starts with four ignorable zero bytes
    if (el.type == OST_IT32) {
      if (src_len < 4) return OM_FORMAT_PARSE_FAILED;
      src += 4;
      src_len -= 4;
    }

    std::vector<uint8_t> ch_r(n_pixels), ch_g(n_pixels), ch_b(n_pixels);
    size_t consumed = 0;

    consumed = decompressChannel(src, src_len, ch_r.data(), n_pixels);
    if (!consumed) return OM_FORMAT_PARSE_FAILED;
    src += consumed; src_len -= consumed;

    consumed = decompressChannel(src, src_len, ch_g.data(), n_pixels);
    if (!consumed) return OM_FORMAT_PARSE_FAILED;
    src += consumed; src_len -= consumed;

    consumed = decompressChannel(src, src_len, ch_b.data(), n_pixels);
    if (!consumed) return OM_FORMAT_PARSE_FAILED;

    std::vector<uint8_t> ch_a(n_pixels, 0xFF);
    uint32_t mask_type = maskTypeFor(el.type);
    if (mask_type) {
      const ParsedElement* mask_el = findElement(mask_type);
      if (mask_el && mask_el->data_len >= n_pixels) {
        std::vector<uint8_t> mask_raw = readElementData(*mask_el);
        if (mask_raw.size() >= n_pixels) {
          memcpy(ch_a.data(), mask_raw.data(), n_pixels);
        }
      }
    }

    out.resize(n_pixels * 4);
    for (size_t i = 0; i < n_pixels; ++i) {
      out[i * 4 + 0] = ch_r[i];
      out[i * 4 + 1] = ch_g[i];
      out[i * 4 + 2] = ch_b[i];
      out[i * 4 + 3] = ch_a[i];
    }
    return OM_SUCCESS;
  }

  // 8-bit indexed decoder
  static auto decode8bit(const ParsedElement& el,
                         std::span<const uint8_t> raw,
                         std::vector<uint8_t>& out) -> OMError {
    ElementInfo info = elementInfo(el.type);
    uint32_t w = info.width;
    uint32_t h = info.height;
    size_t n_pixels = static_cast<size_t>(w) * h;

    if (raw.size() < n_pixels) return OM_FORMAT_PARSE_FAILED;

    out.resize(n_pixels * 4);
    for (size_t i = 0; i < n_pixels; ++i) {
      uint8_t r, g, b;
      mac8bppColor(raw[i], r, g, b);
      out[i * 4 + 0] = r;
      out[i * 4 + 1] = g;
      out[i * 4 + 2] = b;
      out[i * 4 + 3] = 0xFF;
    }
    return OM_SUCCESS;
  }

  // 4-bit indexed decoder
  static auto decode4bit(const ParsedElement& el,
                         std::span<const uint8_t> raw,
                         std::vector<uint8_t>& out) -> OMError {
    ElementInfo info = elementInfo(el.type);
    uint32_t w = info.width;
    uint32_t h = info.height;
    size_t n_pixels = static_cast<size_t>(w) * h;
    size_t n_bytes = (n_pixels + 1) / 2;

    if (raw.size() < n_bytes) return OM_FORMAT_PARSE_FAILED;

    out.resize(n_pixels * 4);
    for (size_t i = 0; i < n_pixels; ++i) {
      uint8_t byte = raw[i / 2];
      uint8_t nibble = (i % 2 == 0) ? (byte >> 4) : (byte & 0x0F);
      const uint8_t* rgb = MAC_PALETTE_4[nibble];
      out[i * 4 + 0] = rgb[0];
      out[i * 4 + 1] = rgb[1];
      out[i * 4 + 2] = rgb[2];
      out[i * 4 + 3] = 0xFF;
    }
    return OM_SUCCESS;
  }

  // 1-bit mono decoder
  static auto decode1bit(const ParsedElement& el,
                         std::span<const uint8_t> raw,
                         std::vector<uint8_t>& out,
                         bool has_mask) -> OMError {
    ElementInfo info = elementInfo(el.type);
    uint32_t w = info.width;
    uint32_t h = info.height;
    size_t n_pixels = static_cast<size_t>(w) * h;
    size_t row_bytes = (w + 7) / 8;
    size_t plane_bytes = row_bytes * h;

    size_t needed = has_mask ? plane_bytes * 2 : plane_bytes;
    if (raw.size() < needed) return OM_FORMAT_PARSE_FAILED;

    out.resize(n_pixels * 4);
    for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
        size_t byte_idx = y * row_bytes + x / 8;
        uint8_t bit_mask = static_cast<uint8_t>(0x80) >> (x % 8);

        bool pixel_black = (raw[byte_idx] & bit_mask) != 0;
        uint8_t col = pixel_black ? 0x00 : 0xFF;

        uint8_t alpha = 0xFF;
        if (has_mask) {
          bool opaque = (raw[plane_bytes + byte_idx] & bit_mask) != 0;
          alpha = opaque ? 0xFF : 0x00;
        }

        setPixel(out, w, x, y, col, col, col, alpha);
      }
    }
    return OM_SUCCESS;
  }
};

const FormatDescriptor FORMAT_ICNS = {
    .container_id = OM_CONTAINER_ICNS,
    .name = "icns",
    .long_name = "Apple ICNS",
    .demuxer_factory = [] { return std::make_unique<ICNSDemuxer>(); },
    .muxer_factory = {},
};

} // namespace openmedia
