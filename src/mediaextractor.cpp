#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <cstring>
#include <formats.hpp>
#include <openmedia/format_api.hpp>
#include <openmedia/io.hpp>
#include <openmedia/log.hpp>
#include <util/demuxer_base.hpp>
#include <util/dynamic_loader.hpp>
#include <vector>

namespace openmedia {

typedef struct AMediaDataSource AMediaDataSource;
typedef ssize_t (*AMediaDataSourceReadAt)(void* userdata, off64_t offset, void* buffer, size_t size);
typedef off64_t (*AMediaDataSourceGetSize)(void* userdata);

struct MediaNDKFunctions {
  PFN<AMediaDataSource*()> AMediaDataSource_new = nullptr;
  PFN<void(AMediaDataSource*)> AMediaDataSource_delete = nullptr;
  PFN<void(AMediaDataSource*, void*)> AMediaDataSource_setUserdata = nullptr;
  PFN<void(AMediaDataSource*, AMediaDataSourceReadAt)> AMediaDataSource_setReadAt = nullptr;
  PFN<void(AMediaDataSource*, AMediaDataSourceGetSize)> AMediaDataSource_setGetSize = nullptr;
  PFN<media_status_t(AMediaExtractor*, AMediaDataSource*)> AMediaExtractor_setDataSourceCustom = nullptr;

  DynamicLoader library_;
  bool loaded_ = false;
  bool success_ = false;

  auto load() -> bool {
    if (loaded_) return success_;

    library_.open("libmediandk.so");
    if (!library_.success()) {
      loaded_ = true;
      return false;
    }

    AMediaDataSource_new = library_.getProcAddress<PFN<AMediaDataSource*()>>("AMediaDataSource_new");
    AMediaDataSource_delete = library_.getProcAddress<PFN<void(AMediaDataSource*)>>("AMediaDataSource_delete");
    AMediaDataSource_setUserdata = library_.getProcAddress<PFN<void(AMediaDataSource*, void*)>>("AMediaDataSource_setUserdata");
    AMediaDataSource_setReadAt = library_.getProcAddress<PFN<void(AMediaDataSource*, AMediaDataSourceReadAt)>>("AMediaDataSource_setReadAt");
    AMediaDataSource_setGetSize = library_.getProcAddress<PFN<void(AMediaDataSource*, AMediaDataSourceGetSize)>>("AMediaDataSource_setGetSize");
    AMediaExtractor_setDataSourceCustom = library_.getProcAddress<PFN<media_status_t(AMediaExtractor*, AMediaDataSource*)>>("AMediaExtractor_setDataSourceCustom");

    success_ = AMediaDataSource_new && AMediaDataSource_delete &&
               AMediaDataSource_setUserdata && AMediaDataSource_setReadAt &&
               AMediaDataSource_setGetSize && AMediaExtractor_setDataSourceCustom;
    loaded_ = true;
    return success_;
  }
};

static auto mediaNDKFunctions() -> MediaNDKFunctions& {
  static MediaNDKFunctions functions;
  return functions;
}

static auto mimeToCodecId(const char* mime) -> OMCodecId {
  if (!mime) return OM_CODEC_NONE;
  std::string_view s(mime);
  if (s == "video/avc") return OM_CODEC_H264;
  if (s == "video/hevc") return OM_CODEC_H265;
  if (s == "video/vvc") return OM_CODEC_H266;
  if (s == "video/x-vnd.on2.vp8") return OM_CODEC_VP8;
  if (s == "video/x-vnd.on2.vp9") return OM_CODEC_VP9;
  if (s == "video/av01") return OM_CODEC_AV1;
  if (s == "video/mp4v-es") return OM_CODEC_MPEG4;
  if (s == "video/3gpp") return OM_CODEC_H263;
  if (s == "audio/mp4a-latm") return OM_CODEC_AAC;
  if (s == "audio/mpeg") return OM_CODEC_MP3;
  if (s == "audio/opus") return OM_CODEC_OPUS;
  if (s == "audio/vorbis") return OM_CODEC_VORBIS;
  if (s == "audio/flac") return OM_CODEC_FLAC;
  return OM_CODEC_NONE;
}

class MediaExtractorDemuxer final : public BaseDemuxer {
  AMediaExtractor* extractor_ = nullptr;
  AMediaDataSource* ndk_source_ = nullptr;

  struct DataSource {
    InputStream* stream;
  };
  DataSource data_source_ = {};

  static auto readAt(void* userdata, off64_t offset, void* buffer, size_t size) -> ssize_t {
    auto* ds = static_cast<DataSource*>(userdata);
    if (ds->stream->seek(offset, Whence::BEG)) {
      return ds->stream->read({static_cast<uint8_t*>(buffer), size});
    }
    return -1;
  }

  static auto getSize(void* userdata) -> off64_t {
    auto* ds = static_cast<DataSource*>(userdata);
    return ds->stream->size();
  }

public:
  MediaExtractorDemuxer() {
    extractor_ = AMediaExtractor_new();
  }

  ~MediaExtractorDemuxer() override {
    if (extractor_) {
      AMediaExtractor_delete(extractor_);
    }
    if (ndk_source_ && mediaNDKFunctions().AMediaDataSource_delete) {
      mediaNDKFunctions().AMediaDataSource_delete(ndk_source_);
    }
  }

  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    input_ = std::move(input);
    if (!input_ || !input_->isValid()) return OM_IO_INVALID_STREAM;

    auto& media_ndk = mediaNDKFunctions();
    if (!media_ndk.load()) {
      return OM_COMMON_NOT_SUPPORTED;
    }

    data_source_.stream = input_.get();

    ndk_source_ = media_ndk.AMediaDataSource_new();
    media_ndk.AMediaDataSource_setUserdata(ndk_source_, &data_source_);
    media_ndk.AMediaDataSource_setReadAt(ndk_source_, readAt);
    media_ndk.AMediaDataSource_setGetSize(ndk_source_, getSize);

    media_status_t status = media_ndk.AMediaExtractor_setDataSourceCustom(extractor_, ndk_source_);
    if (status != AMEDIA_OK) {
      media_ndk.AMediaDataSource_delete(ndk_source_);
      ndk_source_ = nullptr;
      return OM_IO_OPEN_FAILED;
    }

    size_t track_count = AMediaExtractor_getTrackCount(extractor_);
    for (size_t i = 0; i < track_count; ++i) {
      AMediaFormat* format = AMediaExtractor_getTrackFormat(extractor_, i);
      Track track;
      track.index = static_cast<int32_t>(i);

      const char* mime = nullptr;
      if (AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mime)) {
        track.format.codec_id = mimeToCodecId(mime);
        if (strncmp(mime, "video/", 6) == 0) {
          track.format.type = OM_MEDIA_VIDEO;
          AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, (int32_t*) &track.format.video.width);
          AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, (int32_t*) &track.format.video.height);
        } else if (strncmp(mime, "audio/", 6) == 0) {
          track.format.type = OM_MEDIA_AUDIO;
          AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, (int32_t*) &track.format.audio.sample_rate);
          AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, (int32_t*) &track.format.audio.channels);
        }
      }

      int64_t duration;
      if (AMediaFormat_getInt64(format, AMEDIAFORMAT_KEY_DURATION, &duration)) {
        track.duration = duration;
      }

      track.time_base = {1, 1000000}; // MediaExtractor uses microseconds

      uint8_t* csd;
      size_t csd_size;
      if (AMediaFormat_getBuffer(format, "csd-0", (void**) &csd, &csd_size)) {
        track.extradata.assign(csd, csd + csd_size);
      }
      if (AMediaFormat_getBuffer(format, "csd-1", (void**) &csd, &csd_size)) {
        track.extradata.insert(track.extradata.end(), csd, csd + csd_size);
      }

      tracks_.push_back(track);
      AMediaFormat_delete(format);

      AMediaExtractor_selectTrack(extractor_, i);
    }

    return OM_SUCCESS;
  }

  auto readPacket() -> Result<Packet, OMError> override {
    Packet packet;
    packet.allocate(8192); // Default size or find another way

    ssize_t read_size = AMediaExtractor_readSampleData(extractor_, packet.bytes.data(), packet.bytes.size());
    if (read_size < 0) return Err(OM_FORMAT_END_OF_FILE);
    packet.bytes = packet.bytes.subspan(0, read_size);

    packet.stream_index = AMediaExtractor_getSampleTrackIndex(extractor_);
    packet.pts = AMediaExtractor_getSampleTime(extractor_);
    packet.dts = packet.pts;
    packet.is_keyframe = (AMediaExtractor_getSampleFlags(extractor_) & AMEDIAEXTRACTOR_SAMPLE_FLAG_SYNC) != 0;

    AMediaExtractor_advance(extractor_);
    return Ok(std::move(packet));
  }

  auto seek(int32_t stream_idx, int64_t timestamp, SeekMode mode) -> OMError override {
    int64_t seek_ts = timestamp;
    if (stream_idx >= 0 && stream_idx < static_cast<int32_t>(tracks_.size())) {
      seek_ts = timestamp * 1000000LL * tracks_[stream_idx].time_base.num / tracks_[stream_idx].time_base.den;
    }

    ::SeekMode am_mode = AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC;
    switch (mode) {
      case SeekMode::PREVIOUS_SYNC: am_mode = AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC; break;
      case SeekMode::NEXT_SYNC: am_mode = AMEDIAEXTRACTOR_SEEK_NEXT_SYNC; break;
      case SeekMode::CLOSEST_SYNC: am_mode = AMEDIAEXTRACTOR_SEEK_CLOSEST_SYNC; break;
      default: break;
    }

    media_status_t status = AMediaExtractor_seekTo(extractor_, seek_ts, am_mode);
    return (status == AMEDIA_OK) ? OM_SUCCESS : OM_IO_SEEK_FAILED;
  }
};

const FormatDescriptor FORMAT_MEDIAEXTRACTOR = {
    .container_id = OM_CONTAINER_NONE,
    .name = "mediaextractor",
    .long_name = "Android MediaExtractor",
    .demuxer_factory = []() { return std::make_unique<MediaExtractorDemuxer>(); },
};

} // namespace openmedia
