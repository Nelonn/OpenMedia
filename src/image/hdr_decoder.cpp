#include <algorithm>
#include <cmath>
#include <cstring>
#include <codecs.hpp>
#include <image/hdr_common.hpp>
#include <openmedia/video.hpp>
#include <vector>

namespace openmedia {

namespace {

// IEEE 754 binary32 -> binary16 (round-to-nearest-even, portable).
auto halfFromFloat(float value) -> uint16_t {
  uint32_t x;
  memcpy(&x, &value, 4);

  const uint32_t sign = (x >> 16) & 0x8000u;
  const uint32_t mantissa = x & 0x7FFFFFu;
  const uint32_t exponent = (x >> 23) & 0xFFu;

  if (exponent == 0xFFu) {
    return static_cast<uint16_t>(sign | 0x7C00u | (mantissa ? (mantissa >> 13) | 0x200u : 0u));
  }

  int32_t e = static_cast<int32_t>(exponent) - 127 + 15;
  if (e >= 0x1F) {
    return static_cast<uint16_t>(sign | 0x7C00u);
  }
  if (e <= 0) {
    if (e < -10) {
      return static_cast<uint16_t>(sign);
    }
    uint32_t m = mantissa | 0x800000u;
    const uint32_t shift = static_cast<uint32_t>(14 - e);
    uint32_t h = m >> shift;
    if ((m >> (shift - 1)) & 1u) {
      h++;
    }
    return static_cast<uint16_t>(sign | h);
  }
  return static_cast<uint16_t>(sign | static_cast<uint32_t>(e) << 10 | (mantissa >> 13));
}

// Decode one scanline of RGBE pixels into scan (w * 4 bytes). Handles both the
// new scanline RLE and the old flat layout, detecting RLE from the header bytes.
auto readScanline(const uint8_t* data, size_t size, size_t& pos, uint32_t w,
                  uint8_t* scan) -> bool {
  if (pos + 4 > size) return false;

  const bool rle = data[pos] == 2 && data[pos + 1] == 2 &&
                   ((static_cast<uint32_t>(data[pos + 2]) << 8) | data[pos + 3]) == w;
  if (!rle) {
    if (pos + static_cast<size_t>(w) * 4 > size) return false;
    memcpy(scan, data + pos, static_cast<size_t>(w) * 4);
    pos += static_cast<size_t>(w) * 4;
    return true;
  }

  pos += 4;
  std::vector<uint8_t> comps(static_cast<size_t>(w) * 4);
  for (uint32_t c = 0; c < 4; c++) {
    uint8_t* comp = comps.data() + static_cast<size_t>(c) * w;
    uint32_t n = 0;
    while (n < w) {
      if (pos >= size) return false;
      const uint8_t count = data[pos++];
      if (count > 128) {
        if (pos >= size) return false;
        const uint8_t value = data[pos++];
        const uint32_t run = static_cast<uint32_t>(count - 128);
        for (uint32_t i = 0; i < run && n < w; i++) comp[n++] = value;
      } else {
        if (pos + count > size) return false;
        for (uint32_t i = 0; i < count && n < w; i++) comp[n++] = data[pos++];
      }
    }
  }

  for (uint32_t x = 0; x < w; x++) {
    for (uint32_t c = 0; c < 4; c++) {
      scan[x * 4 + c] = comps[static_cast<size_t>(c) * w + x];
    }
  }
  return true;
}

// Decode RGBE/XYZE pixels into tightly packed RGBA64 (half floats).
auto decodeRadiance(const uint8_t* data, size_t size, const hdr::Header& hdr,
                    std::vector<uint8_t>& out) -> bool {
  const uint32_t w = hdr.width;
  const uint32_t h = hdr.height;
  if (w == 0 || h == 0) return false;
  if (w > 65535) return false; // RLE width is 16-bit

  out.resize(static_cast<size_t>(w) * h * 8);
  size_t pos = hdr.data_offset;
  std::vector<uint8_t> scan(static_cast<size_t>(w) * 4);

  for (uint32_t y = 0; y < h; y++) {
    if (!readScanline(data, size, pos, w, scan.data())) return false;

    const uint32_t dst_y = hdr.flip_vertical ? (h - 1 - y) : y;
    uint16_t* dst = reinterpret_cast<uint16_t*>(out.data() + static_cast<size_t>(dst_y) * w * 8);

    for (uint32_t x = 0; x < w; x++) {
      const uint32_t sx = hdr.flip_horizontal ? (w - 1 - x) : x;
      const uint8_t* px = scan.data() + static_cast<size_t>(sx) * 4;

      float r, g, b;
      if (hdr.is_xyze) {
        const float scale = std::ldexp(1.0f, static_cast<int>(px[3]) - 136);
        const float X = static_cast<float>(px[0]) * scale;
        const float Y = static_cast<float>(px[1]) * scale;
        const float Z = static_cast<float>(px[2]) * scale;
        r = 3.2406f * X - 1.5372f * Y - 0.4986f * Z;
        g = -0.9689f * X + 1.8758f * Y + 0.0415f * Z;
        b = 0.0557f * X - 0.2040f * Y + 1.0570f * Z;
      } else {
        const float scale = std::ldexp(1.0f, static_cast<int>(px[3]) - 136);
        r = static_cast<float>(px[0]) * scale;
        g = static_cast<float>(px[1]) * scale;
        b = static_cast<float>(px[2]) * scale;
      }

      dst[x * 4 + 0] = halfFromFloat(r);
      dst[x * 4 + 1] = halfFromFloat(g);
      dst[x * 4 + 2] = halfFromFloat(b);
      dst[x * 4 + 3] = 0x3C00; // alpha = 1.0 half
    }
  }
  return true;
}

} // namespace

class RGBEDecoder final : public Decoder {
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  bool initialized_ = false;

public:
  RGBEDecoder() = default;
  ~RGBEDecoder() override = default;

  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_RGBE) {
      return OM_CODEC_INVALID_PARAMS;
    }
    width_ = options.format.image.width;
    height_ = options.format.image.height;
    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!initialized_) return std::nullopt;

    DecodingInfo info;
    info.media_type = OM_MEDIA_IMAGE;
    info.video_format = {OM_FORMAT_RGBA64, width_, height_};
    return info;
  }

  void flush() override {}

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    std::vector<Frame> frames;

    if (packet.bytes.empty() || packet.bytes.size() < 9) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    hdr::Header h;
    if (!hdr::parseHeader(packet.bytes.data(), packet.bytes.size(), h)) {
      return Err(OM_CODEC_DECODE_FAILED);
    }
    width_ = h.width;
    height_ = h.height;

    std::vector<uint8_t> pixels;
    if (!decodeRadiance(packet.bytes.data(), packet.bytes.size(), h, pixels)) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    Picture pic(OM_FORMAT_RGBA64, width_, height_);
    for (uint32_t y = 0; y < height_; y++) {
      memcpy(pic.planes.data[0] + static_cast<size_t>(y) * pic.planes.linesize[0],
             pixels.data() + static_cast<size_t>(y) * width_ * 8,
             static_cast<size_t>(width_) * 8);
    }

    Frame frame;
    frame.pts = packet.pts;
    frame.dts = packet.dts;
    frame.data = std::move(pic);
    frames.push_back(std::move(frame));

    return Ok(std::move(frames));
  }
};

const CodecDescriptor CODEC_RGBE = {
  .codec_id = OM_CODEC_RGBE,
  .type = OM_MEDIA_IMAGE,
  .name = "rgbe",
  .long_name = "RGBE (Radiance HDR) decoder",
  .vendor = "OpenMedia",
  .flags = NONE,
  .decoder_factory = [] { return std::make_unique<RGBEDecoder>(); },
};

} // namespace openmedia
