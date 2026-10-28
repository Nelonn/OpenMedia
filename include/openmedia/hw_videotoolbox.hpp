#pragma once

#if defined(__APPLE__)
#include <CoreVideo/CVPixelBuffer.h>
#include <openmedia/video.hpp>

namespace openmedia {

// DecoderOptions::hardware_output returns these instead of HostPicture when
// using VideoToolbox. getPicture() is a CVPixelBufferRef; CPU planes are empty.
// Retaining this object keeps the IOSurface out of the decoder's reuse pool.
class OPENMEDIA_ABI VideoToolboxPicture final : public HardwarePicture {
public:
  explicit VideoToolboxPicture(CVPixelBufferRef buffer)
      : HardwarePicture(HWDeviceType::VIDEOTOOLBOX, buffer) {
    if (buffer) CVPixelBufferRetain(buffer);
  }
  ~VideoToolboxPicture() override {
    if (picture_) CVPixelBufferRelease(static_cast<CVPixelBufferRef>(picture_));
  }
  VideoToolboxPicture(const VideoToolboxPicture&) = delete;
  auto operator=(const VideoToolboxPicture&) -> VideoToolboxPicture& = delete;
};

} // namespace openmedia
#endif
