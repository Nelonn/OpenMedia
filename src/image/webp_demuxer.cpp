#include <webp/demux.h>
#include <cstring>
#include <openmedia/format_api.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/track.hpp>
#include <util/demuxer_base.hpp>
#include <util/io_util.hpp>

namespace openmedia {

class WEBPDemuxer final : public BaseDemuxer {
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  int frame_count_ = 1;
  int64_t duration_ms_ = 0;
  bool is_animated_ = false;

  std::vector<uint8_t> file_buf_;
  WebPData webp_data_ = {};
  WebPDemuxer* demuxer_ = nullptr;

  WebPIterator iter_ = {};
  bool iter_valid_ = false;
  int next_frame_n_ = 1;

public:
  ~WEBPDemuxer() override {
    releaseIter();
    if (demuxer_) {
      WebPDemuxDelete(demuxer_);
      demuxer_ = nullptr;
    }
  }

  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    input_ = std::move(input);
    if (!input_ || !input_->isValid()) {
      return OM_IO_INVALID_STREAM;
    }

    uint8_t header[12];
    if (input_->read(header) < 12) {
      return OM_IO_NOT_ENOUGH_DATA;
    }
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WEBP", 4) != 0) {
      return OM_FORMAT_PARSE_FAILED;
    }

    input_->seek(0, Whence::BEG);
    const int64_t file_size = input_->size();
    if (file_size <= 0) {
      return OM_IO_NOT_ENOUGH_DATA;
    }
    file_buf_.resize(static_cast<size_t>(file_size));
    const size_t bytes_read = input_->read(file_buf_);
    file_buf_.resize(bytes_read);

    webp_data_.bytes = file_buf_.data();
    webp_data_.size = file_buf_.size();

    demuxer_ = WebPDemux(&webp_data_);
    if (!demuxer_) {
      return OM_FORMAT_PARSE_FAILED;
    }

    width_ = WebPDemuxGetI(demuxer_, WEBP_FF_CANVAS_WIDTH);
    height_ = WebPDemuxGetI(demuxer_, WEBP_FF_CANVAS_HEIGHT);
    frame_count_ = static_cast<int>(WebPDemuxGetI(demuxer_, WEBP_FF_FRAME_COUNT));
    const uint32_t flags = WebPDemuxGetI(demuxer_, WEBP_FF_FORMAT_FLAGS);
    is_animated_ = (flags & ANIMATION_FLAG) != 0;

    if (width_ == 0 || height_ == 0 || frame_count_ == 0) {
      return OM_FORMAT_PARSE_FAILED;
    }

    duration_ms_ = calcTotalDuration();

    Track track;
    track.index = 0;
    track.format.type = OM_MEDIA_IMAGE;
    track.format.codec_id = OM_CODEC_WEBP;
    track.time_base = {1, 1000}; // milliseconds
    track.duration = duration_ms_ > 0 ? duration_ms_ : frame_count_;
    track.format.image.width = width_;
    track.format.image.height = height_;

    tracks_.push_back(track);

    // Position iterator at first frame
    next_frame_n_ = 1;

    return OM_SUCCESS;
  }

  auto readPacket() -> Result<Packet, OMError> override {
    if (next_frame_n_ > frame_count_) {
      return Err(OM_FORMAT_END_OF_FILE);
    }

    releaseIter();
    if (!WebPDemuxGetFrame(demuxer_, next_frame_n_, &iter_)) {
      return Err(OM_FORMAT_END_OF_FILE);
    }
    iter_valid_ = true;

    Packet pkt;
    pkt.stream_index = 0;
    pkt.is_keyframe = true;

    pkt.pts = calcCumulativeDelay(next_frame_n_ - 1);
    pkt.dts = pkt.pts;
    pkt.pos = -1;

    pkt.allocate(iter_.fragment.size);
    memcpy(pkt.bytes.data(), iter_.fragment.bytes, iter_.fragment.size);

    ++next_frame_n_;
    return Ok(std::move(pkt));
  }

  auto seek(int32_t stream_idx, int64_t timestamp, SeekMode mode) -> OMError override {
    if (!is_animated_) {
      next_frame_n_ = 1;
      return OM_SUCCESS;
    }

    if (stream_idx >= 0) {
      timestamp *= 1000;
    }

    int target = 1;
    int64_t t = 0;
    for (int i = 1; i <= frame_count_; ++i) {
      WebPIterator it;
      if (!WebPDemuxGetFrame(demuxer_, i, &it)) break;
      if (t > timestamp) {
        WebPDemuxReleaseIterator(&it);
        break;
      }
      t += it.duration * 1000;
      target = i;
      WebPDemuxReleaseIterator(&it);
    }

    releaseIter();
    next_frame_n_ = target;
    return OM_SUCCESS;
  }

private:
  void releaseIter() {
    if (iter_valid_) {
      WebPDemuxReleaseIterator(&iter_);
      iter_valid_ = false;
    }
  }

  auto calcTotalDuration() const -> int64_t {
    int64_t total = 0;
    for (int i = 1; i <= frame_count_; ++i) {
      WebPIterator it;
      if (!WebPDemuxGetFrame(demuxer_, i, &it)) break;
      total += it.duration;
      WebPDemuxReleaseIterator(&it);
    }
    return total;
  }

  auto calcCumulativeDelay(int frames_before) const -> int64_t {
    int64_t t = 0;
    for (int i = 1; i <= frames_before && i <= frame_count_; ++i) {
      WebPIterator it;
      if (!WebPDemuxGetFrame(demuxer_, i, &it)) break;
      t += it.duration;
      WebPDemuxReleaseIterator(&it);
    }
    return t;
  }
};

auto create_webp_demuxer() -> std::unique_ptr<Demuxer> {
  return std::make_unique<WEBPDemuxer>();
}

const FormatDescriptor FORMAT_WEBP = {
    .container_id = OM_CONTAINER_WEBP,
    .name = "webp",
    .long_name = "WebP",
    .demuxer_factory = [] { return create_webp_demuxer(); },
    .muxer_factory = {},
};

} // namespace openmedia