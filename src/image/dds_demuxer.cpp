#include <algorithm>
#include <cstring>
#include <cstdint>
#define TINYDDSLOADER_IMPLEMENTATION
#include <tinyddsloader.h>
#include <util/demuxer_base.hpp>
#include <util/io_util.hpp>
#include <openmedia/format_api.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/track.hpp>
#include <vector>

namespace openmedia {

namespace {

using DXGI = tinyddsloader::DDSFile::DXGIFormat;

auto codecForFormat(DXGI fmt) -> OMCodecId {
  switch (fmt) {
    case DXGI::BC1_Typeless:
    case DXGI::BC1_UNorm:
    case DXGI::BC1_UNorm_SRGB:
      return OM_CODEC_BC1;
    case DXGI::BC2_Typeless:
    case DXGI::BC2_UNorm:
    case DXGI::BC2_UNorm_SRGB:
      return OM_CODEC_BC2;
    case DXGI::BC3_Typeless:
    case DXGI::BC3_UNorm:
    case DXGI::BC3_UNorm_SRGB:
      return OM_CODEC_BC3;
    case DXGI::BC4_Typeless:
    case DXGI::BC4_UNorm:
    case DXGI::BC4_SNorm:
      return OM_CODEC_BC4;
    case DXGI::BC5_Typeless:
    case DXGI::BC5_UNorm:
    case DXGI::BC5_SNorm:
      return OM_CODEC_BC5;
    case DXGI::BC6H_Typeless:
    case DXGI::BC6H_UF16:
    case DXGI::BC6H_SF16:
      return OM_CODEC_BC6H;
    case DXGI::BC7_Typeless:
    case DXGI::BC7_UNorm:
    case DXGI::BC7_UNorm_SRGB:
      return OM_CODEC_BC7;
    default:
      return OM_CODEC_RAW_VIDEO;
  }
}

auto isHDRFormat(DXGI fmt) -> bool {
  switch (fmt) {
    case DXGI::R16G16B16A16_Float:
    case DXGI::R16G16B16A16_UNorm:
    case DXGI::R16G16B16A16_Typeless:
    case DXGI::R32G32B32A32_Float:
    case DXGI::R32G32B32A32_Typeless:
    case DXGI::R9G9B9E5_SHAREDEXP:
      return true;
    default:
      return false;
  }
}

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

// Shared exponent RGB9E5 -> three halfs.
// V = (m / 512) * 2^(E - 24); half mantissa = m << 1, biased half exponent = E - 9.
auto decodeSharedExp(const uint8_t* src, uint16_t out[3]) -> void {
  const uint32_t bits = load_u32_le(src);
  const int32_t e = static_cast<int32_t>((bits >> 27) & 0x1F);
  const uint32_t exponent = static_cast<uint32_t>(std::clamp(e - 9, 0, 30)) << 10;
  out[0] = static_cast<uint16_t>(exponent | (((bits >> 18) & 0x1FF) << 1));
  out[1] = static_cast<uint16_t>(exponent | (((bits >> 9) & 0x1FF) << 1));
  out[2] = static_cast<uint16_t>(exponent | ((bits & 0x1FF) << 1));
}

auto decodeUncompressed(const tinyddsloader::DDSFile::ImageData& img, DXGI fmt, std::vector<uint8_t>& out) -> bool {
  const uint32_t w = img.m_width;
  const uint32_t h = img.m_height;
  const uint8_t* src = static_cast<const uint8_t*>(img.m_mem);

  if (isHDRFormat(fmt)) {
    out.resize(static_cast<size_t>(w) * h * 8);
    for (uint32_t y = 0; y < h; y++) {
      const uint8_t* row = src + static_cast<size_t>(y) * img.m_memPitch;
      uint16_t* dst = reinterpret_cast<uint16_t*>(out.data() + static_cast<size_t>(y) * w * 8);
      switch (fmt) {
        case DXGI::R16G16B16A16_Float:
        case DXGI::R16G16B16A16_UNorm:
        case DXGI::R16G16B16A16_Typeless:
          memcpy(dst, row, static_cast<size_t>(w) * 8);
          break;
        case DXGI::R32G32B32A32_Float:
        case DXGI::R32G32B32A32_Typeless:
          for (uint32_t x = 0; x < w; x++) {
            const float* f = reinterpret_cast<const float*>(row + static_cast<size_t>(x) * 16);
            dst[x * 4 + 0] = halfFromFloat(f[0]);
            dst[x * 4 + 1] = halfFromFloat(f[1]);
            dst[x * 4 + 2] = halfFromFloat(f[2]);
            dst[x * 4 + 3] = halfFromFloat(f[3]);
          }
          break;
        case DXGI::R9G9B9E5_SHAREDEXP:
          for (uint32_t x = 0; x < w; x++) {
            uint16_t rgb[3];
            decodeSharedExp(row + static_cast<size_t>(x) * 4, rgb);
            dst[x * 4 + 0] = rgb[0];
            dst[x * 4 + 1] = rgb[1];
            dst[x * 4 + 2] = rgb[2];
            dst[x * 4 + 3] = 0x3C00;
          }
          break;
        default:
          return false;
      }
    }
    return true;
  }

  out.resize(static_cast<size_t>(w) * h * 4);
  for (uint32_t y = 0; y < h; y++) {
    const uint8_t* row = src + static_cast<size_t>(y) * img.m_memPitch;
    uint8_t* dst = out.data() + static_cast<size_t>(y) * w * 4;

    switch (fmt) {
      case DXGI::R8G8B8A8_Typeless:
      case DXGI::R8G8B8A8_UNorm:
      case DXGI::R8G8B8A8_UNorm_SRGB:
      case DXGI::R8G8B8A8_UInt:
      case DXGI::R8G8B8A8_SNorm:
      case DXGI::R8G8B8A8_SInt:
        memcpy(dst, row, static_cast<size_t>(w) * 4);
        break;
      case DXGI::B8G8R8A8_Typeless:
      case DXGI::B8G8R8A8_UNorm:
      case DXGI::B8G8R8A8_UNorm_SRGB:
        for (uint32_t x = 0; x < w; x++) {
          dst[x * 4 + 0] = row[x * 4 + 2];
          dst[x * 4 + 1] = row[x * 4 + 1];
          dst[x * 4 + 2] = row[x * 4 + 0];
          dst[x * 4 + 3] = row[x * 4 + 3];
        }
        break;
      case DXGI::B8G8R8X8_Typeless:
      case DXGI::B8G8R8X8_UNorm:
      case DXGI::B8G8R8X8_UNorm_SRGB:
        for (uint32_t x = 0; x < w; x++) {
          dst[x * 4 + 0] = row[x * 4 + 2];
          dst[x * 4 + 1] = row[x * 4 + 1];
          dst[x * 4 + 2] = row[x * 4 + 0];
          dst[x * 4 + 3] = 0xFF;
        }
        break;
      case DXGI::R8G8_Typeless:
      case DXGI::R8G8_UNorm:
      case DXGI::R8G8_UInt:
      case DXGI::R8G8_SNorm:
      case DXGI::R8G8_SInt:
        for (uint32_t x = 0; x < w; x++) {
          dst[x * 4 + 0] = row[x * 2 + 0];
          dst[x * 4 + 1] = row[x * 2 + 1];
          dst[x * 4 + 2] = 0;
          dst[x * 4 + 3] = 0xFF;
        }
        break;
      case DXGI::R8_Typeless:
      case DXGI::R8_UNorm:
      case DXGI::R8_UInt:
      case DXGI::R8_SNorm:
      case DXGI::R8_SInt:
        for (uint32_t x = 0; x < w; x++) {
          dst[x * 4 + 0] = row[x];
          dst[x * 4 + 1] = row[x];
          dst[x * 4 + 2] = row[x];
          dst[x * 4 + 3] = 0xFF;
        }
        break;
      case DXGI::A8_UNorm:
        for (uint32_t x = 0; x < w; x++) {
          dst[x * 4 + 0] = 0;
          dst[x * 4 + 1] = 0;
          dst[x * 4 + 2] = 0;
          dst[x * 4 + 3] = row[x];
        }
        break;
      case DXGI::R10G10B10A2_UNorm:
      case DXGI::R10G10B10A2_Typeless:
      case DXGI::R10G10B10A2_UInt:
        for (uint32_t x = 0; x < w; x++) {
          const uint32_t p = load_u32_le(row + static_cast<size_t>(x) * 4);
          dst[x * 4 + 0] = static_cast<uint8_t>(((p >> 0) & 0x3FF) * 255 / 1023);
          dst[x * 4 + 1] = static_cast<uint8_t>(((p >> 10) & 0x3FF) * 255 / 1023);
          dst[x * 4 + 2] = static_cast<uint8_t>(((p >> 20) & 0x3FF) * 255 / 1023);
          dst[x * 4 + 3] = static_cast<uint8_t>(((p >> 30) & 0x3) * 255 / 3);
        }
        break;
      default:
        return false;
    }
  }
  return true;
}

} // namespace

class DDSDemuxer final : public BaseDemuxer {
  std::vector<uint8_t> raw_;
  DXGI format_ = DXGI::Unknown;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  bool packet_read_ = false;

public:
  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    input_ = std::move(input);
    if (!input_ || !input_->isValid()) {
      return OM_IO_INVALID_STREAM;
    }

    const int64_t size = input_->size();
    if (size < 0) {
      return OM_IO_INVALID_STREAM;
    }
    raw_.resize(static_cast<size_t>(size));
    if (size > 0 && input_->read({raw_.data(), raw_.size()}) < static_cast<size_t>(size)) {
      return OM_IO_NOT_ENOUGH_DATA;
    }

    tinyddsloader::DDSFile dds;
    const tinyddsloader::Result result = dds.Load(raw_.data(), raw_.size());
    if (result != tinyddsloader::Result::Success) {
      return OM_FORMAT_PARSE_FAILED;
    }

    if (dds.GetTextureDimension() != tinyddsloader::DDSFile::TextureDimension::Texture2D) {
      // Only 2D textures map cleanly onto OpenMedia's single-image model.
      return OM_FORMAT_NOT_SUPPORTED;
    }

    width_ = dds.GetWidth();
    height_ = dds.GetHeight();
    format_ = dds.GetFormat();

    if (width_ == 0 || height_ == 0) {
      return OM_FORMAT_PARSE_FAILED;
    }

    Track track;
    track.index = 0;
    track.format.type = OM_MEDIA_IMAGE;
    track.format.codec_id = codecForFormat(format_);
    track.time_base = {1, 1};
    track.duration = 1;
    track.format.image.width = width_;
    track.format.image.height = height_;
    if (track.format.codec_id == OM_CODEC_RAW_VIDEO) {
      track.format.image.format = isHDRFormat(format_) ? OM_FORMAT_RGBA64 : OM_FORMAT_R8G8B8A8;
    }
    tracks_.push_back(track);

    return OM_SUCCESS;
  }

  auto readPacket() -> Result<Packet, OMError> override {
    if (packet_read_) {
      return Err(OM_FORMAT_END_OF_FILE);
    }
    packet_read_ = true;

    Packet pkt;
    pkt.stream_index = 0;
    pkt.pos = 0;
    pkt.pts = 0;
    pkt.dts = 0;
    pkt.is_keyframe = true;

    if (tinyddsloader::DDSFile::IsCompressed(format_)) {
      // Compressed: hand the container bytes to the BC decoder.
      pkt.allocate(raw_.size());
      if (!raw_.empty()) {
        memcpy(pkt.bytes.data(), raw_.data(), raw_.size());
      }
      pkt.bytes = pkt.bytes.subspan(0, raw_.size());
      return Ok(std::move(pkt));
    }

    // Uncompressed: the packet is already the decoded image, no codec involved.
    tinyddsloader::DDSFile dds;
    if (dds.Load(raw_.data(), raw_.size()) != tinyddsloader::Result::Success) {
      return Err(OM_FORMAT_PARSE_FAILED);
    }
    const tinyddsloader::DDSFile::ImageData* img = dds.GetImageData(0, 0);
    if (!img || !img->m_mem) {
      return Err(OM_FORMAT_PARSE_FAILED);
    }

    std::vector<uint8_t> decoded;
    if (!decodeUncompressed(*img, format_, decoded)) {
      return Err(OM_FORMAT_NOT_SUPPORTED);
    }
    pkt.allocate(decoded.size());
    if (!decoded.empty()) {
      memcpy(pkt.bytes.data(), decoded.data(), decoded.size());
    }
    pkt.bytes = pkt.bytes.subspan(0, decoded.size());
    return Ok(std::move(pkt));
  }

  auto seek(int32_t /*stream_idx*/, int64_t timestamp, SeekMode /*mode*/) -> OMError override {
    if (timestamp <= 0) {
      packet_read_ = false;
    }
    return OM_SUCCESS;
  }
};

const FormatDescriptor FORMAT_DDS = {
    .container_id = OM_CONTAINER_DDS,
    .name = "dds",
    .long_name = "DDS (DirectDraw Surface)",
    .demuxer_factory = [] { return std::make_unique<DDSDemuxer>(); },
    .muxer_factory = {},
};

} // namespace openmedia
