#pragma once

#include <mutex>
#include <util/dynamic_loader.hpp>
#include "avutil.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

namespace openmedia {

class LibAVFormat {
public:
  static auto getInstance() -> LibAVFormat&;

  auto load() -> bool;
  auto isLoaded() const -> bool;

  PFN<AVFormatContext*()> avformat_alloc_context = nullptr;
  PFN<int(AVFormatContext**, const char*, const AVInputFormat*, AVDictionary**)> avformat_open_input = nullptr;
  PFN<int(AVFormatContext*, AVDictionary**)> avformat_find_stream_info = nullptr;
  PFN<void(AVFormatContext**)> avformat_close_input = nullptr;
  PFN<int(AVFormatContext*, AVPacket*)> av_read_frame = nullptr;
  PFN<int(AVFormatContext*, int, int64_t, int)> av_seek_frame = nullptr;
  PFN<int(AVFormatContext*, int, int64_t, int64_t, int64_t, int)> avformat_seek_file = nullptr;
  PFN<AVIOContext*(unsigned char*, int, int, void*, int (*)(void*, uint8_t*, int), int (*)(void*, const uint8_t*, int), int64_t (*)(void*, int64_t, int))> avio_alloc_context = nullptr;
  PFN<void(AVIOContext**)> avio_context_free = nullptr;
  PFN<int(AVFormatContext**, const AVOutputFormat*, const char*, const char*)> avformat_alloc_output_context2 = nullptr;
  PFN<int(AVFormatContext*, AVDictionary**)> avformat_write_header = nullptr;
  PFN<int(AVFormatContext*, AVPacket*)> av_write_frame = nullptr;
  PFN<int(AVFormatContext*, AVPacket*)> av_interleaved_write_frame = nullptr;
  PFN<int(AVFormatContext*)> av_write_trailer = nullptr;
  PFN<AVStream*(AVFormatContext*, const AVCodec*)> avformat_new_stream = nullptr;
  PFN<void(AVFormatContext*)> avformat_free_context = nullptr;

private:
  LibAVFormat() = default;
  LibAVFormat(const LibAVFormat&) = delete;
  LibAVFormat& operator=(const LibAVFormat&) = delete;

  DynamicLoader library_;
  bool loaded_ = false;
  std::mutex load_mutex_;
};

} // namespace openmedia
