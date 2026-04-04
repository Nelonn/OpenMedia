#include "avutil.hpp"

namespace openmedia {

auto LibAVUtil::getInstance() -> LibAVUtil& {
  static LibAVUtil instance;
  return instance;
}

auto LibAVUtil::load() -> bool {
  if (loaded_) return true;

  std::lock_guard<std::mutex> lock(load_mutex_);
  if (loaded_) return true;

#if defined(_WIN32)
  const char* library_name = "avutil-60.dll";
#elif defined(__APPLE__)
  const char* library_name = "libavutil-60.dylib";
#else
  const char* library_name = "libavutil-60.so";
#endif

  library_.open(library_name);
  if (!library_.success()) {
    return false;
  }

  av_malloc = library_.getProcAddress<decltype(av_malloc)>("av_malloc");
  av_free = library_.getProcAddress<decltype(av_free)>("av_free");
  av_frame_alloc = library_.getProcAddress<decltype(av_frame_alloc)>("av_frame_alloc");
  av_frame_free = library_.getProcAddress<decltype(av_frame_free)>("av_frame_free");
  av_frame_unref = library_.getProcAddress<decltype(av_frame_unref)>("av_frame_unref");
  av_frame_ref = library_.getProcAddress<decltype(av_frame_ref)>("av_frame_ref");
  av_frame_clone = library_.getProcAddress<decltype(av_frame_clone)>("av_frame_clone");
  av_image_fill_arrays = library_.getProcAddress<decltype(av_image_fill_arrays)>("av_image_fill_arrays");
  av_image_get_buffer_size = library_.getProcAddress<decltype(av_image_get_buffer_size)>("av_image_get_buffer_size");
  av_get_bytes_per_sample = library_.getProcAddress<decltype(av_get_bytes_per_sample)>("av_get_bytes_per_sample");
  av_get_sample_fmt = library_.getProcAddress<decltype(av_get_sample_fmt)>("av_get_sample_fmt");
  av_sample_fmt_is_planar = library_.getProcAddress<decltype(av_sample_fmt_is_planar)>("av_sample_fmt_is_planar");
  av_get_pix_fmt = library_.getProcAddress<decltype(av_get_pix_fmt)>("av_get_pix_fmt");
  av_get_pix_fmt_name = library_.getProcAddress<decltype(av_get_pix_fmt_name)>("av_get_pix_fmt_name");
  av_get_sample_fmt_name = library_.getProcAddress<decltype(av_get_sample_fmt_name)>("av_get_sample_fmt_name");
  av_dict_set = library_.getProcAddress<PFN<int(AVDictionary**, const char*, const char*, int)>>("av_dict_set");
  av_dict_free = library_.getProcAddress<PFN<void(AVDictionary**)>>("av_dict_free");

  if (!av_malloc || !av_free || !av_frame_alloc || !av_frame_free || !av_frame_unref) {
    return false;
  }

  loaded_ = true;
  return true;
}

auto LibAVUtil::isLoaded() const -> bool {
  return loaded_;
}

auto avPixelFormatToOmPixelFormat(AVPixelFormat av_fmt) -> OMPixelFormat {
  switch (av_fmt) {
    case AV_PIX_FMT_RGB24: return OM_FORMAT_R8G8B8A8;
    case AV_PIX_FMT_RGBA: return OM_FORMAT_R8G8B8A8;
    case AV_PIX_FMT_BGRA: return OM_FORMAT_B8G8R8A8;
    case AV_PIX_FMT_YUV420P: return OM_FORMAT_YUV420P;
    case AV_PIX_FMT_YUV422P: return OM_FORMAT_YUV422P;
    case AV_PIX_FMT_YUV444P: return OM_FORMAT_YUV444P;
    case AV_PIX_FMT_YUVJ420P: return OM_FORMAT_YUVJ420P;
    case AV_PIX_FMT_YUVJ422P: return OM_FORMAT_YUVJ422P;
    case AV_PIX_FMT_YUVJ444P: return OM_FORMAT_YUVJ444P;
    case AV_PIX_FMT_NV12: return OM_FORMAT_NV12;
    case AV_PIX_FMT_NV21: return OM_FORMAT_NV21;
    case AV_PIX_FMT_GRAY8: return OM_FORMAT_GRAY8;
    case AV_PIX_FMT_GRAY16LE:
    case AV_PIX_FMT_GRAY16BE: return OM_FORMAT_GRAY16;
    case AV_PIX_FMT_P010LE:
    case AV_PIX_FMT_P010BE: return OM_FORMAT_P010;
    case AV_PIX_FMT_YUV420P10LE:
    case AV_PIX_FMT_YUV420P10BE: return OM_FORMAT_YUV420P10;
    case AV_PIX_FMT_YUV420P12LE:
    case AV_PIX_FMT_YUV420P12BE: return OM_FORMAT_YUV420P12;
    case AV_PIX_FMT_YUV420P16LE:
    case AV_PIX_FMT_YUV420P16BE: return OM_FORMAT_YUV420P16;
    case AV_PIX_FMT_YUV422P10LE:
    case AV_PIX_FMT_YUV422P10BE: return OM_FORMAT_YUV422P10;
    case AV_PIX_FMT_YUV422P12LE:
    case AV_PIX_FMT_YUV422P12BE: return OM_FORMAT_YUV422P12;
    case AV_PIX_FMT_YUV422P16LE:
    case AV_PIX_FMT_YUV422P16BE: return OM_FORMAT_YUV422P16;
    case AV_PIX_FMT_YUV444P10LE:
    case AV_PIX_FMT_YUV444P10BE: return OM_FORMAT_YUV444P10;
    case AV_PIX_FMT_YUV444P12LE:
    case AV_PIX_FMT_YUV444P12BE: return OM_FORMAT_YUV444P12;
    case AV_PIX_FMT_YUV444P16LE:
    case AV_PIX_FMT_YUV444P16BE: return OM_FORMAT_YUV444P16;
    default: return OM_FORMAT_UNKNOWN;
  }
}

auto avColorSpaceToOmColorSpace(AVColorSpace av_cs) -> OMColorSpace {
  switch (av_cs) {
    case AVCOL_SPC_BT709: return OM_COLOR_SPACE_BT709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
      return OM_COLOR_SPACE_BT601;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return OM_COLOR_SPACE_BT2020;
    case AVCOL_SPC_SMPTE240M: return OM_COLOR_SPACE_SMPTE240M;
    default: return OM_COLOR_SPACE_UNKNOWN;
  }
}

auto avColorTransferToOmTransfer(AVColorTransferCharacteristic av_trc) -> OMTransferCharacteristic {
  switch (av_trc) {
    case AVCOL_TRC_BT709: return OM_TRANSFER_BT709;
    case AVCOL_TRC_GAMMA22: return OM_TRANSFER_GAMMA22;
    case AVCOL_TRC_GAMMA28: return OM_TRANSFER_GAMMA28;
    case AVCOL_TRC_SMPTE170M: return OM_TRANSFER_BT601;
    case AVCOL_TRC_SMPTE240M: return OM_TRANSFER_SMPTE240M;
    case AVCOL_TRC_LINEAR: return OM_TRANSFER_LINEAR;
    case AVCOL_TRC_SMPTE2084: return OM_TRANSFER_SMPTE2084;
    case AVCOL_TRC_ARIB_STD_B67: return OM_TRANSFER_HLG;
    case AVCOL_TRC_IEC61966_2_1: return OM_TRANSFER_SRGB;
    default: return OM_TRANSFER_UNKNOWN;
  }
}

/*auto avColorPrimariesToOmPrimaries(AVColorPrimaries av_pri) -> OMColorPrimaries {
  switch (av_pri) {
    case AVCOL_PRI_BT709: return OM_COLOR_PRIMARIES_BT709;
    case AVCOL_PRI_BT601: return OM_COLOR_PRIMARIES_BT601;
    case AVCOL_PRI_BT2020: return OM_COLOR_PRIMARIES_BT2020;
    case AVCOL_PRI_SMPTE240M: return OM_COLOR_PRIMARIES_SMPTE240M;
    case AVCOL_PRI_SMPTE432: return OM_COLOR_PRIMARIES_P3DCI;
    case AVCOL_PRI_SMPTE431: return OM_COLOR_PRIMARIES_P3DISPLAY;
    case AVCOL_PRI_EBU3213: return OM_COLOR_PRIMARIES_EBU3213;
    default: return OM_COLOR_PRIMARIES_UNSPECIFIED;
  }
}

auto avColorRangeToOmRange(AVColorRange av_range) -> OMColorRange {
  switch (av_range) {
    case AVCOL_RANGE_MPEG: return OM_COLOR_RANGE_MPEG;
    case AVCOL_RANGE_JPEG: return OM_COLOR_RANGE_JPEG;
    default: return OM_COLOR_RANGE_UNSPECIFIED;
  }
}

auto avChromaLocationToOmChroma(AVChromaLocation av_loc) -> OMChromaLocation {
  switch (av_loc) {
    case AVCHROMA_LOC_LEFT: return OM_CHROMA_LOC_LEFT;
    case AVCHROMA_LOC_CENTER: return OM_CHROMA_LOC_CENTER;
    case AVCHROMA_LOC_TOPLEFT: return OM_CHROMA_LOC_TOPLEFT;
    case AVCHROMA_LOC_TOP: return OM_CHROMA_LOC_TOP;
    case AVCHROMA_LOC_BOTTOMLEFT: return OM_CHROMA_LOC_BOTTOMLEFT;
    case AVCHROMA_LOC_BOTTOM: return OM_CHROMA_LOC_BOTTOM;
    default: return OM_CHROMA_LOC_UNSPECIFIED;
  }
}*/

auto avSampleFormatToOmSampleFormat(AVSampleFormat av_fmt) -> OMSampleFormat {
  switch (av_fmt) {
    case AV_SAMPLE_FMT_U8: return OM_SAMPLE_U8;
    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S16P: return OM_SAMPLE_S16;
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S32P: return OM_SAMPLE_S32;
    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_FLTP: return OM_SAMPLE_F32;
    case AV_SAMPLE_FMT_DBL:
    case AV_SAMPLE_FMT_DBLP: return OM_SAMPLE_F64;
    default: return OM_SAMPLE_UNKNOWN;
  }
}

} // namespace openmedia
