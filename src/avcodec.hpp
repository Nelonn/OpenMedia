#pragma once

#include <mutex>
#include <openmedia/codec_api.hpp>
#include <util/dynamic_loader.hpp>
#include "avutil.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace openmedia {

class LibAVCodec {
public:
  static auto getInstance() -> LibAVCodec&;

  auto load() -> bool;
  auto isLoaded() const -> bool;

  PFN<const AVCodec*(AVCodecID)> avcodec_find_decoder = nullptr;
  PFN<const AVCodec*(AVCodecID)> avcodec_find_encoder = nullptr;
  PFN<AVCodecContext*(const AVCodec*)> avcodec_alloc_context3 = nullptr;
  PFN<int(AVCodecContext*, const AVCodec*, AVDictionary**)> avcodec_open2 = nullptr;
  PFN<void(AVCodecContext**)> avcodec_free_context = nullptr;
  PFN<int(AVCodecContext*, const AVPacket*)> avcodec_send_packet = nullptr;
  PFN<int(AVCodecContext*, AVFrame*)> avcodec_receive_frame = nullptr;
  PFN<int(AVCodecContext*, const AVFrame*)> avcodec_send_frame = nullptr;
  PFN<int(AVCodecContext*, AVPacket*)> avcodec_receive_packet = nullptr;
  PFN<void(AVCodecContext*)> avcodec_flush_buffers = nullptr;
  PFN<AVMediaType(AVCodecID)> avcodec_get_type = nullptr;
  PFN<AVPacket*()> av_packet_alloc = nullptr;
  PFN<void(AVPacket**)> av_packet_free = nullptr;
  PFN<void(AVPacket*)> av_packet_unref = nullptr;
  PFN<int(AVPacket*, const AVPacket*)> av_packet_ref = nullptr;
  PFN<AVPacket*(const AVPacket*)> av_packet_clone = nullptr;
  PFN<void(AVPacket*, AVPacket*)> av_packet_move_ref = nullptr;
  PFN<int(AVPacket*, int)> av_new_packet = nullptr;
  PFN<int(AVPacket*, int)> av_grow_packet = nullptr;
  PFN<void(AVPacket*, int)> av_shrink_packet = nullptr;

private:
  LibAVCodec() = default;
  LibAVCodec(const LibAVCodec&) = delete;
  LibAVCodec& operator=(const LibAVCodec&) = delete;

  DynamicLoader library_;
  bool loaded_ = false;
  std::mutex load_mutex_;
};

static auto avErrorToOmError(int err) -> OMError {
  if (err >= 0) return OM_SUCCESS;

  if (err == AVERROR_EOF) return OM_IO_END_OF_STREAM;
  if (err == AVERROR(EAGAIN)) return OM_COMMON_UNKNOWN_ERROR;
  if (err == AVERROR(EINVAL)) return OM_COMMON_INVALID_ARGUMENT;
  if (err == AVERROR(ENOMEM)) return OM_COMMON_OUT_OF_MEMORY;

  return OM_COMMON_UNKNOWN_ERROR;
}

} // namespace openmedia
