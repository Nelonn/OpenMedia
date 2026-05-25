#pragma once

#include <openmedia/codec_defs.h>
#include <openmedia/media.hpp>
#include <cstdint>
#include <openmedia/audio.hpp>
#include <openmedia/dictionary.hpp>
#include <openmedia/video.hpp>
#include <vector>

OM_ENUM(OMDisposition, uint16_t) {
    OM_DISPOSITION_NONE = 0,

    OM_DISPOSITION_DEFAULT = 1 << 0,
    OM_DISPOSITION_FORCED = 1 << 1,
    OM_DISPOSITION_DEPENDENT = 1 << 2,

    OM_DISPOSITION_ORIGINAL = 1 << 3,
    OM_DISPOSITION_DUB = 1 << 4,
    OM_DISPOSITION_COMMENT = 1 << 5,
    OM_DISPOSITION_LYRICS = 1 << 6,
    OM_DISPOSITION_KARAOKE = 1 << 7,
    OM_DISPOSITION_CLEAN_EFFECTS = 1 << 8,

    OM_DISPOSITION_HEARING_IMPAIRED = 1 << 9,
    OM_DISPOSITION_VISUAL_IMPAIRED = 1 << 10,

    OM_DISPOSITION_CAPTIONS = 1 << 11,
    OM_DISPOSITION_DESCRIPTIONS = 1 << 12,

    OM_DISPOSITION_COVER = 1 << 13,
    OM_DISPOSITION_TIMED_THUMBNAILS = 1 << 14,

    OM_DISPOSITION_METADATA = 1 << 15,
};

namespace openmedia {

/**
 * MediaFormat represents the properties of the ENCODED content.
 * For decoded data properties, see VideoFormat or AudioFormat.
 */
struct OPENMEDIA_ABI MediaFormat {
  OMMediaType type = OM_MEDIA_NONE;
  OMCodecId codec_id = OM_CODEC_NONE;
  OMProfile profile = OM_PROFILE_NONE;
  int32_t level = 0;

  union {
    char dummy = 0;
    struct {
      uint32_t sample_rate;
      uint32_t channels;
      uint32_t bit_depth;
    } audio;
    struct {
      uint32_t width;
      uint32_t height;
      Rational framerate;
      OMColorSpace color_space;
      OMTransferCharacteristic transfer_char;
      OMColorPrimaries color_primaries;
      OMColorRange color_range;
      OMMasteringDisplayMetadata mastering_display;
      OMContentLightLevel content_light_level;
    } video;
    struct {
      uint32_t width;
      uint32_t height;
    } image;
  };
};

struct OPENMEDIA_ABI Track {
  int32_t index = -1;
  int32_t id = -1;

  MediaFormat format = {};
  std::vector<uint8_t> extradata;
  uint32_t bitrate = 0; // bps
  Rational time_base = {};
  int64_t start_time = -1;
  int64_t duration = -1;
  int64_t nb_frames = 0;

  Dictionary metadata;
  OMDisposition disposition = OM_DISPOSITION_NONE;

  Track() {}

  auto isImage() const -> bool {
    return format.type == OM_MEDIA_IMAGE || (disposition & OM_DISPOSITION_COVER);
  }

  auto isCover() const -> bool {
    return (disposition & OM_DISPOSITION_COVER) != 0;
  }
};

} // namespace openmedia
