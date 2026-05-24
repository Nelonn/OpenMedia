#pragma once

#include <mutex>
#include <openmedia/audio.hpp>
#include <openmedia/video.hpp>
#include <util/dynamic_loader.hpp>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/mastering_display_metadata.h>
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

} // namespace openmedia
