#include <cstdio>
#include <jpeglib.h>
#include <setjmp.h>
#include <algorithm>
#include <cstring>
#include <codecs.hpp>
#include <openmedia/video.hpp>
#include <vector>
#include <util/io_util.hpp>

namespace openmedia {

struct JPEGSourceManager {
  struct jpeg_source_mgr pub;
  const uint8_t* data = nullptr;
  size_t size = 0;
  bool start_of_file = true;
};

struct JPEGErrorManager {
  struct jpeg_error_mgr pub;
  jmp_buf setjmp_buffer;
};

static void jpeg_error_exit(j_common_ptr cinfo) {
  auto* err = reinterpret_cast<JPEGErrorManager*>(cinfo->err);
  char msg_buf[JMSG_LENGTH_MAX];
  (*cinfo->err->format_message)(cinfo, msg_buf);
  fprintf(stderr, "[JPEGDecoder] libjpeg error: %s\n", msg_buf);
  longjmp(err->setjmp_buffer, 1);
}

static void jpeg_init_source(j_decompress_ptr cinfo) {
  auto* src = reinterpret_cast<JPEGSourceManager*>(cinfo->src);
  src->start_of_file = true;
  src->pub.next_input_byte = src->data;
  src->pub.bytes_in_buffer = src->size;
}

static auto jpeg_fill_input_buffer(j_decompress_ptr cinfo) -> boolean {
  auto* src = reinterpret_cast<JPEGSourceManager*>(cinfo->src);
  static constexpr uint8_t EOI[2] = {0xFF, JPEG_EOI};
  src->pub.next_input_byte = EOI;
  src->pub.bytes_in_buffer = 2;
  return TRUE;
}

static void jpeg_skip_input_data(j_decompress_ptr cinfo, long num_bytes) {
  auto* src = reinterpret_cast<JPEGSourceManager*>(cinfo->src);
  if (num_bytes > 0 && num_bytes <= static_cast<long>(src->pub.bytes_in_buffer)) {
    src->pub.next_input_byte += num_bytes;
    src->pub.bytes_in_buffer -= num_bytes;
  }
}

static void jpeg_term_source(j_decompress_ptr /*cinfo*/) {}

class JPEGDecoder final : public Decoder {
  struct jpeg_decompress_struct cinfo_ {};
  JPEGErrorManager jerr_ {};
  bool decompress_started_ = false;
  bool initialized_ = false;
  uint32_t width_ = 0;
  uint32_t height_ = 0;

public:
  JPEGDecoder() {
    cinfo_.err = jpeg_std_error(&jerr_.pub);
    jerr_.pub.error_exit = jpeg_error_exit;
    jerr_.pub.emit_message = [](j_common_ptr cinfo, int msg_level) {
      if (msg_level >= 0) return;
      char msg_buf[JMSG_LENGTH_MAX];
      (*cinfo->err->format_message)(cinfo, msg_buf);
      fprintf(stderr, "[JPEGDecoder] libjpeg warning: %s\n", msg_buf);
    };
    jpeg_create_decompress(&cinfo_);
  }

  ~JPEGDecoder() override {
    jpeg_destroy_decompress(&cinfo_);
  }

  auto configure(const DecoderOptions& options) -> OMError override {
    if (options.format.codec_id != OM_CODEC_JPEG) {
      return OM_CODEC_INVALID_PARAMS;
    }
    initialized_ = true;
    width_ = options.format.image.width;
    height_ = options.format.image.height;
    return OM_SUCCESS;
  }

  auto getInfo() -> std::optional<DecodingInfo> override {
    if (!initialized_) return std::nullopt;

    DecodingInfo info;
    info.media_type = OM_MEDIA_IMAGE;
    info.video_format = {OM_FORMAT_R8G8B8A8, width_, height_};
    return info;
  }

  void flush() override {
    if (decompress_started_) {
      jpeg_abort_decompress(&cinfo_);
      decompress_started_ = false;
    }
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    std::vector<Frame> frames;

    if (packet.bytes.empty()) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    if (decompress_started_) {
      jpeg_abort_decompress(&cinfo_);
      decompress_started_ = false;
    }

    if (setjmp(jerr_.setjmp_buffer)) {
      decompress_started_ = false;
      return Err(OM_CODEC_DECODE_FAILED);
    }

    JPEGSourceManager src;
    src.pub.init_source = jpeg_init_source;
    src.pub.fill_input_buffer = jpeg_fill_input_buffer;
    src.pub.skip_input_data = jpeg_skip_input_data;
    src.pub.resync_to_restart = jpeg_resync_to_restart;
    src.pub.term_source = jpeg_term_source;
    src.data = packet.bytes.data();
    src.size = packet.bytes.size();
    src.start_of_file = true;

    cinfo_.src = &src.pub;

    if (jpeg_read_header(&cinfo_, TRUE) != JPEG_HEADER_OK) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    cinfo_.out_color_space = JCS_RGB;

    if (jpeg_start_decompress(&cinfo_) != TRUE) {
      return Err(OM_CODEC_DECODE_FAILED);
    }
    decompress_started_ = true;

    uint32_t width = cinfo_.output_width;
    uint32_t height = cinfo_.output_height;
    uint32_t stride = cinfo_.output_width * cinfo_.output_components;

    Picture pic(OM_FORMAT_R8G8B8A8, width, height);

    JSAMPARRAY buffer = (*cinfo_.mem->alloc_sarray)(
        reinterpret_cast<j_common_ptr>(&cinfo_),
        JPOOL_IMAGE,
        stride,
        1);

    while (cinfo_.output_scanline < height) {
      uint32_t current_line = cinfo_.output_scanline;

      if (jpeg_read_scanlines(&cinfo_, buffer, 1) != 1) {
        jpeg_abort_decompress(&cinfo_);
        decompress_started_ = false;
        return Err(OM_CODEC_DECODE_FAILED);
      }

      uint8_t* dst = pic.planes.data[0] + current_line * pic.planes.linesize[0];

      for (uint32_t x = 0; x < width; x++) {
        dst[x * 4 + 0] = buffer[0][x * 3 + 0];
        dst[x * 4 + 1] = buffer[0][x * 3 + 1];
        dst[x * 4 + 2] = buffer[0][x * 3 + 2];
        dst[x * 4 + 3] = 0xFF;
      }
    }

    jpeg_finish_decompress(&cinfo_);
    decompress_started_ = false;

    Frame frame;
    frame.pts = packet.pts;
    frame.dts = packet.dts;
    frame.data = std::move(pic);
    frames.push_back(std::move(frame));

    return Ok(std::move(frames));
  }
};

const CodecDescriptor CODEC_JPEG = {
  .codec_id = OM_CODEC_JPEG,
  .type = OM_MEDIA_IMAGE,
  .name = "jpeg",
  .long_name = "JPEG image decoder",
  .vendor = "libjpeg-turbo",
  .flags = NONE,
  .decoder_factory = [] { return std::make_unique<JPEGDecoder>(); },
};

} // namespace openmedia
