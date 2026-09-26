#pragma once

#include <cstdint>
#include <openmedia/error.h>
#include <openmedia/media.hpp>
#include <openmedia/result.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openmedia::hls {

// HLS is two documents deep: a master playlist lists variants, and each variant's
// segments live in a media playlist of its own, fetched separately. So parsing is two
// calls with a fetch in between, and a single-variant stream is published as a media
// playlist with no master above it at all.
enum class PlaylistKind : uint8_t {
  Unknown, // not a playlist, or nothing in it says which kind
  Master,
  Media,
};

struct ByteRange {
  int64_t offset = 0;
  int64_t length = -1; // -1 reaches to the end of the resource

  auto isWhole() const -> bool { return offset == 0 && length < 0; }
};

enum class KeyMethod : uint8_t {
  None,
  Aes128,
  SampleAes,
  SampleAesCtr,
  Unsupported, // a method this parser does not know; the segments are unreadable
};

// `#EXT-X-KEY`. The key behind `uri` is the caller's to fetch, like everything else.
struct Key {
  KeyMethod method = KeyMethod::None;
  std::string uri; // resolved
  std::string iv;  // as written, `0x...`; empty means derive from the sequence number
  std::string format;
  std::string format_versions;

  auto isEncrypted() const -> bool { return method != KeyMethod::None; }
};

struct MediaSegment {
  std::string url; // resolved
  ByteRange range;
  double duration = 0;    // seconds, from `#EXTINF`
  uint64_t sequence = 0;  // its media sequence number
  std::string title;      // the text after the duration in `#EXTINF`
  uint32_t bitrate_kbps = 0;

  // A discontinuity is declared between segments, so it belongs to the one that
  // follows it: timestamps, and possibly the encoding itself, start over here.
  bool discontinuity = false;
  uint64_t discontinuity_sequence = 0;

  // `#EXT-X-GAP`: the segment is announced but not actually available.
  bool gap = false;

  // Milliseconds since the Unix epoch, from `#EXT-X-PROGRAM-DATE-TIME`, carried
  // forward across the segments that follow the tag. -1 when the playlist never
  // gives one.
  int64_t program_date_time_ms = -1;

  // The `#EXT-X-MAP` in force: an initialization section, which is what a
  // fragmented-MP4 playlist puts its `moov` in. Empty for MPEG-TS, which carries
  // its own headers in every segment.
  std::string init_url;
  ByteRange init_range;

  Key key; // the `#EXT-X-KEY` in force
};

enum class PlaylistType : uint8_t {
  Unspecified, // may be appended to and may drop segments from the front
  Event,       // only ever appended to
  Vod,         // never changes
};

struct MediaPlaylist {
  uint32_t version = 1;
  double target_duration = 0; // the longest any segment may be, in seconds
  uint64_t media_sequence = 0;
  uint64_t discontinuity_sequence = 0;
  PlaylistType type = PlaylistType::Unspecified;
  bool endlist = false;        // no more segments will ever be added
  bool iframes_only = false;   // every segment is a single I-frame
  bool independent_segments = false;
  bool has_start = false;
  double start_offset = 0; // `#EXT-X-START`, seconds; negative counts from the end

  std::vector<MediaSegment> segments;

  // `#EXT-X-ENDLIST` is the only promise that the playlist will not change.
  auto isLive() const -> bool { return !endlist; }

  auto totalDuration() const -> double; // seconds
  auto isEncrypted() const -> bool;
};

struct Resolution {
  uint32_t width = 0;
  uint32_t height = 0;
};

// `#EXT-X-STREAM-INF`, one rendition of the whole presentation.
struct Variant {
  std::string url; // resolved; the media playlist to fetch next
  uint32_t bandwidth = 0;
  uint32_t average_bandwidth = 0;
  std::string codecs;
  Resolution resolution;
  Rational framerate = {};
  std::string video_range; // SDR, HLG, PQ
  // Which `#EXT-X-MEDIA` groups this variant draws its other tracks from. An
  // empty group means that track is muxed into the variant itself.
  std::string audio_group;
  std::string video_group;
  std::string subtitle_group;
  std::string closed_captions_group;
  // From `#EXT-X-I-FRAME-STREAM-INF`: a trick-play track, not something to play
  // straight through.
  bool iframes_only = false;
};

enum class RenditionType : uint8_t { Unknown, Audio, Video, Subtitles, ClosedCaptions };

// `#EXT-X-MEDIA`, one alternative track within a group.
struct Rendition {
  RenditionType type = RenditionType::Unknown;
  std::string group_id;
  std::string name;
  std::string language;
  std::string assoc_language;
  // Resolved, and empty when there is no separate playlist: closed captions live
  // inside the video, and a rendition may describe a track muxed into the variant.
  std::string url;
  bool is_default = false;
  bool autoselect = false;
  bool forced = false;
  std::string characteristics;
  uint32_t channels = 0; // from CHANNELS, whose first field is the count
};

struct MasterPlaylist {
  uint32_t version = 1;
  bool independent_segments = false;
  std::vector<Variant> variants; // highest bandwidth first
  std::vector<Rendition> renditions;
};

// Decided by the tags present: `.m3u8` says nothing about which kind it is.
auto detectKind(std::span<const uint8_t> document) -> PlaylistKind;

auto parseMaster(std::span<const uint8_t> document, std::string_view playlist_url)
    -> Result<MasterPlaylist, OMError>;

auto parseMedia(std::span<const uint8_t> document, std::string_view playlist_url)
    -> Result<MediaPlaylist, OMError>;

// Exposed so they can be tested on their own.

// `KEY=VALUE` separated by commas, where a quoted value may contain commas of its own
// -- which `CODECS="avc1.4d401f,mp4a.40.2"` always does, and which is the one thing a
// naive split gets wrong. Quotes are removed.
auto parseAttributes(std::string_view list) -> std::vector<std::pair<std::string, std::string>>;

// `<length>[@<offset>]`. With no offset the range follows `previous_end`, the end of
// the last range read from the same resource.
auto parseByteRange(std::string_view text, int64_t previous_end) -> ByteRange;

// Milliseconds since the Unix epoch, or -1.
auto parseIso8601Ms(std::string_view text) -> int64_t;

} // namespace openmedia::hls
