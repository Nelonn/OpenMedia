#pragma once

#include <openmedia/video.hpp>

#include <cstdint>

namespace openmedia {

// A picture that lives in Linux DMA-BUFs, as a capture or a compositor exported
// it: up to four planes, each a file descriptor with an offset and a pitch,
// laid out by a DRM fourcc and modifier. An encoder that can import it reads it
// where it is (VA-API: NV12 and P010); nothing goes through the CPU.
//
// The file descriptors are borrowed for the call to Encoder::encode() that is
// handed the picture: the encoder imports the buffers there (once per buffer,
// recognised again by the DMA-BUF's identity) and keeps its own references.
// The contents must be complete on the GPU when encode() is called, and left
// alone until the packet for this picture has come out -- at once with the
// encoder's low_latency option, or by the next encode() call without it.
//
// buffer_width x buffer_height is what the planes actually hold, at least
// width x height. A producer that pads the picture to the encoder's alignment
// (and repeats the edge into the padding) says so here, and the encoder then
// reads whole blocks out of the buffer rather than past its end.
class OPENMEDIA_ABI DrmPrimePicture final : public HardwarePicture {
public:
  struct Plane {
    int fd = -1;
    uint32_t offset = 0;
    uint32_t pitch = 0;
  };

  DrmPrimePicture() : HardwarePicture(HWDeviceType::DRM_PRIME) {}

  uint32_t fourcc = 0;   // DRM_FORMAT_NV12 or DRM_FORMAT_P010
  uint64_t modifier = 0; // DRM_FORMAT_MOD_LINEAR is 0
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t buffer_width = 0;  // 0: width
  uint32_t buffer_height = 0; // 0: height
  Plane planes[4] = {};
  uint32_t plane_count = 0;
};

} // namespace openmedia
