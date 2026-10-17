#include <adts.hpp>

#include <algorithm>
#include <cstring>
#include <deque>
#include <optional>
#include <string_view>
#include <util/timescale.hpp>

namespace openmedia {
namespace {

constexpr uint32_t SAMPLE_RATES[] = {96000, 88200, 64000, 48000, 44100, 32000,
                                     24000, 22050, 16000, 12000, 11025, 8000, 7350};
constexpr uint8_t CHANNELS[] = {0, 1, 2, 3, 4, 5, 6, 8};
constexpr std::string_view TIMESTAMP_OWNER = "com.apple.streaming.transportStreamTimestamp";
constexpr uint64_t TIMESTAMP_MASK = (UINT64_C(1) << 33) - 1;

auto id3Size(std::span<const uint8_t> bytes) -> size_t {
  if (bytes.size() < 10 || std::memcmp(bytes.data(), "ID3", 3) != 0) return 0;
  if ((bytes[6] | bytes[7] | bytes[8] | bytes[9]) & 0x80) return 0;
  return 10 + (static_cast<size_t>(bytes[6]) << 21) + (static_cast<size_t>(bytes[7]) << 14)
            + (static_cast<size_t>(bytes[8]) << 7) + bytes[9] + ((bytes[5] & 0x10) ? 10 : 0);
}

auto adtsHeader(std::span<const uint8_t> bytes) -> bool {
  return bytes.size() >= 7 && bytes[0] == 0xFF && (bytes[1] & 0xF6) == 0xF0
         && ((bytes[2] >> 2) & 0x0F) < std::size(SAMPLE_RATES);
}

auto readTimestamp(std::span<const uint8_t> tag) -> std::optional<uint64_t> {
  if (tag.size() < 10 || std::memcmp(tag.data(), "ID3", 3) != 0) return std::nullopt;
  const uint8_t version = tag[3];
  if (version != 3 && version != 4) return std::nullopt;
  size_t at = 10;
  const size_t end = std::min(tag.size(), id3Size(tag));
  while (at + 10 <= end && tag[at] != 0) {
    size_t size = version == 4
                      ? (static_cast<size_t>(tag[at + 4] & 0x7F) << 21)
                            | (static_cast<size_t>(tag[at + 5] & 0x7F) << 14)
                            | (static_cast<size_t>(tag[at + 6] & 0x7F) << 7)
                            | (tag[at + 7] & 0x7F)
                      : (static_cast<size_t>(tag[at + 4]) << 24)
                            | (static_cast<size_t>(tag[at + 5]) << 16)
                            | (static_cast<size_t>(tag[at + 6]) << 8) | tag[at + 7];
    if (size > end - at - 10) break;
    if (std::memcmp(tag.data() + at, "PRIV", 4) == 0 && size >= TIMESTAMP_OWNER.size() + 1 + 8) {
      const uint8_t* payload = tag.data() + at + 10;
      if (std::memcmp(payload, TIMESTAMP_OWNER.data(), TIMESTAMP_OWNER.size()) == 0
          && payload[TIMESTAMP_OWNER.size()] == 0) {
        uint64_t ticks = 0;
        for (size_t i = 0; i < 8; ++i) ticks = (ticks << 8) | payload[TIMESTAMP_OWNER.size() + 1 + i];
        return ticks & TIMESTAMP_MASK;
      }
    }
    at += 10 + size;
  }
  return std::nullopt;
}

class AdtsReader final : public SegmentReader {
  std::vector<Track> tracks_;
  Dictionary metadata_;
  std::deque<Packet> packets_;
  std::vector<uint8_t> carry_;
  bool segment_open_ = false;
  int64_t expected_start_ns_ = 0;
  int64_t cursor_ = 0;
  bool clock_set_ = false;
  uint32_t sample_rate_ = 0;
  std::optional<uint64_t> segment_timestamp_;
  std::optional<uint64_t> anchor_timestamp_;
  int64_t anchor_ns_ = 0;
  size_t frames_ = 0;

public:
  auto openInit(std::span<const uint8_t> init) -> OMError override {
    return init.empty() ? OM_SUCCESS : OM_FORMAT_NOT_SUPPORTED;
  }
  auto tracks() const -> const std::vector<Track>& override { return tracks_; }
  auto metadata() const -> const Dictionary& override { return metadata_; }
  void expectSegmentStart(int64_t start_ns) override { expected_start_ns_ = start_ns; }

  auto pushSegment(std::vector<uint8_t> bytes) -> OMError override {
    if (const OMError error = beginSegment(); error != OM_SUCCESS) return error;
    if (const OMError error = feedSegment(bytes); error != OM_SUCCESS) return error;
    return endSegment();
  }
  auto beginSegment() -> OMError override {
    packets_.clear();
    carry_.clear();
    segment_timestamp_.reset();
    clock_set_ = false;
    frames_ = 0;
    segment_open_ = true;
    return OM_SUCCESS;
  }
  auto feedSegment(std::span<const uint8_t> bytes) -> OMError override {
    if (!segment_open_) return OM_COMMON_NOT_INITIALIZED;
    carry_.insert(carry_.end(), bytes.begin(), bytes.end());
    size_t at = 0;
    while (at < carry_.size()) {
      const auto available = std::span<const uint8_t>(carry_).subspan(at);
      if (available.size() >= 3 && std::memcmp(available.data(), "ID3", 3) == 0) {
        if (available.size() < 10) break;
        const size_t size = id3Size(available);
        if (size < 10 || size > (1u << 20)) return OM_FORMAT_INVALID_HEADER;
        if (available.size() < size) break;
        if (const auto stamp = readTimestamp(available.first(size))) segment_timestamp_ = *stamp;
        at += size;
        continue;
      }
      if (available.size() < 7) break;
      if (!adtsHeader(available)) {
        ++at; // resynchronise past padding or a damaged frame
        continue;
      }
      const size_t frame_size = ((static_cast<size_t>(available[3]) & 3) << 11)
                                | (static_cast<size_t>(available[4]) << 3) | (available[5] >> 5);
      const size_t header_size = (available[1] & 1) ? 7 : 9;
      if (frame_size <= header_size) return OM_FORMAT_INVALID_HEADER;
      if (available.size() < frame_size) break;
      const uint32_t rate_index = (available[2] >> 2) & 15;
      const uint32_t rate = SAMPLE_RATES[rate_index];
      if (sample_rate_ != 0 && sample_rate_ != rate) return OM_FORMAT_NOT_SUPPORTED;
      if (tracks_.empty()) {
        sample_rate_ = rate;
        const uint32_t channel_config = ((available[2] & 1) << 2) | (available[3] >> 6);
        const uint32_t object_type = (available[2] >> 6) + 1;
        Track track;
        track.index = 0;
        track.id = 0;
        track.format.type = OM_MEDIA_AUDIO;
        track.format.codec_id = OM_CODEC_AAC;
        track.format.profile = static_cast<OMProfile>(object_type);
        track.format.audio.sample_rate = rate;
        track.format.audio.channels = channel_config < std::size(CHANNELS) ? CHANNELS[channel_config] : 0;
        track.time_base = {1, static_cast<int32_t>(rate)};
        track.extradata = {static_cast<uint8_t>((object_type << 3) | (rate_index >> 1)),
                           static_cast<uint8_t>(((rate_index & 1) << 7) | (channel_config << 3))};
        tracks_.push_back(std::move(track));
      }
      if (!clock_set_) {
        int64_t start_ns = expected_start_ns_;
        if (segment_timestamp_) {
          if (!anchor_timestamp_) {
            anchor_timestamp_ = segment_timestamp_;
            anchor_ns_ = expected_start_ns_;
          }
          start_ns = anchor_ns_ + static_cast<int64_t>((*segment_timestamp_ - *anchor_timestamp_)
                                                   & TIMESTAMP_MASK) * INT64_C(1'000'000'000) / 90000;
        }
        cursor_ = nsToScale(start_ns, sample_rate_);
        clock_set_ = true;
      }
      Packet packet;
      packet.allocate(frame_size - header_size);
      std::memcpy(packet.bytes.data(), available.data() + header_size, packet.bytes.size());
      packet.stream_index = 0;
      packet.pts = packet.dts = cursor_;
      packet.duration = 1024 * (static_cast<int64_t>(available[6] & 3) + 1);
      packet.is_keyframe = true;
      cursor_ += packet.duration;
      packets_.push_back(std::move(packet));
      ++frames_;
      at += frame_size;
    }
    carry_.erase(carry_.begin(), carry_.begin() + static_cast<std::ptrdiff_t>(at));
    return OM_SUCCESS;
  }
  auto endSegment() -> OMError override {
    if (!segment_open_) return OM_COMMON_NOT_INITIALIZED;
    segment_open_ = false;
    carry_.clear();
    return frames_ == 0 ? OM_FORMAT_NO_STREAMS : OM_SUCCESS;
  }
  auto segmentOpen() const -> bool override { return segment_open_; }
  auto needsSegment() const -> bool override { return packets_.empty(); }
  auto readPacket() -> Result<Packet, OMError> override {
    if (packets_.empty()) return Err(OM_IO_NOT_ENOUGH_DATA);
    Packet packet = std::move(packets_.front());
    packets_.pop_front();
    return Ok(std::move(packet));
  }
  auto nextDecodeTimeNs() const -> int64_t override {
    return packets_.empty() ? INT64_MAX : scaleToNs(packets_.front().dts, sample_rate_);
  }
  auto remainingDurationNs() const -> int64_t override {
    if (packets_.empty()) return 0;
    const Packet& last = packets_.back();
    return scaleToNs(last.dts + last.duration - packets_.front().dts, sample_rate_);
  }
  auto timescaleOf(int32_t) const -> uint32_t override { return sample_rate_; }
  void flushSegments() override {
    packets_.clear();
    carry_.clear();
    segment_open_ = false;
    anchor_timestamp_.reset();
    clock_set_ = false;
  }
};

} // namespace

auto looksLikeAdts(std::span<const uint8_t> bytes) -> bool {
  size_t at = 0;
  while (bytes.size() - at >= 3 && std::memcmp(bytes.data() + at, "ID3", 3) == 0) {
    const size_t size = id3Size(bytes.subspan(at));
    if (size < 10 || size > bytes.size() - at) return false;
    at += size;
  }
  return adtsHeader(bytes.subspan(at));
}

auto createAdtsReader() -> std::unique_ptr<SegmentReader> {
  return std::make_unique<AdtsReader>();
}

} // namespace openmedia
