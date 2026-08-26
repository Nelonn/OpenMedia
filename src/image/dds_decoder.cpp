#define BCDEC_IMPLEMENTATION
#include <bcdec.h>

#include <algorithm>
#include <cstring>
#include <codecs.hpp>
#include <openmedia/video.hpp>
#include <cstdint>
#include <tinyddsloader.h>
#include <util/io_util.hpp>
#include <vector>

namespace openmedia {

namespace {

using DXGI = tinyddsloader::DDSFile::DXGIFormat;

auto isSignedFormat(DXGI fmt) -> bool {
  return fmt == DXGI::BC4_SNorm || fmt == DXGI::BC5_SNorm || fmt == DXGI::BC6H_SF16;
}

auto isBC6H(DXGI fmt) -> bool {
  return fmt == DXGI::BC6H_Typeless || fmt == DXGI::BC6H_UF16 || fmt == DXGI::BC6H_SF16;
}

// Decompress a single BC mip level (mip 0, array/face 0) into the picture.
auto decodeCompressedMip(const tinyddsloader::DDSFile::ImageData& img, DXGI fmt, Picture& pic) -> bool {
  const uint32_t w = img.m_width;
  const uint32_t h = img.m_height;
  const uint32_t bw = (w + 3) / 4;
  const uint32_t bh = (h + 3) / 4;

  const bool hdr = isBC6H(fmt);
  const uint8_t* src_base = static_cast<const uint8_t*>(img.m_mem);

  const bool sixteen_byte =
      fmt == DXGI::BC2_Typeless || fmt == DXGI::BC2_UNorm || fmt == DXGI::BC2_UNorm_SRGB ||
      fmt == DXGI::BC3_Typeless || fmt == DXGI::BC3_UNorm || fmt == DXGI::BC3_UNorm_SRGB ||
      fmt == DXGI::BC5_Typeless || fmt == DXGI::BC5_UNorm || fmt == DXGI::BC5_SNorm ||
      fmt == DXGI::BC6H_Typeless || fmt == DXGI::BC6H_UF16 || fmt == DXGI::BC6H_SF16 ||
      fmt == DXGI::BC7_Typeless || fmt == DXGI::BC7_UNorm || fmt == DXGI::BC7_UNorm_SRGB;

  for (uint32_t by = 0; by < bh; by++) {
    const uint32_t base_y = by * 4;
    for (uint32_t bx = 0; bx < bw; bx++) {
      const uint32_t base_x = bx * 4;
      const size_t block_offset = static_cast<size_t>(by) * img.m_memPitch + static_cast<size_t>(bx) * (sixteen_byte ? 16u : 8u);
      const uint8_t* block = src_base + block_offset;

      if (hdr) {
        uint16_t tmp[16 * 3];
        bcdec_bc6h_half(block, tmp, 4 * 3, isSignedFormat(fmt) ? 1 : 0);
        uint8_t* dst_row = pic.planes.data[0] + static_cast<size_t>(base_y) * pic.planes.linesize[0];
        for (uint32_t py = 0; py < 4 && base_y + py < h; py++) {
          uint16_t* dst = reinterpret_cast<uint16_t*>(dst_row + (static_cast<size_t>(py) * pic.planes.linesize[0]) + static_cast<size_t>(base_x) * 8);
          for (uint32_t px = 0; px < 4 && base_x + px < w; px++) {
            const uint16_t* src = tmp + (py * 4 + px) * 3;
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = 0x3C00; // alpha = 1.0 half
            dst += 4;
          }
        }
        continue;
      }

      uint8_t tmp[64];
      switch (fmt) {
        case DXGI::BC1_Typeless:
        case DXGI::BC1_UNorm:
        case DXGI::BC1_UNorm_SRGB:
          bcdec_bc1(block, tmp, 16);
          break;
        case DXGI::BC2_Typeless:
        case DXGI::BC2_UNorm:
        case DXGI::BC2_UNorm_SRGB:
          bcdec_bc2(block, tmp, 16);
          break;
        case DXGI::BC3_Typeless:
        case DXGI::BC3_UNorm:
        case DXGI::BC3_UNorm_SRGB:
          bcdec_bc3(block, tmp, 16);
          break;
        case DXGI::BC4_Typeless:
        case DXGI::BC4_UNorm:
        case DXGI::BC4_SNorm:
          bcdec_bc4(block, tmp, 4);
          break;
        case DXGI::BC5_Typeless:
        case DXGI::BC5_UNorm:
        case DXGI::BC5_SNorm:
          bcdec_bc5(block, tmp, 8);
          break;
        case DXGI::BC7_Typeless:
        case DXGI::BC7_UNorm:
        case DXGI::BC7_UNorm_SRGB:
          bcdec_bc7(block, tmp, 16);
          break;
        default:
          return false;
      }

      const bool single = (fmt == DXGI::BC4_Typeless || fmt == DXGI::BC4_UNorm || fmt == DXGI::BC4_SNorm);
      const bool dual = (fmt == DXGI::BC5_Typeless || fmt == DXGI::BC5_UNorm || fmt == DXGI::BC5_SNorm);

      for (uint32_t py = 0; py < 4 && base_y + py < h; py++) {
        uint8_t* dst = pic.planes.data[0] + static_cast<size_t>(base_y + py) * pic.planes.linesize[0] + static_cast<size_t>(base_x) * 4;
        for (uint32_t px = 0; px < 4 && base_x + px < w; px++) {
          if (single) {
            const uint8_t v = tmp[py * 4 + px];
            dst[0] = v;
            dst[1] = v;
            dst[2] = v;
            dst[3] = 0xFF;
          } else if (dual) {
            dst[0] = tmp[py * 8 + px * 2 + 0];
            dst[1] = tmp[py * 8 + px * 2 + 1];
            dst[2] = 0;
            dst[3] = 0xFF;
          } else {
            memcpy(dst, tmp + py * 16 + px * 4, 4);
          }
          dst += 4;
        }
      }
    }
  }
  return true;
}

} // namespace

class DDSDecoder final : public Decoder {
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  OMPixelFormat output_format_ = OM_FORMAT_R8G8B8A8;
  bool initialized_ = false;

public:
  DDSDecoder() = default;
  ~DDSDecoder() override = default;

  static auto isSupported(OMCodecId codec_id) -> bool {
    switch (codec_id) {
      case OM_CODEC_BC1:
      case OM_CODEC_BC2:
      case OM_CODEC_BC3:
      case OM_CODEC_BC4:
      case OM_CODEC_BC5:
      case OM_CODEC_BC6H:
      case OM_CODEC_BC7:
        return true;
      default:
        return false;
    }
  }

  auto configure(const DecoderOptions& options) -> OMError override {
    if (!isSupported(options.format.codec_id)) {
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
    info.video_format = {output_format_, width_, height_};
    return info;
  }

  void flush() override {}

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    std::vector<Frame> frames;

    if (packet.bytes.empty() || packet.bytes.size() < 4) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    tinyddsloader::DDSFile dds;
    const tinyddsloader::Result result = dds.Load(packet.bytes.data(), packet.bytes.size());
    if (result != tinyddsloader::Result::Success) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    const uint32_t w = dds.GetWidth();
    const uint32_t h = dds.GetHeight();
    if (w == 0 || h == 0) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    const DXGI fmt = dds.GetFormat();
    if (!tinyddsloader::DDSFile::IsCompressed(fmt)) {
      return Err(OM_CODEC_NOT_SUPPORTED);
    }
    output_format_ = isBC6H(fmt) ? OM_FORMAT_RGBA64 : OM_FORMAT_R8G8B8A8;
    width_ = w;
    height_ = h;

    const uint32_t mip_count = dds.GetMipCount() > 0 ? dds.GetMipCount() : 1;
    const uint32_t array_size = dds.GetArraySize() > 0 ? dds.GetArraySize() : 1;
    const uint32_t total_images = mip_count * array_size;

    if (packet.pts >= 0) {
      uint32_t frame_idx = static_cast<uint32_t>(packet.pts);
      if (frame_idx >= total_images) {
        frame_idx = 0;
      }
      const uint32_t array_idx = frame_idx / mip_count;
      const uint32_t mip_idx = frame_idx % mip_count;

      const tinyddsloader::DDSFile::ImageData* img = dds.GetImageData(mip_idx, array_idx);
      if (!img || !img->m_mem) {
        return Err(OM_CODEC_DECODE_FAILED);
      }

      Picture pic(output_format_, img->m_width, img->m_height);
      if (!decodeCompressedMip(*img, fmt, pic)) {
        return Err(OM_CODEC_NOT_SUPPORTED);
      }

      Frame frame;
      frame.pts = packet.pts;
      frame.dts = packet.dts;
      frame.data = std::move(pic);
      frames.push_back(std::move(frame));
    } else {
      for (uint32_t array_idx = 0; array_idx < array_size; ++array_idx) {
        for (uint32_t mip_idx = 0; mip_idx < mip_count; ++mip_idx) {
          const tinyddsloader::DDSFile::ImageData* img = dds.GetImageData(mip_idx, array_idx);
          if (!img || !img->m_mem) continue;

          Picture pic(output_format_, img->m_width, img->m_height);
          if (!decodeCompressedMip(*img, fmt, pic)) continue;

          Frame frame;
          frame.pts = static_cast<int64_t>(array_idx * mip_count + mip_idx);
          frame.dts = frame.pts;
          frame.data = std::move(pic);
          frames.push_back(std::move(frame));
        }
      }
    }

    return Ok(std::move(frames));
  }
};

const CodecDescriptor CODEC_BC1 = {
  .codec_id = OM_CODEC_BC1,
  .type = OM_MEDIA_IMAGE,
  .name = "bc1",
  .long_name = "BC1 (DXT1) texture decoder",
  .vendor = "OpenMedia",
  .flags = NONE,
  .decoder_factory = [] { return std::make_unique<DDSDecoder>(); },
};

const CodecDescriptor CODEC_BC2 = {
  .codec_id = OM_CODEC_BC2,
  .type = OM_MEDIA_IMAGE,
  .name = "bc2",
  .long_name = "BC2 (DXT3) texture decoder",
  .vendor = "OpenMedia",
  .flags = NONE,
  .decoder_factory = [] { return std::make_unique<DDSDecoder>(); },
};

const CodecDescriptor CODEC_BC3 = {
  .codec_id = OM_CODEC_BC3,
  .type = OM_MEDIA_IMAGE,
  .name = "bc3",
  .long_name = "BC3 (DXT5) texture decoder",
  .vendor = "OpenMedia",
  .flags = NONE,
  .decoder_factory = [] { return std::make_unique<DDSDecoder>(); },
};

const CodecDescriptor CODEC_BC4 = {
  .codec_id = OM_CODEC_BC4,
  .type = OM_MEDIA_IMAGE,
  .name = "bc4",
  .long_name = "BC4 (ATI1) texture decoder",
  .vendor = "OpenMedia",
  .flags = NONE,
  .decoder_factory = [] { return std::make_unique<DDSDecoder>(); },
};

const CodecDescriptor CODEC_BC5 = {
  .codec_id = OM_CODEC_BC5,
  .type = OM_MEDIA_IMAGE,
  .name = "bc5",
  .long_name = "BC5 (ATI2) texture decoder",
  .vendor = "OpenMedia",
  .flags = NONE,
  .decoder_factory = [] { return std::make_unique<DDSDecoder>(); },
};

const CodecDescriptor CODEC_BC6H = {
  .codec_id = OM_CODEC_BC6H,
  .type = OM_MEDIA_IMAGE,
  .name = "bc6h",
  .long_name = "BC6H HDR texture decoder",
  .vendor = "OpenMedia",
  .flags = NONE,
  .decoder_factory = [] { return std::make_unique<DDSDecoder>(); },
};

const CodecDescriptor CODEC_BC7 = {
  .codec_id = OM_CODEC_BC7,
  .type = OM_MEDIA_IMAGE,
  .name = "bc7",
  .long_name = "BC7 (BPTC) texture decoder",
  .vendor = "OpenMedia",
  .flags = NONE,
  .decoder_factory = [] { return std::make_unique<DDSDecoder>(); },
};

} // namespace openmedia
