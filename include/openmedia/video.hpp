#pragma once

#include <openmedia/byte.h>
#include <openmedia/macro.h>
#include <cstdint>
#include <memory>
#include <openmedia/buffer.hpp>
#include <utility>
#include <variant>
#include <vector>

OM_ENUM(OMPixelFormat, uint32_t) {
    OM_FORMAT_UNKNOWN = 0,
    OM_FORMAT_R8G8B8A8 = OM_MAGIC('RGBA'),
    OM_FORMAT_B8G8R8A8 = OM_MAGIC('BGRA'),
    OM_FORMAT_YUV420P = OM_MAGIC('I420'),
    OM_FORMAT_YUV422P = OM_MAGIC('I422'),
    OM_FORMAT_YUV444P = OM_MAGIC('I444'),
    OM_FORMAT_YUV410P = OM_MAGIC('YUV9'),
    OM_FORMAT_YUV411P = OM_MAGIC('Y41B'),
    OM_FORMAT_YUVJ420P = OM_MAGIC('J420'),
    OM_FORMAT_YUVJ422P = OM_MAGIC('J422'),
    OM_FORMAT_YUVJ444P = OM_MAGIC('J444'),
    OM_FORMAT_NV12 = OM_MAGIC('NV12'),
    OM_FORMAT_NV21 = OM_MAGIC('NV21'),
    OM_FORMAT_NV16 = OM_MAGIC('NV16'),
    OM_FORMAT_NV24 = OM_MAGIC('NV24'),
    OM_FORMAT_GRAY8 = OM_MAGIC('Y800'),
    OM_FORMAT_GRAY16 = OM_MAGIC('Y16 '),
    OM_FORMAT_P010 = OM_MAGIC('P010'),
    OM_FORMAT_P016 = OM_MAGIC('P016'),
    OM_FORMAT_YUV420P10 = OM_MAGIC('I010'),
    OM_FORMAT_YUV420P12 = OM_MAGIC('I012'),
    OM_FORMAT_YUV420P16 = OM_MAGIC('I016'),
    OM_FORMAT_YUV422P10 = OM_MAGIC('I210'),
    OM_FORMAT_YUV422P12 = OM_MAGIC('I212'),
    OM_FORMAT_YUV422P16 = OM_MAGIC('I216'),
    OM_FORMAT_YUV444P10 = OM_MAGIC('I410'),
    OM_FORMAT_YUV444P12 = OM_MAGIC('I412'),
    OM_FORMAT_YUV444P16 = OM_MAGIC('I416'),
    OM_FORMAT_RGB32 = OM_MAGIC('BGR '),
    OM_FORMAT_RGBA64 = OM_MAGIC('RG64'),
    OM_FORMAT_PAL8 = OM_MAGIC('PAL8'),
};

OM_ENUM(OMColorSpace, uint8_t) {
    OM_COLOR_SPACE_UNKNOWN = 0,
    OM_COLOR_SPACE_BT601 = 1,
    OM_COLOR_SPACE_BT709 = 2,
    OM_COLOR_SPACE_BT2020 = 3,
    OM_COLOR_SPACE_BT2020_CL = 4,
    OM_COLOR_SPACE_SMPTE240M = 5,
    OM_COLOR_SPACE_FCC = 6,
    OM_COLOR_SPACE_GB2312 = 7,
    OM_COLOR_SPACE_YCGCO = 8,
    OM_COLOR_SPACE_SMPTE428 = 9,
    OM_COLOR_SPACE_CHROMA_DERIVED_NCL = 10,
    OM_COLOR_SPACE_CHROMA_DERIVED_CL = 11,
    OM_COLOR_SPACE_ICTCP = 12,
    OM_COLOR_SPACE_RGB = 13,
};

OM_ENUM(OMTransferCharacteristic, uint8_t) {
    OM_TRANSFER_UNKNOWN = 0,
    OM_TRANSFER_BT709 = 1,
    OM_TRANSFER_GAMMA22 = 2, // Gamma 2.2
    OM_TRANSFER_BT470M = OM_TRANSFER_GAMMA22,
    OM_TRANSFER_GAMMA28 = 3, // Gamma 2.8
    OM_TRANSFER_BT470BG = OM_TRANSFER_GAMMA28,
    OM_TRANSFER_BT601 = 4,
    OM_TRANSFER_SMPTE240M = 5,
    OM_TRANSFER_LINEAR = 6,
    OM_TRANSFER_LOG = 7,
    OM_TRANSFER_LOG_SQRT = 8,
    OM_TRANSFER_IEC61966_2_4 = 9, // xvYCC
    OM_TRANSFER_BT1361_ECG = 10,
    OM_TRANSFER_IEC61966_2_1 = 11, // sRGB / sYCC
    OM_TRANSFER_SRGB = OM_TRANSFER_IEC61966_2_1,
    OM_TRANSFER_BT2020_10 = 12,
    OM_TRANSFER_BT2020_12 = 13,
    OM_TRANSFER_SMPTE2084 = 14, // PQ / HDR10
    OM_TRANSFER_PQ = OM_TRANSFER_SMPTE2084,
    OM_TRANSFER_SMPTE428 = 15,     // DCI Cinema
    OM_TRANSFER_ARIB_STD_B67 = 16, // HLG
    OM_TRANSFER_HLG = OM_TRANSFER_ARIB_STD_B67,
};

namespace openmedia {

static auto getBytesPerPixel(OMPixelFormat fmt, uint8_t plane_idx) noexcept -> uint32_t {
  switch (fmt) {
    case OM_FORMAT_GRAY8:
    case OM_FORMAT_YUV420P:
    case OM_FORMAT_YUV422P:
    case OM_FORMAT_YUV444P:
    case OM_FORMAT_YUVJ420P:
    case OM_FORMAT_YUVJ422P:
    case OM_FORMAT_YUVJ444P:
    case OM_FORMAT_NV12:
    case OM_FORMAT_NV21:
    case OM_FORMAT_NV16:
    case OM_FORMAT_NV24:
    case OM_FORMAT_YUV410P:
    case OM_FORMAT_YUV411P:
      return 1;
    case OM_FORMAT_GRAY16:
      return 2;
    case OM_FORMAT_P010:
    case OM_FORMAT_P016:
    case OM_FORMAT_YUV420P10:
    case OM_FORMAT_YUV420P12:
    case OM_FORMAT_YUV420P16:
    case OM_FORMAT_YUV422P10:
    case OM_FORMAT_YUV422P12:
    case OM_FORMAT_YUV422P16:
    case OM_FORMAT_YUV444P10:
    case OM_FORMAT_YUV444P12:
    case OM_FORMAT_YUV444P16:
      return 2;
    case OM_FORMAT_R8G8B8A8:
    case OM_FORMAT_B8G8R8A8:
    case OM_FORMAT_RGB32:
      return 4;
    case OM_FORMAT_RGBA64:
      return 8;
    case OM_FORMAT_PAL8:
      return plane_idx == 0 ? 1 : 4;
    default:
      return 1;
  }
}

enum class HWDeviceType : uint8_t {
  NONE = 0,
  VULKAN,
  DX11,
  DX12,
  VAAPI,
  AMF,
  CUDA,
  QSV,
};

struct OPENMEDIA_ABI HostPicture {
  std::shared_ptr<Buffer> buffer;
};

class OPENMEDIA_ABI HardwarePicture {
protected:
  HWDeviceType type_ = HWDeviceType::NONE;

public:
  explicit HardwarePicture(HWDeviceType type)
      : type_(type) {}
  virtual ~HardwarePicture() = default;

  auto getType() const noexcept -> HWDeviceType { return type_; }
};

using PictureBuffer = std::variant<HostPicture, HardwarePicture>;

struct OPENMEDIA_ABI VideoFormat {
  OMPixelFormat format;
  uint32_t width;
  uint32_t height;
};

static auto getNumPlanes(OMPixelFormat fmt) -> uint32_t {
  switch (fmt) {
    // Packed RGB / grayscale
    case OM_FORMAT_R8G8B8A8:
    case OM_FORMAT_B8G8R8A8:
    case OM_FORMAT_RGB32:
    case OM_FORMAT_RGBA64:
    case OM_FORMAT_GRAY8:
    case OM_FORMAT_GRAY16:
      return 1;

      // Palette (index + palette)
    case OM_FORMAT_PAL8:
      return 2;

      // Semi-planar (Y + interleaved UV)
    case OM_FORMAT_NV12:
    case OM_FORMAT_NV21:
    case OM_FORMAT_NV16:
    case OM_FORMAT_NV24:
    case OM_FORMAT_P010:
    case OM_FORMAT_P016:
      return 2;

      // Planar (Y, U, V separate)
    case OM_FORMAT_YUV420P:
    case OM_FORMAT_YUVJ420P:
    case OM_FORMAT_YUV422P:
    case OM_FORMAT_YUVJ422P:
    case OM_FORMAT_YUV444P:
    case OM_FORMAT_YUVJ444P:
    case OM_FORMAT_YUV410P:
    case OM_FORMAT_YUV411P:
    case OM_FORMAT_YUV420P10:
    case OM_FORMAT_YUV420P12:
    case OM_FORMAT_YUV420P16:
    case OM_FORMAT_YUV422P10:
    case OM_FORMAT_YUV422P12:
    case OM_FORMAT_YUV422P16:
    case OM_FORMAT_YUV444P10:
    case OM_FORMAT_YUV444P12:
    case OM_FORMAT_YUV444P16:
      return 3;

    default:
      return 1;
  }
}

struct OPENMEDIA_ABI Picture {
  PictureBuffer buffer;

  bool is_keyframe = false;

  OMPixelFormat format;
  uint32_t width;
  uint32_t height;
  OMColorSpace color_space;
  OMTransferCharacteristic transfer_char;

  PlaneSpan<4> planes;

  Picture()
      : format(OM_FORMAT_NV12), width(0), height(0), color_space(OM_COLOR_SPACE_BT709), transfer_char(OM_TRANSFER_BT709) {}

  Picture(OMPixelFormat fmt, uint32_t w, uint32_t h)
      : format(fmt), width(w), height(h), color_space(OM_COLOR_SPACE_BT709), transfer_char(OM_TRANSFER_BT709) {
    allocate();
  }

  void allocate() {
    int num_planes = getNumPlanes(format);
    size_t total_size = 0;
    std::vector<size_t> plane_sizes(num_planes);
    std::vector<uint32_t> plane_strides(num_planes);

    for (int i = 0; i < num_planes; ++i) {
      auto dims = getPlaneDimensions(i);
      auto info = getPlaneInfo(i, dims.first);
      plane_strides[i] = info.second;
      plane_sizes[i] = static_cast<size_t>(info.second) * dims.second;
      total_size += plane_sizes[i];
    }

    HostPicture& host_picture = buffer.emplace<HostPicture>();
    host_picture.buffer = BufferPool::getInstance().get(total_size);

    uint8_t* raw_ptr = host_picture.buffer->bytes().data();
    size_t offset = 0;
    planes.count = 0;
    for (int i = 0; i < num_planes; ++i) {
      planes.setData(i, raw_ptr + offset, plane_strides[i]);
      offset += plane_sizes[i];
    }
  }

  void unref() {
    planes = PlaneSpan<4> {};
  }

  auto getPlaneDimensions(int plane_idx) const -> std::pair<uint32_t, uint32_t> {
    if (plane_idx >= getNumPlanes(format))
      return {0, 0};

    switch (format) {
      // 4:2:0 planar + variants
      case OM_FORMAT_YUV420P:
      case OM_FORMAT_YUVJ420P:
      case OM_FORMAT_YUV420P10:
      case OM_FORMAT_YUV420P12:
      case OM_FORMAT_YUV420P16:
        if (plane_idx == 0)
          return {width, height};
        else
          return {(width + 1) / 2, (height + 1) / 2};

        // 4:2:0 semi-planar
      case OM_FORMAT_NV12:
      case OM_FORMAT_NV21:
      case OM_FORMAT_P010:
      case OM_FORMAT_P016:
        if (plane_idx == 0)
          return {width, height};
        else
          return {width, (height + 1) / 2};

        // 4:2:2 planar + variants
      case OM_FORMAT_YUV422P:
      case OM_FORMAT_YUVJ422P:
      case OM_FORMAT_YUV422P10:
      case OM_FORMAT_YUV422P12:
      case OM_FORMAT_YUV422P16:
        if (plane_idx == 0)
          return {width, height};
        else
          return {(width + 1) / 2, height};

        // 4:2:2 semi-planar
      case OM_FORMAT_NV16:
        if (plane_idx == 0)
          return {width, height};
        else
          return {width, height};

        // 4:4:4 planar + variants
      case OM_FORMAT_YUV444P:
      case OM_FORMAT_YUVJ444P:
      case OM_FORMAT_YUV444P10:
      case OM_FORMAT_YUV444P12:
      case OM_FORMAT_YUV444P16:
        return {width, height};

        // 4:4:4 semi-planar
      case OM_FORMAT_NV24:
        return {width, height};

        // 4:1:1
      case OM_FORMAT_YUV411P:
        if (plane_idx == 0)
          return {width, height};
        else
          return {(width + 3) / 4, height};

        // 4:1:0
      case OM_FORMAT_YUV410P:
        if (plane_idx == 0)
          return {width, height};
        else
          return {(width + 3) / 4, (height + 3) / 4};

        // Palette
      case OM_FORMAT_PAL8:
        if (plane_idx == 0)
          return {width, height};
        else
          return {256, 1};

      default:
        return {width, height};
    }
  }

  auto getPlaneInfo(int plane_idx, uint32_t plane_width) const -> std::pair<uint32_t, uint32_t> {
    uint32_t bpp = getBytesPerPixel(format, plane_idx);
    uint32_t plane_stride = plane_width * bpp;
    plane_stride = (plane_stride + 15) & ~15;
    return {bpp, plane_stride};
  }
};

} // namespace openmedia
