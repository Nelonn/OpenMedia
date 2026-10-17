#include <algorithm>
#include <formats.hpp>
#include <hls/m3u8.hpp>
#include <map>
#include <memory>
#include <openmedia/io.hpp>
#include <openmedia/streaming.hpp>
#include <streaming/segment_demuxer.hpp>
#include <streaming/segment_index.hpp>
#include <util/io_util.hpp>
#include <utility>
#include <vector>

namespace openmedia {
namespace {

// A playlist measures segments in decimal seconds, which milliseconds hold exactly.
// Used only for addressing segments: the timestamps handed to the caller come from
// the media itself, in whatever timescale its header declares.
constexpr uint32_t PLAYLIST_TIMESCALE = 1000;

// What a variant says about the playlist it points at. Kept separate from the
// playlist because a rendition's own playlist carries none of it.
struct IndexDescription {
  std::string id;
  std::string codecs;
  uint32_t bandwidth = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

// `#EXT-X-KEY` turned into the form the demuxer and the caller both read.
auto segmentEncryptionOf(const hls::Key& key, uint64_t sequence)
    -> std::shared_ptr<const SegmentEncryption> {
  if (!key.isEncrypted()) return nullptr;

  auto out = std::make_shared<SegmentEncryption>();
  out->method = key.method_name;
  out->uri = key.uri;
  out->key_format = key.format.empty() ? std::string("identity") : key.format;
  out->key_format_versions = key.format_versions;
  out->sequence = sequence;
  out->whole_segment = key.method == hls::KeyMethod::Aes128;

  // An initialization vector the tag did not state is the media sequence number,
  // big-endian in the low eight bytes of the block (RFC 8216 5.2).
  out->iv_explicit = hls::parseHexIv(key.iv, out->iv);
  if (!out->iv_explicit) {
    for (int i = 0; i < 8; ++i) {
      out->iv[15 - i] = static_cast<uint8_t>((sequence >> (8 * i)) & 0xFFu);
    }
  }
  return out;
}

// An HLS media playlist, answering the questions the segment demuxer asks.
//
// A playlist is already the explicit segment list DASH computes from a template, so
// the only thing to build is each segment's running start time -- a playlist states
// only how long each one lasts.
class HlsSegmentIndex final : public SegmentIndex {
  // Shared, not copied: a four-hour playlist is thousands of segments, each with its
  // own URL and a repeat of the initialization section and key in force.
  std::shared_ptr<const hls::MediaPlaylist> playlist_;
  IndexDescription description_;
  // Each segment's start in milliseconds, plus one entry for the end, so a binary
  // search answers ordinalForTime directly.
  std::vector<int64_t> starts_;
  // Per segment, the `#EXT-X-KEY` in force there, or null where it is in the clear.
  // Built once here rather than in segmentAt(), which is called per packet.
  std::vector<std::shared_ptr<const SegmentEncryption>> keys_;

public:
  HlsSegmentIndex(std::shared_ptr<const hls::MediaPlaylist> playlist,
                  IndexDescription description)
      : playlist_(std::move(playlist)), description_(std::move(description)) {
    const std::vector<hls::MediaSegment>& segments = playlist_->segments;

    starts_.reserve(segments.size() + 1);
    keys_.reserve(segments.size());
    int64_t cursor = 0;
    for (const hls::MediaSegment& segment : segments) {
      starts_.push_back(cursor);
      cursor += static_cast<int64_t>(segment.duration * 1000.0 + 0.5);
      // One per segment even where a run of them is under the same `#EXT-X-KEY`:
      // every one carries its own media sequence number, and that is what a derived
      // initialization vector is made of. Sharing them would have been a few hundred
      // kilobytes on a four-hour playlist, against a copy of the tag itself that the
      // playlist is already keeping per segment.
      keys_.push_back(segmentEncryptionOf(segment.key, segment.sequence));
    }
    starts_.push_back(cursor);
  }

  auto initSegment() const -> std::optional<SegmentRequest> override {
    if (playlist_->segments.empty()) return std::nullopt;
    return initOf(playlist_->segments.front());
  }

  auto segmentCount() const -> std::optional<uint64_t> override {
    // What this playlist listed, live or not: a growing playlist grows in a later
    // copy of itself, and fetching that copy is the caller's to do.
    return static_cast<uint64_t>(playlist_->segments.size());
  }

  auto segmentAt(uint64_t ordinal) const -> std::optional<Segment> override {
    if (ordinal >= playlist_->segments.size()) return std::nullopt;
    const auto at = static_cast<size_t>(ordinal);
    const hls::MediaSegment& segment = playlist_->segments[at];
    const int64_t start = starts_[at];

    Segment out;
    out.request = SegmentRequest {segment.url, segment.range.offset, segment.range.length};
    out.time = start;
    out.duration = static_cast<uint64_t>(starts_[at + 1] - start);
    out.init = initOf(segment);
    out.gap = segment.gap;
    // A playlist's first segment is a discontinuity in all but name: the clock before
    // it is whatever the last read left behind, which after a seek or a quality
    // switch has nothing to do with this one.
    out.discontinuity = segment.discontinuity || at == 0;
    out.program_date_time_ms = segment.program_date_time_ms;
    out.encryption = keys_[at];
    return out;
  }

  auto ordinalForTime(int64_t media_time) const -> uint64_t override {
    const size_t count = playlist_->segments.size();
    if (count == 0 || media_time <= 0) return 0;
    // The first segment starting after the target, stepped back one.
    const auto after = std::upper_bound(
        starts_.begin(), starts_.begin() + static_cast<ptrdiff_t>(count), media_time);
    if (after == starts_.begin()) return 0;
    return static_cast<uint64_t>((after - starts_.begin()) - 1);
  }

  auto timescale() const -> uint32_t override { return PLAYLIST_TIMESCALE; }

  auto id() const -> std::string_view override { return description_.id; }
  auto bandwidth() const -> uint32_t override { return description_.bandwidth; }
  auto codecs() const -> std::string_view override { return description_.codecs; }
  auto width() const -> uint32_t override { return description_.width; }
  auto height() const -> uint32_t override { return description_.height; }

  auto isLive() const -> bool { return playlist_->isLive(); }
  auto totalDurationNs() const -> int64_t { return starts_.back() * INT64_C(1'000'000); }
  auto isEmpty() const -> bool { return playlist_->segments.empty(); }

  // `#EXT-X-TARGETDURATION`: the longest any one segment may be, which is how long
  // a live playlist stays current.
  auto targetDurationNs() const -> int64_t {
    return static_cast<int64_t>(playlist_->target_duration * 1e9);
  }

  // `#EXT-X-START`, where the playlist asks playback to begin. A negative offset
  // counts back from the end, which only a playlist with a known end can mean.
  auto startOffsetNs() const -> int64_t {
    if (!playlist_->has_start) return 0;
    const double total = playlist_->totalDuration();
    const double offset = playlist_->start_offset < 0 ? total + playlist_->start_offset
                                                      : playlist_->start_offset;
    return static_cast<int64_t>(std::clamp(offset, 0.0, total) * 1e9);
  }

private:
  static auto initOf(const hls::MediaSegment& segment) -> std::optional<SegmentRequest> {
    if (segment.init_url.empty()) return std::nullopt;
    return SegmentRequest {segment.init_url, segment.init_range.offset,
                           segment.init_range.length};
  }
};

auto mediaTypeOf(hls::RenditionType type) -> OMMediaType {
  switch (type) {
    case hls::RenditionType::Audio: return OM_MEDIA_AUDIO;
    case hls::RenditionType::Video: return OM_MEDIA_VIDEO;
    case hls::RenditionType::Subtitles: return OM_MEDIA_SUBTITLE;
    default: return OM_MEDIA_NONE;
  }
}

// What `#EXT-X-MEDIA` says about a rendition's part in the presentation. The
// CHARACTERISTICS strings are Apple's media characteristic identifiers, of which
// the two accessibility ones are what a track list has somewhere to put.
auto dispositionOf(const hls::Rendition& rendition) -> OMDisposition {
  auto flags = static_cast<uint16_t>(OM_DISPOSITION_NONE);
  if (rendition.is_default) flags |= OM_DISPOSITION_DEFAULT;
  if (rendition.forced) flags |= OM_DISPOSITION_FORCED;
  if (rendition.characteristics.find("public.accessibility.transcribes-spoken-dialog") !=
          std::string::npos ||
      rendition.characteristics.find(
          "public.accessibility.describes-music-and-sound") != std::string::npos) {
    flags |= OM_DISPOSITION_HEARING_IMPAIRED;
  }
  if (rendition.characteristics.find("public.accessibility.describes-video") !=
      std::string::npos) {
    flags |= OM_DISPOSITION_VISUAL_IMPAIRED;
  }
  if (rendition.type == hls::RenditionType::ClosedCaptions) flags |= OM_DISPOSITION_CAPTIONS;
  return static_cast<OMDisposition>(flags);
}

auto sameLanguage(std::string_view a, std::string_view b) -> bool {
  const auto primary = [](std::string_view tag) {
    const size_t dash = tag.find('-');
    return dash == std::string_view::npos ? tag : tag.substr(0, dash);
  };
  const std::string_view left = primary(a);
  const std::string_view right = primary(b);
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) ==
                  std::tolower(static_cast<unsigned char>(y));
         });
}

// `hls::Key` as the public API reports it. `session` tells a master playlist's
// `#EXT-X-SESSION-KEY` from a media playlist's `#EXT-X-KEY`, which is the one thing
// a caller has to know before it can decide when to ask for a licence.
auto publicKeyOf(const hls::Key& key, bool session) -> HlsKey {
  HlsKey out;
  out.method = key.method_name;
  out.uri = key.uri;
  // The spec's default: an ordinary key fetched over HTTP.
  out.key_format = key.format.empty() ? std::string("identity") : key.format;
  out.key_format_versions = key.format_versions;
  out.iv = key.iv;
  out.session = session;
  return out;
}

// Two tags naming the same key, which is what a playlist repeating one does between
// every pair of segments it covers. The initialization vector is left out: a
// per-segment one does not make a second key, it makes the same key used again.
auto sameKey(const HlsKey& a, const HlsKey& b) -> bool {
  return a.method == b.method && a.uri == b.uri && a.key_format == b.key_format &&
         a.session == b.session;
}

void appendKey(std::vector<HlsKey>& keys, HlsKey key) {
  const bool seen = std::any_of(keys.begin(), keys.end(), [&](const HlsKey& existing) {
    return sameKey(existing, key);
  });
  if (!seen) keys.push_back(std::move(key));
}

class HlsPlaylistsImpl final : public HlsPlaylists {
  StreamOptions options_;
  std::optional<hls::MasterPlaylist> master_;
  // Keyed by the URL it was fetched from, which is how a variant or rendition
  // names it.
  std::map<std::string, std::shared_ptr<const hls::MediaPlaylist>> media_;
  // Arrival order, for the case where there is no master and the first playlist
  // added is simply the stream.
  std::vector<std::string> arrival_;

public:
  explicit HlsPlaylistsImpl(const StreamOptions& options) : options_(options) {}

  auto add(std::span<const uint8_t> document, std::string_view url) -> OMError override {
    if (document.empty()) return OM_COMMON_INVALID_ARGUMENT;

    switch (hls::detectKind(document)) {
      case hls::PlaylistKind::Master: {
        auto parsed = hls::parseMaster(document, url);
        if (parsed.isErr()) return std::move(parsed).unwrapErr();
        master_ = std::move(parsed).unwrap();
        return OM_SUCCESS;
      }
      case hls::PlaylistKind::Media: {
        auto parsed = hls::parseMedia(document, url);
        if (parsed.isErr()) return std::move(parsed).unwrapErr();
        const std::string key(url);
        if (!media_.contains(key)) arrival_.push_back(key);
        media_.insert_or_assign(
            key, std::make_shared<const hls::MediaPlaylist>(std::move(parsed).unwrap()));
        return OM_SUCCESS;
      }
      case hls::PlaylistKind::Unknown: break;
    }
    return OM_FORMAT_INVALID_HEADER;
  }

  auto needed() const -> std::vector<std::string> override {
    std::vector<std::string> wanted;
    if (!master_) {
      // A lone media playlist is the whole stream, and before the first one
      // arrives there is nothing to say.
      return wanted;
    }
    for (const std::string& url : requiredUrls()) {
      if (!media_.contains(url)) wanted.push_back(url);
    }
    return wanted;
  }

  auto variants() const -> std::vector<HlsVariant> override {
    std::vector<HlsVariant> ladder;
    if (!master_) return ladder;

    const hls::Variant* chosen = chosenVariant();
    for (const hls::Variant& variant : master_->variants) {
      if (variant.iframes_only) continue; // not something to play straight through
      ladder.push_back(HlsVariant {variant.url, variant.backup_urls, variant.codecs,
                                   variant.bandwidth, variant.resolution.width,
                                   variant.resolution.height, &variant == chosen});
    }
    return ladder;
  }

  auto keys() const -> std::vector<HlsKey> override {
    std::vector<HlsKey> keys;
    // The master playlist first, since those are the keys a player can act on
    // before it has fetched anything else.
    if (master_) {
      for (const hls::Key& key : master_->session_keys) {
        appendKey(keys, publicKeyOf(key, /*session=*/true));
      }
    }
    // In the order the playlists were added, so a presentation's answer does not
    // depend on how a std::map happened to order its URLs.
    for (const std::string& url : arrival_) {
      const auto found = media_.find(url);
      if (found == media_.end()) continue;
      for (const hls::MediaSegment& segment : found->second->segments) {
        if (segment.key.isEncrypted()) appendKey(keys, publicKeyOf(segment.key, false));
      }
    }
    return keys;
  }

  auto open() -> Result<std::unique_ptr<SegmentedDemuxer>, OMError> override {
    if (media_.empty()) return Err(OM_IO_NOT_ENOUGH_DATA);
    for (const std::string& url : requiredUrls()) {
      if (!media_.contains(url)) return Err(OM_IO_NOT_ENOUGH_DATA);
    }

    SegmentSource source = master_ ? sourceFromMaster() : sourceFromLoneMedia();
    if (source.tracks.empty()) return Err(OM_FORMAT_NO_STREAMS);
    return createSegmentDemuxer(std::move(source), options_);
  }

private:
  // The variant to play, under the caps. Sorted best first, so the first that fits
  // is the best that fits.
  auto chosenVariant() const -> const hls::Variant* {
    if (!master_ || master_->variants.empty()) return nullptr;
    const hls::Variant* fallback = nullptr;
    for (const hls::Variant& variant : master_->variants) {
      // Trick-play tracks are not something to play straight through.
      if (variant.iframes_only) continue;
      fallback = fallback ? fallback : &variant;
      if (options_.max_bandwidth != 0 && variant.bandwidth > options_.max_bandwidth) continue;
      if (options_.max_width != 0 && variant.resolution.width > options_.max_width) continue;
      if (options_.max_height != 0 && variant.resolution.height > options_.max_height) continue;
      return &variant;
    }
    return fallback;
  }

  // Renditions worth reading alongside the chosen variant: those with a playlist of
  // their own, in the groups the variant draws from.
  auto chosenRenditions() const -> std::vector<const hls::Rendition*> {
    std::vector<const hls::Rendition*> chosen;
    const hls::Variant* variant = chosenVariant();
    if (!master_ || !variant) return chosen;

    for (const hls::RenditionType type : {hls::RenditionType::Audio,
                                          hls::RenditionType::Subtitles}) {
      if (type == hls::RenditionType::Subtitles && !options_.with_subtitles) continue;
      const std::string& group = (type == hls::RenditionType::Audio) ? variant->audio_group
                                                                    : variant->subtitle_group;
      if (group.empty()) continue; // that track is muxed into the variant itself

      std::vector<const hls::Rendition*> group_members;
      for (const hls::Rendition& rendition : master_->renditions) {
        if (rendition.type != type || rendition.group_id != group) continue;
        if (rendition.url.empty()) continue; // nothing separate to fetch
        group_members.push_back(&rendition);
      }
      if (group_members.empty()) continue;

      // One rendition per group: a group's members are the same track in different
      // languages, and reading all of them downloads every language to play one.
      const hls::Rendition* pick = nullptr;
      if (!options_.language.empty()) {
        for (const hls::Rendition* candidate : group_members) {
          if (sameLanguage(candidate->language, options_.language)) {
            pick = candidate;
            break;
          }
        }
      }
      // DEFAULT=YES is what the playlist asks for when nothing else was asked for,
      // and AUTOSELECT=NO withholds a rendition from exactly this choice (RFC 8216
      // 4.3.4.1) -- a commentary or a descriptive track nobody asked to hear.
      if (!pick) {
        for (const hls::Rendition* candidate : group_members) {
          if (candidate->is_default) {
            pick = candidate;
            break;
          }
        }
      }
      if (!pick) {
        for (const hls::Rendition* candidate : group_members) {
          if (candidate->autoselect) {
            pick = candidate;
            break;
          }
        }
      }
      chosen.push_back(pick ? pick : group_members.front());
    }
    return chosen;
  }

  // Where a variant's segments are listed: the first of its addresses actually in
  // hand, since a caller that could not reach the primary may have fetched a backup
  // and a rendition fetched from a backup is the same rendition.
  auto playlistUrlOf(const hls::Variant& variant) const -> const std::string& {
    if (media_.contains(variant.url)) return variant.url;
    for (const std::string& backup : variant.backup_urls) {
      if (media_.contains(backup)) return backup;
    }
    return variant.url;
  }

  auto requiredUrls() const -> std::vector<std::string> {
    std::vector<std::string> urls;
    if (const hls::Variant* variant = chosenVariant()) urls.push_back(playlistUrlOf(*variant));
    for (const hls::Rendition* rendition : chosenRenditions()) urls.push_back(rendition->url);
    return urls;
  }

  auto makeIndex(const std::string& url, IndexDescription description) const
      -> std::unique_ptr<HlsSegmentIndex> {
    const auto found = media_.find(url);
    if (found == media_.end()) return nullptr;
    return std::make_unique<HlsSegmentIndex>(found->second, std::move(description));
  }

  // What the presentation as a whole is, as the playlist being read states it.
  static void describeSource(SegmentSource& source, const HlsSegmentIndex& index) {
    source.duration_ns = index.totalDurationNs();
    source.live = index.isLive();
    source.start_offset_ns = index.startOffsetNs();
    source.target_duration_ns = index.targetDurationNs();
  }

  auto sourceFromLoneMedia() const -> SegmentSource {
    SegmentSource source;
    const std::string& url = arrival_.front();

    IndexDescription description;
    description.id = url;
    auto index = makeIndex(url, std::move(description));
    if (!index || index->isEmpty()) return source;

    describeSource(source, *index);

    SegmentTrack track;
    // Nothing in a media playlist says whether it carries video, audio or both;
    // the initialization segment does, and the reader reports it once parsed.
    track.type = OM_MEDIA_NONE;
    track.alternatives.push_back(std::move(index));
    source.tracks.push_back(std::move(track));
    return source;
  }

  auto sourceFromMaster() const -> SegmentSource {
    SegmentSource source;
    const hls::Variant* chosen = chosenVariant();
    if (!chosen) return source;
    const HlsSegmentIndex* described = nullptr;

    // The variants are one track's alternatives. Only those whose playlist is in
    // hand are offered: switching to the rest would mean fetching a playlist
    // mid-read, which this demuxer does not do.
    SegmentTrack variants;
    variants.type = OM_MEDIA_NONE; // the initialization segment knows
    size_t chosen_at = 0;
    const HlsSegmentIndex* first_readable = nullptr;
    for (const hls::Variant& variant : master_->variants) {
      if (variant.iframes_only) continue;
      if (!media_.contains(playlistUrlOf(variant))) continue;

      IndexDescription description;
      // Named by its primary address whichever one was fetched, since this is the
      // name selectRepresentation() takes and it must mean the same rendition every
      // time.
      description.id = variant.url;
      description.codecs = variant.codecs;
      description.bandwidth = variant.bandwidth;
      description.width = variant.resolution.width;
      description.height = variant.resolution.height;

      auto index = makeIndex(playlistUrlOf(variant), std::move(description));
      if (!index || index->isEmpty()) continue;
      if (!first_readable) first_readable = index.get();
      // Described by the variant that will actually be read, not by whichever
      // happened to be listed first: the two agree on a well-made ladder, and where
      // they do not there is no reason to report the one nothing will play.
      if (&variant == chosen) {
        chosen_at = variants.alternatives.size();
        described = index.get();
      }
      variants.alternatives.push_back(std::move(index));
    }
    if (variants.alternatives.empty()) return source;
    // The chosen variant's own playlist turned out unreadable, so what will be
    // played -- and what describes the presentation -- is the first that was not.
    describeSource(source, *(described ? described : first_readable));

    // The segment demuxer picks the first alternative that fits its caps, which
    // were already applied here against what the master said; putting the choice
    // first makes the two agree.
    if (chosen_at != 0) {
      std::swap(variants.alternatives[0], variants.alternatives[chosen_at]);
    }
    source.tracks.push_back(std::move(variants));

    for (const hls::Rendition* rendition : chosenRenditions()) {
      IndexDescription description;
      description.id = rendition->url;
      auto index = makeIndex(rendition->url, std::move(description));
      if (!index || index->isEmpty()) continue;

      SegmentTrack track;
      track.type = mediaTypeOf(rendition->type);
      track.language = rendition->language;
      track.name = rendition->name;
      track.disposition = dispositionOf(*rendition);
      track.channels = rendition->channels;
      track.alternatives.push_back(std::move(index));
      source.tracks.push_back(std::move(track));
    }
    return source;
  }
};

// Registered so a playlist opened through FormatRegistry behaves like any other
// container, reading everything it points at off the same filesystem. A caller
// fetching from anywhere else goes through createHlsPlaylists() and says how.
class HlsFormatDemuxer final : public Demuxer {
  std::unique_ptr<Demuxer> inner_;
  std::vector<Track> no_tracks_;
  Dictionary no_metadata_;

public:
  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    close();
    if (!input || !input->isValid()) return OM_IO_INVALID_STREAM;

    // A playlist names everything relative to itself, and this wrapper reads it off
    // the filesystem beside it. A stream that cannot say where it came from leaves
    // no "beside": resolving against the working directory would open whatever files
    // happen to sit there. Such a playlist goes through openHlsPlaylist(), which
    // takes its URL as an argument rather than guessing.
    const std::string source = input->sourcePath();
    if (source.empty()) return OM_COMMON_INVALID_ARGUMENT;

    const std::vector<uint8_t> document = readAll(*input);
    if (document.empty()) return OM_IO_NOT_ENOUGH_DATA;

    StreamOptions options;
    options.fetch = fileFetch();
    auto opened = openHlsPlaylist(document, source, options);
    if (opened.isErr()) return std::move(opened).unwrapErr();
    inner_ = std::move(opened).unwrap();
    return OM_SUCCESS;
  }

  void close() override { inner_.reset(); }

  auto tracks() const -> const std::vector<Track>& override {
    return inner_ ? inner_->tracks() : no_tracks_;
  }

  auto metadata() const -> const Dictionary& override {
    return inner_ ? inner_->metadata() : no_metadata_;
  }

  auto readPacket() -> Result<Packet, OMError> override {
    return inner_ ? inner_->readPacket() : Err(OM_COMMON_NOT_INITIALIZED);
  }

  auto seek(int32_t stream_idx, int64_t timestamp, SeekMode mode) -> OMError override {
    return inner_ ? inner_->seek(stream_idx, timestamp, mode) : OM_COMMON_NOT_INITIALIZED;
  }
};

} // namespace

auto createHlsPlaylists(const StreamOptions& options) -> std::unique_ptr<HlsPlaylists> {
  return std::make_unique<HlsPlaylistsImpl>(options);
}

namespace {

// Every address `url` can be fetched from: itself, then the backups of the variant
// it names. A rendition's own playlist has none, and neither has a non-variant.
auto addressesOf(const HlsPlaylists& playlists, const std::string& url)
    -> std::vector<std::string> {
  std::vector<std::string> addresses {url};
  for (const HlsVariant& variant : playlists.variants()) {
    if (variant.url != url) continue;
    addresses.insert(addresses.end(), variant.backup_urls.begin(), variant.backup_urls.end());
    break;
  }
  return addresses;
}

} // namespace

auto openHlsPlaylist(std::span<const uint8_t> document, std::string_view url,
                     const StreamOptions& options)
    -> Result<std::unique_ptr<SegmentedDemuxer>, OMError> {
  if (document.empty()) return Err(OM_COMMON_INVALID_ARGUMENT);

  auto playlists = createHlsPlaylists(options);
  if (const OMError error = playlists->add(document, url); error != OM_SUCCESS) {
    return Err(error);
  }

  // A master playlist needs a second round. A fetch function means the caller is
  // content to block, so it happens here; without one the round trip is the
  // caller's, through createHlsPlaylists().
  const std::vector<std::string> wanted = playlists->needed();
  if (!wanted.empty()) {
    if (!options.fetch) return Err(OM_IO_NOT_ENOUGH_DATA);
    for (const std::string& next : wanted) {
      OMError error = OM_SUCCESS;
      // The one asked for, then the other places the same rendition is published --
      // which is what a master playlist lists failover variants for.
      for (const std::string& address : addressesOf(*playlists, next)) {
        auto bytes = options.fetch(SegmentRequest {address, 0, -1});
        if (bytes.isErr()) {
          error = std::move(bytes).unwrapErr();
          continue;
        }
        const std::vector<uint8_t> fetched = std::move(bytes).unwrap();
        error = playlists->add(fetched, address);
        if (error == OM_SUCCESS) break;
      }
      if (error != OM_SUCCESS) return Err(error);
    }
  }
  return playlists->open();
}

auto hlsPlaylistKeys(std::span<const uint8_t> document, std::string_view url)
    -> std::vector<HlsKey> {
  std::vector<HlsKey> keys;
  if (document.empty()) return keys;

  // Either kind of playlist can name keys: a media playlist per segment, a master
  // playlist for the presentation. Which one this is decides where to look, and a
  // document that is neither has nothing to key.
  switch (hls::detectKind(document)) {
    case hls::PlaylistKind::Master: {
      auto parsed = hls::parseMaster(document, url);
      if (parsed.isErr()) return keys;
      for (const hls::Key& key : std::move(parsed).unwrap().session_keys) {
        appendKey(keys, publicKeyOf(key, /*session=*/true));
      }
      return keys;
    }
    case hls::PlaylistKind::Media: {
      auto parsed = hls::parseMedia(document, url);
      if (parsed.isErr()) return keys;
      for (const hls::MediaSegment& segment : std::move(parsed).unwrap().segments) {
        if (segment.key.isEncrypted()) appendKey(keys, publicKeyOf(segment.key, false));
      }
      return keys;
    }
    case hls::PlaylistKind::Unknown: return keys;
  }
  return keys;
}

const FormatDescriptor FORMAT_HLS = {
    .container_id = OM_CONTAINER_HLS,
    .name = "hls",
    .long_name = "HTTP Live Streaming",
    .demuxer_factory = [] { return std::make_unique<HlsFormatDemuxer>(); },
    .muxer_factory = {},
};

} // namespace openmedia
