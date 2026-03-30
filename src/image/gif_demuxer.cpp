#include <gif_lib.h>
#include <cstring>
#include <util/demuxer_base.hpp>
#include <util/io_util.hpp>
#include <openmedia/format_api.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/track.hpp>

namespace openmedia {

class GIFDemuxer final : public BaseDemuxer {
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  int frame_count_ = 0;
  int64_t data_start_ = 0;
  bool packet_read_ = false;

  static auto read_from_stream(GifFileType* gif, GifByteType* buf, int len) -> int {
    auto* input = static_cast<InputStream*>(gif->UserData);
    if (!input || !input->isValid()) {
      return 0;
    }
    return static_cast<int>(input->read(std::span(buf, static_cast<size_t>(len))));
  }

public:
  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    input_ = std::move(input);
    if (!input_ || !input_->isValid()) {
      return OM_IO_INVALID_STREAM;
    }

    // Check GIF signature (6 bytes: "GIF87a" or "GIF89a")
    uint8_t signature[6];
    if (input_->read(signature) < 6) {
      return OM_IO_NOT_ENOUGH_DATA;
    }

    if (memcmp(signature, "GIF8", 4) != 0 ||
        (memcmp(signature + 4, "7a", 2) != 0 && memcmp(signature + 4, "9a", 2) != 0)) {
      return OM_FORMAT_PARSE_FAILED;
    }

    data_start_ = 6;

    // Parse Logical Screen Descriptor (7 bytes after signature)
    uint8_t lsd[7];
    if (input_->read(lsd) < 7) {
      return OM_IO_NOT_ENOUGH_DATA;
    }

    width_ = lsd[0] | (lsd[1] << 8);
    height_ = lsd[2] | (lsd[3] << 8);

    if (width_ == 0 || height_ == 0) {
      return OM_FORMAT_PARSE_FAILED;
    }

    data_start_ = 0;
    input_->seek(data_start_, Whence::BEG);

    // Count frames by parsing the GIF
    int error;
    GifFileType* gif = DGifOpen(input_.get(), read_from_stream, &error);
    if (!gif) {
      return OM_FORMAT_PARSE_FAILED;
    }

    frame_count_ = 0;
    if (DGifSlurp(gif) == GIF_OK) {
      frame_count_ = gif->ImageCount;
    }

    DGifCloseFile(gif, &error);

    // Reset stream position
    input_->seek(data_start_, Whence::BEG);

    // Create track
    Track track;
    track.index = 0;
    track.format.type = OM_MEDIA_IMAGE;
    track.format.codec_id = OM_CODEC_GIF;
    track.time_base = {1, 100}; // GIF frames typically in centiseconds
    track.duration = frame_count_;

    track.format.image.width = width_;
    track.format.image.height = height_;

    tracks_.push_back(track);

    return OM_SUCCESS;
  }

  auto readPacket() -> Result<Packet, OMError> override {
    if (packet_read_) {
      return Err(OM_FORMAT_END_OF_FILE);
    }
    packet_read_ = true;

    Packet pkt;
    size_t size = static_cast<size_t>(input_->size());
    pkt.allocate(size);
    pkt.stream_index = 0;
    pkt.pos = 0;
    pkt.pts = 0;
    pkt.dts = 0;
    pkt.is_keyframe = true;

    input_->seek(0, Whence::BEG);
    size_t bytes_read = input_->read(pkt.bytes);
    pkt.bytes = pkt.bytes.subspan(0, bytes_read);

    return Ok(std::move(pkt));
  }

  auto seek(int32_t stream_idx, int64_t timestamp, SeekMode mode) -> OMError override {
    return OM_SUCCESS; // Single image or simple frame seek
  }
};

const FormatDescriptor FORMAT_GIF = {
    .container_id = OM_CONTAINER_GIF,
    .name = "gif",
    .long_name = "GIF (Graphics Interchange Format)",
    .demuxer_factory = [] { return std::make_unique<GIFDemuxer>(); },
    .muxer_factory = {},
};

} // namespace openmedia
