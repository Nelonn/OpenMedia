#include <gif_lib.h>
#include <algorithm>
#include <cstring>
#include <codecs.hpp>
#include <openmedia/video.hpp>
#include <vector>

namespace openmedia {

struct GIFStreamData {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t offset = 0;
};

static auto gif_read_from_memory(GifFileType* gif, GifByteType* buf, int len) -> int {
  auto* gif_data = static_cast<GIFStreamData*>(gif->UserData);
  if (!gif_data || !gif_data->data) {
    return 0;
  }

  size_t remaining = gif_data->size - gif_data->offset;
  size_t to_read = static_cast<size_t>(len) < remaining ? static_cast<size_t>(len) : remaining;

  if (to_read == 0) {
    return 0;
  }

  memcpy(buf, gif_data->data + gif_data->offset, to_read);
  gif_data->offset += to_read;

  return static_cast<int>(to_read);
}

class GIFDecoder final : public Decoder {
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  int frame_index_ = 0;
  bool initialized_ = false;

public:
  GIFDecoder() = default;
  ~GIFDecoder() override = default;

  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_GIF) {
      return OM_CODEC_INVALID_PARAMS;
    }
    width_ = options.format.image.width;
    height_ = options.format.image.height;
    frame_index_ = 0;
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

  void flush() override {
    frame_index_ = 0;
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    std::vector<Frame> frames;

    if (packet.bytes.empty()) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    int error;
    GIFStreamData gif_data;
    gif_data.data = packet.bytes.data();
    gif_data.size = packet.bytes.size();
    gif_data.offset = 0;

    GifFileType* gif = DGifOpen(&gif_data, gif_read_from_memory, &error);
    if (!gif) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    if (DGifSlurp(gif) != GIF_OK) {
      DGifCloseFile(gif, &error);
      return Err(OM_CODEC_DECODE_FAILED);
    }

    int target_frame = frame_index_;
    if (target_frame >= gif->ImageCount) {
      target_frame = 0;
    }
    frame_index_ = (target_frame + 1) % gif->ImageCount;

    SavedImage* frame = &gif->SavedImages[target_frame];
    int frame_width = frame->ImageDesc.Width;
    int frame_height = frame->ImageDesc.Height;
    int left = frame->ImageDesc.Left;
    int top = frame->ImageDesc.Top;

    Picture pic(OM_FORMAT_R8G8B8A8, width_, height_);

    ColorMapObject* color_map = frame->ImageDesc.ColorMap
                                    ? frame->ImageDesc.ColorMap
                                    : gif->SColorMap;

    memset(pic.planes.data[0], 0, static_cast<size_t>(pic.planes.linesize[0]) * height_);

    int transparent_index = NO_TRANSPARENT_COLOR;
    for (int i = 0; i < frame->ExtensionBlockCount; i++) {
      ExtensionBlock* eb = &frame->ExtensionBlocks[i];
      if (eb->Function == GRAPHICS_EXT_FUNC_CODE) {
        GraphicsControlBlock gcb;
        if (DGifExtensionToGCB(eb->ByteCount, eb->Bytes, &gcb) == GIF_OK) {
          transparent_index = gcb.TransparentColor;
        }
        break;
      }
    }

    uint8_t* pixels = pic.planes.data[0];

    int interlace_offset[] = {0, 4, 2, 1};
    int interlace_jumps[] = {8, 8, 4, 2};
    int pass = frame->ImageDesc.Interlace ? 0 : 3;

    for (int y = 0; y < frame_height; y++) {
      int actual_y;
      if (frame->ImageDesc.Interlace) {
        actual_y = y * interlace_jumps[pass] + interlace_offset[pass];
        if (actual_y >= frame_height && pass < 3) {
          pass++;
          y = -1;
          continue;
        }
      } else {
        actual_y = y;
      }

      uint8_t* row = pixels + (top + actual_y) * pic.planes.linesize[0] + left * 4;
      uint8_t* src = frame->RasterBits + y * frame_width;

      for (int x = 0; x < frame_width; x++) {
        uint8_t color_index = src[x];

        if (transparent_index != NO_TRANSPARENT_COLOR &&
            color_index == static_cast<uint8_t>(transparent_index)) {
          continue;
        }

        if (color_map && color_index < color_map->ColorCount) {
          GifColorType color = color_map->Colors[color_index];
          row[x * 4 + 0] = color.Red;
          row[x * 4 + 1] = color.Green;
          row[x * 4 + 2] = color.Blue;
          row[x * 4 + 3] = 255;
        } else {
          row[x * 4 + 0] = 0;
          row[x * 4 + 1] = 0;
          row[x * 4 + 2] = 0;
          row[x * 4 + 3] = 255;
        }
      }
    }

    Frame frame_out;
    frame_out.pts = packet.pts;
    frame_out.dts = packet.dts;
    frame_out.data = std::move(pic);
    frames.push_back(std::move(frame_out));

    DGifCloseFile(gif, &error);

    return Ok(std::move(frames));
  }
};

const CodecDescriptor CODEC_GIF = {
  .codec_id = OM_CODEC_GIF,
  .type = OM_MEDIA_IMAGE,
  .name = "gif",
  .long_name = "GIF image decoder",
  .vendor = "libgif",
  .flags = NONE,
  .decoder_factory = [] { return std::make_unique<GIFDecoder>(); },
};

} // namespace openmedia
