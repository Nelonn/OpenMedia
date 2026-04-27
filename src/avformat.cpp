#include "avformat.hpp"

#include <atomic>
#include <cstring>
#include <mutex>
#include <openmedia/format_api.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/track.hpp>
#include <thread>
#include <util/demuxer_base.hpp>
#include <avcodec.hpp>

namespace openmedia {

auto LibAVFormat::getInstance() -> LibAVFormat& {
  static LibAVFormat instance;
  return instance;
}

auto LibAVFormat::load() -> bool {
  if (loaded_) return true;

  std::lock_guard<std::mutex> lock(load_mutex_);
  if (loaded_) return true;

  if (!LibAVUtil::getInstance().isLoaded()) {
    if (!LibAVUtil::getInstance().load()) {
      return false;
    }
  }
  if (!LibAVCodec::getInstance().isLoaded()) {
    if (!LibAVCodec::getInstance().load()) {
      return false;
    }
  }

#if defined(_WIN32)
  library_.open("avformat-62.dll");
#elif defined(__APPLE__)
  static constexpr const char* kAppleCandidates[] = {
      "/opt/homebrew/lib/libavformat.dylib",
      "/opt/homebrew/lib/libavformat.62.dylib",
      "libavformat-62.dylib",
  };
  for (const char* candidate : kAppleCandidates) {
    library_.open(candidate);
    if (library_.success()) break;
  }
#else
  library_.open("libavformat-62.so");
#endif
  if (!library_.success()) {
    return false;
  }

  avformat_alloc_context = library_.getProcAddress<PFN<AVFormatContext*()>>("avformat_alloc_context");
  avformat_open_input = library_.getProcAddress<PFN<int(AVFormatContext**, const char*, const AVInputFormat*, AVDictionary**)>>("avformat_open_input");
  avformat_find_stream_info = library_.getProcAddress<PFN<int(AVFormatContext*, AVDictionary**)>>("avformat_find_stream_info");
  avformat_close_input = library_.getProcAddress<PFN<void(AVFormatContext**)>>("avformat_close_input");
  av_read_frame = library_.getProcAddress<PFN<int(AVFormatContext*, AVPacket*)>>("av_read_frame");
  av_seek_frame = library_.getProcAddress<PFN<int(AVFormatContext*, int, int64_t, int)>>("av_seek_frame");
  avformat_seek_file = library_.getProcAddress<PFN<int(AVFormatContext*, int, int64_t, int64_t, int64_t, int)>>("avformat_seek_file");
  avio_alloc_context = library_.getProcAddress<PFN<AVIOContext*(uint8_t*, int, int, void*, int (*)(void*, uint8_t*, int), int (*)(void*, const uint8_t*, int), int64_t (*)(void*, int64_t, int))>>("avio_alloc_context");
  avio_context_free = library_.getProcAddress<PFN<void(AVIOContext**)>>("avio_context_free");
  avformat_alloc_output_context2 = library_.getProcAddress<PFN<int(AVFormatContext**, const AVOutputFormat*, const char*, const char*)>>("avformat_alloc_output_context2");
  avformat_write_header = library_.getProcAddress<PFN<int(AVFormatContext*, AVDictionary**)>>("avformat_write_header");
  av_write_frame = library_.getProcAddress<PFN<int(AVFormatContext*, AVPacket*)>>("av_write_frame");
  av_interleaved_write_frame = library_.getProcAddress<PFN<int(AVFormatContext*, AVPacket*)>>("av_interleaved_write_frame");
  av_write_trailer = library_.getProcAddress<PFN<int(AVFormatContext*)>>("av_write_trailer");
  avformat_new_stream = library_.getProcAddress<PFN<AVStream*(AVFormatContext*, const AVCodec*)>>("avformat_new_stream");
  avformat_free_context = library_.getProcAddress<PFN<void(AVFormatContext*)>>("avformat_free_context");

  // Verify required functions
  if (!avformat_alloc_context || !avformat_open_input || !avformat_find_stream_info ||
      !avformat_close_input || !av_read_frame || !av_seek_frame ||
      !avio_alloc_context || !avio_context_free) {
    return false;
  }

  loaded_ = true;
  return true;
}

auto LibAVFormat::isLoaded() const -> bool {
  return loaded_;
}

static auto avCodecIdToOmCodecId(AVCodecID codec_id) -> OMCodecId {
  switch (codec_id) {
    // Video codecs
    case AV_CODEC_ID_H261: return OM_CODEC_H261;
    case AV_CODEC_ID_H263: return OM_CODEC_H263;
    case AV_CODEC_ID_H264: return OM_CODEC_H264;
    case AV_CODEC_ID_HEVC: return OM_CODEC_H265;
    case AV_CODEC_ID_VVC: return OM_CODEC_H266;
    case AV_CODEC_ID_EVC: return OM_CODEC_EVC;
    case AV_CODEC_ID_LCEVC: return OM_CODEC_LCEVC;
    case AV_CODEC_ID_VP8: return OM_CODEC_VP8;
    case AV_CODEC_ID_VP9: return OM_CODEC_VP9;
    case AV_CODEC_ID_AV1: return OM_CODEC_AV1;
    case AV_CODEC_ID_MPEG4: return OM_CODEC_MPEG4;
    case AV_CODEC_ID_THEORA: return OM_CODEC_THEORA;

    // Audio codecs
    case AV_CODEC_ID_ALAC: return OM_CODEC_ALAC;
    case AV_CODEC_ID_AAC: return OM_CODEC_AAC;
    case AV_CODEC_ID_MP3: return OM_CODEC_MP3;
    case AV_CODEC_ID_OPUS: return OM_CODEC_OPUS;
    case AV_CODEC_ID_VORBIS: return OM_CODEC_VORBIS;
    case AV_CODEC_ID_FLAC: return OM_CODEC_FLAC;
    case AV_CODEC_ID_PCM_S16LE: return OM_CODEC_PCM_S16LE;
    case AV_CODEC_ID_PCM_S32LE: return OM_CODEC_PCM_S32LE;
    case AV_CODEC_ID_AC3: return OM_CODEC_AC3;
    case AV_CODEC_ID_EAC3: return OM_CODEC_EAC3;
    default: return OM_CODEC_NONE;
  }
}

static auto avMediaTypeToOmMediaType(AVMediaType media_type) -> OMMediaType {
  switch (media_type) {
    case AVMEDIA_TYPE_VIDEO: return OM_MEDIA_VIDEO;
    case AVMEDIA_TYPE_AUDIO: return OM_MEDIA_AUDIO;
    case AVMEDIA_TYPE_SUBTITLE: return OM_MEDIA_SUBTITLE;
    case AVMEDIA_TYPE_DATA: return OM_MEDIA_DATA;
    default: return OM_MEDIA_NONE;
  }
}

class FFmpegDemuxer final : public BaseDemuxer {
private:
  static constexpr size_t IO_BUFFER_SIZE = 32768;

  AVFormatContext* fmt_ctx_ = nullptr;
  AVIOContext* avio_ctx_ = nullptr;
  uint8_t* io_buf_ = nullptr;

  AVPacket* packet_ = nullptr;

  std::mutex seek_mutex_;
  std::atomic_bool stop_reading_ {false};

public:
  FFmpegDemuxer() = default;

  ~FFmpegDemuxer() override {
    close();
  }

  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    auto& format_loader = LibAVFormat::getInstance();
    auto& codec_loader = LibAVCodec::getInstance();
    auto& util_loader = LibAVUtil::getInstance();

    if (!codec_loader.isLoaded()) {
      if (!codec_loader.load()) {
        return OM_FORMAT_NOT_SUPPORTED;
      }
    }
    if (!format_loader.isLoaded()) {
      if (!format_loader.load()) {
        return OM_FORMAT_NOT_SUPPORTED;
      }
    }

    input_ = std::move(input);
    if (!input_ || !input_->isValid()) {
      return OM_IO_INVALID_STREAM;
    }

    io_buf_ = static_cast<uint8_t*>(util_loader.av_malloc(IO_BUFFER_SIZE));
    if (!io_buf_) {
      return OM_COMMON_OUT_OF_MEMORY;
    }

    InputStream* raw_input = input_.get();
    const bool seekable = raw_input->canSeek();

    avio_ctx_ = format_loader.avio_alloc_context(
        io_buf_,
        static_cast<int>(IO_BUFFER_SIZE),
        0, // write_flag = 0 (read-only)
        raw_input,
        &ioRead,
        nullptr,
        seekable ? &ioSeek : nullptr);

    if (!avio_ctx_) {
      util_loader.av_free(io_buf_);
      io_buf_ = nullptr;
      return OM_COMMON_OUT_OF_MEMORY;
    }

    avio_ctx_->seekable = seekable ? AVIO_SEEKABLE_NORMAL : 0;

    fmt_ctx_ = format_loader.avformat_alloc_context();
    if (!fmt_ctx_) {
      format_loader.avio_context_free(&avio_ctx_);
      avio_ctx_ = nullptr;
      return OM_COMMON_OUT_OF_MEMORY;
    }

    fmt_ctx_->pb = avio_ctx_;

    int ret = format_loader.avformat_open_input(&fmt_ctx_, nullptr, nullptr, nullptr);
    if (ret < 0) {
      close();
      return avErrorToOmError(ret);
    }

    ret = format_loader.avformat_find_stream_info(fmt_ctx_, nullptr);
    if (ret < 0) {
      close();
      return avErrorToOmError(ret);
    }

    // Allocate packet for reading (from avcodec/avutil)
    packet_ = codec_loader.av_packet_alloc();
    if (!packet_) {
      close();
      return OM_COMMON_OUT_OF_MEMORY;
    }

    createTracks();

    initialized_ = true;
    return OM_SUCCESS;
  }

  auto readPacket() -> Result<Packet, OMError> override {
    if (!initialized_ || !fmt_ctx_ || !packet_) {
      return Err(OM_COMMON_NOT_INITIALIZED);
    }

    auto& format_loader = LibAVFormat::getInstance();
    auto& util_loader = LibAVUtil::getInstance();
    auto& codec_loader = LibAVCodec::getInstance();

    // Unref previous packet
    codec_loader.av_packet_unref(packet_);

    // Read next packet
    int ret = format_loader.av_read_frame(fmt_ctx_, packet_);
    if (ret < 0) {
      return Err(avErrorToOmError(ret));
    }

    // Convert to our Packet format
    Packet om_packet;
    om_packet.allocate(static_cast<size_t>(packet_->size));
    std::memcpy(om_packet.bytes.data(), packet_->data, packet_->size);
    om_packet.pts = packet_->pts;
    om_packet.dts = packet_->dts;
    om_packet.stream_index = packet_->stream_index;
    om_packet.flags = packet_->flags;
    om_packet.duration = packet_->duration;
    om_packet.pos = packet_->pos;
    om_packet.is_keyframe = (packet_->flags & AV_PKT_FLAG_KEY) != 0;

    return Ok(std::move(om_packet));
  }

  auto seek(int32_t stream_idx, int64_t timestamp, SeekMode mode) -> OMError override {
    if (!initialized_ || !fmt_ctx_) {
      return OM_COMMON_NOT_INITIALIZED;
    }

    std::lock_guard<std::mutex> lock(seek_mutex_);

    auto& format_loader = LibAVFormat::getInstance();

    int ret = format_loader.av_seek_frame(fmt_ctx_, stream_idx, timestamp, mode == SeekMode::DONT_SYNC ? AVSEEK_FLAG_ANY : AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
      return avErrorToOmError(ret);
    }

    return OM_SUCCESS;
  }

private:
  void createTracks() {
    if (!fmt_ctx_) return;

    for (unsigned int i = 0; i < fmt_ctx_->nb_streams; ++i) {
      AVStream* stream = fmt_ctx_->streams[i];
      if (!stream || !stream->codecpar) continue;

      Track track;
      track.index = stream->index;
      track.id = static_cast<int32_t>(stream->id);
      track.format.type = avMediaTypeToOmMediaType(stream->codecpar->codec_type);
      track.format.codec_id = avCodecIdToOmCodecId(stream->codecpar->codec_id);
      if (stream->codecpar->profile >= 0) {
        track.format.profile = static_cast<OMProfile>(stream->codecpar->profile);
        if (track.format.codec_id == OM_CODEC_AAC) {
          track.format.profile++;
        }
      }
      if (stream->codecpar->level >= 0) {
        track.format.level = stream->codecpar->level;
      }
      track.time_base = {stream->time_base.num, stream->time_base.den};
      track.duration = stream->duration;
      track.bitrate = static_cast<uint32_t>(stream->codecpar->bit_rate);

      if (track.format.type == OM_MEDIA_VIDEO) {
        track.format.video.width = static_cast<uint32_t>(stream->codecpar->width);
        track.format.video.height = static_cast<uint32_t>(stream->codecpar->height);
        track.format.video.framerate = {stream->avg_frame_rate.num, stream->avg_frame_rate.den};
      } else if (track.format.type == OM_MEDIA_AUDIO) {
        track.format.audio.sample_rate = static_cast<uint32_t>(stream->codecpar->sample_rate);
        track.format.audio.channels = static_cast<uint32_t>(stream->codecpar->ch_layout.nb_channels);
      }

      if (stream->codecpar->extradata && stream->codecpar->extradata_size > 0) {
        track.extradata.assign(
            stream->codecpar->extradata,
            stream->codecpar->extradata + stream->codecpar->extradata_size);
      }

      tracks_.push_back(std::move(track));
    }
  }

  void close() override {
    initialized_ = false;
    stop_reading_.store(true);

    auto& format_loader = LibAVFormat::getInstance();
    auto& util_loader = LibAVUtil::getInstance();
    auto& codec_loader = LibAVCodec::getInstance();

    if (packet_) {
      codec_loader.av_packet_free(&packet_);
    }

    if (fmt_ctx_) {
      format_loader.avformat_close_input(&fmt_ctx_);
      fmt_ctx_ = nullptr;
    }

    if (avio_ctx_) {
      format_loader.avio_context_free(&avio_ctx_);
      avio_ctx_ = nullptr;
    }

    if (io_buf_) {
      util_loader.av_free(io_buf_);
      io_buf_ = nullptr;
    }

    BaseDemuxer::close();
  }

  static int ioRead(void* opaque, uint8_t* buf, int buf_size) {
    auto* input = static_cast<InputStream*>(opaque);
    if (!input) return AVERROR_EOF;

    auto bytes_read = input->read(std::span<uint8_t>(buf, static_cast<size_t>(buf_size)));

    if (bytes_read == 0) {
      return AVERROR_EOF;
    }

    return static_cast<int>(bytes_read);
  }

  static auto ioSeek(void* opaque, int64_t offset, int whence) -> int64_t {
    auto* input = static_cast<InputStream*>(opaque);
    if (!input) return -1;

    if (whence == AVSEEK_SIZE) {
      return input->size();
    }

    if (whence & AVSEEK_FORCE) {
      whence &= ~AVSEEK_FORCE;
    }

    Whence mode;
    switch (whence) {
      case SEEK_SET: mode = Whence::BEG; break;
      case SEEK_CUR: mode = Whence::CUR; break;
      case SEEK_END: mode = Whence::END; break;
      default: return -1;
    }

    if (!input->seek(offset, mode)) {
      return -1;
    }

    return input->tell();
  }

  bool initialized_ = false;
};

// Helper for freeing AVIO context
static void avio_context_free_fn(AVIOContext*& ctx) {
  LibAVFormat::getInstance().avio_context_free(&ctx);
  ctx = nullptr;
}

// ============================================================================
// FFmpeg Muxer Implementation
// ============================================================================

static auto omCodecIdToAvCodecId(OMCodecId codec_id) -> AVCodecID {
  switch (codec_id) {
    case OM_CODEC_H264: return AV_CODEC_ID_H264;
    case OM_CODEC_H265: return AV_CODEC_ID_HEVC;
    case OM_CODEC_VP8: return AV_CODEC_ID_VP8;
    case OM_CODEC_VP9: return AV_CODEC_ID_VP9;
    case OM_CODEC_AV1: return AV_CODEC_ID_AV1;
    case OM_CODEC_MPEG4: return AV_CODEC_ID_MPEG4;
    case OM_CODEC_AAC: return AV_CODEC_ID_AAC;
    case OM_CODEC_MP3: return AV_CODEC_ID_MP3;
    case OM_CODEC_OPUS: return AV_CODEC_ID_OPUS;
    case OM_CODEC_VORBIS: return AV_CODEC_ID_VORBIS;
    case OM_CODEC_FLAC: return AV_CODEC_ID_FLAC;
    case OM_CODEC_PCM_S16LE: return AV_CODEC_ID_PCM_S16LE;
    case OM_CODEC_PCM_S32LE: return AV_CODEC_ID_PCM_S32LE;
    case OM_CODEC_AC3: return AV_CODEC_ID_AC3;
    case OM_CODEC_EAC3: return AV_CODEC_ID_EAC3;
    default: return AV_CODEC_ID_NONE;
  }
}

static auto omMediaTypeToAvMediaType(OMMediaType media_type) -> AVMediaType {
  switch (media_type) {
    case OM_MEDIA_VIDEO: return AVMEDIA_TYPE_VIDEO;
    case OM_MEDIA_AUDIO: return AVMEDIA_TYPE_AUDIO;
    case OM_MEDIA_SUBTITLE: return AVMEDIA_TYPE_SUBTITLE;
    case OM_MEDIA_DATA: return AVMEDIA_TYPE_DATA;
    default: return AVMEDIA_TYPE_UNKNOWN;
  }
}

class FFmpegMuxer final : public BaseMuxer {
private:
  static constexpr size_t IO_BUFFER_SIZE = 65536; // 64 KiB

  AVFormatContext* fmt_ctx_ = nullptr;
  AVIOContext* avio_ctx_ = nullptr;
  uint8_t* io_buf_ = nullptr;

  AVPacket* packet_ = nullptr;

  std::mutex write_mutex_;

public:
  FFmpegMuxer() = default;

  ~FFmpegMuxer() override {
    close();
  }

  auto open(std::unique_ptr<OutputStream> output, LoggerRef logger = {}) -> OMError override {
    auto& format_loader = LibAVFormat::getInstance();
    auto& codec_loader = LibAVCodec::getInstance();
    auto& util_loader = LibAVUtil::getInstance();

    if (!codec_loader.isLoaded()) {
      if (!codec_loader.load()) {
        return OM_FORMAT_NOT_SUPPORTED;
      }
    }
    if (!format_loader.isLoaded()) {
      if (!format_loader.load()) {
        return OM_FORMAT_NOT_SUPPORTED;
      }
    }

    output_ = std::move(output);
    if (!output_ || !output_->isValid()) {
      return OM_IO_INVALID_STREAM;
    }

    // Allocate IO buffer
    io_buf_ = static_cast<uint8_t*>(util_loader.av_malloc(IO_BUFFER_SIZE));
    if (!io_buf_) {
      return OM_COMMON_OUT_OF_MEMORY;
    }

    // Setup AVIO context for writing
    OutputStream* raw_output = output_.get();
    avio_ctx_ = format_loader.avio_alloc_context(
        io_buf_,
        static_cast<int>(IO_BUFFER_SIZE),
        1, // write_flag = 1
        raw_output,
        nullptr, // no read callback
        &ioWrite,
        raw_output->canSeek() ? &ioSeek : nullptr);

    if (!avio_ctx_) {
      util_loader.av_free(io_buf_);
      io_buf_ = nullptr;
      return OM_COMMON_OUT_OF_MEMORY;
    }

    avio_ctx_->seekable = raw_output->canSeek() ? AVIO_SEEKABLE_NORMAL : 0;

    // Allocate output context
    int ret = format_loader.avformat_alloc_output_context2(&fmt_ctx_, nullptr, nullptr, nullptr);
    if (ret < 0 || !fmt_ctx_) {
      close();
      return avErrorToOmError(ret);
    }

    fmt_ctx_->pb = avio_ctx_;
    // Don't write header here - wait until all streams are added
    // Headers will be written when finalize() is called or after all tracks are added

    // Allocate packet for writing
    packet_ = codec_loader.av_packet_alloc();
    if (!packet_) {
      close();
      return OM_COMMON_OUT_OF_MEMORY;
    }

    opened_ = true;
    return OM_SUCCESS;
  }

  auto addTrack(const Track& track) -> int32_t override {
    if (!opened_ || !fmt_ctx_) {
      return -1;
    }

    auto& util_loader = LibAVUtil::getInstance();
    auto& format_loader = LibAVFormat::getInstance();
    auto& codec_loader = LibAVCodec::getInstance();

    AVCodecID codec_id = omCodecIdToAvCodecId(track.format.codec_id);
    if (codec_id == AV_CODEC_ID_NONE) {
      return -1;
    }

    AVStream* stream = format_loader.avformat_new_stream(fmt_ctx_, nullptr);
    if (!stream) {
      return -1;
    }

    stream->id = track.id;
    stream->time_base = {track.time_base.num, track.time_base.den};
    stream->duration = track.duration;

    AVCodecParameters* codecpar = stream->codecpar;
    codecpar->codec_type = omMediaTypeToAvMediaType(track.format.type);
    codecpar->codec_id = codec_id;
    codecpar->profile = track.format.profile;
    codecpar->level = track.format.level;
    codecpar->bit_rate = track.bitrate;

    if (track.format.type == OM_MEDIA_VIDEO) {
      codecpar->width = track.format.video.width;
      codecpar->height = track.format.video.height;
    } else if (track.format.type == OM_MEDIA_AUDIO) {
      codecpar->sample_rate = track.format.audio.sample_rate;
      codecpar->ch_layout.nb_channels = track.format.audio.channels;
    }

    // Copy extradata
    if (!track.extradata.empty()) {
      codecpar->extradata = static_cast<uint8_t*>(util_loader.av_malloc(track.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
      if (!codecpar->extradata) {
        return -1;
      }
      std::memcpy(codecpar->extradata, track.extradata.data(), track.extradata.size());
      codecpar->extradata_size = static_cast<int>(track.extradata.size());
    }

    int32_t track_index = static_cast<int32_t>(tracks_.size());
    tracks_.push_back(track);

    return track_index;
  }

  auto writePacket(const Packet& packet) -> OMError override {
    if (!opened_ || !fmt_ctx_ || !packet_) {
      return OM_COMMON_NOT_INITIALIZED;
    }

    // If this is the first packet, write the header
    if (!finalized_ && !header_written_) {
      auto& format_loader = LibAVFormat::getInstance();
      int ret = format_loader.avformat_write_header(fmt_ctx_, nullptr);
      if (ret < 0) {
        return avErrorToOmError(ret);
      }
      header_written_ = true;
    }

    std::lock_guard<std::mutex> lock(write_mutex_);

    auto& format_loader = LibAVFormat::getInstance();
    auto& codec_loader = LibAVCodec::getInstance();

    // Fill AVPacket from our Packet
    codec_loader.av_packet_unref(packet_);

    int ret = codec_loader.av_new_packet(packet_, static_cast<int>(packet.bytes.size()));
    if (ret < 0) {
      return avErrorToOmError(ret);
    }

    std::memcpy(packet_->data, packet.bytes.data(), packet.bytes.size());
    packet_->pts = packet.pts;
    packet_->dts = packet.dts;
    packet_->stream_index = packet.stream_index;
    packet_->duration = packet.duration;
    packet_->pos = packet.pos;
    if (packet.is_keyframe) {
      packet_->flags |= AV_PKT_FLAG_KEY;
    }

    // Write the packet
    ret = format_loader.av_interleaved_write_frame(fmt_ctx_, packet_);
    if (ret < 0) {
      return avErrorToOmError(ret);
    }

    return OM_SUCCESS;
  }

  auto finalize() -> OMError override {
    if (!opened_ || !fmt_ctx_ || finalized_) {
      return OM_COMMON_NOT_INITIALIZED;
    }

    // Write header if not already done
    if (!header_written_) {
      auto& format_loader = LibAVFormat::getInstance();
      int ret = format_loader.avformat_write_header(fmt_ctx_, nullptr);
      if (ret < 0) {
        return avErrorToOmError(ret);
      }
      header_written_ = true;
    }

    // Write trailer
    auto& format_loader = LibAVFormat::getInstance();
    int ret = format_loader.av_write_trailer(fmt_ctx_);
    if (ret < 0) {
      return avErrorToOmError(ret);
    }

    finalized_ = true;
    return OM_SUCCESS;
  }

private:
  bool header_written_ = false;

  void close() override {
    if (opened_ && !finalized_) {
      finalize();
    }

    auto& format_loader = LibAVFormat::getInstance();
    auto& util_loader = LibAVUtil::getInstance();
    auto& codec_loader = LibAVCodec::getInstance();

    if (packet_) {
      codec_loader.av_packet_free(&packet_);
      packet_ = nullptr;
    }

    if (fmt_ctx_) {
      if (avio_ctx_ && !(fmt_ctx_->oformat->flags & AVFMT_NOFILE)) {
        avio_context_free_fn(avio_ctx_);
      }
      format_loader.avformat_free_context(fmt_ctx_);
      fmt_ctx_ = nullptr;
    }

    if (avio_ctx_) {
      format_loader.avio_context_free(&avio_ctx_);
      avio_ctx_ = nullptr;
    }

    if (io_buf_) {
      util_loader.av_free(io_buf_);
      io_buf_ = nullptr;
    }

    BaseMuxer::close();
  }

  static int ioWrite(void* opaque, const uint8_t* buf, int buf_size) {
    auto* output = static_cast<OutputStream*>(opaque);
    if (!output) return -1;

    auto result = output->write(std::span<const uint8_t>(buf, static_cast<size_t>(buf_size)));
    if (result != OM_SUCCESS) {
      return -1;
    }

    return buf_size;
  }

  static auto ioSeek(void* opaque, int64_t offset, int whence) -> int64_t {
    auto* output = static_cast<OutputStream*>(opaque);
    if (!output) return -1;

    if (whence == AVSEEK_SIZE) {
      return output->tell();
    }

    if (whence & AVSEEK_FORCE) {
      whence &= ~AVSEEK_FORCE;
    }

    Whence mode;
    switch (whence) {
      case SEEK_SET: mode = Whence::BEG; break;
      case SEEK_CUR: mode = Whence::CUR; break;
      case SEEK_END: mode = Whence::END; break;
      default: return -1;
    }

    if (!output->seek(offset, mode)) {
      return -1;
    }

    return output->tell();
  }
};

const FormatDescriptor FORMAT_FFMPEG_BMFF = {
    .container_id = OM_CONTAINER_MP4,
    .name = "ffmpeg_bmff",
    .long_name = "BMFF (FFmpeg)",
    .demuxer_factory = [] { return std::make_unique<FFmpegDemuxer>(); },
    .muxer_factory = [] { return std::make_unique<FFmpegMuxer>(); },
};

const FormatDescriptor FORMAT_FFMPEG_MATROSKA = {
    .container_id = OM_CONTAINER_MKV,
    .name = "ffmpeg_matroska",
    .long_name = "Matroska (FFmpeg)",
    .demuxer_factory = [] { return std::make_unique<FFmpegDemuxer>(); },
    .muxer_factory = [] { return std::make_unique<FFmpegMuxer>(); },
};

} // namespace openmedia
