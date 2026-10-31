#pragma once

#include <stdint.h>
#include <openmedia/macro.h>

#if defined(__cplusplus)
extern "C" {
#endif

OM_ENUM(OMMediaType, uint8_t) {
  OM_MEDIA_NONE = 0,
  OM_MEDIA_AUDIO = 1,
  OM_MEDIA_VIDEO = 2,
  OM_MEDIA_IMAGE = 3,
  OM_MEDIA_SUBTITLE = 4,
  OM_MEDIA_ATTACHMENT = 5,
  OM_MEDIA_DATA = 6,
};

typedef struct OMRational {
  int32_t num = 0;
  int32_t den = 0;
} OMRational;

static inline double OMRational_toDouble(OMRational v) {
  return v.den != 0 ? (double)v.num / (double)v.den : 0.0;
}

static inline float OMRational_toFloat(OMRational v) {
  return v.den != 0 ? (float)v.num / (float)v.den : 0.0f;
}

#if defined(__cplusplus)
}
#endif
