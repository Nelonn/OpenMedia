#include <cstring>
#include <codecs.hpp>
#include <openmedia/video.hpp>

namespace openmedia {

class RawVideoDecoder final : public Decoder {
  OMPixelFormat format_ = OM_FORMAT_R8G8B8A8;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  bool initialized_ = false;

public:
  RawVideoDecoder() = default;
  ~RawVideoDecoder() override = default;

  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_RAW_VIDEO) {
      return OM_CODEC_INVALID_PARAMS;
    }

    if (options.format.type == OM_MEDIA_IMAGE) {
      width_ = options.format.image.width;
      height_ = options.format.image.height;
      if (options.format.image.format != OM_FORMAT_UNKNOWN) {
        format_ = options.format.image.format;
      }
    } else {
      width_ = options.format.video.width;
      height_ = options.format.video.height;
      if (options.format.video.format != OM_FORMAT_UNKNOWN) {
        format_ = options.format.video.format;
      }
    }

    if (width_ == 0 || height_ == 0) {
      return OM_CODEC_INVALID_PARAMS;
    }

    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!initialized_) return std::nullopt;

    DecodingInfo info;
    info.media_type = OM_MEDIA_VIDEO;
    info.video_format = {format_, width_, height_};
    return info;
  }

  void flush() override {}

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    std::vector<Frame> frames;

    // Empty packet is the end-of-stream marker; nothing to produce.
    if (packet.bytes.empty()) {
      return Ok(std::move(frames));
    }

    Picture pic(format_, width_, height_);

    const uint32_t num_planes = getNumPlanes(format_);
    size_t offset = 0;
    for (uint32_t p = 0; p < num_planes; p++) {
      const auto [plane_w, plane_h] = getPlaneDimensions(format_, p, width_, height_);
      const uint32_t bpp = getBytesPerPixel(format_, p);
      const size_t row_bytes = static_cast<size_t>(plane_w) * bpp;

      if (offset + row_bytes * plane_h > packet.bytes.size()) {
        return Err(OM_CODEC_DECODE_FAILED);
      }

      uint8_t* dst = pic.planes.data[p];
      const uint8_t* src = packet.bytes.data() + offset;
      for (uint32_t y = 0; y < plane_h; y++) {
        memcpy(dst + static_cast<size_t>(y) * pic.planes.linesize[p],
               src + static_cast<size_t>(y) * row_bytes, row_bytes);
      }
      offset += row_bytes * plane_h;
    }

    Frame frame;
    frame.pts = packet.pts;
    frame.dts = packet.dts;
    frame.data = std::move(pic);
    frames.push_back(std::move(frame));

    return Ok(std::move(frames));
  }
};

const CodecDescriptor CODEC_RAW_VIDEO = {
  .codec_id = OM_CODEC_RAW_VIDEO,
  .type = OM_MEDIA_VIDEO,
  .name = "rawvideo",
  .long_name = "Raw video decoder",
  .vendor = "OpenMedia",
  .flags = NONE,
  .decoder_factory = [] { return std::make_unique<RawVideoDecoder>(); },
};

} // namespace openmedia
