#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <openmedia/log.hpp>
#include <openmedia/metadata_keys.hpp>
#include <openmedia/track.hpp>
#include <openmedia/video.hpp>
#include <openmedia/error.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Logging helpers. None of this affects playback; it only makes what the
// library does visible, so a broken stream does not look like a slow one.
namespace diag {

inline auto describe(OMError err) -> const char* {
  switch (err) {
    case OM_SUCCESS: return "success";
    case OM_COMMON_INVALID_ARGUMENT: return "invalid argument";
    case OM_COMMON_NOT_INITIALIZED: return "not initialized";
    case OM_COMMON_NOT_SUPPORTED: return "not supported";
    case OM_COMMON_OUT_OF_MEMORY: return "out of memory";
    case OM_IO_INVALID_STREAM: return "invalid stream";
    case OM_IO_SEEK_REQUIRED: return "seek required";
    case OM_IO_SEEK_FAILED: return "seek failed";
    case OM_IO_NOT_ENOUGH_DATA: return "not enough data";
    case OM_IO_END_OF_STREAM: return "end of stream";
    case OM_IO_READ_FAILED: return "read failed";
    case OM_IO_OPEN_FAILED: return "open failed";
    case OM_FORMAT_DETECTION_FAILED: return "format detection failed";
    case OM_FORMAT_NOT_SUPPORTED: return "format not supported";
    case OM_FORMAT_INVALID_HEADER: return "invalid header";
    case OM_FORMAT_CORRUPTED: return "corrupted format";
    case OM_FORMAT_NO_STREAMS: return "no streams found";
    case OM_FORMAT_PARSE_FAILED: return "format parse failed";
    case OM_CODEC_NOT_FOUND: return "codec not found";
    case OM_CODEC_NOT_SUPPORTED: return "codec not supported";
    case OM_CODEC_OPEN_FAILED: return "codec open failed";
    case OM_CODEC_INVALID_PARAMS: return "invalid codec parameters";
    case OM_CODEC_DECODE_FAILED: return "decode failed";
    case OM_CODEC_HWACCEL_FAILED: return "hardware acceleration failed";
    case OM_CODEC_HWACCEL_NOT_FOUND: return "hardware acceleration not found";
    default: return "unknown error";
  }
}

// Routes the library's own log into SDL's.
class SdlLogger final : public openmedia::Logger {
public:
  void log(OMLogCategory category, OMLogLevel level, std::string_view message) override {
    SDL_LogMessage(SDL_LOG_CATEGORY_APPLICATION, priority(level), "[%s] %.*s",
                   name(category), int(message.size()), message.data());
  }

private:
  static auto name(OMLogCategory category) -> const char* {
    switch (category) {
      case OM_CATEGORY_IO: return "io";
      case OM_CATEGORY_MUXER: return "muxer";
      case OM_CATEGORY_DEMUXER: return "demuxer";
      case OM_CATEGORY_ENCODER: return "encoder";
      case OM_CATEGORY_DECODER: return "decoder";
      case OM_CATEGORY_HARDWARE: return "hardware";
      default: return "openmedia";
    }
  }

  static auto priority(OMLogLevel level) -> SDL_LogPriority {
    switch (level) {
      case OM_LEVEL_FATAL:
      case OM_LEVEL_ERROR: return SDL_LOG_PRIORITY_ERROR;
      case OM_LEVEL_WARNING: return SDL_LOG_PRIORITY_WARN;
      case OM_LEVEL_VERBOSE:
      case OM_LEVEL_DEBUG: return SDL_LOG_PRIORITY_DEBUG;
      default: return SDL_LOG_PRIORITY_INFO;
    }
  }
};

inline auto matrixName(OMColorSpace c) -> const char* {
  switch (c) {
    case OM_COLOR_SPACE_BT601: return "BT.601";
    case OM_COLOR_SPACE_BT709: return "BT.709";
    case OM_COLOR_SPACE_BT2020: return "BT.2020-NCL";
    case OM_COLOR_SPACE_BT2020_CL: return "BT.2020-CL";
    case OM_COLOR_SPACE_SMPTE240M: return "SMPTE 240M";
    case OM_COLOR_SPACE_FCC: return "FCC";
    case OM_COLOR_SPACE_YCGCO: return "YCgCo";
    case OM_COLOR_SPACE_SMPTE428: return "SMPTE 428";
    case OM_COLOR_SPACE_CHROMA_DERIVED_NCL: return "chroma-derived NCL";
    case OM_COLOR_SPACE_CHROMA_DERIVED_CL: return "chroma-derived CL";
    case OM_COLOR_SPACE_ICTCP: return "ICtCp";
    case OM_COLOR_SPACE_RGB: return "RGB";
    default: return "unknown";
  }
}

inline auto transferName(OMTransferCharacteristic t) -> const char* {
  switch (t) {
    case OM_TRANSFER_BT709: return "BT.709";
    case OM_TRANSFER_GAMMA22: return "gamma 2.2";
    case OM_TRANSFER_GAMMA28: return "gamma 2.8";
    case OM_TRANSFER_BT601: return "BT.601";
    case OM_TRANSFER_SMPTE240M: return "SMPTE 240M";
    case OM_TRANSFER_LINEAR: return "linear";
    case OM_TRANSFER_LOG: return "log";
    case OM_TRANSFER_LOG_SQRT: return "log sqrt";
    case OM_TRANSFER_IEC61966_2_4: return "xvYCC";
    case OM_TRANSFER_BT1361_ECG: return "BT.1361";
    case OM_TRANSFER_IEC61966_2_1: return "sRGB";
    case OM_TRANSFER_BT2020_10: return "BT.2020 10-bit";
    case OM_TRANSFER_BT2020_12: return "BT.2020 12-bit";
    case OM_TRANSFER_SMPTE2084: return "PQ";
    case OM_TRANSFER_SMPTE428: return "SMPTE 428";
    case OM_TRANSFER_ARIB_STD_B67: return "HLG";
    default: return "unknown";
  }
}

inline auto primariesName(OMColorPrimaries p) -> const char* {
  switch (p) {
    case OM_PRIMARIES_BT709: return "BT.709";
    case OM_PRIMARIES_BT470M: return "BT.470M";
    case OM_PRIMARIES_BT470BG: return "BT.470BG";
    case OM_PRIMARIES_BT601: return "BT.601";
    case OM_PRIMARIES_SMPTE240M: return "SMPTE 240M";
    case OM_PRIMARIES_FILM: return "film";
    case OM_PRIMARIES_BT2020: return "BT.2020";
    case OM_PRIMARIES_SMPTE428: return "SMPTE 428";
    case OM_PRIMARIES_SMPTE431: return "DCI-P3";
    case OM_PRIMARIES_SMPTE432: return "Display P3";
    case OM_PRIMARIES_EBU3213: return "EBU 3213";
    default: return "unknown";
  }
}

inline auto rangeName(OMColorRange r) -> const char* {
  switch (r) {
    case OM_COLOR_RANGE_LIMITED: return "limited";
    case OM_COLOR_RANGE_FULL: return "full";
    default: return "unspecified";
  }
}

inline auto mediaTypeName(OMMediaType type) -> const char* {
  switch (type) {
    case OM_MEDIA_VIDEO: return "video";
    case OM_MEDIA_AUDIO: return "audio";
    case OM_MEDIA_SUBTITLE: return "subtitle";
    case OM_MEDIA_IMAGE: return "image";
    case OM_MEDIA_ATTACHMENT: return "attachment";
    default: return "unknown";
  }
}

// Colour description as the container states it and as the bitstream does;
// logging both shows whether HDR metadata survived. Logs only on change.
class ColorReporter {
public:
  template <typename Desc> // VideoFormat or Picture: same field names
  void report(const char* source, const Desc& d) {
    const uint64_t key = (uint64_t(d.color_space) << 40) | (uint64_t(d.transfer_char) << 32) |
                         (uint64_t(d.color_primaries) << 24) | (uint64_t(d.color_range) << 16) |
                         (uint64_t(d.mastering_display.has_value) << 8) |
                         uint64_t(d.content_light_level.has_value);
    if (key == last_key_ && source == last_source_) return;
    last_key_ = key;
    last_source_ = source;

    SDL_Log("[Colour] %s: primaries=%s transfer=%s matrix=%s range=%s", source,
            primariesName(d.color_primaries), transferName(d.transfer_char), matrixName(d.color_space), rangeName(d.color_range));

    // SEI fixed point: chromaticities in 1/50000, luminance in 1/10000 cd/m2.
    if (const auto& m = d.mastering_display; m.has_value) {
      SDL_Log("[Colour]   mastering G(%.4f,%.4f) B(%.4f,%.4f) R(%.4f,%.4f) WP(%.4f,%.4f) %.4f..%.1f cd/m2",
              m.display_primaries[0][0] / 5e4, m.display_primaries[0][1] / 5e4,
              m.display_primaries[1][0] / 5e4, m.display_primaries[1][1] / 5e4,
              m.display_primaries[2][0] / 5e4, m.display_primaries[2][1] / 5e4,
              m.white_point[0] / 5e4, m.white_point[1] / 5e4,
              m.min_display_mastering_luminance / 1e4, m.max_display_mastering_luminance / 1e4);
    }
    if (const auto& l = d.content_light_level; l.has_value)
      SDL_Log("[Colour]   MaxCLL %u, MaxFALL %u", l.max_content_light_level, l.max_pic_average_light_level);
    else if (!d.mastering_display.has_value && d.transfer_char == OM_TRANSFER_SMPTE2084)
      SDL_Log("[Colour]   PQ transfer but no HDR10 static metadata");
  }

private:
  uint64_t last_key_ = ~0ull;
  const char* last_source_ = nullptr;
};

// A decoder that hands pictures back in decode order makes playback jump back
// and forth, and from here that shows up as presentation timestamps running
// backwards. The first step back is named, the rest counted.
class FrameOrderReporter {
public:
  void report(int64_t pts) {
    if (last_pts_ && pts < *last_pts_ && backwards_++ % 60 == 0)
      SDL_Log("[Frames] Presentation order goes backwards: %lld after %lld (%llu so far)",
              (long long) pts, (long long) *last_pts_, (unsigned long long) backwards_);
    last_pts_ = pts;
  }

  // A seek legitimately moves the timestamps back, so it starts a new run.
  void reset() { last_pts_.reset(); }

private:
  std::optional<int64_t> last_pts_;
  uint64_t backwards_ = 0;
};

// One line per track, so a file's language and title tags can be checked
// against what a tool like mkvinfo says the container holds.
inline void reportTracks(const std::vector<openmedia::Track>& tracks) {
  using namespace openmedia;
  for (size_t i = 0; i < tracks.size(); ++i) {
    const Track& track = tracks[i];
    if (track.format.type == OM_MEDIA_ATTACHMENT) continue;
    const std::string language = track.metadata.getString(LANGUAGE, "-");
    const std::string title = track.metadata.getString(TITLE, "-");
    SDL_Log("[Track] #%zu %s codec=%d lang=%s title=%s%s", i, mediaTypeName(track.format.type),
            int(track.format.codec_id), language.c_str(), title.c_str(),
            (track.disposition & OM_DISPOSITION_DEFAULT) ? " (default)" : "");
  }
}

// Dolby Vision RPUs are not applied; say what that means for this profile.
inline void reportDolbyVision(const openmedia::Track& track) {
  using namespace openmedia;
  if (!track.metadata.getBool(DOLBY_VISION_PRESENT, false)) return;
  const int32_t profile = track.metadata.getInt32(DOLBY_VISION_PROFILE, -1);
  switch (profile) {
    case 8:
      SDL_Log("[Player] Dolby Vision 8 (compat %d): base layer only, no dynamic metadata",
              track.metadata.getInt32(DOLBY_VISION_BL_SIGNAL_COMPATIBILITY_ID, 0));
      break;
    case 5:
      SDL_Log("[Player] Dolby Vision 5: the base layer is IPT, colours will be wrong without the RPU");
      break;
    case 4:
    case 7:
      SDL_Log("[Player] Dolby Vision %d is dual layer; the enhancement layer is ignored", profile);
      break;
    default:
      SDL_Log("[Player] Dolby Vision %d: RPU metadata is not applied", profile);
      break;
  }
}

} // namespace diag
