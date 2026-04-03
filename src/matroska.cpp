#include <mkvmuxer/mkvmuxer.h>
#include <mkvmuxer/mkvwriter.h>
#include <mkvparser/mkvparser.h>
#include <mkvparser/mkvreader.h>
#include <cstring>
#include <format>
#include <map>
#include <openmedia/audio.hpp>
#include <openmedia/format_api.hpp>
#include <openmedia/io.hpp>
#include <openmedia/log.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/result.hpp>
#include <openmedia/track.hpp>
#include <openmedia/video.hpp>
#include <span>
#include <util/demuxer_base.hpp>
#include <util/io_util.hpp>

namespace openmedia {

class InputStreamMkvReader final : public mkvparser::IMkvReader {
  std::unique_ptr<RandomRead> random_;

public:
  explicit InputStreamMkvReader(InputStream* stream) {
    if (stream && stream->canSeek()) {
      random_ = std::make_unique<RandomRead>(stream);
    }
  }

  auto Read(long long position, long length, unsigned char* buffer) -> int override {
    if (!random_ || position < 0 || length < 0) return -1;
    if (!random_->read(position, buffer, static_cast<size_t>(length))) return -1;
    return 0;
  }

  auto Length(long long* total, long long* available) -> int override {
    if (!random_) return -1;
    const int64_t size = random_->size();
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

  auto seek(int32_t stream_idx, int64_t timestamp, SeekMode mode) -> OMError override {
    if (!segment_) return OM_FORMAT_PARSE_FAILED;
    if (track_map_.empty()) return OM_FORMAT_PARSE_FAILED;

    current_block_ = nullptr;
    current_frame_index_ = 0;

    long long target_track_num = track_map_.begin()->first;

    const mkvparser::Cues* cues = segment_->GetCues();

    // Convert timestamp to nanoseconds for Matroska seek.
    // If stream_idx < 0, timestamp is in microseconds; otherwise it's in track time base.
    long long target_ns;
    if (stream_idx < 0) {
      // timestamp is in microseconds, convert to nanoseconds
      target_ns = timestamp * 1'000LL;
    } else {
      // timestamp is in track time base (timecode_scale units), convert to nanoseconds
      target_ns = timestamp * timecode_scale_;
    }

    if (cues) {
      while (!cues->DoneParsing()) {
        cues->LoadCuePoint();
      }

      const mkvparser::Tracks* tracks = segment_->GetTracks();
      const mkvparser::Track* track = tracks->GetTrackByNumber(target_track_num);

      const mkvparser::CuePoint* cue_point = nullptr;
      const mkvparser::CuePoint::TrackPosition* track_pos = nullptr;

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
        const long long cluster_ns = current_cluster_->GetTimeCode() * timecode_scale_;
        if (cluster_ns >= target_ns) break;
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

    if (strcmp(id, "V_VP8") == 0) return OM_CODEC_VP8;
    if (strcmp(id, "V_VP9") == 0) return OM_CODEC_VP9;
    if (strcmp(id, "V_AV1") == 0) return OM_CODEC_AV1;
    if (strcmp(id, "V_MPEG4/ISO/AVC") == 0) return OM_CODEC_H264;
    if (strcmp(id, "V_MPEGH/ISO/HEVC") == 0) return OM_CODEC_H265;
    if (strcmp(id, "V_MPEG2") == 0) return OM_CODEC_MPEG2;
    if (strcmp(id, "V_MPEGI/ISO/VVC") == 0) return OM_CODEC_VVC;

    if (strcmp(id, "A_OPUS") == 0) return OM_CODEC_OPUS;
    if (strcmp(id, "A_VORBIS") == 0) return OM_CODEC_VORBIS;
    if (strcmp(id, "A_AAC") == 0) return OM_CODEC_AAC;
    if (strcmp(id, "A_MPEG/L3") == 0) return OM_CODEC_MP3;
    if (strcmp(id, "A_FLAC") == 0) return OM_CODEC_FLAC;
    if (strcmp(id, "A_PCM/INT/LIT") == 0) return OM_CODEC_PCM_S16LE;
    if (strcmp(id, "A_PCM/INT/BIG") == 0) return OM_CODEC_PCM_S16BE;
    if (strcmp(id, "A_AC3") == 0) return OM_CODEC_AC3;
    if (strcmp(id, "A_EAC3") == 0) return OM_CODEC_EAC3;

    return OM_CODEC_NONE;
  }
};

auto create_matroska_demuxer() -> std::unique_ptr<Demuxer> {
  return std::make_unique<MatroskaDemuxer>();
}

class OutputStreamMkvWriter final : public mkvmuxer::IMkvWriter {
  std::unique_ptr<OutputStream> output_;

public:
  explicit OutputStreamMkvWriter(std::unique_ptr<OutputStream> output)
      : output_(std::move(output)) {}

  auto Write(const void* buf, uint32_t len) -> int32_t override {
    if (!output_ || !output_->isValid() || !buf || len == 0) return -1;
    const auto data = std::span<const uint8_t>(static_cast<const uint8_t*>(buf), len);
    const size_t written = output_->write(data);
    return (written == len) ? 0 : -1;
  }

  auto Position() const -> int64_t override {
    if (!output_) return -1;
    return output_->tell();
  }

  auto Position(int64_t position) -> int32_t override {
    if (!output_ || !output_->canSeek()) return -1;
    return output_->seek(position, Whence::BEG) ? 0 : -1;
  }

  auto Seekable() const -> bool override {
    return output_ && output_->canSeek();
  }

  void ElementStartNotify(uint64_t element_id, int64_t position) override {
    (void) element_id;
    (void) position;
  }
};

class MatroskaMuxer final : public BaseMuxer {
  LoggerRef logger_;
  std::unique_ptr<OutputStreamMkvWriter> mkv_writer_;
  std::unique_ptr<mkvmuxer::Segment> segment_;
  std::map<int32_t, uint64_t> track_index_to_tracknum_;
  std::map<uint64_t, int32_t> tracknum_to_track_index_;
  int32_t next_track_index_ = 0;
  static constexpr int64_t kDefaultTimecodeScale = 1'000'000LL;
  bool writing_started_ = false;

public:
  MatroskaMuxer() = default;
  ~MatroskaMuxer() override { close(); }

  auto open(std::unique_ptr<OutputStream> output, LoggerRef logger) -> OMError override {
    logger_ = logger ? logger : Logger::refDefault();

    if (!output || !output->isValid()) {
      logger_->log(OM_CATEGORY_MUXER, OM_LEVEL_ERROR, "Invalid output stream provided");
      return OM_IO_INVALID_STREAM;
    }

    mkv_writer_ = std::make_unique<OutputStreamMkvWriter>(std::move(output));

    segment_ = std::make_unique<mkvmuxer::Segment>();
    if (!segment_->Init(mkv_writer_.get())) {
      logger_->log(OM_CATEGORY_MUXER, OM_LEVEL_ERROR, "Failed to initialize Matroska segment");
      return OM_FORMAT_MUXING_FAILED;
    }

    segment_->set_mode(mkvmuxer::Segment::kFile);

    segment_->OutputCues(true);

    mkvmuxer::SegmentInfo* info = segment_->GetSegmentInfo();
    if (info) {
      info->set_timecode_scale(kDefaultTimecodeScale);
      info->set_writing_app("OpenMedia");
    }

    opened_ = true;
    finalized_ = false;
    writing_started_ = false;

    return OM_SUCCESS;
  }

  void close() override {
    if (opened_ && !finalized_) {
      finalize();
    }
    segment_.reset();
    mkv_writer_.reset();
    track_index_to_tracknum_.clear();
    tracknum_to_track_index_.clear();
    next_track_index_ = 0;
    writing_started_ = false;
    BaseMuxer::close();
  }

  auto finalize() -> OMError override {
    if (finalized_ || !segment_) {
      return OM_SUCCESS;
    }

    if (!writing_started_) {
      logger_->log(OM_CATEGORY_MUXER, OM_LEVEL_WARNING, "Finalizing muxer without writing any frames");
    }

    if (!segment_->Finalize()) {
      logger_->log(OM_CATEGORY_MUXER, OM_LEVEL_ERROR, "Failed to finalize Matroska segment");
      return OM_FORMAT_MUXING_FAILED;
    }

    finalized_ = true;
    return OM_SUCCESS;
  }

  auto addTrack(const Track& track) -> int32_t override {
    if (!opened_ || finalized_) {
      logger_->log(OM_CATEGORY_MUXER, OM_LEVEL_ERROR, "Cannot add track: muxer not in valid state");
      return -1;
    }

    uint64_t track_number = 0;

    if (track.format.type == OM_MEDIA_VIDEO) {
      track_number = addVideoTrack(track);
    } else if (track.format.type == OM_MEDIA_AUDIO) {
      track_number = addAudioTrack(track);
    } else {
      logger_->log(OM_CATEGORY_MUXER, OM_LEVEL_ERROR, "Unsupported track type for Matroska muxing");
      return -1;
    }

    if (track_number == 0) {
      logger_->log(OM_CATEGORY_MUXER, OM_LEVEL_ERROR, "Failed to add track to Matroska segment");
      return -1;
    }

    const int32_t track_index = next_track_index_++;
    track_index_to_tracknum_[track_index] = track_number;
    tracknum_to_track_index_[track_number] = track_index;

    Track stored_track = track;
    stored_track.index = track_index;
    tracks_.push_back(stored_track);

    return track_index;
  }

  auto writePacket(const Packet& packet) -> OMError override {
    if (!opened_ || finalized_) {
      logger_->log(OM_CATEGORY_MUXER, OM_LEVEL_ERROR, "Cannot write packet: muxer not in valid state");
      return OM_FORMAT_MUXING_FAILED;
    }

    if (packet.stream_index < 0) {
      logger_->log(OM_CATEGORY_MUXER, OM_LEVEL_ERROR, "Invalid stream index in packet");
      return OM_COMMON_INVALID_ARGUMENT;
    }

    auto it = track_index_to_tracknum_.find(packet.stream_index);
    if (it == track_index_to_tracknum_.end()) {
      logger_->log(OM_CATEGORY_MUXER, OM_LEVEL_ERROR, std::format("No track found for stream index, {}", packet.stream_index));
      return OM_FORMAT_STREAM_NOT_FOUND;
    }

    const uint64_t track_number = it->second;

    int64_t timestamp_ns = 0;
    if (packet.pts >= 0) {
      const auto track_it = tracknum_to_track_index_.find(track_number);
      if (track_it != tracknum_to_track_index_.end()) {
        const int32_t idx = track_it->second;
        if (idx >= 0 && idx < static_cast<int32_t>(tracks_.size())) {
          const auto& trk = tracks_[idx];
          if (trk.time_base.den > 0 && trk.time_base.num > 0) {
            const double ts_seconds =
                static_cast<double>(packet.pts) *
                static_cast<double>(trk.time_base.num) /
                static_cast<double>(trk.time_base.den);
            timestamp_ns = static_cast<int64_t>(ts_seconds * 1'000'000'000.0);
          }
        }
      }
    } else if (packet.dts >= 0) {
      const auto track_it = tracknum_to_track_index_.find(track_number);
      if (track_it != tracknum_to_track_index_.end()) {
        const int32_t idx = track_it->second;
        if (idx >= 0 && idx < static_cast<int32_t>(tracks_.size())) {
          const auto& trk = tracks_[idx];
          if (trk.time_base.den > 0 && trk.time_base.num > 0) {
            const double ts_seconds =
                static_cast<double>(packet.dts) *
                static_cast<double>(trk.time_base.num) /
                static_cast<double>(trk.time_base.den);
            timestamp_ns = static_cast<int64_t>(ts_seconds * 1'000'000'000.0);
          }
        }
      }
    } else {
      logger_->log(OM_CATEGORY_MUXER, OM_LEVEL_ERROR, "Packet has no valid PTS or DTS");
      return OM_FORMAT_INVALID_TIMESTAMP;
    }

    if (timestamp_ns < 0) {
      timestamp_ns = 0;
    }

    const bool is_key = packet.is_keyframe;

    if (!segment_->AddFrame(packet.bytes.data(), packet.bytes.size(),
                            track_number, timestamp_ns, is_key)) {
      logger_->log(OM_CATEGORY_MUXER, OM_LEVEL_ERROR, "Failed to add frame to Matroska segment");
      return OM_FORMAT_MUXING_FAILED;
    }

    writing_started_ = true;
    return OM_SUCCESS;
  }

private:
  auto addVideoTrack(const Track& track) -> uint64_t {
    const uint64_t track_number = segment_->AddVideoTrack(
        track.format.video.width, track.format.video.height, 0);

    if (track_number == 0) {
      return 0;
    }

    auto* video_track = static_cast<mkvmuxer::VideoTrack*>(
        segment_->GetTrackByNumber(track_number));

    if (!video_track) {
      return 0;
    }

    const char* codec_id = omCodecToMkvCodec(track.format.codec_id);
    if (codec_id) {
      video_track->set_codec_id(codec_id);
    }

    if (track.format.video.framerate.den > 0 && track.format.video.framerate.num > 0) {
      const double fps =
          static_cast<double>(track.format.video.framerate.num) /
          static_cast<double>(track.format.video.framerate.den);
      video_track->set_frame_rate(fps);
    }

    if (!track.extradata.empty()) {
      video_track->SetCodecPrivate(track.extradata.data(),
                                   track.extradata.size());
    }

    if (track.format.video.width > 0 && track.format.video.height > 0) {
      video_track->set_display_width(track.format.video.width);
      video_track->set_display_height(track.format.video.height);
    }

    return track_number;
  }

  auto addAudioTrack(const Track& track) -> uint64_t {
    const uint64_t track_number = segment_->AddAudioTrack(
        track.format.audio.sample_rate, track.format.audio.channels, 0);

    if (track_number == 0) {
      return 0;
    }

    auto* audio_track = static_cast<mkvmuxer::AudioTrack*>(
        segment_->GetTrackByNumber(track_number));

    if (!audio_track) {
      return 0;
    }

    const char* codec_id = omCodecToMkvCodec(track.format.codec_id);
    if (codec_id) {
      audio_track->set_codec_id(codec_id);
    }

    if (track.format.audio.bit_depth > 0) {
      audio_track->set_bit_depth(track.format.audio.bit_depth);
    }

    if (!track.extradata.empty()) {
      audio_track->SetCodecPrivate(track.extradata.data(),
                                   track.extradata.size());
    }

    return track_number;
  }

  static auto omCodecToMkvCodec(OMCodecId codec_id) -> const char* {
    switch (codec_id) {
      // Video codecs
      case OM_CODEC_VP8:
        return mkvmuxer::Tracks::kVp8CodecId;
      case OM_CODEC_VP9:
        return mkvmuxer::Tracks::kVp9CodecId;
      case OM_CODEC_AV1:
        return mkvmuxer::Tracks::kAv1CodecId;
      case OM_CODEC_H264:
        return "V_MPEG4/ISO/AVC";
      case OM_CODEC_H265:
        return "V_MPEGH/ISO/HEVC";
      case OM_CODEC_MPEG2:
        return "V_MPEG2";
      case OM_CODEC_VVC:
        return "V_MPEGI/ISO/VVC";

      // Audio codecs
      case OM_CODEC_OPUS:
        return mkvmuxer::Tracks::kOpusCodecId;
      case OM_CODEC_VORBIS:
        return mkvmuxer::Tracks::kVorbisCodecId;
      case OM_CODEC_AAC:
        return "A_AAC";
      case OM_CODEC_MP3:
        return "A_MPEG/L3";
      case OM_CODEC_FLAC:
        return "A_FLAC";
      case OM_CODEC_PCM_S16LE:
      case OM_CODEC_PCM_S16BE:
      case OM_CODEC_PCM_F32LE:
        return "A_PCM/INT/LIT";
      case OM_CODEC_AC3:
        return "A_AC3";
      case OM_CODEC_EAC3:
        return "A_EAC3";

      default:
        return nullptr;
    }
  }
};

auto create_matroska_muxer() -> std::unique_ptr<Muxer> {
  return std::make_unique<MatroskaMuxer>();
}

const FormatDescriptor FORMAT_MATROSKA = {
    .container_id = OM_CONTAINER_MKV,
    .name = "matroska",
    .long_name = "Matroska / WebM",
    .demuxer_factory = [] { return create_matroska_demuxer(); },
    .muxer_factory = [] { return create_matroska_muxer(); },
};

} // namespace openmedia
