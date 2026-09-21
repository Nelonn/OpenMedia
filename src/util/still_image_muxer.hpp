#pragma once

#include <cstdint>
#include <openmedia/format_api.hpp>
#include <span>
#include <util/demuxer_base.hpp>
#include <vector>

namespace openmedia {

/**
 * @brief Muxer for containers that are the encoded image itself.
 *
 * PNG, JPEG, BMP and friends have no container layer of their own: the encoder
 * already produced a complete file, so muxing one is writing those bytes out.
 * A format that can also hold an animation overrides writeFrames() and says so
 * through acceptsMultipleFrames().
 */
class StillImageMuxer : public BaseMuxer {
public:
  struct StoredFrame {
    std::vector<uint8_t> bytes;
    int64_t pts = 0;
    int64_t duration = 0; // in track time base, 0 when the packet did not say
  };

  explicit StillImageMuxer(OMCodecId codec_id) noexcept
      : codec_id_(codec_id) {}

  ~StillImageMuxer() override { close(); }

  auto open(std::unique_ptr<OutputStream> output) -> OMError override {
    if (!output || !output->isValid()) {
      return OM_IO_INVALID_STREAM;
    }

    output_ = std::move(output);
    opened_ = true;
    finalized_ = false;
    frames_.clear();
    tracks_.clear();
    return OM_SUCCESS;
  }

  void close() override {
    BaseMuxer::close();
    frames_.clear();
  }

  auto addTrack(const Track& track) -> int32_t override {
    if (!opened_ || finalized_ || !tracks_.empty()) {
      return -1;
    }
    if (track.format.codec_id != codec_id_) {
      return -1;
    }
    if (track.format.type != OM_MEDIA_IMAGE && track.format.type != OM_MEDIA_VIDEO) {
      return -1;
    }

    Track stored_track = track;
    stored_track.index = 0;
    if (stored_track.time_base.num <= 0 || stored_track.time_base.den <= 0) {
      stored_track.time_base = {1, 1000}; // milliseconds
    }

    tracks_.push_back(std::move(stored_track));
    return 0;
  }

  auto writePacket(const Packet& packet) -> OMError override {
    if (!opened_ || finalized_ || tracks_.empty()) {
      return OM_COMMON_NOT_INITIALIZED;
    }
    // Packets straight out of an encoder carry no stream index, so -1 means
    // "the only track there is" rather than a mismatch.
    if (packet.stream_index != 0 && packet.stream_index != -1) {
      return OM_FORMAT_STREAM_NOT_FOUND;
    }
    if (packet.bytes.empty()) {
      return OM_FORMAT_INVALID_PACKET;
    }
    // Concatenating would leave a file that only ever shows the first image,
    // so say so here rather than at finalize().
    if (!frames_.empty() && !acceptsMultipleFrames()) {
      return OM_FORMAT_MUXING_FAILED;
    }

    StoredFrame frame;
    frame.bytes.assign(packet.bytes.begin(), packet.bytes.end());
    frame.pts = packet.pts < 0 ? 0 : packet.pts;
    frame.duration = packet.duration > 0 ? packet.duration : 0;
    frames_.push_back(std::move(frame));
    return OM_SUCCESS;
  }

  auto finalize() -> OMError override {
    if (!opened_ || finalized_) {
      return OM_SUCCESS;
    }
    if (tracks_.empty()) {
      return OM_COMMON_NOT_INITIALIZED;
    }
    if (frames_.empty()) {
      return OM_FORMAT_MUXING_FAILED;
    }

    if (const OMError err = writeFrames(); err != OM_SUCCESS) {
      return err;
    }
    if (!output_->flush()) {
      return OM_IO_WRITE_FAILED;
    }

    finalized_ = true;
    return OM_SUCCESS;
  }

protected:
  // What a frame gets when neither the packet nor its neighbours say anything,
  // matching the 10fps most animation tools default to.
  static constexpr int64_t DEFAULT_FRAME_DURATION_MS = 100;

  std::vector<StoredFrame> frames_;

  auto writeAll(std::span<const uint8_t> bytes) -> bool {
    return output_->write(bytes) == bytes.size();
  }

  virtual auto acceptsMultipleFrames() const -> bool { return false; }

  // The whole job for a still container: hand the encoder's file straight on.
  virtual auto writeFrames() -> OMError {
    return writeAll(frames_.front().bytes) ? OM_SUCCESS : OM_IO_WRITE_FAILED;
  }

  auto toMillis(int64_t value) const -> int64_t {
    const auto tb = tracks_.front().time_base;
    if (tb.num <= 0 || tb.den <= 0) return value;
    return value * static_cast<int64_t>(tb.num) * 1000 / static_cast<int64_t>(tb.den);
  }

  // A packet's own duration wins; otherwise the gap to the next frame stands
  // in, and the last frame reuses whatever the one before it got.
  auto frameDurationMs(size_t index) const -> int64_t {
    const auto& frame = frames_[index];
    if (frame.duration > 0) {
      return toMillis(frame.duration);
    }
    if (index + 1 < frames_.size()) {
      const int64_t delta = frames_[index + 1].pts - frame.pts;
      if (delta > 0) return toMillis(delta);
    } else if (index > 0) {
      return frameDurationMs(index - 1);
    }
    return DEFAULT_FRAME_DURATION_MS;
  }

private:
  OMCodecId codec_id_;
};

} // namespace openmedia
