#pragma once

#include <openmedia/codec_defs.h>
#include <mfapi.h>

namespace openmedia {

inline auto codecIdToMFVideoFormat(OMCodecId codec) -> GUID {
  switch (codec) {
    case OM_CODEC_H263: return MFVideoFormat_H263;
    case OM_CODEC_H264: return MFVideoFormat_H264;
    case OM_CODEC_H265: return MFVideoFormat_HEVC;
    case OM_CODEC_AV1: return MFVideoFormat_AV1;
    case OM_CODEC_VP8: return MFVideoFormat_VP80;
    case OM_CODEC_VP9: return MFVideoFormat_VP90;
    case OM_CODEC_THEORA: return MFVideoFormat_Theora;
    default: return MFVideoFormat_Base;
  }
}

}
