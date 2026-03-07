#include "avformat.hpp"
#include "avcodec.hpp"
#include <util/demuxer_base.hpp>
#include <openmedia/format_api.hpp>
#include <openmedia/packet.hpp>
#include <openmedia/track.hpp>
#include <cstring>
#include <thread>
#include <atomic>
#include <mutex>

namespace openmedia {

auto LibAVFormat::getInstance() -> LibAVFormat& {
  static LibAVFormat instance;
  return instance;
}

auto LibAVFormat::load() -> bool {
  if (loaded_) return true;

  std::lock_guard<std::mutex> lock(load_mutex_);
  if (loaded_) return true;

  // Ensure avutil and avcodec are loaded first (avformat depends on them)
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

  const char* library_name;
#if defined(_WIN32)
  const char* default_lib = "avformat";
#elif defined(__APPLE__)
  const char* default_lib = "libavformat.dylib";
#else
  const char* default_lib = "libavformat.so";
#endif

  if (!library_name) library_name = default_lib;

  // Load avformat library
  library_.open(library_name);
  if (!library_.success()) {
    return false;
  }

  // Load all function pointers
  avformat_alloc_context = library_.getProcAddress<PFN<AVFormatContext*()>>("avformat_alloc_context");
  avformat_open_input = library_.getProcAddress<PFN<int(AVFormatContext**, const char*, const AVInputFormat*, AVDictionary**)>>("avformat_open_input");
  avformat_find_stream_info = library_.getProcAddress<PFN<int(AVFormatContext*, AVDictionary**)>>("avformat_find_stream_info");
  avformat_close_input = library_.getProcAddress<PFN<void(AVFormatContext**)>>("avformat_close_input");
  av_read_frame = library_.getProcAddress<PFN<int(AVFormatContext*, AVPacket*)>>("av_read_frame");
  av_seek_frame = library_.getProcAddress<PFN<int(AVFormatContext*, int, int64_t, int)>>("av_seek_frame");
  avformat_seek_file = library_.getProcAddress<PFN<int(AVFormatContext*, int, int64_t, int)>>("avformat_seek_file");
  avio_alloc_context = library_.getProcAddress<PFN<AVIOContext*(uint8_t*, int, int, void*, int (*)(void*, uint8_t*, int), int (*)(void*, uint8_t*, int), int64_t (*)(void*, int64_t, int))>>("avio_alloc_context");
  avio_context_free = library_.getProcAddress<PFN<void(AVIOContext**)>>("avio_context_free");
  av_malloc = library_.getProcAddress<PFN<void*(size_t)>>("av_malloc");
  av_free = library_.getProcAddress<PFN<void(void*)>>("av_free");
  avformat_alloc_output_context2 = library_.getProcAddress<PFN<int(AVFormatContext*, AVFormatContext**, int, AVDictionary**)>>("avformat_alloc_output_context2");
  avformat_write_header = library_.getProcAddress<PFN<int(AVFormatContext*, const char*, int, const char*)>>("avformat_write_header");
  av_write_frame = library_.getProcAddress<PFN<int(AVFormatContext*, AVPacket*)>>("av_write_frame");
  av_interleaved_write_frame = library_.getProcAddress<PFN<int(AVFormatContext*, AVPacket*)>>("av_interleaved_write_frame");
  av_write_trailer = library_.getProcAddress<PFN<int(AVFormatContext*)>>("av_write_trailer");
  avformat_new_stream = library_.getProcAddress<PFN<AVStream*(AVFormatContext*, const AVCodec*)>>("avformat_new_stream");

  // Verify required functions
  if (!avformat_alloc_context || !avformat_open_input || !avformat_find_stream_info ||
      !avformat_close_input || !av_read_frame || !av_seek_frame ||
      !avio_alloc_context || !avio_context_free || !av_malloc || !av_free) {
    return false;
  }

  loaded_ = true;
  return true;
}

auto LibAVFormat::isLoaded() const -> bool {
  return loaded_;
}

// ============================================================================
// Format Conversion Helpers
// ============================================================================

static auto avCodecIdToOmCodecId(AVCodecID codec_id) -> OMCodecId {
  switch (codec_id) {
    case AV_CODEC_ID_H264: return OM_CODEC_H264;
    case AV_CODEC_ID_HEVC: return OM_CODEC_H265;
    case AV_CODEC_ID_VP8: return OM_CODEC_VP8;
    case AV_CODEC_ID_VP9: return OM_CODEC_VP9;
    case AV_CODEC_ID_AV1: return OM_CODEC_AV1;
    case AV_CODEC_ID_MPEG4: return OM_CODEC_MPEG4;
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

static auto avErrorToOmError(int err) -> OMError {
  if (err >= 0) return OM_SUCCESS;

  if (err == AVERROR_EOF) return OM_IO_END_OF_STREAM;
  if (err == AVERROR(EAGAIN)) return OM_COMMON_UNKNOWN_ERROR;
  if (err == AVERROR(EINVAL)) return OM_COMMON_INVALID_ARGUMENT;
  if (err == AVERROR(ENOMEM)) return OM_COMMON_OUT_OF_MEMORY;

  return OM_COMMON_UNKNOWN_ERROR;
}

// ============================================================================
// FFmpeg Demuxer Implementation
// ============================================================================

class FFmpegDemuxer final : public BaseDemuxer {
private:
  static constexpr size_t k_io_buffer_size = 65536; // 64 KiB

  AVFormatContext* fmt_ctx_ = nullptr;
  AVIOContext* avio_ctx_ = nullptr;
  uint8_t* io_buf_ = nullptr;

  AVPacket* packet_ = nullptr;

  std::mutex seek_mutex_;
  std::atomic_bool stop_reading_{false};

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

    // Allocate IO buffer
    io_buf_ = static_cast<uint8_t*>(format_loader.av_malloc(k_io_buffer_size));
    if (!io_buf_) {
      return OM_COMMON_OUT_OF_MEMORY;
    }

    // Setup AVIO context
    InputStream* raw_input = input_.get();
    const bool seekable = raw_input->canSeek();

    avio_ctx_ = format_loader.avio_alloc_context(
        io_buf_,
        static_cast<int>(k_io_buffer_size),
        0, // write_flag = 0 (read-only)
        raw_input,
        &readPacketCallback,
        nullptr, // no write callback
        seekable ? &seekCallback : nullptr);

    if (!avio_ctx_) {
      format_loader.av_free(io_buf_);
      io_buf_ = nullptr;
      return OM_COMMON_OUT_OF_MEMORY;
    }

    avio_ctx_->seekable = seekable ? AVIO_SEEKABLE_NORMAL : 0;

    // Allocate format context
    fmt_ctx_ = format_loader.avformat_alloc_context();
    if (!fmt_ctx_) {
      format_loader.avio_context_free(&avio_ctx_);
      avio_ctx_ = nullptr;
      return OM_COMMON_OUT_OF_MEMORY;
    }

    fmt_ctx_->pb = avio_ctx_;

    // Open input (url is nullptr since we use custom IO)
    int ret = format_loader.avformat_open_input(&fmt_ctx_, nullptr, nullptr, nullptr);
    if (ret < 0) {
      close();
      return avErrorToOmError(ret);
    }

    // Find stream info
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

    // Create track information
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
    // TODO: Properly convert AVPacket to Packet
    // om_packet.setData(packet_->data, packet_->data + packet_->size);
    // om_packet.setPts(packet_->pts);
    // om_packet.setDts(packet_->dts);
    // om_packet.setStreamIndex(packet_->stream_index);
    // om_packet.setFlags(packet_->flags);

    return Ok(std::move(om_packet));
  }

  auto seek(int64_t timestamp_ns, int32_t stream_index) -> OMError override {
    if (!initialized_ || !fmt_ctx_) {
      return OM_COMMON_NOT_INITIALIZED;
    }

    std::lock_guard<std::mutex> lock(seek_mutex_);

    auto& format_loader = LibAVFormat::getInstance();
    int ret = format_loader.av_seek_frame(fmt_ctx_, stream_index, timestamp_ns, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
      return avErrorToOmError(ret);
    }

    // Flush codec buffers
    // Note: This would require access to codec contexts

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
      track.format.profile = static_cast<OMProfile>(stream->codecpar->profile);
      track.format.level = stream->codecpar->level;
      track.time_base = {stream->time_base.num, stream->time_base.den};
      track.duration = stream->duration;
      track.bitrate = static_cast<uint32_t>(stream->codecpar->bit_rate);

      // Extract codec-specific info
      if (track.format.type == OM_MEDIA_VIDEO) {
        track.format.video.width = static_cast<uint32_t>(stream->codecpar->width);
        track.format.video.height = static_cast<uint32_t>(stream->codecpar->height);
        track.format.video.framerate = {stream->avg_frame_rate.num, stream->avg_frame_rate.den};
      } else if (track.format.type == OM_MEDIA_AUDIO) {
        track.format.audio.sample_rate = static_cast<uint32_t>(stream->codecpar->sample_rate);
        track.format.audio.channels = static_cast<uint32_t>(stream->codecpar->ch_layout.nb_channels);
      }

      // Copy extradata
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
      format_loader.av_free(io_buf_);
      io_buf_ = nullptr;
    }

    BaseDemuxer::close();
  }

  // IO callbacks
  static int readPacketCallback(void* opaque, uint8_t* buf, int buf_size) {
    auto* input = static_cast<InputStream*>(opaque);
    if (!input) return AVERROR_EOF;

    auto bytes_read = input->read(std::span(buf, static_cast<size_t>(buf_size)));

    if (bytes_read == 0) {
      return AVERROR_EOF;
    }

    return static_cast<int>(bytes_read);
  }

  static int64_t seekCallback(void* opaque, int64_t offset, int whence) {
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

    auto result = input->seek(offset, mode);
    return (result == OM_SUCCESS) ? input->tell() : -1;
  }

  bool initialized_ = false;
};

const FormatDescriptor FORMAT_FFMPEG = {
    .container_id = OM_CONTAINER_NONE,
    .name = "ffmpeg",
    .long_name = "FFmpeg",
    .demuxer_factory = [] { return std::make_unique<FFmpegDemuxer>(); },
    .muxer_factory = {},
};

} // namespace openmedia
