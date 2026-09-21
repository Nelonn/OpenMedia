#include <webp/mux.h>
#include <cstring>
#include <image/webp_common.hpp>
#include <openmedia/format_api.hpp>
#include <openmedia/metadata_keys.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/track.hpp>
#include <util/still_image_muxer.hpp>
#include <vector>

namespace openmedia {

namespace {

struct MuxGuard {
  WebPMux* mux;
  ~MuxGuard() { WebPMuxDelete(mux); }
};

struct DataGuard {
  WebPData* data;
  ~DataGuard() { WebPDataClear(data); }
};

auto toOMError(WebPMuxError err) -> OMError {
  switch (err) {
    case WEBP_MUX_OK:               return OM_SUCCESS;
    case WEBP_MUX_MEMORY_ERROR:     return OM_COMMON_OUT_OF_MEMORY;
    case WEBP_MUX_INVALID_ARGUMENT: return OM_COMMON_INVALID_ARGUMENT;
    case WEBP_MUX_BAD_DATA:         return OM_FORMAT_INVALID_PACKET;
    case WEBP_MUX_NOT_ENOUGH_DATA:  return OM_IO_NOT_ENOUGH_DATA;
    default:                        return OM_FORMAT_MUXING_FAILED;
  }
}

auto isWebPFile(std::span<const uint8_t> bytes) -> bool {
  return bytes.size() > 12 &&
         memcmp(bytes.data(), "RIFF", 4) == 0 &&
         memcmp(bytes.data() + 8, "WEBP", 4) == 0;
}

auto setChunk(WebPMux* mux, const char fourcc[4], const Dictionary& metadata, const Key& key) -> OMError {
  const auto blob = metadata.getBinary(key);
  if (!blob.has_value() || blob->empty()) return OM_SUCCESS;
  const WebPData data = {blob->data(), blob->size()};
  return toOMError(WebPMuxSetChunk(mux, fourcc, &data, /*copy_data=*/1));
}

} // namespace

class WEBPMuxer final : public StillImageMuxer {
public:
  WEBPMuxer()
      : StillImageMuxer(OM_CODEC_WEBP) {}

  ~WEBPMuxer() override { StillImageMuxer::close(); }

protected:
  auto acceptsMultipleFrames() const -> bool override { return true; }

  auto writeFrames() -> OMError override {
    const auto& metadata = tracks_.front().metadata;
    const bool has_chunks = hasBlob(metadata, ICC_PROFILE) ||
                            hasBlob(metadata, EXIF) ||
                            hasBlob(metadata, XMP);

    // A lone still that is already a complete WebP file needs no container
    // work: pushing it through the mux would only rebuild the same bytes.
    if (frames_.size() == 1 && !has_chunks && isWebPFile(frames_.front().bytes)) {
      return StillImageMuxer::writeFrames();
    }
    return assemble(metadata);
  }

private:
  static auto hasBlob(const Dictionary& metadata, const Key& key) -> bool {
    const auto blob = metadata.getBinary(key);
    return blob.has_value() && !blob->empty();
  }

  auto assemble(const Dictionary& metadata) -> OMError {
    WebPMux* mux = WebPMuxNew();
    if (mux == nullptr) {
      return OM_COMMON_OUT_OF_MEMORY;
    }
    MuxGuard mux_guard {mux};

    if (frames_.size() == 1) {
      const WebPData image = {frames_.front().bytes.data(), frames_.front().bytes.size()};
      if (const OMError err = toOMError(WebPMuxSetImage(mux, &image, /*copy_data=*/1));
          err != OM_SUCCESS) {
        return err;
      }
    } else {
      const OMError err = pushAnimation(mux, metadata);
      if (err != OM_SUCCESS) return err;
    }

    if (const OMError err = setChunk(mux, "ICCP", metadata, ICC_PROFILE); err != OM_SUCCESS) return err;
    if (const OMError err = setChunk(mux, "EXIF", metadata, EXIF); err != OM_SUCCESS) return err;
    if (const OMError err = setChunk(mux, "XMP ", metadata, XMP); err != OM_SUCCESS) return err;

    WebPData output = {};
    WebPDataInit(&output);
    DataGuard output_guard {&output};

    if (const OMError err = toOMError(WebPMuxAssemble(mux, &output)); err != OM_SUCCESS) {
      return err;
    }
    if (!writeAll(std::span(output.bytes, output.size))) {
      return OM_IO_WRITE_FAILED;
    }
    return OM_SUCCESS;
  }

  auto pushAnimation(WebPMux* mux, const Dictionary& metadata) -> OMError {
    WebPMuxAnimParams params = {};
    params.loop_count = metadata.getInt32(ANIMATION_LOOP_COUNT, 0);
    // libwebp packs the canvas colour as 0xBBGGRRAA; the metadata key uses the
    // 0xAARRGGBB everything else in the library speaks, and defaults to the
    // opaque white libwebp's own tools pick.
    const auto argb = static_cast<uint32_t>(
        metadata.getInt64(ANIMATION_BACKGROUND_COLOR, 0xFFFFFFFF));
    params.bgcolor = ((argb & 0x000000FFu) << 24) | // blue
                     ((argb & 0x0000FF00u) << 8) |  // green
                     ((argb & 0x00FF0000u) >> 8) |  // red
                     ((argb & 0xFF000000u) >> 24);  // alpha

    if (const OMError err = toOMError(WebPMuxSetAnimationParams(mux, &params));
        err != OM_SUCCESS) {
      return err;
    }

    for (size_t i = 0; i < frames_.size(); ++i) {
      WebPMuxFrameInfo info = {};
      info.bitstream = {frames_[i].bytes.data(), frames_[i].bytes.size()};
      info.x_offset = 0;
      info.y_offset = 0;
      info.duration = static_cast<int>(frameDurationMs(i));
      info.id = WEBP_CHUNK_ANMF;
      // Every packet is a full-canvas keyframe, so replacing the canvas
      // outright reproduces it exactly; blending would composite frames with
      // alpha over their predecessor instead.
      info.dispose_method = WEBP_MUX_DISPOSE_NONE;
      info.blend_method = WEBP_MUX_NO_BLEND;

      if (const OMError err = toOMError(WebPMuxPushFrame(mux, &info, /*copy_data=*/1));
          err != OM_SUCCESS) {
        return err;
      }
    }

    // Only worth stating for an animation: the bound is enforced at assembly,
    // and a still gets its canvas from the image itself.
    const auto& video = tracks_.front().format.video;
    if (video.width > 0 && video.height > 0) {
      if (const OMError err = toOMError(WebPMuxSetCanvasSize(
              mux, static_cast<int>(video.width), static_cast<int>(video.height)));
          err != OM_SUCCESS) {
        return err;
      }
    }

    return OM_SUCCESS;
  }
};

auto createWEBPMuxer() -> std::unique_ptr<Muxer> {
  return std::make_unique<WEBPMuxer>();
}

} // namespace openmedia
