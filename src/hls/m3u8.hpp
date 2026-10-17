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
// segments live in a media playlist fetched separately -- hence two parse calls with a
// fetch in between. A single-variant stream is published as a media playlist alone.
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
  Unsupported, // unknown method, so the segments are unreadable
};

// `#EXT-X-KEY`. The key behind `uri` is the caller's to fetch, like everything else.
struct Key {
  KeyMethod method = KeyMethod::None;
  // METHOD= as written. Kept beside the enum so a method this parser does not know
  // still reaches a caller that does, under the name the playlist gave it.
  std::string method_name;
  std::string uri; // resolved
  std::string iv;  // as written, `0x...`; empty means derive from the sequence number
  std::string format;
  std::string format_versions;

  auto isEncrypted() const -> bool { return method != KeyMethod::None; }
  auto operator==(const Key&) const -> bool = default;
};

struct MediaSegment {
  std::string url; // resolved
  ByteRange range;
  double duration = 0;   // seconds, from `#EXTINF`
  uint64_t sequence = 0; // its media sequence number, which is also its derived IV

  // Declared between segments, so it belongs to the one that follows: timestamps,
  // and possibly the encoding, start over here.
  bool discontinuity = false;

  // `#EXT-X-GAP`: the segment is announced but not actually available.
  bool gap = false;

  // From `#EXT-X-PROGRAM-DATE-TIME`, carried forward across the segments after the
  // tag. -1 when the playlist never gives one.
  int64_t program_date_time_ms = -1;

  // The `#EXT-X-MAP` in force, where a fragmented-MP4 playlist puts its `moov`.
  // Empty for MPEG-TS, which carries its own headers in every segment.
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
  double target_duration = 0; // the longest any segment may be, in seconds
  uint64_t media_sequence = 0;
  PlaylistType type = PlaylistType::Unspecified;
  bool endlist = false; // no more segments will ever be added
  bool has_start = false;
  double start_offset = 0; // `#EXT-X-START`, seconds; negative counts from the end

  std::vector<MediaSegment> segments;

  // Either promise that the playlist will not change: `#EXT-X-ENDLIST`, or a type
  // of VOD, which says outright that the playlist is finished whether or not the
  // tag announcing it is there.
  auto isLive() const -> bool { return !endlist && type != PlaylistType::Vod; }

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
  // Where the same rendition is published besides `url`, in playlist order. A
  // variant repeated with its attributes unchanged is another address for that one
  // quality, not another quality (RFC 8216 6.3.1), so such entries collapse here.
  std::vector<std::string> backup_urls;
  uint32_t bandwidth = 0;
  uint32_t average_bandwidth = 0;
  std::string codecs;
  Resolution resolution;
  Rational framerate = {};
  std::string video_range; // SDR, HLG, PQ
  // Which `#EXT-X-MEDIA` groups this variant draws its other tracks from. Empty
  // means that track is muxed into the variant itself.
  std::string audio_group;
  std::string video_group;
  std::string subtitle_group;
  std::string closed_captions_group;
  // `#EXT-X-I-FRAME-STREAM-INF`: trick play, not something to play straight through.
  bool iframes_only = false;
};

enum class RenditionType : uint8_t { Unknown, Audio, Video, Subtitles, ClosedCaptions };

// `#EXT-X-MEDIA`, one alternative track within a group.
struct Rendition {
  RenditionType type = RenditionType::Unknown;
  std::string group_id;
  std::string name;
  std::string language;
  // Resolved, and empty where there is no separate playlist -- closed captions live
  // inside the video, and a rendition may describe a track muxed into the variant.
  std::string url;
  bool is_default = false;
  bool autoselect = false;
  bool forced = false;
  std::string characteristics;
  uint32_t channels = 0; // from CHANNELS, whose first field is the count
};

struct MasterPlaylist {
  std::vector<Variant> variants; // highest bandwidth first
  std::vector<Rendition> renditions;
  // `#EXT-X-SESSION-KEY`: the keys the presentation uses, named without naming a
  // segment, so a player can ask for its licence before fetching a media playlist.
  std::vector<Key> session_keys;
};

// Decided by the tags present: `.m3u8` says nothing about which kind it is.
auto detectKind(std::span<const uint8_t> document) -> PlaylistKind;

auto parseMaster(std::span<const uint8_t> document, std::string_view playlist_url)
    -> Result<MasterPlaylist, OMError>;

auto parseMedia(std::span<const uint8_t> document, std::string_view playlist_url)
    -> Result<MediaPlaylist, OMError>;

// Exposed so they can be tested on their own.

// `KEY=VALUE` separated by commas. A quoted value may hold commas of its own, which
// `CODECS="avc1.4d401f,mp4a.40.2"` always does and a naive split always gets wrong.
// Quotes are removed.
auto parseAttributes(std::string_view list) -> std::vector<std::pair<std::string, std::string>>;

// `<length>[@<offset>]`. With no offset the range follows `previous_end`, the end of
// the previous range over the same resource.
auto parseByteRange(std::string_view text, int64_t previous_end) -> ByteRange;

// Milliseconds since the Unix epoch, or -1.
auto parseIso8601Ms(std::string_view text) -> int64_t;

// `IV=0x...` into the 16 bytes of a cipher block. False when the attribute is
// absent or not 16 bytes of hex, which is what says the vector has to be derived
// from the media sequence number instead.
auto parseHexIv(std::string_view text, uint8_t out[16]) -> bool;

} // namespace openmedia::hls
