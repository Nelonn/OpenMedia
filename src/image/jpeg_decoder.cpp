#include <cstdio>
#include <jpeglib.h>
#include <setjmp.h>
#include <codecs.hpp>
#include <openmedia/video.hpp>
#include <vector>

namespace openmedia {

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

class JPEGDecoder final : public Decoder {
  struct jpeg_decompress_struct cinfo_ {};
  JPEGErrorManager jerr_ {};
  // libjpeg leaves the decompressor via longjmp, which skips destructors, so
  // everything alive across a decode has to outlive the stack frame.
  Picture picture_;
  std::vector<JSAMPROW> rows_;
  bool initialized_ = false;
  uint32_t width_ = 0;
  uint32_t height_ = 0;

  // Puts the decompressor back into its start state from wherever the previous
  // packet left it, including from inside an error.
  void reset() {
    jpeg_abort_decompress(&cinfo_);
    picture_ = {};
  }

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
    width_ = options.format.video.width;
    height_ = options.format.video.height;
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
    reset();
  }

  auto decode(const Packet& packet) -> Result<std::vector<Frame>, OMError> override {
    if (packet.bytes.empty()) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    reset();

    if (setjmp(jerr_.setjmp_buffer)) {
      reset();
      return Err(OM_CODEC_DECODE_FAILED);
    }

    jpeg_mem_src(&cinfo_, packet.bytes.data(), static_cast<unsigned long>(packet.bytes.size()));

    if (jpeg_read_header(&cinfo_, TRUE) != JPEG_HEADER_OK) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    // libjpeg-turbo writes RGBA directly, so no intermediate row buffer or
    // channel expansion is needed; it fills alpha with 0xFF itself.
    cinfo_.out_color_space = JCS_EXT_RGBA;

    if (jpeg_start_decompress(&cinfo_) != TRUE) {
      return Err(OM_CODEC_DECODE_FAILED);
    }

    width_ = cinfo_.output_width;
    height_ = cinfo_.output_height;

    picture_ = Picture(OM_FORMAT_R8G8B8A8, width_, height_);

    uint8_t* plane = picture_.planes.data[0];
    ptrdiff_t stride = picture_.planes.linesize[0];
    rows_.resize(height_);
    for (uint32_t y = 0; y < height_; y++) {
      rows_[y] = plane + y * stride;
    }

    while (cinfo_.output_scanline < height_) {
      JDIMENSION scanline = cinfo_.output_scanline;
      if (jpeg_read_scanlines(&cinfo_, rows_.data() + scanline, height_ - scanline) == 0) {
        reset();
        return Err(OM_CODEC_DECODE_FAILED);
      }
    }

    jpeg_finish_decompress(&cinfo_);

    Frame frame;
    frame.pts = packet.pts;
    frame.dts = packet.dts;
    frame.data = std::move(picture_);

    std::vector<Frame> frames;
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
