#include <hls/m3u8.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <util/url.hpp>

namespace openmedia::hls {
namespace {

constexpr std::string_view HEADER = "#EXTM3U";

auto isSpace(char c) -> bool {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

auto trim(std::string_view text) -> std::string_view {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && isSpace(text[begin])) ++begin;
  while (end > begin && isSpace(text[end - 1])) --end;
  return text.substr(begin, end - begin);
}

auto asText(std::span<const uint8_t> document) -> std::string_view {
  std::string_view text(reinterpret_cast<const char*>(document.data()), document.size());
  // A playlist is required to be UTF-8 without a byte-order mark, but one turns
  // up often enough that refusing the playlist over it would be perverse.
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
  return text;
}

auto toUInt(std::string_view text) -> uint64_t {
  const std::string owned(trim(text));
  char* end = nullptr;
  const unsigned long long value = std::strtoull(owned.c_str(), &end, 10);
  return (end == owned.c_str()) ? 0 : static_cast<uint64_t>(value);
}

auto toDouble(std::string_view text) -> double {
  const std::string owned(trim(text));
  char* end = nullptr;
  const double value = std::strtod(owned.c_str(), &end);
  return (end == owned.c_str()) ? 0.0 : value;
}

// Walks the lines of a playlist, dropping blank ones and comments. A line
// beginning with `#` is a tag only when it begins with `#EXT`; anything else is a
// comment and carries no meaning.
class LineReader {
  std::string_view text_;
  size_t pos_ = 0;

public:
  explicit LineReader(std::string_view text) : text_(text) {}

  auto next(std::string_view& line) -> bool {
    while (pos_ < text_.size()) {
      const size_t end = text_.find('\n', pos_);
      const std::string_view raw =
          text_.substr(pos_, end == std::string_view::npos ? std::string_view::npos : end - pos_);
      pos_ = (end == std::string_view::npos) ? text_.size() : end + 1;

      const std::string_view trimmed = trim(raw);
      if (trimmed.empty()) continue;
      if (trimmed.starts_with('#') && !trimmed.starts_with("#EXT")) continue;
      line = trimmed;
      return true;
    }
    return false;
  }
};

// Splits `#EXT-X-NAME:value` into its name and its value. A tag with no colon
// has no value, which for the boolean tags is the whole of their meaning.
void splitTag(std::string_view line, std::string_view& name, std::string_view& value) {
  const size_t colon = line.find(':');
  if (colon == std::string_view::npos) {
    name = line;
    value = {};
    return;
  }
  name = line.substr(0, colon);
  value = line.substr(colon + 1);
}

class Attributes {
  std::vector<std::pair<std::string, std::string>> entries_;
public:
  explicit Attributes(std::string_view list) : entries_(parseAttributes(list)) {}

  auto find(std::string_view key) const -> std::string_view {
    for (const auto& [name, value] : entries_) {
      if (name == key) return value;
    }
    return {};
  }

  auto has(std::string_view key) const -> bool {
    return std::any_of(entries_.begin(), entries_.end(),
                       [&](const auto& entry) { return entry.first == key; });
  }

  auto text(std::string_view key) const -> std::string_view { return find(key); }
  auto number(std::string_view key) const -> uint64_t { return toUInt(find(key)); }
  auto real(std::string_view key) const -> double { return toDouble(find(key)); }
  auto isYes(std::string_view key) const -> bool { return find(key) == "YES"; }

  auto resolution(std::string_view key) const -> Resolution {
    const std::string_view value = find(key);
    const size_t x = value.find_first_of("xX");
    if (x == std::string_view::npos) return {};
    return {static_cast<uint32_t>(toUInt(value.substr(0, x))),
            static_cast<uint32_t>(toUInt(value.substr(x + 1)))};
  }

  // A frame rate is written as a decimal, which loses the ratio it came from, so
  // the two families that actually occur are reconstructed: whole rates, and the
  // `n * 1000/1001` ones. The tolerance is set by how a playlist writes them --
  // `29.970`, not `29.97002997` -- so it has to allow for that rounding rather
  // than for floating-point error.
  auto framerate(std::string_view key) const -> Rational {
    const double value = real(key);
    if (value <= 0) return {};

    const double integral = std::round(value);
    if (std::abs(value - integral) < 1e-6) {
      return {static_cast<int32_t>(integral), 1};
    }

    const double base = std::round(value * 1001.0 / 1000.0);
    if (base > 0 && std::abs(value - base * 1000.0 / 1001.0) < 5e-4) {
      return {static_cast<int32_t>(base) * 1000, 1001};
    }
    return {static_cast<int32_t>(std::round(value * 1000.0)), 1000};
  }

  // `CHANNELS` is a slash-separated list whose first field is the count.
  auto channels(std::string_view key) const -> uint32_t {
    const std::string_view value = find(key);
    const size_t slash = value.find('/');
    return static_cast<uint32_t>(
        toUInt(slash == std::string_view::npos ? value : value.substr(0, slash)));
  }
};

auto keyMethodOf(std::string_view text) -> KeyMethod {
  if (text == "NONE") return KeyMethod::None;
  if (text == "AES-128") return KeyMethod::Aes128;
  if (text == "SAMPLE-AES") return KeyMethod::SampleAes;
  if (text == "SAMPLE-AES-CTR") return KeyMethod::SampleAesCtr;
  return KeyMethod::Unsupported;
}

auto renditionTypeOf(std::string_view text) -> RenditionType {
  if (text == "AUDIO") return RenditionType::Audio;
  if (text == "VIDEO") return RenditionType::Video;
  if (text == "SUBTITLES") return RenditionType::Subtitles;
  if (text == "CLOSED-CAPTIONS") return RenditionType::ClosedCaptions;
  return RenditionType::Unknown;
}

} // namespace

auto parseAttributes(std::string_view list) -> std::vector<std::pair<std::string, std::string>> {
  std::vector<std::pair<std::string, std::string>> entries;

  size_t i = 0;
  while (i < list.size()) {
    while (i < list.size() && (isSpace(list[i]) || list[i] == ',')) ++i;
    if (i >= list.size()) break;

    const size_t equals = list.find('=', i);
    if (equals == std::string_view::npos) break;
    const std::string_view name = trim(list.substr(i, equals - i));

    size_t value_begin = equals + 1;
    size_t value_end = value_begin;
    std::string_view value;

    if (value_begin < list.size() && list[value_begin] == '"') {
      // A quoted value runs to the closing quote, and the commas inside it are
      // part of the value -- which is what makes CODECS="a,b" a list of one
      // attribute rather than two.
      const size_t close = list.find('"', value_begin + 1);
      if (close == std::string_view::npos) {
        value = list.substr(value_begin + 1);
        value_end = list.size();
      } else {
        value = list.substr(value_begin + 1, close - value_begin - 1);
        value_end = close + 1;
      }
    } else {
      value_end = list.find(',', value_begin);
      if (value_end == std::string_view::npos) value_end = list.size();
      value = trim(list.substr(value_begin, value_end - value_begin));
    }

    if (!name.empty()) entries.emplace_back(std::string(name), std::string(value));
    i = value_end;
    // Step over the separator, if the value did not already consume it.
    while (i < list.size() && list[i] != ',') ++i;
  }
  return entries;
}

auto parseByteRange(std::string_view text, int64_t previous_end) -> ByteRange {
  text = trim(text);
  const size_t at = text.find('@');
  if (at == std::string_view::npos) {
    // No offset given: the range picks up where the last one from the same
    // resource left off. That is what lets a playlist address a single file as a
    // run of adjacent segments.
    return {previous_end < 0 ? 0 : previous_end, static_cast<int64_t>(toUInt(text))};
  }
  return {static_cast<int64_t>(toUInt(text.substr(at + 1))),
          static_cast<int64_t>(toUInt(text.substr(0, at)))};
}

auto parseIso8601Ms(std::string_view text) -> int64_t {
  // YYYY-MM-DDThh:mm:ss[.sss][Z|±hh:mm]
  text = trim(text);
  if (text.size() < 19 || text[4] != '-' || text[7] != '-') return -1;
  if (text[10] != 'T' && text[10] != 't' && text[10] != ' ') return -1;
  if (text[13] != ':' || text[16] != ':') return -1;

  const int64_t year = static_cast<int64_t>(toUInt(text.substr(0, 4)));
  const int64_t month = static_cast<int64_t>(toUInt(text.substr(5, 2)));
  const int64_t day = static_cast<int64_t>(toUInt(text.substr(8, 2)));
  const int64_t hour = static_cast<int64_t>(toUInt(text.substr(11, 2)));
  const int64_t minute = static_cast<int64_t>(toUInt(text.substr(14, 2)));
  const int64_t second = static_cast<int64_t>(toUInt(text.substr(17, 2)));
  if (month < 1 || month > 12 || day < 1 || day > 31) return -1;

  // Days from the civil date, by Howard Hinnant's algorithm: the era arithmetic
  // avoids a leap-year table and is exact for every date a playlist can name.
  const int64_t y = year - (month <= 2 ? 1 : 0);
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t year_of_era = y - era * 400;
  const int64_t day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  const int64_t days = era * 146097 + day_of_era - 719468;

  int64_t ms = ((days * 24 + hour) * 60 + minute) * 60 + second;
  ms *= 1000;

  size_t i = 19;
  if (i < text.size() && (text[i] == '.' || text[i] == ',')) {
    ++i;
    int64_t fraction = 0;
    int64_t scale = 100;
    while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) != 0)) {
      if (scale > 0) {
        fraction += (text[i] - '0') * scale;
        scale /= 10;
      }
      ++i;
    }
    ms += fraction;
  }

  if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
    const int64_t sign = text[i] == '-' ? -1 : 1;
    ++i;
    if (i + 2 <= text.size()) {
      const int64_t offset_hours = static_cast<int64_t>(toUInt(text.substr(i, 2)));
      i += 2;
      if (i < text.size() && text[i] == ':') ++i;
      const int64_t offset_minutes =
          (i + 2 <= text.size()) ? static_cast<int64_t>(toUInt(text.substr(i, 2))) : 0;
      // The timestamp is local to that offset, so the offset comes back off to
      // reach UTC.
      ms -= sign * (offset_hours * 60 + offset_minutes) * 60 * 1000;
    }
  }
  return ms;
}

auto detectKind(std::span<const uint8_t> document) -> PlaylistKind {
  const std::string_view text = asText(document);
  LineReader reader(text);

  std::string_view line;
  if (!reader.next(line) || line != HEADER) return PlaylistKind::Unknown;

  // Whichever kind of tag appears first settles it. A playlist carrying both is
  // invalid; taking the first match keeps the answer stable rather than depending
  // on which tag happens to appear last.
  while (reader.next(line)) {
    if (line.starts_with("#EXT-X-STREAM-INF") || line.starts_with("#EXT-X-I-FRAME-STREAM-INF") ||
        line.starts_with("#EXT-X-MEDIA:") || line.starts_with("#EXT-X-SESSION-DATA") ||
        line.starts_with("#EXT-X-SESSION-KEY")) {
      return PlaylistKind::Master;
    }
    if (line.starts_with("#EXTINF") || line.starts_with("#EXT-X-TARGETDURATION") ||
        line.starts_with("#EXT-X-MEDIA-SEQUENCE") || line.starts_with("#EXT-X-ENDLIST") ||
        line.starts_with("#EXT-X-PLAYLIST-TYPE") || line.starts_with("#EXT-X-MAP")) {
      return PlaylistKind::Media;
    }
  }
  return PlaylistKind::Unknown;
}

auto parseMaster(std::span<const uint8_t> document, std::string_view playlist_url)
    -> Result<MasterPlaylist, OMError> {
  const std::string_view text = asText(document);
  LineReader reader(text);

  std::string_view line;
  if (!reader.next(line) || line != HEADER) return Err(OM_FORMAT_INVALID_HEADER);

  MasterPlaylist playlist;
  // A `#EXT-X-STREAM-INF` describes the variant named on the line after it, so the
  // attributes are held until that URI turns up.
  bool expecting_uri = false;
  Variant pending;

  while (reader.next(line)) {
    if (!line.starts_with('#')) {
      if (expecting_uri) {
        pending.url = resolveUrl(playlist_url, line);
        playlist.variants.push_back(std::move(pending));
        pending = Variant {};
        expecting_uri = false;
      }
      // A URI with no `#EXT-X-STREAM-INF` in front of it belongs to nothing.
      continue;
    }

    std::string_view name;
    std::string_view value;
    splitTag(line, name, value);

    if (name == "#EXT-X-VERSION") {
      playlist.version = static_cast<uint32_t>(toUInt(value));
    } else if (name == "#EXT-X-INDEPENDENT-SEGMENTS") {
      playlist.independent_segments = true;
    } else if (name == "#EXT-X-STREAM-INF") {
      const Attributes attributes(value);
      pending = Variant {};
      pending.bandwidth = static_cast<uint32_t>(attributes.number("BANDWIDTH"));
      pending.average_bandwidth = static_cast<uint32_t>(attributes.number("AVERAGE-BANDWIDTH"));
      pending.codecs = attributes.text("CODECS");
      pending.resolution = attributes.resolution("RESOLUTION");
      pending.framerate = attributes.framerate("FRAME-RATE");
      pending.video_range = attributes.text("VIDEO-RANGE");
      pending.audio_group = attributes.text("AUDIO");
      pending.video_group = attributes.text("VIDEO");
      pending.subtitle_group = attributes.text("SUBTITLES");
      pending.closed_captions_group = attributes.text("CLOSED-CAPTIONS");
      expecting_uri = true;
    } else if (name == "#EXT-X-I-FRAME-STREAM-INF") {
      // This one carries its URI as an attribute rather than on the next line.
      const Attributes attributes(value);
      Variant variant;
      variant.iframes_only = true;
      variant.bandwidth = static_cast<uint32_t>(attributes.number("BANDWIDTH"));
      variant.average_bandwidth = static_cast<uint32_t>(attributes.number("AVERAGE-BANDWIDTH"));
      variant.codecs = attributes.text("CODECS");
      variant.resolution = attributes.resolution("RESOLUTION");
      variant.video_range = attributes.text("VIDEO-RANGE");
      const std::string_view uri = attributes.find("URI");
      if (uri.empty()) continue;
      variant.url = resolveUrl(playlist_url, uri);
      playlist.variants.push_back(std::move(variant));
    } else if (name == "#EXT-X-MEDIA") {
      const Attributes attributes(value);
      Rendition rendition;
      rendition.type = renditionTypeOf(attributes.find("TYPE"));
      rendition.group_id = attributes.text("GROUP-ID");
      rendition.name = attributes.text("NAME");
      rendition.language = attributes.text("LANGUAGE");
      rendition.assoc_language = attributes.text("ASSOC-LANGUAGE");
      rendition.is_default = attributes.isYes("DEFAULT");
      rendition.autoselect = attributes.isYes("AUTOSELECT");
      rendition.forced = attributes.isYes("FORCED");
      rendition.characteristics = attributes.text("CHARACTERISTICS");
      rendition.channels = attributes.channels("CHANNELS");
      if (const std::string_view uri = attributes.find("URI"); !uri.empty()) {
        rendition.url = resolveUrl(playlist_url, uri);
      }
      playlist.renditions.push_back(std::move(rendition));
    }
    // Everything else -- session data, session keys, tags from a later revision --
    // describes the presentation to something other than a reader of it.
  }

  if (playlist.variants.empty()) return Err(OM_FORMAT_NO_STREAMS);

  // Highest bandwidth first, so picking under a cap is a scan from the front and
  // picking the best of all is the first entry -- the same order parseManifest()
  // puts DASH representations in.
  std::stable_sort(playlist.variants.begin(), playlist.variants.end(),
                   [](const Variant& a, const Variant& b) { return a.bandwidth > b.bandwidth; });
  return Ok(std::move(playlist));
}

auto parseMedia(std::span<const uint8_t> document, std::string_view playlist_url)
    -> Result<MediaPlaylist, OMError> {
  const std::string_view text = asText(document);
  LineReader reader(text);

  std::string_view line;
  if (!reader.next(line) || line != HEADER) return Err(OM_FORMAT_INVALID_HEADER);

  MediaPlaylist playlist;

  // State that a tag sets and the segments after it inherit, until it is set again.
  Key key;
  std::string init_url;
  ByteRange init_range;
  double duration = 0;
  std::string title;
  uint32_t bitrate_kbps = 0;
  bool have_duration = false;
  bool discontinuity = false;
  bool gap = false;
  int64_t program_date_time_ms = -1;
  uint64_t discontinuity_sequence = 0;
  bool have_sequence_start = false;

  // `#EXT-X-BYTERANGE` without an offset continues from the previous range of the
  // same resource. Which resource that is only becomes known on the URI line
  // below, so the tag's text is held until then rather than resolved on sight.
  std::string pending_range;
  std::string range_url;
  int64_t range_end = 0;
  bool have_range = false;

  while (reader.next(line)) {
    if (!line.starts_with('#')) {
      if (!have_duration) continue; // a URI with no `#EXTINF` is not a segment

      MediaSegment segment;
      segment.url = resolveUrl(playlist_url, line);
      segment.duration = duration;
      segment.title = title;
      segment.bitrate_kbps = bitrate_kbps;
      segment.discontinuity = discontinuity;
      segment.discontinuity_sequence = discontinuity_sequence;
      segment.gap = gap;
      segment.program_date_time_ms = program_date_time_ms;
      segment.init_url = init_url;
      segment.init_range = init_range;
      segment.key = key;
      segment.sequence = playlist.media_sequence + playlist.segments.size();

      if (have_range) {
        if (segment.url != range_url) {
          range_url = segment.url;
          range_end = 0;
        }
        segment.range = parseByteRange(pending_range, range_end);
        range_end = segment.range.offset + std::max<int64_t>(segment.range.length, 0);
      }

      // Program dates apply from the tag onwards, each segment starting where the
      // one before it ended.
      if (program_date_time_ms >= 0) {
        program_date_time_ms += static_cast<int64_t>(duration * 1000.0);
      }

      playlist.segments.push_back(std::move(segment));

      have_duration = false;
      have_range = false;
      duration = 0;
      title.clear();
      discontinuity = false;
      gap = false;
      continue;
    }

    std::string_view name;
    std::string_view value;
    splitTag(line, name, value);

    if (name == "#EXTINF") {
      const size_t comma = value.find(',');
      duration = toDouble(comma == std::string_view::npos ? value : value.substr(0, comma));
      title = (comma == std::string_view::npos) ? std::string()
                                                : std::string(trim(value.substr(comma + 1)));
      have_duration = true;
    } else if (name == "#EXT-X-BYTERANGE") {
      pending_range.assign(value);
      have_range = true;
    } else if (name == "#EXT-X-DISCONTINUITY") {
      discontinuity = true;
      ++discontinuity_sequence;
    } else if (name == "#EXT-X-GAP") {
      gap = true;
    } else if (name == "#EXT-X-VERSION") {
      playlist.version = static_cast<uint32_t>(toUInt(value));
    } else if (name == "#EXT-X-TARGETDURATION") {
      playlist.target_duration = toDouble(value);
    } else if (name == "#EXT-X-MEDIA-SEQUENCE") {
      // Only meaningful before the first segment, and the sequence numbers
      // already handed out would be wrong if it moved afterwards.
      if (!have_sequence_start && playlist.segments.empty()) {
        playlist.media_sequence = toUInt(value);
        have_sequence_start = true;
      }
    } else if (name == "#EXT-X-DISCONTINUITY-SEQUENCE") {
      if (playlist.segments.empty()) {
        playlist.discontinuity_sequence = toUInt(value);
        discontinuity_sequence = playlist.discontinuity_sequence;
      }
    } else if (name == "#EXT-X-PLAYLIST-TYPE") {
      const std::string_view type = trim(value);
      if (type == "VOD") {
        playlist.type = PlaylistType::Vod;
      } else if (type == "EVENT") {
        playlist.type = PlaylistType::Event;
      }
    } else if (name == "#EXT-X-ENDLIST") {
      playlist.endlist = true;
    } else if (name == "#EXT-X-I-FRAMES-ONLY") {
      playlist.iframes_only = true;
    } else if (name == "#EXT-X-INDEPENDENT-SEGMENTS") {
      playlist.independent_segments = true;
    } else if (name == "#EXT-X-START") {
      const Attributes attributes(value);
      playlist.start_offset = attributes.real("TIME-OFFSET");
      playlist.has_start = true;
    } else if (name == "#EXT-X-BITRATE") {
      bitrate_kbps = static_cast<uint32_t>(toUInt(value));
    } else if (name == "#EXT-X-PROGRAM-DATE-TIME") {
      program_date_time_ms = parseIso8601Ms(value);
    } else if (name == "#EXT-X-KEY") {
      const Attributes attributes(value);
      key = Key {};
      key.method = keyMethodOf(attributes.find("METHOD"));
      if (key.method != KeyMethod::None) {
        if (const std::string_view uri = attributes.find("URI"); !uri.empty()) {
          key.uri = resolveUrl(playlist_url, uri);
        }
        key.iv = attributes.text("IV");
        key.format = attributes.text("KEYFORMAT");
        key.format_versions = attributes.text("KEYFORMATVERSIONS");
      }
    } else if (name == "#EXT-X-MAP") {
      const Attributes attributes(value);
      const std::string_view uri = attributes.find("URI");
      init_url = uri.empty() ? std::string() : resolveUrl(playlist_url, uri);
      init_range = ByteRange {};
      if (attributes.has("BYTERANGE")) {
        init_range = parseByteRange(attributes.find("BYTERANGE"), 0);
      }
    }
  }

  if (playlist.segments.empty()) return Err(OM_FORMAT_NO_STREAMS);
  return Ok(std::move(playlist));
}

auto MediaPlaylist::totalDuration() const -> double {
  double total = 0;
  for (const MediaSegment& segment : segments) total += segment.duration;
  return total;
}

auto MediaPlaylist::isEncrypted() const -> bool {
  return std::any_of(segments.begin(), segments.end(),
                     [](const MediaSegment& segment) { return segment.key.isEncrypted(); });
}

} // namespace openmedia::hls
