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
  size_t usable_ = 0;

public:
  HlsSegmentIndex(std::shared_ptr<const hls::MediaPlaylist> playlist,
                  IndexDescription description)
      : playlist_(std::move(playlist)), description_(std::move(description)) {
    const std::vector<hls::MediaSegment>& segments = playlist_->segments;

    // A `#EXT-X-MAP` replaced part-way through changes the parameter sets the
    // segments after it decode against, and a reader holds one initialization
    // segment at a time -- so the run stops there rather than handing over segments
    // that would decode as noise. MPEG-TS names no map, so its run is the whole
    // playlist.
    const std::string init = segments.empty() ? std::string() : segments.front().init_url;
    while (usable_ < segments.size() && segments[usable_].init_url == init) ++usable_;

    starts_.reserve(usable_ + 1);
    int64_t cursor = 0;
    for (size_t i = 0; i < usable_; ++i) {
      starts_.push_back(cursor);
      cursor += static_cast<int64_t>(segments[i].duration * 1000.0 + 0.5);
    }
    starts_.push_back(cursor);
  }

  auto initSegment() const -> std::optional<SegmentRequest> override {
    if (playlist_->segments.empty()) return std::nullopt;
    const hls::MediaSegment& first = playlist_->segments.front();
    if (first.init_url.empty()) return std::nullopt;
    return SegmentRequest {first.init_url, first.init_range.offset, first.init_range.length};
  }

  auto segmentCount() const -> std::optional<uint64_t> override {
    // What this playlist listed, live or not: a growing playlist grows in a later
    // copy of itself, and fetching that copy is the caller's to do.
    return static_cast<uint64_t>(usable_);
  }

  auto segmentAt(uint64_t ordinal) const -> std::optional<Segment> override {
    if (ordinal >= usable_) return std::nullopt;
    const hls::MediaSegment& segment = playlist_->segments[static_cast<size_t>(ordinal)];
    const int64_t start = starts_[static_cast<size_t>(ordinal)];
    return Segment {SegmentRequest {segment.url, segment.range.offset, segment.range.length},
                    start, static_cast<uint64_t>(starts_[static_cast<size_t>(ordinal) + 1] - start)};
  }

  auto ordinalForTime(int64_t media_time) const -> uint64_t override {
    if (usable_ == 0 || media_time <= 0) return 0;
    // The first segment starting after the target, stepped back one.
    const auto after = std::upper_bound(starts_.begin(), starts_.begin() + usable_, media_time);
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
  auto isEncrypted() const -> bool { return playlist_->isEncrypted(); }
  auto isEmpty() const -> bool { return usable_ == 0; }
};

auto mediaTypeOf(hls::RenditionType type) -> OMMediaType {
  switch (type) {
    case hls::RenditionType::Audio: return OM_MEDIA_AUDIO;
    case hls::RenditionType::Video: return OM_MEDIA_VIDEO;
    case hls::RenditionType::Subtitles: return OM_MEDIA_SUBTITLE;
    default: return OM_MEDIA_NONE;
  }
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
      if (!pick) {
        for (const hls::Rendition* candidate : group_members) {
          if (candidate->is_default) {
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

  auto sourceFromLoneMedia() const -> SegmentSource {
    SegmentSource source;
    const std::string& url = arrival_.front();

    IndexDescription description;
    description.id = url;
    auto index = makeIndex(url, std::move(description));
    if (!index || index->isEmpty()) return source;

    source.duration_ns = index->totalDurationNs();
    source.live = index->isLive();

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

    // The variants are one track's alternatives. Only those whose playlist is in
    // hand are offered: switching to the rest would mean fetching a playlist
    // mid-read, which this demuxer does not do.
    SegmentTrack variants;
    variants.type = OM_MEDIA_NONE; // the initialization segment knows
    size_t chosen_at = 0;
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
      if (&variant == chosen) chosen_at = variants.alternatives.size();
      if (variants.alternatives.empty()) {
        source.duration_ns = index->totalDurationNs();
        source.live = index->isLive();
      }
      variants.alternatives.push_back(std::move(index));
    }
    if (variants.alternatives.empty()) return source;

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

auto hlsPlaylistKeys(std::span<const uint8_t> document) -> std::vector<HlsKey> {
  std::vector<HlsKey> keys;
  if (document.empty()) return keys;

  // Only a media playlist has keys: a master names no segments, which reads as an
  // empty list rather than an error -- there is nothing to key.
  auto parsed = hls::parseMedia(document, {});
  if (parsed.isErr()) return keys;

  for (const hls::MediaSegment& segment : parsed.unwrap().segments) {
    const hls::Key& key = segment.key;
    if (!key.isEncrypted()) continue;

    HlsKey out;
    switch (key.method) {
      case hls::KeyMethod::Aes128: out.method = "AES-128"; break;
      case hls::KeyMethod::SampleAes: out.method = "SAMPLE-AES"; break;
      case hls::KeyMethod::SampleAesCtr: out.method = "SAMPLE-AES-CTR"; break;
      default: out.method = "UNSUPPORTED"; break;
    }
    out.uri = key.uri;
    // The spec's default: an ordinary key fetched over HTTP.
    out.key_format = key.format.empty() ? std::string("identity") : key.format;
    out.iv = key.iv;

    const bool seen = std::any_of(keys.begin(), keys.end(), [&](const HlsKey& existing) {
      return existing.method == out.method && existing.uri == out.uri &&
             existing.key_format == out.key_format;
    });
    if (!seen) keys.push_back(std::move(out));
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
