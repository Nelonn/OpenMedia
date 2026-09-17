#pragma once

#include <mutex>
#include <openmedia/audio.hpp>
#include <openmedia/video.hpp>
#include <openmedia/codec_defs.h>
#include <util/dynamic_loader.hpp>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/mastering_display_metadata.h>

#include <libavcodec/codec_id.h>
}

struct AVFrame;
struct AVCodec;
struct AVCodecContext;
struct AVPacket;
struct AVCodecParserContext;
struct AVFormatContext;
struct AVIOContext;

namespace openmedia {

class LibAVUtil {
public:
  static auto getInstance() -> LibAVUtil&;

  auto load() -> bool;
  auto isLoaded() const -> bool;

  PFN<void*(size_t)> av_malloc = nullptr;
  PFN<void(void*)> av_free = nullptr;
  PFN<AVFrame*()> av_frame_alloc = nullptr;
  PFN<void(AVFrame**)> av_frame_free = nullptr;
  PFN<void(AVFrame*)> av_frame_unref = nullptr;
  PFN<int(AVFrame*, const AVFrame*)> av_frame_ref = nullptr;
  PFN<AVFrame*(const AVFrame*)> av_frame_clone = nullptr;
  PFN<int(uint8_t*[], int[], const uint8_t*, AVPixelFormat, int, int, int)> av_image_fill_arrays = nullptr;
  PFN<int(AVPixelFormat, int, int, int)> av_image_get_buffer_size = nullptr;
  PFN<int(AVSampleFormat)> av_get_bytes_per_sample = nullptr;
  PFN<AVSampleFormat(const char*)> av_get_sample_fmt = nullptr;
  PFN<int(AVSampleFormat)> av_sample_fmt_is_planar = nullptr;
  PFN<AVPixelFormat(const char*)> av_get_pix_fmt = nullptr;
  PFN<const char*(AVPixelFormat)> av_get_pix_fmt_name = nullptr;
  PFN<const char*(AVSampleFormat)> av_get_sample_fmt_name = nullptr;
  PFN<int(AVDictionary**, const char*, const char*, int)> av_dict_set = nullptr;
  PFN<void(AVDictionary**)> av_dict_free = nullptr;
  PFN<const char*(AVMediaType)> av_get_media_type_string = nullptr;
  PFN<void(void (*)(void*, int, const char*, va_list))> av_log_set_callback = nullptr;
  PFN<AVFrameSideData*(const AVFrame*, AVFrameSideDataType)> av_frame_get_side_data = nullptr;
  PFN<AVFrameSideData*(AVFrame*, AVFrameSideDataType, size_t)> av_frame_new_side_data = nullptr;

private:
  LibAVUtil() = default;
  LibAVUtil(const LibAVUtil&) = delete;
  LibAVUtil& operator=(const LibAVUtil&) = delete;

  DynamicLoader library_;
  bool loaded_ = false;
  std::mutex load_mutex_;
};

auto avPixelFormatToOmPixelFormat(AVPixelFormat av_fmt) -> OMPixelFormat;
auto avColorSpaceToOmColorSpace(AVColorSpace av_cs) -> OMColorSpace;
auto avColorTransferToOmTransfer(AVColorTransferCharacteristic av_trc) -> OMTransferCharacteristic;
auto avColorPrimariesToOmPrimaries(AVColorPrimaries av_pri) -> OMColorPrimaries;
auto avColorRangeToOmRange(AVColorRange av_range) -> OMColorRange;
auto omColorRangeToAvRange(OMColorRange om_range) -> AVColorRange;

auto avSampleFormatToOmSampleFormat(AVSampleFormat av_fmt) -> OMSampleFormat;

auto omPixelFormatToAvPixelFormat(OMPixelFormat om_fmt) -> AVPixelFormat;
auto omColorSpaceToAvColorSpace(OMColorSpace om_cs) -> AVColorSpace;
auto omColorTransferToAvTransfer(OMTransferCharacteristic om_trc) -> AVColorTransferCharacteristic;
auto omColorPrimariesToAvPrimaries(OMColorPrimaries om_pri) -> AVColorPrimaries;
auto omSampleFormatToAvSampleFormat(OMSampleFormat om_fmt) -> AVSampleFormat;

auto av_make_q(int num, int den) -> AVRational;

template <typename T>
struct AVDeleter {
  void operator()(T* ptr) const;
};

template <> void AVDeleter<::AVFrame>::operator()(::AVFrame* ptr) const;
template <> void AVDeleter<::AVCodecContext>::operator()(::AVCodecContext* ptr) const;
template <> void AVDeleter<::AVPacket>::operator()(::AVPacket* ptr) const;
template <> void AVDeleter<::AVCodecParserContext>::operator()(::AVCodecParserContext* ptr) const;
template <> void AVDeleter<::AVFormatContext>::operator()(::AVFormatContext* ptr) const;
template <> void AVDeleter<::AVIOContext>::operator()(::AVIOContext* ptr) const;

template <typename T>
using AVPtr = std::unique_ptr<T, AVDeleter<T>>;

static auto avCodecIdToOmCodecId(AVCodecID id) -> OMCodecId {
  switch (id) {
    case AV_CODEC_ID_H264: return OM_CODEC_H264;
    case AV_CODEC_ID_HEVC: return OM_CODEC_H265;
    case AV_CODEC_ID_VVC: return OM_CODEC_H266;
    case AV_CODEC_ID_EVC: return OM_CODEC_EVC;
    case AV_CODEC_ID_VP8: return OM_CODEC_VP8;
    case AV_CODEC_ID_VP9: return OM_CODEC_VP9;
    case AV_CODEC_ID_AV1: return OM_CODEC_AV1;
    case AV_CODEC_ID_MPEG4: return OM_CODEC_MPEG4;
    case AV_CODEC_ID_PRORES: return OM_CODEC_PRORES;
    case AV_CODEC_ID_AAC: return OM_CODEC_AAC;
    case AV_CODEC_ID_MP3: return OM_CODEC_MP3;
    case AV_CODEC_ID_OPUS: return OM_CODEC_OPUS;
    case AV_CODEC_ID_VORBIS: return OM_CODEC_VORBIS;
    case AV_CODEC_ID_FLAC: return OM_CODEC_FLAC;
    case AV_CODEC_ID_PCM_S16LE: return OM_CODEC_PCM_S16LE;
    case AV_CODEC_ID_PCM_F32LE: return OM_CODEC_PCM_F32LE;
    case AV_CODEC_ID_ALAC: return OM_CODEC_ALAC;
    case AV_CODEC_ID_AC3: return OM_CODEC_AC3;
    case AV_CODEC_ID_EAC3: return OM_CODEC_EAC3;
    case AV_CODEC_ID_AC4: return OM_CODEC_AC4;
    case AV_CODEC_ID_MJPEG: return OM_CODEC_JPEG;
    case AV_CODEC_ID_PNG: return OM_CODEC_PNG;
    case AV_CODEC_ID_WEBP: return OM_CODEC_WEBP;
    case AV_CODEC_ID_BMP: return OM_CODEC_BMP;
    case AV_CODEC_ID_TIFF: return OM_CODEC_TIFF;
    case AV_CODEC_ID_GIF: return OM_CODEC_GIF;
    case AV_CODEC_ID_TARGA: return OM_CODEC_TGA;
    default: return OM_CODEC_NONE;
  }
}

} // namespace openmedia
