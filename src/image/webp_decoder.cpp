#include <webp/decode.h>
#include <cstring>
#include <codecs.hpp>
#include <openmedia/video.hpp>
#include <vector>
#include <util/io_util.hpp>

namespace openmedia {

class WEBPDecoder final : public Decoder {
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  bool initialized_ = false;

public:
  WEBPDecoder() = default;
  ~WEBPDecoder() override = default;

  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_WEBP) {
      return OM_CODEC_INVALID_PARAMS;
    }
    width_ = options.format.image.width;
    height_ = options.format.image.height;
    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!initialized_) return std::nullopt;

    DecodingInfo info = {};
    info.media_type = OM_MEDIA_IMAGE;
    info.video_format = {OM_FORMAT_R8G8B8A8, width_, height_};
    return info;
  }

  void flush() override {}

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    std::vector<Frame> frames;

    if (packet.bytes.empty()) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    int w, h;
    if (!WebPGetInfo(packet.bytes.data(), static_cast<int>(packet.bytes.size()), &w, &h)) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    uint8_t* output = WebPDecodeRGBA(packet.bytes.data(), static_cast<int>(packet.bytes.size()), &w, &h);
    if (!output) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    Picture pic(OM_FORMAT_R8G8B8A8, static_cast<uint32_t>(w), static_cast<uint32_t>(h));

    for (int y = 0; y < h; y++) {
      memcpy(pic.planes.data[0] + y * pic.planes.linesize[0], output + y * w * 4, static_cast<size_t>(w) * 4);
    }

    WebPFree(output);

    Frame frame;
    frame.pts = packet.pts;
    frame.dts = packet.dts;
    frame.data = std::move(pic);
    frames.push_back(std::move(frame));

    return Ok(std::move(frames));
  }
};

const CodecDescriptor CODEC_WEBP = {
  .codec_id = OM_CODEC_WEBP,
  .type = OM_MEDIA_IMAGE,
  .name = "webp",
  .long_name = "WebP image decoder",
  .vendor = "Google",
  .flags = NONE,
  .decoder_factory = [] { return std::make_unique<WEBPDecoder>(); },
};

} // namespace openmedia
