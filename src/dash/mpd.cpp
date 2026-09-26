#include <dash/mpd.hpp>
#include <dash/xml.hpp>
#include <util/url.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>

namespace openmedia::dash {
namespace {

constexpr int64_t NS_PER_SEC = 1'000'000'000;

auto toUInt(std::string_view text, uint64_t fallback = 0) -> uint64_t {
  const std::string owned(text);
  char* end = nullptr;
  const unsigned long long value = std::strtoull(owned.c_str(), &end, 10);
  return (end == owned.c_str()) ? fallback : static_cast<uint64_t>(value);
}

auto toInt(std::string_view text, int64_t fallback = 0) -> int64_t {
  const std::string owned(text);
  char* end = nullptr;
  const long long value = std::strtoll(owned.c_str(), &end, 10);
  return (end == owned.c_str()) ? fallback : static_cast<int64_t>(value);
}

auto attrUInt(const xml::Node& node, std::string_view key, uint64_t fallback = 0) -> uint64_t {
  const auto value = node.attribute(key);
  return value ? toUInt(*value, fallback) : fallback;
}

auto attrInt(const xml::Node& node, std::string_view key, int64_t fallback = 0) -> int64_t {
  const auto value = node.attribute(key);
  return value ? toInt(*value, fallback) : fallback;
}

auto attrText(const xml::Node& node, std::string_view key) -> std::string {
  const auto value = node.attribute(key);
  return value ? std::string(*value) : std::string();
}

auto attrRational(const xml::Node& node, std::string_view key) -> Rational {
  const auto value = node.attribute(key);
  if (!value || value->empty()) return {};
  const size_t slash = value->find('/');
  if (slash == std::string_view::npos) {
    return {static_cast<int32_t>(toInt(*value)), 1};
  }
  const int64_t num = toInt(value->substr(0, slash));
  const int64_t den = toInt(value->substr(slash + 1), 1);
  return {static_cast<int32_t>(num), static_cast<int32_t>(den == 0 ? 1 : den)};
}

// `@audioSamplingRate` may list several rates; the first is the one that
// matters, since a representation carries one.
auto attrFirstUInt(const xml::Node& node, std::string_view key) -> uint32_t {
  const auto value = node.attribute(key);
  if (!value) return 0;
  const size_t space = value->find(' ');
  return static_cast<uint32_t>(
      toUInt(space == std::string_view::npos ? *value : value->substr(0, space)));
}

// `first-last`, inclusive at both ends, as an offset and a length.
auto parseRange(std::string_view text) -> ByteRange {
  const size_t dash = text.find('-');
  if (dash == std::string_view::npos) return {static_cast<int64_t>(toUInt(text)), -1};
  const int64_t first = static_cast<int64_t>(toUInt(text.substr(0, dash)));
  const std::string_view last_text = text.substr(dash + 1);
  if (last_text.empty()) return {first, -1};
  const int64_t last = static_cast<int64_t>(toUInt(last_text));
  return {first, (last >= first) ? (last - first + 1) : -1};
}

auto attrRange(const xml::Node& node, std::string_view key, bool& present) -> ByteRange {
  const auto value = node.attribute(key);
  present = value.has_value() && !value->empty();
  return present ? parseRange(*value) : ByteRange {};
}

} // namespace

auto parseIsoDuration(std::string_view text) -> int64_t {
  if (text.empty()) return -1;
  bool negative = false;
  if (text.front() == '-') {
    negative = true;
    text.remove_prefix(1);
  }
  if (text.empty() || text.front() != 'P') return -1;
  text.remove_prefix(1);

  int64_t ns = 0;
  bool in_time = false;
  bool any = false;

  while (!text.empty()) {
    if (text.front() == 'T') {
      in_time = true;
      text.remove_prefix(1);
      continue;
    }
    size_t i = 0;
    while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) != 0)) ++i;
    if (i < text.size() && (text[i] == '.' || text[i] == ',')) {
      ++i;
      while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) != 0)) ++i;
    }
    if (i == 0 || i >= text.size()) return any ? (negative ? -ns : ns) : -1;

    const std::string number(text.substr(0, i));
    const char unit = text[i];
    text.remove_prefix(i + 1);

    const double value = std::strtod(number.c_str(), nullptr);
    double seconds = 0;
    switch (unit) {
      // Years and months have no fixed length. A manifest that dates a
      // presentation in them is describing something no player will buffer, and
      // the nominal lengths are as close as an answer can get.
      case 'Y': seconds = value * 365.0 * 86400.0; break;
      case 'D': seconds = value * 86400.0; break;
      case 'H': seconds = value * 3600.0; break;
      case 'S': seconds = value; break;
      case 'M': seconds = in_time ? value * 60.0 : value * 30.0 * 86400.0; break;
      case 'W': seconds = value * 7.0 * 86400.0; break;
      default: return any ? (negative ? -ns : ns) : -1;
    }
    ns += static_cast<int64_t>(seconds * static_cast<double>(NS_PER_SEC));
    any = true;
  }
  if (!any) return -1;
  return negative ? -ns : ns;
}

namespace {

// Which identifiers this pass is able to fill in. One that is not available is
// written back out exactly as it was found, format specifier and all, so a later
// pass can still substitute it.
struct TemplateValues {
  std::string_view representation_id;
  uint32_t bandwidth = 0;
  uint64_t number = 0;
  int64_t time = 0;
  bool has_representation = false;
  bool has_number = false;
  bool has_time = false;
};

auto expandWith(std::string_view tmpl, const TemplateValues& values) -> std::string {
  std::string out;
  out.reserve(tmpl.size() + 16);

  for (size_t i = 0; i < tmpl.size();) {
    if (tmpl[i] != '$') {
      out.push_back(tmpl[i++]);
      continue;
    }
    const size_t close = tmpl.find('$', i + 1);
    if (close == std::string_view::npos) { // unterminated: nothing to substitute
      out.append(tmpl.substr(i));
      break;
    }
    const std::string_view body = tmpl.substr(i + 1, close - i - 1);
    i = close + 1;

    if (body.empty()) { // `$$` is an escaped dollar
      out.push_back('$');
      continue;
    }

    const size_t percent = body.find('%');
    const std::string_view name = body.substr(0, percent);
    std::string_view format =
        (percent == std::string_view::npos) ? std::string_view {} : body.substr(percent);

    auto passThrough = [&] {
      out.push_back('$');
      out.append(body);
      out.push_back('$');
    };

    if (name == "RepresentationID") {
      if (values.has_representation) {
        out.append(values.representation_id);
      } else {
        passThrough();
      }
      continue;
    }

    uint64_t value = 0;
    if (name == "Number") {
      if (!values.has_number) {
        passThrough();
        continue;
      }
      value = values.number;
    } else if (name == "Bandwidth") {
      if (!values.has_representation) {
        passThrough();
        continue;
      }
      value = values.bandwidth;
    } else if (name == "Time") {
      if (!values.has_time) {
        passThrough();
        continue;
      }
      value = static_cast<uint64_t>(values.time < 0 ? 0 : values.time);
    } else { // an identifier from some later edition of the spec: leave it alone
      passThrough();
      continue;
    }

    // `%0<width>d` is the only conversion the spec allows, so the width is all
    // there is to read out of it.
    size_t width = 0;
    if (format.size() >= 2 && format.front() == '%') {
      format.remove_prefix(1);
      while (!format.empty() && format.front() == '0') format.remove_prefix(1);
      while (!format.empty() && (std::isdigit(static_cast<unsigned char>(format.front())) != 0)) {
        width = width * 10 + static_cast<size_t>(format.front() - '0');
        format.remove_prefix(1);
      }
    }
    std::string digits = std::to_string(value);
    if (digits.size() < width) digits.insert(0, width - digits.size(), '0');
    out.append(digits);
  }
  return out;
}

} // namespace

auto expandTemplate(std::string_view tmpl, std::string_view representation_id, uint32_t bandwidth,
                    uint64_t number, int64_t time) -> std::string {
  return expandWith(tmpl, {representation_id, bandwidth, number, time, true, true, true});
}

auto expandRepresentationTemplate(std::string_view tmpl, std::string_view representation_id,
                                  uint32_t bandwidth) -> std::string {
  return expandWith(tmpl, {representation_id, bandwidth, 0, 0, true, false, false});
}

// ---------------------------------------------------------------------------
// Representation
// ---------------------------------------------------------------------------

auto Representation::mediaType() const -> OMMediaType {
  if (mime_type.starts_with("video/")) return OM_MEDIA_VIDEO;
  if (mime_type.starts_with("audio/")) return OM_MEDIA_AUDIO;
  if (mime_type.starts_with("text/") || mime_type.starts_with("application/ttml") ||
      mime_type.starts_with("application/mp4;")) {
    return OM_MEDIA_SUBTITLE;
  }
  return OM_MEDIA_NONE;
}

auto Representation::initSegment() const -> std::optional<SegmentRequest> {
  if (init_url.empty()) return std::nullopt;
  return SegmentRequest {init_url, init_range.offset, init_range.length};
}

auto Representation::segmentCount() const -> std::optional<uint64_t> {
  switch (addressing) {
    case Addressing::None: return uint64_t {0};
    case Addressing::Single: return uint64_t {1};
    case Addressing::List: return static_cast<uint64_t>(list.size());
    case Addressing::Timeline: {
      uint64_t total = 0;
      for (const TimelineRun& run : timeline) {
        if (run.repeat < 0) return std::nullopt; // grows as the stream does
        total += static_cast<uint64_t>(run.repeat) + 1;
      }
      return total;
    }
    case Addressing::Number: {
      if (segment_duration == 0 || period_duration_ns <= 0) return std::nullopt;
      const int64_t span = nsToScale(period_duration_ns, timescale);
      if (span <= 0) return std::nullopt;
      return (static_cast<uint64_t>(span) + segment_duration - 1) / segment_duration;
    }
  }
  return std::nullopt;
}

auto Representation::segmentAt(uint64_t ordinal) const -> std::optional<Segment> {
  const auto bound = segmentCount();
  if (bound && ordinal >= *bound) return std::nullopt;

  switch (addressing) {
    case Addressing::None: return std::nullopt;

    case Addressing::Single: {
      const uint64_t duration =
          period_duration_ns > 0 ? static_cast<uint64_t>(nsToScale(period_duration_ns, timescale))
                                 : 0;
      return Segment {SegmentRequest {media_url, 0, -1}, presentation_time_offset, duration};
    }

    case Addressing::List: {
      const ListEntry& entry = list[static_cast<size_t>(ordinal)];
      return Segment {SegmentRequest {entry.url, entry.range.offset, entry.range.length},
                      presentation_time_offset + static_cast<int64_t>(ordinal * segment_duration),
                      segment_duration};
    }

    case Addressing::Number: {
      const uint64_t number = start_number + ordinal;
      const int64_t time =
          presentation_time_offset + static_cast<int64_t>(ordinal * segment_duration);
      return Segment {SegmentRequest {expandTemplate(media_template, id, bandwidth, number, time),
                                      0, -1},
                      time, segment_duration};
    }

    case Addressing::Timeline: {
      uint64_t remaining = ordinal;
      for (const TimelineRun& run : timeline) {
        const uint64_t in_run =
            (run.repeat < 0) ? std::numeric_limits<uint64_t>::max()
                             : static_cast<uint64_t>(run.repeat) + 1;
        if (remaining >= in_run) {
          remaining -= in_run;
          continue;
        }
        const int64_t time = run.start + static_cast<int64_t>(remaining * run.duration);
        const uint64_t number = start_number + ordinal;
        return Segment {
            SegmentRequest {expandTemplate(media_template, id, bandwidth, number, time), 0, -1},
            time, run.duration};
      }
      return std::nullopt;
    }
  }
  return std::nullopt;
}

auto Representation::ordinalForTime(int64_t time) const -> uint64_t {
  switch (addressing) {
    case Addressing::None:
    case Addressing::Single: return 0;

    case Addressing::List:
    case Addressing::Number: {
      if (segment_duration == 0) return 0;
      const int64_t elapsed = time - presentation_time_offset;
      if (elapsed <= 0) return 0;
      uint64_t ordinal = static_cast<uint64_t>(elapsed) / segment_duration;
      if (const auto bound = segmentCount(); bound && *bound != 0 && ordinal >= *bound) {
        ordinal = *bound - 1;
      }
      return ordinal;
    }

    case Addressing::Timeline: {
      uint64_t ordinal = 0;
      uint64_t last = 0;
      for (const TimelineRun& run : timeline) {
        if (run.duration == 0) continue;
        const uint64_t in_run = (run.repeat < 0)
                                    ? std::numeric_limits<uint64_t>::max()
                                    : static_cast<uint64_t>(run.repeat) + 1;
        const int64_t run_end =
            run.start + static_cast<int64_t>(std::min<uint64_t>(in_run, 1ull << 40) * run.duration);
        if (time < run.start) return ordinal;
        if (time < run_end || run.repeat < 0) {
          const uint64_t within = static_cast<uint64_t>(time - run.start) / run.duration;
          return ordinal + std::min(within, in_run - 1);
        }
        ordinal += in_run;
        last = ordinal;
      }
      return last == 0 ? 0 : last - 1;
    }
  }
  return 0;
}

auto Representation::coveredDuration() const -> uint64_t {
  switch (addressing) {
    case Addressing::None: return 0;
    case Addressing::Single:
      return period_duration_ns > 0 ? static_cast<uint64_t>(nsToScale(period_duration_ns, timescale))
                                    : 0;
    case Addressing::List: return static_cast<uint64_t>(list.size()) * segment_duration;
    case Addressing::Number: {
      const auto bound = segmentCount();
      return bound ? *bound * segment_duration : 0;
    }
    case Addressing::Timeline: {
      uint64_t total = 0;
      for (const TimelineRun& run : timeline) {
        if (run.repeat < 0) return 0;
        total += (static_cast<uint64_t>(run.repeat) + 1) * run.duration;
      }
      return total;
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Manifest parsing
// ---------------------------------------------------------------------------

namespace {

// Attributes a Representation may inherit from its AdaptationSet, and an
// AdaptationSet from its Period.
struct MediaAttrs {
  std::string mime_type;
  std::string codecs;
  std::string language;
  std::string content_type;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t sample_rate = 0;
  uint32_t channels = 0;
  uint32_t bandwidth = 0;
  Rational framerate = {};
};

void readMediaAttrs(const xml::Node& node, MediaAttrs& attrs) {
  if (node.attribute("mimeType")) attrs.mime_type = attrText(node, "mimeType");
  if (node.attribute("codecs")) attrs.codecs = attrText(node, "codecs");
  if (node.attribute("lang")) attrs.language = attrText(node, "lang");
  if (node.attribute("contentType")) attrs.content_type = attrText(node, "contentType");
  if (node.attribute("width")) attrs.width = static_cast<uint32_t>(attrUInt(node, "width"));
  if (node.attribute("height")) attrs.height = static_cast<uint32_t>(attrUInt(node, "height"));
  if (node.attribute("audioSamplingRate")) {
    attrs.sample_rate = attrFirstUInt(node, "audioSamplingRate");
  }
  if (node.attribute("bandwidth")) {
    attrs.bandwidth = static_cast<uint32_t>(attrUInt(node, "bandwidth"));
  }
  if (node.attribute("frameRate")) attrs.framerate = attrRational(node, "frameRate");

  // Channel count lives in a descriptor rather than an attribute, and the
  // scheme that names it varies; every scheme in use puts the count in @value.
  for (const xml::Node* config : node.childrenNamed("AudioChannelConfiguration")) {
    const uint32_t value = static_cast<uint32_t>(attrUInt(*config, "value"));
    if (value != 0) attrs.channels = value;
  }
}

// Everything about segment addressing that a level can declare, accumulated as
// the parse descends.
struct SegmentCtx {
  uint32_t timescale = 0;
  uint64_t start_number = 0;
  uint64_t duration = 0;
  int64_t presentation_time_offset = 0;

  std::string media;
  std::string initialization;

  ByteRange init_range;
  bool has_init_range = false;
  std::string init_source_url;

  std::vector<TimelineRun> timeline;
  bool has_timeline = false;

  std::vector<ListEntry> list;
  bool has_list = false;

  ByteRange index_range;
  bool has_segment_base = false;
};

// SegmentBase attributes, shared by SegmentTemplate and SegmentList.
void readSegmentBaseAttrs(const xml::Node& node, std::string_view base_url, SegmentCtx& ctx) {
  if (node.attribute("timescale")) {
    ctx.timescale = static_cast<uint32_t>(attrUInt(node, "timescale"));
  }
  if (node.attribute("presentationTimeOffset")) {
    ctx.presentation_time_offset = attrInt(node, "presentationTimeOffset");
  }
  if (node.attribute("indexRange")) {
    bool present = false;
    ctx.index_range = attrRange(node, "indexRange", present);
  }
  if (const xml::Node* init = node.child("Initialization")) {
    bool present = false;
    const ByteRange range = attrRange(*init, "range", present);
    if (present) {
      ctx.init_range = range;
      ctx.has_init_range = true;
    }
    if (const auto source = init->attribute("sourceURL"); source && !source->empty()) {
      ctx.init_source_url = resolveUrl(base_url, *source);
    }
  }
}

void readMultipleSegmentBaseAttrs(const xml::Node& node, SegmentCtx& ctx) {
  if (node.attribute("duration")) ctx.duration = attrUInt(node, "duration");
  if (node.attribute("startNumber")) ctx.start_number = attrUInt(node, "startNumber");

  if (const xml::Node* tl = node.child("SegmentTimeline")) {
    ctx.timeline.clear();
    ctx.has_timeline = true;
    int64_t cursor = 0;
    for (const xml::Node* s : tl->childrenNamed("S")) {
      TimelineRun run;
      // @t is optional after the first entry: a run with none starts where the
      // previous one ended, which is what makes a timeline a timeline.
      run.start = s->attribute("t") ? attrInt(*s, "t") : cursor;
      run.duration = attrUInt(*s, "d");
      run.repeat = attrInt(*s, "r", 0);
      if (run.duration == 0) continue; // a zero-length run addresses nothing
      if (run.repeat >= 0) {
        cursor = run.start + static_cast<int64_t>((static_cast<uint64_t>(run.repeat) + 1) *
                                                  run.duration);
      }
      ctx.timeline.push_back(run);
    }
  }
}

void readSegmentTemplate(const xml::Node& node, std::string_view base_url, SegmentCtx& ctx) {
  readSegmentBaseAttrs(node, base_url, ctx);
  readMultipleSegmentBaseAttrs(node, ctx);
  if (const auto media = node.attribute("media")) ctx.media = std::string(*media);
  if (const auto init = node.attribute("initialization")) {
    ctx.initialization = std::string(*init);
  }
}

void readSegmentList(const xml::Node& node, std::string_view base_url, SegmentCtx& ctx) {
  readSegmentBaseAttrs(node, base_url, ctx);
  readMultipleSegmentBaseAttrs(node, ctx);

  ctx.list.clear();
  ctx.has_list = true;
  for (const xml::Node* entry : node.childrenNamed("SegmentURL")) {
    ListEntry item;
    const auto media = entry->attribute("media");
    item.url = resolveUrl(base_url, media ? *media : std::string_view {});
    bool present = false;
    item.range = attrRange(*entry, "mediaRange", present);
    if (item.url.empty()) continue;
    ctx.list.push_back(std::move(item));
  }
}

void readSegmentBase(const xml::Node& node, std::string_view base_url, SegmentCtx& ctx) {
  readSegmentBaseAttrs(node, base_url, ctx);
  ctx.has_segment_base = true;
}

// Applies whichever of the three addressing elements a level carries. Each is
// read fresh, so a Representation naming its own SegmentTemplate replaces the
// AdaptationSet's rather than merging with it -- except for the attributes it
// leaves out, which is exactly what inheritance is for.
void readAddressing(const xml::Node& node, std::string_view base_url, SegmentCtx& ctx) {
  if (const xml::Node* tmpl = node.child("SegmentTemplate")) {
    readSegmentTemplate(*tmpl, base_url, ctx);
  }
  if (const xml::Node* list = node.child("SegmentList")) {
    readSegmentList(*list, base_url, ctx);
  }
  if (const xml::Node* segment_base = node.child("SegmentBase")) {
    readSegmentBase(*segment_base, base_url, ctx);
  }
}

// The first non-empty BaseURL of a level, resolved against what it inherits.
// Alternatives beyond the first describe other servers holding the same bytes,
// which is a redundancy policy and so the caller's business, not the parser's.
auto descendBaseUrl(const xml::Node& node, const std::string& inherited) -> std::string {
  for (const xml::Node* base : node.childrenNamed("BaseURL")) {
    if (!base->text.empty()) return resolveUrl(inherited, base->text);
  }
  return inherited;
}

auto mediaTypeOf(const MediaAttrs& attrs) -> OMMediaType {
  if (attrs.content_type == "video") return OM_MEDIA_VIDEO;
  if (attrs.content_type == "audio") return OM_MEDIA_AUDIO;
  if (attrs.content_type == "text") return OM_MEDIA_SUBTITLE;
  if (attrs.mime_type.starts_with("video/")) return OM_MEDIA_VIDEO;
  if (attrs.mime_type.starts_with("audio/")) return OM_MEDIA_AUDIO;
  if (attrs.mime_type.starts_with("text/") || attrs.mime_type.starts_with("application/ttml")) {
    return OM_MEDIA_SUBTITLE;
  }
  return OM_MEDIA_NONE;
}

auto buildRepresentation(const xml::Node& node, const MediaAttrs& inherited_attrs,
                         const SegmentCtx& inherited_ctx, const std::string& inherited_base,
                         int64_t period_duration_ns) -> Representation {
  MediaAttrs attrs = inherited_attrs;
  readMediaAttrs(node, attrs);

  const std::string base = descendBaseUrl(node, inherited_base);
  SegmentCtx ctx = inherited_ctx;
  readAddressing(node, base, ctx);

  Representation rep;
  rep.id = attrText(node, "id");
  rep.bandwidth = attrs.bandwidth;
  rep.mime_type = attrs.mime_type;
  rep.codecs = attrs.codecs;
  rep.width = attrs.width;
  rep.height = attrs.height;
  rep.framerate = attrs.framerate;
  rep.sample_rate = attrs.sample_rate;
  rep.channels = attrs.channels;

  rep.timescale = ctx.timescale != 0 ? ctx.timescale : 1;
  rep.start_number = ctx.start_number != 0 ? ctx.start_number : 1;
  rep.segment_duration = ctx.duration;
  rep.presentation_time_offset = ctx.presentation_time_offset;
  rep.timeline = ctx.timeline;
  rep.index_range = ctx.index_range;
  rep.period_duration_ns = period_duration_ns;

  if (ctx.has_list) {
    rep.addressing = Addressing::List;
    rep.list = ctx.list;
  } else if (!ctx.media.empty()) {
    rep.addressing = ctx.has_timeline ? Addressing::Timeline : Addressing::Number;
    // Only $Number$ and $Time$ are left after this, so naming a segment later
    // needs nothing but its number and its time.
    rep.media_template =
        resolveUrl(base, expandRepresentationTemplate(ctx.media, rep.id, rep.bandwidth));
  } else {
    rep.addressing = Addressing::Single;
    rep.media_url = base;
  }

  // An initialization segment can be named three ways, and a manifest may use
  // more than one: a template, an explicit sourceURL, or a byte range of the
  // media resource itself.
  if (!ctx.initialization.empty()) {
    rep.init_url =
        resolveUrl(base, expandRepresentationTemplate(ctx.initialization, rep.id, rep.bandwidth));
    if (ctx.has_init_range) rep.init_range = ctx.init_range;
  } else if (!ctx.init_source_url.empty()) {
    rep.init_url = ctx.init_source_url;
    if (ctx.has_init_range) rep.init_range = ctx.init_range;
  } else if (ctx.has_init_range) {
    rep.init_url = rep.addressing == Addressing::Single ? rep.media_url : base;
    rep.init_range = ctx.init_range;
  }

  if (rep.mime_type.empty() && !rep.codecs.empty()) {
    // Nothing said what this is; the codec string is the last clue available.
    rep.mime_type = rep.codecs.starts_with("mp4a") || rep.codecs.starts_with("opus") ||
                            rep.codecs.starts_with("ac-3") || rep.codecs.starts_with("ec-3") ||
                            rep.codecs.starts_with("flac")
                        ? "audio/mp4"
                        : "video/mp4";
  }
  return rep;
}

} // namespace

auto parseManifest(std::span<const uint8_t> document, std::string_view manifest_url)
    -> Result<Manifest, OMError> {
  auto parsed = xml::parse(
      std::string_view(reinterpret_cast<const char*>(document.data()), document.size()));
  if (parsed.isErr()) return Err(std::move(parsed).unwrapErr());

  const xml::Node root = std::move(parsed).unwrap();
  if (root.name != "MPD") return Err(OM_FORMAT_INVALID_HEADER);

  Manifest manifest;
  manifest.dynamic = root.attribute("type").value_or("static") == "dynamic";
  manifest.duration_ns =
      std::max<int64_t>(0, parseIsoDuration(root.attribute("mediaPresentationDuration")
                                                .value_or(std::string_view {})));
  manifest.min_buffer_ns =
      std::max<int64_t>(0, parseIsoDuration(root.attribute("minBufferTime")
                                                .value_or(std::string_view {})));
  manifest.minimum_update_period_ns =
      root.attribute("minimumUpdatePeriod")
          ? parseIsoDuration(*root.attribute("minimumUpdatePeriod"))
          : -1;
  manifest.availability_start_time = attrText(root, "availabilityStartTime");

  const std::string mpd_base = descendBaseUrl(root, std::string(manifest_url));

  MediaAttrs root_attrs;
  readMediaAttrs(root, root_attrs);
  SegmentCtx root_ctx;
  readAddressing(root, mpd_base, root_ctx);

  const std::vector<const xml::Node*> period_nodes = root.childrenNamed("Period");

  // A period without @duration runs until the next one starts, or to the end of
  // the presentation when it is the last.
  std::vector<int64_t> starts(period_nodes.size(), 0);
  for (size_t i = 0; i < period_nodes.size(); ++i) {
    const int64_t declared =
        period_nodes[i]->attribute("start")
            ? parseIsoDuration(*period_nodes[i]->attribute("start"))
            : -1;
    starts[i] = declared >= 0 ? declared : (i == 0 ? 0 : -1);
  }

  for (size_t i = 0; i < period_nodes.size(); ++i) {
    const xml::Node& node = *period_nodes[i];

    Period period;
    period.id = attrText(node, "id");
    period.start_ns = starts[i] >= 0 ? starts[i] : 0;
    period.duration_ns =
        std::max<int64_t>(0, parseIsoDuration(node.attribute("duration")
                                                  .value_or(std::string_view {})));
    if (period.duration_ns == 0) {
      if (i + 1 < period_nodes.size() && starts[i + 1] > period.start_ns) {
        period.duration_ns = starts[i + 1] - period.start_ns;
      } else if (manifest.duration_ns > period.start_ns) {
        period.duration_ns = manifest.duration_ns - period.start_ns;
      }
    }

    const std::string period_base = descendBaseUrl(node, mpd_base);
    MediaAttrs period_attrs = root_attrs;
    readMediaAttrs(node, period_attrs);
    SegmentCtx period_ctx = root_ctx;
    readAddressing(node, period_base, period_ctx);

    for (const xml::Node* set_node : node.childrenNamed("AdaptationSet")) {
      const std::string set_base = descendBaseUrl(*set_node, period_base);
      MediaAttrs set_attrs = period_attrs;
      readMediaAttrs(*set_node, set_attrs);
      SegmentCtx set_ctx = period_ctx;
      readAddressing(*set_node, set_base, set_ctx);

      AdaptationSet set;
      set.type = mediaTypeOf(set_attrs);
      set.language = set_attrs.language;

      for (const xml::Node* rep_node : set_node->childrenNamed("Representation")) {
        Representation rep =
            buildRepresentation(*rep_node, set_attrs, set_ctx, set_base, period.duration_ns);
        if (rep.addressing == Addressing::None) continue;
        if (set.type == OM_MEDIA_NONE) set.type = rep.mediaType();
        set.representations.push_back(std::move(rep));
      }
      if (set.representations.empty()) continue;

      // Best quality first, so picking a representation under a cap is a scan
      // from the front and picking the best of all is the first entry.
      std::stable_sort(set.representations.begin(), set.representations.end(),
                       [](const Representation& a, const Representation& b) {
                         return a.bandwidth > b.bandwidth;
                       });
      period.sets.push_back(std::move(set));
    }
    if (!period.sets.empty()) manifest.periods.push_back(std::move(period));
  }

  if (manifest.periods.empty()) return Err(OM_FORMAT_NO_STREAMS);

  if (manifest.duration_ns == 0) {
    int64_t total = 0;
    for (const Period& period : manifest.periods) {
      total = std::max(total, period.start_ns + period.duration_ns);
    }
    manifest.duration_ns = total;
  }
  return Ok(std::move(manifest));
}

} // namespace openmedia::dash
