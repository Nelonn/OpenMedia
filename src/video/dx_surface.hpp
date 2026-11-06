#pragma once

// The surfaces DXVA decodes into, D3D11 and D3D12 alike, and what the pictures
// made from them are called.

#include <dxgiformat.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <openmedia/video.hpp>

namespace openmedia::dx_surface {

// 4:2:2 and 4:4:4 have no planar decode formats: the RExt profiles write packed pixels.
inline auto decodeFormat(int chroma_format_idc, int bit_depth) -> DXGI_FORMAT {
  switch (chroma_format_idc) {
    case 3:
      return bit_depth <= 8    ? DXGI_FORMAT_AYUV
             : bit_depth <= 10 ? DXGI_FORMAT_Y410
                               : DXGI_FORMAT_Y416;
    case 2:
      return bit_depth <= 8    ? DXGI_FORMAT_YUY2
             : bit_depth <= 10 ? DXGI_FORMAT_Y210
                               : DXGI_FORMAT_Y216;
    default: return bit_depth > 8 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
  }
}

inline auto planeCount(DXGI_FORMAT format) -> uint32_t {
  return format == DXGI_FORMAT_NV12 || format == DXGI_FORMAT_P010 ? 2 : 1;
}

// The texture handed out as it is.
inline auto hardwareFormat(DXGI_FORMAT format) -> OMPixelFormat {
  switch (format) {
    case DXGI_FORMAT_P010: return OM_FORMAT_P010;
    case DXGI_FORMAT_AYUV: return OM_FORMAT_AYUV;
    case DXGI_FORMAT_Y410: return OM_FORMAT_Y410;
    case DXGI_FORMAT_Y416: return OM_FORMAT_Y416;
    case DXGI_FORMAT_YUY2: return OM_FORMAT_YUY2;
    case DXGI_FORMAT_Y210: return OM_FORMAT_Y210;
    case DXGI_FORMAT_Y216: return OM_FORMAT_Y216;
    default: return OM_FORMAT_NV12;
  }
}

// The texture read back: packed formats become planar, deep samples stay
// MSB-aligned in 16 bits as in P010 (and as NVDEC hands them out).
inline auto hostFormat(DXGI_FORMAT format) -> OMPixelFormat {
  switch (format) {
    case DXGI_FORMAT_P010: return OM_FORMAT_P010;
    case DXGI_FORMAT_AYUV: return OM_FORMAT_YUV444P;
    case DXGI_FORMAT_Y410:
    case DXGI_FORMAT_Y416: return OM_FORMAT_YUV444P16;
    case DXGI_FORMAT_YUY2: return OM_FORMAT_YUV422P;
    case DXGI_FORMAT_Y210:
    case DXGI_FORMAT_Y216: return OM_FORMAT_YUV422P16;
    default: return OM_FORMAT_NV12;
  }
}

namespace detail {

template<typename T>
auto load(const uint8_t* p) -> T {
  T value;
  std::memcpy(&value, p, sizeof(value));
  return value;
}

template<typename T>
struct PlanarRow {
  T* y;
  T* u;
  T* v;
};

// Hands `unpack` each row of a packed surface with the matching rows of the planar picture.
template<typename T, typename Unpack>
void unpackRows(const uint8_t* src, size_t pitch, uint32_t height, Picture& pic, Unpack&& unpack) {
  const auto row_of = [&](uint32_t plane, uint32_t row) {
    return reinterpret_cast<T*>(pic.planes.getData(plane) + pic.planes.getLinesize(plane) * row);
  };
  for (uint32_t row = 0; row < height; ++row)
    unpack(src + pitch * row, PlanarRow<T> {row_of(0, row), row_of(1, row), row_of(2, row)});
}

// Y0 U Y1 V, in bytes (YUY2) or 16-bit words (Y210, Y216).
template<typename T>
void unpackYuyv(const uint8_t* src, size_t pitch, uint32_t width, uint32_t height, Picture& pic) {
  unpackRows<T>(src, pitch, height, pic, [&](const uint8_t* s, PlanarRow<T> d) {
    for (uint32_t pair = 0; pair < (width + 1) / 2; ++pair, s += 4 * sizeof(T)) {
      d.y[2 * pair] = load<T>(s);
      if (2 * pair + 1 < width) d.y[2 * pair + 1] = load<T>(s + 2 * sizeof(T));
      d.u[pair] = load<T>(s + sizeof(T));
      d.v[pair] = load<T>(s + 3 * sizeof(T));
    }
  });
}

} // namespace detail

// Copies a mapped surface into `pic`, allocated as hostFormat(format). `chroma` is
// the second plane of NV12 and P010 and unused for the packed formats.
inline void copyToHost(DXGI_FORMAT format, const uint8_t* src, size_t pitch, const uint8_t* chroma,
                       size_t chroma_pitch, uint32_t width, uint32_t height, Picture& pic) {
  using detail::load;
  using detail::PlanarRow;
  using detail::unpackRows;
  switch (format) {
    case DXGI_FORMAT_AYUV: // V, U, Y, A bytes
      return unpackRows<uint8_t>(src, pitch, height, pic, [&](const uint8_t* s, PlanarRow<uint8_t> d) {
        for (uint32_t x = 0; x < width; ++x, s += 4) {
          d.v[x] = s[0];
          d.u[x] = s[1];
          d.y[x] = s[2];
        }
      });
    case DXGI_FORMAT_Y410: // one word: U in bits 0-9, Y 10-19, V 20-29
      return unpackRows<uint16_t>(src, pitch, height, pic, [&](const uint8_t* s, PlanarRow<uint16_t> d) {
        const auto field = [](uint32_t word, int shift) { return static_cast<uint16_t>(((word >> shift) & 0x3ffu) << 6); };
        for (uint32_t x = 0; x < width; ++x, s += 4) {
          const auto word = load<uint32_t>(s);
          d.u[x] = field(word, 0);
          d.y[x] = field(word, 10);
          d.v[x] = field(word, 20);
        }
      });
    case DXGI_FORMAT_Y416: // U, Y, V, A in 16 bits each
      return unpackRows<uint16_t>(src, pitch, height, pic, [&](const uint8_t* s, PlanarRow<uint16_t> d) {
        for (uint32_t x = 0; x < width; ++x, s += 8) {
          d.u[x] = load<uint16_t>(s);
          d.y[x] = load<uint16_t>(s + 2);
          d.v[x] = load<uint16_t>(s + 4);
        }
      });
    case DXGI_FORMAT_YUY2: return detail::unpackYuyv<uint8_t>(src, pitch, width, height, pic);
    case DXGI_FORMAT_Y210:
    case DXGI_FORMAT_Y216: return detail::unpackYuyv<uint16_t>(src, pitch, width, height, pic);
    default: { // NV12, P010
      const size_t row_bytes = size_t(width) * (format == DXGI_FORMAT_P010 ? 2 : 1);
      const auto copy_rows = [&](uint32_t plane, const uint8_t* from, size_t from_pitch, uint32_t rows) {
        for (uint32_t row = 0; row < rows; ++row)
          std::memcpy(pic.planes.getData(plane) + pic.planes.getLinesize(plane) * row, from + from_pitch * row, row_bytes);
      };
      copy_rows(0, src, pitch, height);
      copy_rows(1, chroma, chroma_pitch, (height + 1) / 2);
      return;
    }
  }
}

} // namespace openmedia::dx_surface
