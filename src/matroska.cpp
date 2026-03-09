#include <cstring>
#include <map>
#include <openmedia/audio.hpp>
#include <openmedia/format_api.hpp>
#include <openmedia/io.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/result.hpp>
#include <openmedia/track.hpp>
#include <openmedia/video.hpp>
#include <span>
#include <util/demuxer_base.hpp>
#include <util/io_util.hpp>

#include <mkvparser/mkvparser.h>
#include <mkvparser/mkvreader.h>

namespace openmedia {

class InputStreamMkvReader final : public mkvparser::IMkvReader {
  InputStream* stream_;

public:
  explicit InputStreamMkvReader(InputStream* stream)
      : stream_(stream) {}

  auto Read(long long position, long length, unsigned char* buffer) -> int override {
    if (!stream_ || position < 0 || length < 0) return -1;
    if (!stream_->seek(static_cast<int64_t>(position), Whence::BEG)) return -1;

    const size_t read = stream_->read(std::span<uint8_t>(buffer, static_cast<size_t>(length)));
    return (read == static_cast<size_t>(length)) ? 0 : -1;
  }

  auto Length(long long* total, long long* available) -> int override {
    if (!stream_) return -1;
    const int64_t size = stream_->size();
    if (total) *total = size;
    if (available) *available = size;
    return (size >= 0) ? 0 : -1;
  }
};

class MatroskaDemuxer final : public BaseDemuxer {
  std::unique_ptr<InputStream> input_;
  std::unique_ptr<InputStreamMkvReader> mkv_reader_;
  std::unique_ptr<mkvparser::Segment> segment_;

  std::map<int32_t, int32_t> track_map_;

  const mkvparser::Cluster* current_cluster_ = nullptr;
  const mkvparser::BlockEntry* current_block_entry_ = nullptr;
  long long next_cluster_pos_ = 0;

  long long timecode_scale_ = 1'000'000LL;

  const mkvparser::Block* current_block_ = nullptr;
  int current_frame_index_ = 0;
  int32_t current_stream_index_ = 0;
  bool current_is_keyframe_ = false;
  int64_t current_timestamp_tc_ = 0;

public:
  MatroskaDemuxer() = default;
  ~MatroskaDemuxer() override { close(); }

  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    input_ = std::move(input);
    if (!input_ || !input_->isValid()) return OM_IO_INVALID_STREAM;

    mkv_reader_ = std::make_unique<InputStreamMkvReader>(input_.get());

    long long pos = 0;
    mkvparser::EBMLHeader ebml_header;
    if (ebml_header.Parse(mkv_reader_.get(), pos) < 0)
      return OM_FORMAT_PARSE_FAILED;

    mkvparser::Segment* raw_segment = nullptr;
    if (mkvparser::Segment::CreateInstance(mkv_reader_.get(), pos, raw_segment) != 0)
      return OM_FORMAT_PARSE_FAILED;

    segment_.reset(raw_segment);

    if (segment_->ParseHeaders() < 0)
      return OM_FORMAT_PARSE_FAILED;

    const mkvparser::Tracks* tracks = segment_->GetTracks();
    if (!tracks) return OM_FORMAT_PARSE_FAILED;

    if (const mkvparser::SegmentInfo* info = segment_->GetInfo()) {
      timecode_scale_ = info->GetTimeCodeScale();
      if (timecode_scale_ <= 0) timecode_scale_ = 1'000'000LL;
    }

    if (OMError err = buildTrackMap(tracks); err != OM_SUCCESS)
      return err;

    if (const mkvparser::SegmentInfo* info = segment_->GetInfo()) {
      const long long duration_ns = info->GetDuration();
      const long long timecode_scale = info->GetTimeCodeScale();
      int64_t duration = static_cast<int64_t>(static_cast<double>(duration_ns) / static_cast<double>(timecode_scale));
      for (Track& t : tracks_) {
        t.duration = duration;
      }
    }

    long long cluster_pos = 0;
    long cluster_size = 0;
    if (segment_->LoadCluster(cluster_pos, cluster_size) < 0)
      return OM_FORMAT_PARSE_FAILED;

    next_cluster_pos_ = cluster_pos;
    current_cluster_ = segment_->GetFirst();

    return OM_SUCCESS;
  }

  void close() override {
    current_block_ = nullptr;
    current_frame_index_ = 0;
    current_block_entry_ = nullptr;
    current_cluster_ = nullptr;
    next_cluster_pos_ = 0;
    timecode_scale_ = 1'000'000LL;
    segment_.reset();
    mkv_reader_.reset();
    track_map_.clear();
    input_.reset();
  }

  auto readPacket() -> Result<Packet, OMError> override {
    if (current_block_ && current_frame_index_ < current_block_->GetFrameCount())
      return readFrameFromCurrentBlock();

    while (true) {
      if (!current_cluster_ || current_cluster_->EOS())
        return Err(OM_FORMAT_END_OF_FILE);

      const mkvparser::BlockEntry* entry = nullptr;
      if (!current_block_entry_) {
        if (current_cluster_->GetFirst(entry) < 0)
          return Err(OM_FORMAT_PARSE_FAILED);
      } else {
        if (current_cluster_->GetNext(current_block_entry_, entry) < 0)
          return Err(OM_FORMAT_PARSE_FAILED);
      }

      if (!entry || entry->EOS()) {
        long load_size = 0;
        const int rc = segment_->LoadCluster(next_cluster_pos_, load_size);

        if (rc < 0)
          return Err(OM_FORMAT_PARSE_FAILED);

        if (rc == 1) {
          current_cluster_ = nullptr;
        } else {
          current_cluster_ = segment_->GetLast();
        }

        current_block_entry_ = nullptr;
        continue;
      }

      current_block_entry_ = entry;

      const mkvparser::Block* block = entry->GetBlock();
      if (!block || block->GetFrameCount() <= 0) continue;

      const long long track_num = block->GetTrackNumber();
      auto it = track_map_.find(static_cast<int32_t>(track_num));
      if (it == track_map_.end()) continue;

      current_block_ = block;
      current_frame_index_ = 0;
      current_stream_index_ = it->second;
      current_is_keyframe_ = block->IsKey();

      const long long raw_ns = block->GetTime(current_cluster_);
      // Convert nanoseconds to stream time base units (timecode scale units).
      // time_base = {timecode_scale_, 1_000_000_000}, so 1 unit = timecode_scale_ ns.
      current_timestamp_tc_ = raw_ns >= 0 ? raw_ns / timecode_scale_ : 0;

      return readFrameFromCurrentBlock();
    }
  }

  auto seek(int64_t timestamp_ms, int32_t stream_index) -> OMError override {
    if (!segment_) return OM_FORMAT_PARSE_FAILED;

    current_block_ = nullptr;
    current_frame_index_ = 0;

    long long target_track_num = -1;
    for (const auto& [tnum, sidx] : track_map_) {
      if (sidx == stream_index) {
        target_track_num = tnum;
        break;
      }
    }

    const mkvparser::Cues* cues = segment_->GetCues();

    if (cues && target_track_num > 0) {
      while (!cues->DoneParsing()) {
        cues->LoadCuePoint();
      }

      const mkvparser::Tracks* tracks = segment_->GetTracks();
      const mkvparser::Track* track =
          tracks->GetTrackByNumber(static_cast<unsigned long>(target_track_num));

      const mkvparser::CuePoint* cue_point = nullptr;
      const mkvparser::CuePoint::TrackPosition* track_pos = nullptr;

      const long long target_ns = timestamp_ms * 1'000'000LL;

      if (cues->Find(target_ns, track, cue_point, track_pos) && track_pos) {
        const long long abs_pos =
            segment_->m_start + static_cast<long long>(track_pos->m_pos);
        current_cluster_ = segment_->FindOrPreloadCluster(abs_pos);
        next_cluster_pos_ = abs_pos;
      } else {
        current_cluster_ = segment_->GetFirst();
        next_cluster_pos_ = 0;
      }
    } else {
      current_cluster_ = segment_->GetFirst();
      next_cluster_pos_ = 0;

      while (current_cluster_ && !current_cluster_->EOS()) {
        // Convert cluster timecode units to ms for comparison.
        // cluster timecode * timecode_scale_ gives nanoseconds; divide by 1_000_000 for ms.
        const long long cluster_ms =
            (current_cluster_->GetTimeCode() * timecode_scale_) / 1'000'000LL;
        if (cluster_ms >= timestamp_ms)
          break;
        current_cluster_ = segment_->GetNext(current_cluster_);
      }
    }

    current_block_entry_ = nullptr;
    return OM_SUCCESS;
  }

private:
  auto readFrameFromCurrentBlock() -> Result<Packet, OMError> {
    const mkvparser::Block::Frame& frame =
        current_block_->GetFrame(current_frame_index_++);

    Packet pkt;
    pkt.allocate(static_cast<size_t>(frame.len));

    if (frame.Read(mkv_reader_.get(), pkt.bytes.data()) < 0)
      return Err(OM_FORMAT_PARSE_FAILED);

    pkt.pts = current_timestamp_tc_;
    pkt.dts = current_timestamp_tc_;

    const auto* block_group = dynamic_cast<const mkvparser::BlockGroup*>(current_block_entry_);
    pkt.duration = (block_group && block_group->GetDurationTimeCode() > 0)
                       ? block_group->GetDurationTimeCode()
                       : 0;

    pkt.stream_index = current_stream_index_;
    pkt.is_keyframe = current_is_keyframe_;
    pkt.pos = static_cast<int64_t>(frame.pos);

    if (current_frame_index_ >= current_block_->GetFrameCount()) {
      current_block_ = nullptr;
    }

    return Ok(std::move(pkt));
  }

  auto buildTrackMap(const mkvparser::Tracks* tracks) -> OMError {
    const unsigned long count = tracks->GetTracksCount();
    int32_t next_index = 0;

    const Rational mkv_time_base = {
        static_cast<int32_t>(timecode_scale_),
        1'000'000'000};

    for (unsigned long i = 0; i < count; ++i) {
      const mkvparser::Track* t = tracks->GetTrackByIndex(i);
      if (!t) continue;

      const long type = t->GetType();

      if (type == mkvparser::Track::kVideo) {
        const auto* vt = static_cast<const mkvparser::VideoTrack*>(t);

        Track track {};
        track.index = next_index;
        track.id = static_cast<int32_t>(t->GetNumber());
        track.time_base = mkv_time_base;
        track.format.type = OM_MEDIA_VIDEO;
        track.format.codec_id = mkvCodecIdToOMCodec(t->GetCodecId());
        track.format.video.width = static_cast<uint32_t>(vt->GetWidth());
        track.format.video.height = static_cast<uint32_t>(vt->GetHeight());

        const double fps = vt->GetFrameRate();
        track.format.video.framerate = fps > 0
                                           ? Rational {static_cast<int32_t>(fps * 1000.0 + 0.5), 1000}
                                           : Rational {0, 1};

        size_t cp_size = 0;
        const unsigned char* cp = t->GetCodecPrivate(cp_size);
        if (cp && cp_size) {
          track.extradata.assign(cp, cp + cp_size);
        }

        tracks_.push_back(track);
        track_map_[static_cast<int32_t>(t->GetNumber())] = next_index++;

      } else if (type == mkvparser::Track::kAudio) {
        const auto* at = static_cast<const mkvparser::AudioTrack*>(t);

        Track track {};
        track.index = next_index;
        track.id = static_cast<int32_t>(t->GetNumber());
        track.time_base = mkv_time_base;
        track.format.type = OM_MEDIA_AUDIO;
        track.format.codec_id = mkvCodecIdToOMCodec(t->GetCodecId());
        track.format.audio.sample_rate = static_cast<uint32_t>(at->GetSamplingRate());
        track.format.audio.channels = static_cast<uint32_t>(at->GetChannels());
        track.format.audio.bit_depth = static_cast<uint32_t>(at->GetBitDepth());

        size_t cp_size = 0;
        const unsigned char* cp = t->GetCodecPrivate(cp_size);
        if (cp && cp_size) {
          track.extradata.assign(cp, cp + cp_size);
        }

        tracks_.push_back(track);
        track_map_[static_cast<int32_t>(t->GetNumber())] = next_index++;
      }
    }

    return OM_SUCCESS;
  }

  static auto mkvCodecIdToOMCodec(const char* id) -> OMCodecId {
    if (!id) return OM_CODEC_NONE;

    if (std::strcmp(id, "V_VP8") == 0) return OM_CODEC_VP8;
    if (std::strcmp(id, "V_VP9") == 0) return OM_CODEC_VP9;
    if (std::strcmp(id, "V_AV1") == 0) return OM_CODEC_AV1;
    if (std::strcmp(id, "V_MPEG4/ISO/AVC") == 0) return OM_CODEC_H264;
    if (std::strcmp(id, "V_MPEGH/ISO/HEVC") == 0) return OM_CODEC_H265;
    if (std::strcmp(id, "V_MPEG2") == 0) return OM_CODEC_MPEG2;
    if (std::strcmp(id, "V_MPEGI/ISO/VVC") == 0) return OM_CODEC_VVC;

    if (std::strcmp(id, "A_OPUS") == 0) return OM_CODEC_OPUS;
    if (std::strcmp(id, "A_VORBIS") == 0) return OM_CODEC_VORBIS;
    if (std::strcmp(id, "A_AAC") == 0) return OM_CODEC_AAC;
    if (std::strcmp(id, "A_MPEG/L3") == 0) return OM_CODEC_MP3;
    if (std::strcmp(id, "A_FLAC") == 0) return OM_CODEC_FLAC;
    if (std::strcmp(id, "A_PCM/INT/LIT") == 0) return OM_CODEC_PCM_S16LE;
    if (std::strcmp(id, "A_PCM/INT/BIG") == 0) return OM_CODEC_PCM_S16BE;
    if (std::strcmp(id, "A_AC3") == 0) return OM_CODEC_AC3;
    if (std::strcmp(id, "A_EAC3") == 0) return OM_CODEC_EAC3;

    return OM_CODEC_NONE;
  }
};

auto create_matroska_demuxer() -> std::unique_ptr<Demuxer> {
  return std::make_unique<MatroskaDemuxer>();
}

const FormatDescriptor FORMAT_MATROSKA = {
    .container_id = OM_CONTAINER_MKV,
    .name = "matroska",
    .long_name = "Matroska / WebM",
    .demuxer_factory = [] { return create_matroska_demuxer(); },
    .muxer_factory = {},
};

} // namespace openmedia
