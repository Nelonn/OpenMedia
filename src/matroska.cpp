#include <mkvmuxer/mkvmuxer.h>
#include <mkvmuxer/mkvwriter.h>
#include <mkvparser/mkvparser.h>
#include <mkvparser/mkvreader.h>
#include <annexb.hpp>
#include <cstdlib>
#include <cstring>
#include <format>
#include <limits>
#include <map>
#include <nal_config.hpp>
#include <optional>
#include <openmedia/audio.hpp>
#include <openmedia/format_api.hpp>
#include <openmedia/io.hpp>
#include <openmedia/log.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/result.hpp>
#include <openmedia/track.hpp>
#include <openmedia/video.hpp>
#include <span>
#include <util/color_codes.hpp>
#include <util/demuxer_base.hpp>
#include <util/io_util.hpp>

namespace openmedia {

static constexpr uint64_t MKV_TRACK_ENTRY = 0xAE;
static constexpr uint64_t MKV_BLOCK_ADDITION_MAPPING = 0x41E4;
static constexpr uint64_t MKV_BLOCK_ADD_ID_VALUE = 0x41F0;
static constexpr uint64_t MKV_BLOCK_ADD_ID_TYPE = 0x41E7;
static constexpr uint64_t MKV_BLOCK_ADD_ID_EXTRA_DATA = 0x41ED;
static constexpr uint64_t MKV_BLOCK_ADD_ID_TYPE_DVCC = 0x64766343; // 'dvcC'
static constexpr uint64_t MKV_BLOCK_ADD_ID_TYPE_DVVC = 0x64767643; // 'dvvC'

struct EbmlElementView {
  uint64_t id = 0;
  uint64_t size = 0;
  size_t payload = 0;
  size_t end = 0;
};

static auto readEbmlVint(std::span<const uint8_t> data, size_t& pos, bool strip_marker) -> std::optional<uint64_t> {
  if (pos >= data.size()) return std::nullopt;

  const uint8_t first = data[pos];
  uint8_t marker = 0x80;
  size_t length = 1;
  while (length <= 8 && (first & marker) == 0) {
    marker >>= 1u;
    ++length;
  }
  if (length > 8 || pos + length > data.size()) return std::nullopt;

  uint64_t value = strip_marker ? (first & ~marker) : first;
  for (size_t i = 1; i < length; ++i) {
    value = (value << 8u) | data[pos + i];
  }

  pos += length;
  return value;
}

static auto nextEbmlElement(std::span<const uint8_t> data, size_t& pos) -> std::optional<EbmlElementView> {
  const auto id = readEbmlVint(data, pos, false);
  const auto size = readEbmlVint(data, pos, true);
  if (!id || !size) return std::nullopt;

  const size_t payload = pos;
  if (*size > data.size() - payload) return std::nullopt;
  const size_t end = payload + static_cast<size_t>(*size);
  pos = end;
  return EbmlElementView {.id = *id, .size = *size, .payload = payload, .end = end};
}

static auto readEbmlUInt(std::span<const uint8_t> data) -> std::optional<uint64_t> {
  if (data.empty() || data.size() > 8) return std::nullopt;
  uint64_t value = 0;
  for (const uint8_t b : data) value = (value << 8u) | b;
  return value;
}

static void setDolbyVisionConfigurationMetadata(Dictionary& metadata, std::span<const uint8_t> data) {
  if (data.size() < 4) return;

  const uint8_t major = data[0];
  const uint8_t minor = data[1];
  const uint8_t profile_level = data[2];
  const uint8_t flags = data[3];

  metadata.setBool(DOLBY_VISION_PRESENT, true);
  metadata.setBinary(DOLBY_VISION_CONFIG, data);
  metadata.setInt32(DOLBY_VISION_VERSION_MAJOR, major);
  metadata.setInt32(DOLBY_VISION_VERSION_MINOR, minor);
  metadata.setInt32(DOLBY_VISION_PROFILE, profile_level >> 1u);
  metadata.setInt32(DOLBY_VISION_LEVEL, ((profile_level & 0x01u) << 5u) | (flags >> 3u));
  metadata.setBool(DOLBY_VISION_RPU_PRESENT, (flags & 0x04u) != 0);
  metadata.setBool(DOLBY_VISION_EL_PRESENT, (flags & 0x02u) != 0);
  metadata.setBool(DOLBY_VISION_BL_PRESENT, (flags & 0x01u) != 0);

  if (data.size() >= 5) {
    metadata.setInt32(DOLBY_VISION_BL_SIGNAL_COMPATIBILITY_ID, data[4] >> 4u);
  }
}

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

  // Length-prefixed NAL streams (CodecPrivate is an avcC/hvcC/vvcC record) are
  // rewritten to Annex-B on the way out; indexed by track index.
  std::map<int32_t, std::unique_ptr<BitStreamFilter>> bsf_;

  const mkvparser::Cluster* current_cluster_ = nullptr;
  const mkvparser::BlockEntry* current_block_entry_ = nullptr;
  // A seek leaves current_block_entry_ on the entry to read, not on a read one.
  bool resume_at_entry_ = false;

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

    long long load_pos = 0;
    long load_size = 0;
    if (segment_->LoadCluster(load_pos, load_size) < 0)
      return OM_FORMAT_PARSE_FAILED;

    current_cluster_ = segment_->GetFirst();

    return OM_SUCCESS;
  }

  void close() override {
    current_block_ = nullptr;
    current_frame_index_ = 0;
    current_block_entry_ = nullptr;
    resume_at_entry_ = false;
    current_cluster_ = nullptr;
    timecode_scale_ = 1'000'000LL;
    segment_.reset();
    mkv_reader_.reset();
    track_map_.clear();
    bsf_.clear();
    input_.reset();
  }

  auto readPacket() -> Result<Packet, OMError> override {
    if (current_block_ && current_frame_index_ < current_block_->GetFrameCount())
      return readFrameFromCurrentBlock();

    while (true) {
      if (!current_cluster_ || current_cluster_->EOS())
        return Err(OM_FORMAT_END_OF_FILE);

      const mkvparser::BlockEntry* entry = nullptr;
      if (resume_at_entry_) {
        entry = current_block_entry_;
        resume_at_entry_ = false;
      } else if (!current_block_entry_) {
        if (current_cluster_->GetFirst(entry) < 0)
          return Err(OM_FORMAT_PARSE_FAILED);
      } else {
        if (current_cluster_->GetNext(current_block_entry_, entry) < 0)
          return Err(OM_FORMAT_PARSE_FAILED);
      }

      if (!entry || entry->EOS()) {
        auto next = advanceCluster(current_cluster_);
        if (next.isErr()) return Err(std::move(next).unwrapErr());

        current_cluster_ = next.unwrap();
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
    if (!segment_ || track_map_.empty()) return OM_FORMAT_PARSE_FAILED;
    if (timestamp < 0) return OM_COMMON_INVALID_ARGUMENT;

    const mkvparser::Tracks* tracks = segment_->GetTracks();
    if (!tracks) return OM_FORMAT_PARSE_FAILED;

    long long target_track_num = track_map_.begin()->first;
    if (stream_idx >= 0) {
      bool found = false;
      for (const auto& [num, idx] : track_map_) {
        if (idx == stream_idx) {
          target_track_num = num;
          found = true;
          break;
        }
      }
      if (!found) return OM_FORMAT_STREAM_NOT_FOUND;
    }

    const long long target_ns =
        (stream_idx < 0) ? timestamp * 1'000LL : timestamp * timecode_scale_;

    current_block_ = nullptr;
    current_frame_index_ = 0;
    current_block_entry_ = nullptr;
    resume_at_entry_ = false;
    current_cluster_ = nullptr;

    const mkvparser::Track* track = tracks->GetTrackByNumber(target_track_num);
    const mkvparser::Cues* cues = segment_->GetCues();

    const bool positioned = cues && track && seekWithCues(*cues, *track, target_ns, mode);
    if (!positioned) {
      if (const OMError err = seekByClusterScan(target_ns, mode); err != OM_SUCCESS)
        return err;
    }

    // Matroska indexes clusters; only DONT_SYNC asks for finer than that.
    if (mode == SeekMode::DONT_SYNC) skipToNearestBlock(target_ns, target_track_num);

    return OM_SUCCESS;
  }

private:
  // A seek preloads its cluster by position, which leaves it outside the
  // sequential parse LoadCluster() continues (its position is an out-parameter,
  // not a request), so only GetNext() knows what follows. Null means exhausted.
  auto advanceCluster(const mkvparser::Cluster* cluster)
      -> Result<const mkvparser::Cluster*, OMError> {
    const mkvparser::Cluster* next = segment_->GetNext(cluster);

    while (next && next->EOS()) {
      long long load_pos = 0;
      long load_size = 0;
      const long rc = segment_->LoadCluster(load_pos, load_size);
      if (rc < 0) return Err(OM_FORMAT_PARSE_FAILED);
      if (rc > 0) {
        next = nullptr;
        break;
      }
      next = segment_->GetNext(cluster);
    }

    return Ok(next);
  }

  // False when the cues cannot answer the seek and the caller should scan.
  auto seekWithCues(const mkvparser::Cues& cues, const mkvparser::Track& track,
                    long long target_ns, SeekMode mode) -> bool {
    // A damaged element leaves the parse position untouched, which DoneParsing()
    // alone never escapes.
    while (!cues.DoneParsing()) {
      if (!cues.LoadCuePoint()) break;
    }

    // Cues::Find() only ever returns the cue at or before the target, and cue
    // points need not index every track.
    const mkvparser::CuePoint::TrackPosition* prev_pos = nullptr;
    const mkvparser::CuePoint::TrackPosition* next_pos = nullptr;
    long long prev_ns = 0;
    long long next_ns = 0;

    for (const mkvparser::CuePoint* cp = cues.GetFirst(); cp != nullptr;
         cp = cues.GetNext(cp)) {
      const mkvparser::CuePoint::TrackPosition* tp = cp->Find(&track);
      if (!tp) continue;

      const long long cue_ns = cp->GetTime(segment_.get());
      if (cue_ns <= target_ns) {
        prev_pos = tp;
        prev_ns = cue_ns;
      } else {
        next_pos = tp;
        next_ns = cue_ns;
        break;
      }
    }

    const mkvparser::CuePoint::TrackPosition* chosen = nullptr;
    switch (mode) {
      case SeekMode::PREVIOUS_SYNC:
      case SeekMode::DONT_SYNC:
        chosen = prev_pos ? prev_pos : next_pos;
        break;

      case SeekMode::NEXT_SYNC:
        // A cue on the target already is the next sync point.
        chosen = (prev_pos && prev_ns == target_ns) ? prev_pos : next_pos;
        if (!chosen && prev_pos) {
          current_cluster_ = nullptr; // past the last sync point
          return true;
        }
        break;

      case SeekMode::CLOSEST_SYNC:
        if (prev_pos && next_pos) {
          chosen = (target_ns - prev_ns <= next_ns - target_ns) ? prev_pos : next_pos;
        } else {
          chosen = prev_pos ? prev_pos : next_pos;
        }
        break;
    }

    if (!chosen) return false;

    // CueClusterPosition and FindOrPreloadCluster() are both relative to the
    // start of the segment payload, so adding m_start would count it twice.
    current_cluster_ = segment_->FindOrPreloadCluster(chosen->m_pos);
    return current_cluster_ != nullptr;
  }

  // Without cues the only index is the cluster timecode, which in practice
  // lands on a keyframe just as the cues would have.
  auto seekByClusterScan(long long target_ns, SeekMode mode) -> OMError {
    const mkvparser::Cluster* prev = nullptr;
    const mkvparser::Cluster* next = nullptr;
    long long prev_ns = 0;
    long long next_ns = 0;

    for (const mkvparser::Cluster* cluster = segment_->GetFirst();
         cluster && !cluster->EOS();) {
      const long long timecode = cluster->GetTimeCode();
      if (timecode < 0) break; // the cluster could not be loaded

      const long long cluster_ns = timecode * timecode_scale_;
      if (cluster_ns > target_ns) {
        next = cluster;
        next_ns = cluster_ns;
        break;
      }

      prev = cluster;
      prev_ns = cluster_ns;

      auto advanced = advanceCluster(cluster);
      if (advanced.isErr()) return std::move(advanced).unwrapErr();
      cluster = advanced.unwrap();
    }

    switch (mode) {
      case SeekMode::NEXT_SYNC:
        current_cluster_ = (prev && prev_ns == target_ns) ? prev : next;
        break;

      case SeekMode::CLOSEST_SYNC:
        if (prev && next) {
          current_cluster_ = (target_ns - prev_ns <= next_ns - target_ns) ? prev : next;
        } else {
          current_cluster_ = prev ? prev : next;
        }
        break;

      default: // PREVIOUS_SYNC, DONT_SYNC
        current_cluster_ = prev ? prev : next;
        break;
    }

    return OM_SUCCESS;
  }

  // DONT_SYNC wants the nearest block, not the nearest sync point, and only the
  // cluster it sits in is indexed.
  void skipToNearestBlock(long long target_ns, long long track_num) {
    const mkvparser::Cluster* cluster = current_cluster_;

    const mkvparser::Cluster* best_cluster = nullptr;
    const mkvparser::BlockEntry* best_entry = nullptr;
    long long best_delta = std::numeric_limits<long long>::max();

    bool finished = false;
    while (!finished && cluster && !cluster->EOS()) {
      const mkvparser::BlockEntry* entry = nullptr;
      if (cluster->GetFirst(entry) < 0) break;

      while (entry && !entry->EOS()) {
        const mkvparser::Block* block = entry->GetBlock();
        if (block && block->GetTrackNumber() == track_num) {
          const long long block_ns = block->GetTime(cluster);
          const long long delta = std::llabs(block_ns - target_ns);
          if (delta < best_delta) {
            best_delta = delta;
            best_entry = entry;
            best_cluster = cluster;
          } else if (block_ns > target_ns) {
            finished = true; // past the target and no longer improving
            break;
          }
        }

        const mkvparser::BlockEntry* following = nullptr;
        if (cluster->GetNext(entry, following) < 0) {
          finished = true;
          break;
        }
        entry = following;
      }

      if (finished) break;

      auto advanced = advanceCluster(cluster);
      if (advanced.isErr()) break;
      cluster = advanced.unwrap();
    }

    if (!best_entry) return;

    current_cluster_ = best_cluster;
    current_block_entry_ = best_entry;
    resume_at_entry_ = true;
  }

  auto readFrameFromCurrentBlock() -> Result<Packet, OMError> {
    const mkvparser::Block::Frame& frame =
        current_block_->GetFrame(current_frame_index_++);

    std::vector<uint8_t> raw(static_cast<size_t>(frame.len));
    if (frame.Read(mkv_reader_.get(), raw.data()) < 0)
      return Err(OM_FORMAT_PARSE_FAILED);

    std::vector<uint8_t> converted;
    FilteredBitstream filtered;
    std::span<const uint8_t> payload(raw);

    if (auto it = bsf_.find(current_stream_index_); it != bsf_.end()) {
      if (current_is_keyframe_) {
        converted = it->second->convert(payload, true);
        payload = converted;
      } else {
        filtered = it->second->filter(std::move(raw));
        payload = filtered.bytes;
      }
    }

    Packet pkt;
    pkt.allocate(payload.size());
    std::memcpy(pkt.bytes.data(), payload.data(), payload.size());

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
          applyNalDecoderConfig(track, next_index);
        }

        applyColour(vt, track);
        parseDolbyVisionBlockAdditionMapping(t, track);

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

  // H.264/H.265/H.266 in Matroska store the same decoder configuration record
  // MP4 keeps in its sample entry, and the frames are length-prefixed NAL units
  // rather than Annex-B. Decoders here expect Annex-B, so replace the extradata
  // with the parameter sets and install a filter for the frames.
  void applyNalDecoderConfig(Track& track, int32_t stream_index) {
    // Some muxers store the parameter sets as a plain Annex-B stream instead of
    // a configuration record; those frames are already Annex-B too.
    if (isAnnexBBitstream(track.extradata)) return;

    std::optional<NalDecoderConfig> config;
    switch (track.format.codec_id) {
      case OM_CODEC_H264: config = parseAvcDecoderConfig(track.extradata); break;
      case OM_CODEC_H265: config = parseHevcDecoderConfig(track.extradata); break;
      case OM_CODEC_VVC:  config = parseVvcDecoderConfig(track.extradata); break;
      default: return;
    }
    if (!config || config->annexb_extradata.empty()) return;

    if (config->profile_idc) {
      track.format.profile = config->constrained_baseline
          ? OM_PROFILE_H264_CONSTRAINED_BASELINE
          : static_cast<OMProfile>(config->profile_idc);
    }

    track.extradata = config->annexb_extradata;
    bsf_[stream_index] = std::make_unique<AnnexBFilter>(
        config->nal_length_size, std::move(config->annexb_extradata));
  }

  // Matroska describes colour in the Colour element of the video track using
  // the same ISO/IEC 23001-8 codes MP4 keeps in `colr`, plus MasteringMetadata
  // and MaxCLL/MaxFALL for HDR10. libwebm parses the element for us; all that
  // is left is the unit conversion, because Matroska stores chromaticities and
  // luminance as floats while the rest of the pipeline uses the SEI fixed point.
  static void applyColour(const mkvparser::VideoTrack* vt, Track& track) {
    const mkvparser::Colour* colour = vt->GetColour();
    if (!colour) return;

    auto& video = track.format.video;
    const long long absent = mkvparser::Colour::kValueNotPresent;

    if (colour->matrix_coefficients != absent) {
      video.color_space = color_codes::colorSpaceFromMatrix(
          static_cast<uint32_t>(colour->matrix_coefficients));
    }
    if (colour->transfer_characteristics != absent) {
      video.transfer_char = color_codes::transferFromCode(
          static_cast<uint32_t>(colour->transfer_characteristics));
    }
    if (colour->primaries != absent) {
      video.color_primaries = color_codes::primariesFromCode(
          static_cast<uint32_t>(colour->primaries));
      // Files that name the primaries but not the matrix are common; the
      // matrix that normally accompanies them is a better guess than BT.709.
      if (colour->matrix_coefficients == absent) {
        video.color_space = color_codes::colorSpaceFromPrimaries(
            static_cast<uint32_t>(colour->primaries));
      }
    }
    if (colour->range != absent) {
      // 0 unspecified, 1 broadcast (limited), 2 full, 3 defined by the matrix.
      switch (colour->range) {
        case 1: video.color_range = OM_COLOR_RANGE_LIMITED; break;
        case 2: video.color_range = OM_COLOR_RANGE_FULL; break;
        default: break;
      }
    }

    auto& cll = video.content_light_level;
    if (colour->max_cll != absent && colour->max_cll > 0) {
      cll.max_content_light_level = static_cast<uint16_t>(colour->max_cll);
      cll.has_value = true;
    }
    if (colour->max_fall != absent && colour->max_fall > 0) {
      cll.max_pic_average_light_level = static_cast<uint16_t>(colour->max_fall);
      cll.has_value = true;
    }

    const mkvparser::MasteringMetadata* mm = colour->mastering_metadata;
    if (!mm) return;

    // Chromaticities are 0..1 and stored in 0.00002 units; luminance is cd/m^2
    // stored in 0.0001 units. The primaries array follows the SEI order: green,
    // blue, red.
    auto chromaticity = [](const mkvparser::PrimaryChromaticity* c,
                           uint16_t (&out)[2]) -> bool {
      if (!c || c->x == mkvparser::MasteringMetadata::kValueNotPresent ||
          c->y == mkvparser::MasteringMetadata::kValueNotPresent) {
        return false;
      }
      out[0] = static_cast<uint16_t>(c->x * 50000.0f + 0.5f);
      out[1] = static_cast<uint16_t>(c->y * 50000.0f + 0.5f);
      return true;
    };

    auto& md = video.mastering_display;
    bool any = false;
    any |= chromaticity(mm->g, md.display_primaries[0]);
    any |= chromaticity(mm->b, md.display_primaries[1]);
    any |= chromaticity(mm->r, md.display_primaries[2]);
    any |= chromaticity(mm->white_point, md.white_point);

    if (mm->luminance_max != mkvparser::MasteringMetadata::kValueNotPresent) {
      md.max_display_mastering_luminance =
          static_cast<uint32_t>(mm->luminance_max * 10000.0f + 0.5f);
      any = true;
    }
    if (mm->luminance_min != mkvparser::MasteringMetadata::kValueNotPresent) {
      md.min_display_mastering_luminance =
          static_cast<uint32_t>(mm->luminance_min * 10000.0f + 0.5f);
      any = true;
    }
    md.has_value = any;
  }

  void parseDolbyVisionBlockAdditionMapping(const mkvparser::Track* parser_track, Track& track) {
    if (!parser_track || !mkv_reader_ || parser_track->m_element_size <= 0) return;
    if (parser_track->m_element_size > static_cast<long long>(std::numeric_limits<int>::max())) return;

    std::vector<uint8_t> track_entry(static_cast<size_t>(parser_track->m_element_size));
    if (mkv_reader_->Read(parser_track->m_element_start,
                          static_cast<long>(track_entry.size()),
                          track_entry.data()) != 0) {
      return;
    }

    size_t pos = 0;
    auto root = nextEbmlElement(track_entry, pos);
    if (!root || root->id != MKV_TRACK_ENTRY) return;

    auto payload = std::span<const uint8_t>(track_entry.data() + root->payload, root->end - root->payload);
    pos = 0;
    while (pos < payload.size()) {
      auto child = nextEbmlElement(payload, pos);
      if (!child) return;
      if (child->id != MKV_BLOCK_ADDITION_MAPPING) continue;

      auto mapping = payload.subspan(child->payload, child->end - child->payload);
      size_t mapping_pos = 0;
      std::optional<uint64_t> block_add_id;
      std::optional<uint64_t> block_add_type;
      std::span<const uint8_t> extra_data;

      while (mapping_pos < mapping.size()) {
        auto item = nextEbmlElement(mapping, mapping_pos);
        if (!item) return;

        auto item_payload = mapping.subspan(item->payload, item->end - item->payload);
        if (item->id == MKV_BLOCK_ADD_ID_VALUE) {
          block_add_id = readEbmlUInt(item_payload);
        } else if (item->id == MKV_BLOCK_ADD_ID_TYPE) {
          block_add_type = readEbmlUInt(item_payload);
        } else if (item->id == MKV_BLOCK_ADD_ID_EXTRA_DATA) {
          extra_data = item_payload;
        }
      }

      if (block_add_type == MKV_BLOCK_ADD_ID_TYPE_DVCC ||
          block_add_type == MKV_BLOCK_ADD_ID_TYPE_DVVC) {
        track.metadata.setBool(DOLBY_VISION_PRESENT, true);
        if (block_add_id) {
          track.metadata.setInt32(DOLBY_VISION_BLOCK_ADD_ID, static_cast<int32_t>(*block_add_id));
        }
        setDolbyVisionConfigurationMetadata(track.metadata, extra_data);
      }
    }
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

class OutputStreamMkvWriter final : public mkvmuxer::IMkvWriter {
  std::unique_ptr<OutputStream> output_;

public:
  explicit OutputStreamMkvWriter(std::unique_ptr<OutputStream> output)
      : output_(std::move(output)) {}

  auto Write(const void* buf, mkvmuxer::uint32 len) -> mkvmuxer::int32 override {
    if (!output_ || !output_->isValid() || !buf || len == 0) return -1;
    const auto data = std::span<const uint8_t>(static_cast<const uint8_t*>(buf), len);
    const size_t written = output_->write(data);
    return (written == len) ? 0 : -1;
  }

  auto Position() const -> mkvmuxer::int64 override {
    if (!output_) return -1;
    return output_->tell();
  }

  auto Position(mkvmuxer::int64 position) -> mkvmuxer::int32 override {
    if (!output_ || !output_->canSeek()) return -1;
    return output_->seek(position, Whence::BEG) ? 0 : -1;
  }

  auto Seekable() const -> bool override {
    return output_ && output_->canSeek();
  }

  void ElementStartNotify(mkvmuxer::uint64 element_id, mkvmuxer::int64 position) override {
    (void) element_id;
    (void) position;
  }
};

class MatroskaMuxer final : public BaseMuxer {
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

  auto open(std::unique_ptr<OutputStream> output) -> OMError override {
    if (!output || !output->isValid()) {
      log(OM_CATEGORY_MUXER, OM_LEVEL_ERROR, "Invalid output stream provided");
      return OM_IO_INVALID_STREAM;
    }

    mkv_writer_ = std::make_unique<OutputStreamMkvWriter>(std::move(output));

    segment_ = std::make_unique<mkvmuxer::Segment>();
    if (!segment_->Init(mkv_writer_.get())) {
      log(OM_CATEGORY_MUXER, OM_LEVEL_ERROR, "Failed to initialize Matroska segment");
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
      log(OM_CATEGORY_MUXER, OM_LEVEL_WARNING, "Finalizing muxer without writing any frames");
    }

    if (!segment_->Finalize()) {
      log(OM_CATEGORY_MUXER, OM_LEVEL_ERROR, "Failed to finalize Matroska segment");
      return OM_FORMAT_MUXING_FAILED;
    }

    finalized_ = true;
    return OM_SUCCESS;
  }

  auto addTrack(const Track& track) -> int32_t override {
    if (!opened_ || finalized_) {
      log(OM_CATEGORY_MUXER, OM_LEVEL_ERROR, "Cannot add track: muxer not in valid state");
      return -1;
    }

    uint64_t track_number = 0;

    if (track.format.type == OM_MEDIA_VIDEO) {
      track_number = addVideoTrack(track);
    } else if (track.format.type == OM_MEDIA_AUDIO) {
      track_number = addAudioTrack(track);
    } else {
      log(OM_CATEGORY_MUXER, OM_LEVEL_ERROR, "Unsupported track type for Matroska muxing");
      return -1;
    }

    if (track_number == 0) {
      log(OM_CATEGORY_MUXER, OM_LEVEL_ERROR, "Failed to add track to Matroska segment");
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
      log(OM_CATEGORY_MUXER, OM_LEVEL_ERROR, "Cannot write packet: muxer not in valid state");
      return OM_FORMAT_MUXING_FAILED;
    }

    if (packet.stream_index < 0) {
      log(OM_CATEGORY_MUXER, OM_LEVEL_ERROR, "Invalid stream index in packet");
      return OM_COMMON_INVALID_ARGUMENT;
    }

    auto it = track_index_to_tracknum_.find(packet.stream_index);
    if (it == track_index_to_tracknum_.end()) {
      log(OM_CATEGORY_MUXER, OM_LEVEL_ERROR, std::format("No track found for stream index, {}", packet.stream_index));
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
      log(OM_CATEGORY_MUXER, OM_LEVEL_ERROR, "Packet has no valid PTS or DTS");
      return OM_FORMAT_INVALID_TIMESTAMP;
    }

    if (timestamp_ns < 0) {
      timestamp_ns = 0;
    }

    const bool is_key = packet.is_keyframe;

    if (!segment_->AddFrame(packet.bytes.data(), packet.bytes.size(),
                            track_number, timestamp_ns, is_key)) {
      log(OM_CATEGORY_MUXER, OM_LEVEL_ERROR, "Failed to add frame to Matroska segment");
      return OM_FORMAT_MUXING_FAILED;
    }

    writing_started_ = true;
    return OM_SUCCESS;
  }

private:
  // Mirror of applyColour on the way out: without this a remux of an HDR10
  // source would lose everything the Colour element carried.
  static void writeColour(mkvmuxer::VideoTrack* video_track, const Track& track) {
    const auto& video = track.format.video;
    mkvmuxer::Colour colour;
    bool any = false;

    if (video.color_space != OM_COLOR_SPACE_UNKNOWN) {
      colour.set_matrix_coefficients(color_codes::matrixFromColorSpace(video.color_space));
      any = true;
    }
    if (video.transfer_char != OM_TRANSFER_UNKNOWN) {
      colour.set_transfer_characteristics(color_codes::codeFromTransfer(video.transfer_char));
      any = true;
    }
    if (video.color_primaries != OM_PRIMARIES_UNKNOWN) {
      colour.set_primaries(color_codes::codeFromPrimaries(video.color_primaries));
      any = true;
    }
    if (video.color_range != OM_COLOR_RANGE_UNSPECIFIED) {
      colour.set_range(video.color_range == OM_COLOR_RANGE_FULL ? 2u : 1u);
      any = true;
    }

    if (video.content_light_level.has_value) {
      colour.set_max_cll(video.content_light_level.max_content_light_level);
      colour.set_max_fall(video.content_light_level.max_pic_average_light_level);
      any = true;
    }

    if (video.mastering_display.has_value) {
      const auto& md = video.mastering_display;
      auto chromaticity = [](const uint16_t (&v)[2]) {
        return mkvmuxer::PrimaryChromaticity(static_cast<float>(v[0]) / 50000.0f,
                                             static_cast<float>(v[1]) / 50000.0f);
      };
      // display_primaries is in SEI order: green, blue, red.
      const mkvmuxer::PrimaryChromaticity g = chromaticity(md.display_primaries[0]);
      const mkvmuxer::PrimaryChromaticity b = chromaticity(md.display_primaries[1]);
      const mkvmuxer::PrimaryChromaticity r = chromaticity(md.display_primaries[2]);
      const mkvmuxer::PrimaryChromaticity wp = chromaticity(md.white_point);

      mkvmuxer::MasteringMetadata mastering;
      if (mastering.SetChromaticity(&r, &g, &b, &wp)) {
        mastering.set_luminance_max(static_cast<float>(md.max_display_mastering_luminance) / 10000.0f);
        mastering.set_luminance_min(static_cast<float>(md.min_display_mastering_luminance) / 10000.0f);
        if (colour.SetMasteringMetadata(mastering)) any = true;
      }
    }

    // Colour::Valid() rejects out-of-range values; writing an invalid element
    // would fail the whole segment, so drop it instead.
    if (any && colour.Valid()) video_track->SetColour(colour);
  }

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

    writeColour(video_track, track);

    if (const Value* value = track.metadata.get(DOLBY_VISION_BLOCK_ADD_ID)) {
      if (auto block_add_id = value->toInt64(); block_add_id && *block_add_id > 0) {
        video_track->set_max_block_additional_id(static_cast<uint64_t>(*block_add_id));
      }
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

const FormatDescriptor FORMAT_MATROSKA = {
    .container_id = OM_CONTAINER_MKV,
    .name = "matroska",
    .long_name = "Matroska / WebM",
    .demuxer_factory = [] { return std::make_unique<MatroskaDemuxer>(); },
    .muxer_factory = [] { return std::make_unique<MatroskaMuxer>(); },
};

} // namespace openmedia
