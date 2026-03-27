#include <openmedia/codec_api.hpp>

namespace openmedia {

auto getCodecMeta(OMCodecId codec_id) -> CodecMeta {
  switch (codec_id) {
    case OM_CODEC_NONE:
      return {};

      // Video
    case OM_CODEC_H261: return {"H.261", "H.261", OM_MEDIA_VIDEO};
    case OM_CODEC_H262: return {"H.262", "MPEG-2 Video", OM_MEDIA_VIDEO};
    case OM_CODEC_H263: return {"H.263", "H.263", OM_MEDIA_VIDEO};
    case OM_CODEC_H264: return {"H.264 / AVC", "H.264 / AVC (Advanced Video Coding)", OM_MEDIA_VIDEO};
    case OM_CODEC_H265: return {"H.265 / HEVC", "H.265 / HEVC (High Efficiency Video Coding)", OM_MEDIA_VIDEO};
    case OM_CODEC_H266: return {"H.266 / VVC", "H.266 / VVC (Versatile Video Coding)", OM_MEDIA_VIDEO};
    case OM_CODEC_EVC: return {"EVC", "EVC (Essential Video Coding)", OM_MEDIA_VIDEO};
    case OM_CODEC_VP8: return {"VP8", "VP8", OM_MEDIA_VIDEO};
    case OM_CODEC_VP9: return {"VP9", "VP9", OM_MEDIA_VIDEO};
    case OM_CODEC_AV1: return {"AV1", "AV1 (AOMedia Video 1)", OM_MEDIA_VIDEO};
    case OM_CODEC_MPEG4: return {"MPEG-4", "MPEG-4 Video", OM_MEDIA_VIDEO};
    case OM_CODEC_THEORA:
      return {"Theora", "Theora", OM_MEDIA_VIDEO};

      // Audio
    case OM_CODEC_PCM_U8: return {"PCM U8", "PCM Unsigned 8-bit", OM_MEDIA_AUDIO};
    case OM_CODEC_PCM_S16LE: return {"PCM S16LE", "PCM Signed 16-bit Little-Endian", OM_MEDIA_AUDIO};
    case OM_CODEC_PCM_S32LE: return {"PCM S32LE", "PCM Signed 32-bit Little-Endian", OM_MEDIA_AUDIO};
    case OM_CODEC_PCM_S16BE: return {"PCM S16BE", "PCM Signed 16-bit Big-Endian", OM_MEDIA_AUDIO};
    case OM_CODEC_PCM_S32BE: return {"PCM S32BE", "PCM Signed 32-bit Big-Endian", OM_MEDIA_AUDIO};
    case OM_CODEC_PCM_F32LE: return {"PCM F32LE", "PCM Float 32-bit Little-Endian", OM_MEDIA_AUDIO};
    case OM_CODEC_PCM_F64LE: return {"PCM F64LE", "PCM Float 64-bit Little-Endian", OM_MEDIA_AUDIO};
    case OM_CODEC_FLAC: return {"FLAC", "Free Lossless Audio Codec", OM_MEDIA_AUDIO};
    case OM_CODEC_MP2: return {"MP2", "MPEG Audio Layer II", OM_MEDIA_AUDIO};
    case OM_CODEC_MP3: return {"MP3", "MPEG Audio Layer III", OM_MEDIA_AUDIO};
    case OM_CODEC_AAC: return {"AAC", "Advanced Audio Coding", OM_MEDIA_AUDIO};
    case OM_CODEC_OPUS: return {"Opus", "Opus", OM_MEDIA_AUDIO};
    case OM_CODEC_VORBIS: return {"Vorbis", "Vorbis", OM_MEDIA_AUDIO};
    case OM_CODEC_ALAC: return {"ALAC", "Apple Lossless Audio Codec", OM_MEDIA_AUDIO};
    case OM_CODEC_APE: return {"APE", "Monkey's Audio", OM_MEDIA_AUDIO};
    case OM_CODEC_WMA: return {"WMA", "Windows Media Audio", OM_MEDIA_AUDIO};
    case OM_CODEC_DTS: return {"DTS", "DTS Coherent Acoustics", OM_MEDIA_AUDIO};
    case OM_CODEC_AC3: return {"AC-3", "Dolby Digital", OM_MEDIA_AUDIO};
    case OM_CODEC_EAC3:
      return {"E-AC-3", "Dolby Digital Plus", OM_MEDIA_AUDIO};

      // Image
    case OM_CODEC_JPEG: return {"JPEG", "Joint Photographic Experts Group", OM_MEDIA_IMAGE};
    case OM_CODEC_JPEGXL: return {"JPEG XL", "JPEG XL", OM_MEDIA_IMAGE};
    case OM_CODEC_JPEGXR: return {"JPEG XR", "JPEG Extended Range", OM_MEDIA_IMAGE};
    case static_cast<uint32_t>(OM_CODEC_PNG): return {"PNG", "Portable Network Graphics", OM_MEDIA_IMAGE};
    case OM_CODEC_WEBP: return {"WebP", "WebP", OM_MEDIA_IMAGE};
    case OM_CODEC_BMP: return {"BMP", "Bitmap", OM_MEDIA_IMAGE};
    case OM_CODEC_GIF: return {"GIF", "Graphics Interchange Format", OM_MEDIA_IMAGE};
    case OM_CODEC_TIFF: return {"TIFF", "Tagged Image File Format", OM_MEDIA_IMAGE};
    case OM_CODEC_AVIF: return {"AVIF", "AV1 Image File Format", OM_MEDIA_IMAGE};
    case OM_CODEC_HEIC: return {"HEIC", "High Efficiency Image Format", OM_MEDIA_IMAGE};
    case OM_CODEC_TGA: return {"TGA", "Truevision TGA", OM_MEDIA_IMAGE};
    case OM_CODEC_EXR: return {"EXR", "OpenEXR", OM_MEDIA_IMAGE};

    default: return {};
  }
}

auto profileToString(OMCodecId codec, OMProfile profile) -> std::string_view {
  switch (codec) {
    case OM_CODEC_H264:
      switch (profile) {
        case OM_PROFILE_H264_BASELINE: return "Baseline";
        case OM_PROFILE_H264_CONSTRAINED_BASELINE: return "Constrained Baseline";
        case OM_PROFILE_H264_MAIN: return "Main";
        case OM_PROFILE_H264_EXTENDED: return "Extended";
        case OM_PROFILE_H264_HIGH: return "High";
        case OM_PROFILE_H264_HIGH_10: return "High 10";
        case OM_PROFILE_H264_HIGH_422: return "High 4:2:2";
        case OM_PROFILE_H264_HIGH_444_PREDICTIVE: return "High 4:4:4 Predictive";
        default: break;
      }
      break;

    case OM_CODEC_H265:
      switch (profile) {
        case OM_PROFILE_H265_MAIN: return "Main";
        case OM_PROFILE_H265_MAIN_10: return "Main 10";
        case OM_PROFILE_H265_MAIN_STILL_PICTURE: return "Main Still Picture";
        case OM_PROFILE_H265_REXT: return "Range Extensions";
        default: break;
      }
      break;

    case OM_CODEC_H266:
      switch (profile) {
        case OM_PROFILE_H266_MAIN_10: return "Main 10";
        case OM_PROFILE_H266_MAIN_10_444: return "Main 10 4:4:4";
        default: break;
      }
      break;

    case OM_CODEC_VP9:
      switch (profile) {
        case OM_PROFILE_VP9_0: return "Profile 0";
        case OM_PROFILE_VP9_1: return "Profile 1";
        case OM_PROFILE_VP9_2: return "Profile 2";
        case OM_PROFILE_VP9_3: return "Profile 3";
        default: break;
      }
      break;

    case OM_CODEC_AV1:
      switch (profile) {
        case OM_PROFILE_AV1_MAIN: return "Main";
        case OM_PROFILE_AV1_HIGH: return "High";
        case OM_PROFILE_AV1_PROFESSIONAL: return "Professional";
        default: break;
      }
      break;

    case OM_CODEC_AAC:
      switch (profile) {
        case OM_PROFILE_AAC_MAIN: return "Main";
        case OM_PROFILE_AAC_LC: return "LC";
        case OM_PROFILE_AAC_SSR: return "SSR";
        case OM_PROFILE_AAC_LTP: return "LTP";
        case OM_PROFILE_AAC_HE: return "HE-AAC";
        case OM_PROFILE_AAC_HE_V2: return "HE-AAC v2";
        case OM_PROFILE_AAC_LD: return "LD";
        case OM_PROFILE_AAC_ELD: return "ELD";
        case OM_PROFILE_AAC_USAC: return "xHE-AAC (USAC)";
        case OM_PROFILE_MPEG2_AAC_LOW: return "MPEG-2 LC";
        case OM_PROFILE_MPEG2_AAC_HE: return "MPEG-2 HE-AAC";
        default: break;
      }
      break;

    default: break;
  }

  return {};
}

} // namespace openmedia
