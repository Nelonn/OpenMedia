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

// A playlist measures its segments in seconds, written as decimals. Milliseconds
// are the unit those convert into without losing anything a playlist can express,
// and are only ever used for addressing segments -- the timestamps handed to the
// caller come from the media itself, in whatever timescale its header declares.
constexpr uint32_t PLAYLIST_TIMESCALE = 1000;

// Descriptive fields a variant contributes about the playlist it points at. A
// rendition's own playlist has none of them, which is why they are separate from
// the playlist itself.
struct IndexDescription {
  std::string id;
  std::string codecs;
  uint32_t bandwidth = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

// An HLS media playlist, answering the questions the segment demuxer asks.
//
// A playlist is already the explicit list of segments that DASH has to compute
// from a template, so most of this is arithmetic over durations. The one thing it
// has to build is the running start time of each segment, since a playlist states
// only how long each one lasts.
class HlsSegmentIndex final : public SegmentIndex {
  // Shared, not copied. A four-hour playlist is a couple of thousand segments, each
  // holding its own URL and a repeat of whichever initialization section and key were
  // in force, and every variant would otherwise get a second copy of all of it.
  std::shared_ptr<const hls::MediaPlaylist> playlist_;
  IndexDescription description_;
  // Start time of each segment in milliseconds, with one extra entry for the end,
  // so a binary search over it answers ordinalForTime directly.
  std::vector<int64_t> starts_;
  size_t usable_ = 0;

public:
  HlsSegmentIndex(std::shared_ptr<const hls::MediaPlaylist> playlist,
                  IndexDescription description)
      : playlist_(std::move(playlist)), description_(std::move(description)) {
    const std::vector<hls::MediaSegment>& segments = playlist_->segments;

    // `#EXT-X-MAP` may be replaced part-way through a playlist, which changes the
    // parameter sets the segments after it decode against. A reader holds one
    // initialization segment at a time, so the run addressed here stops where the
    // header changes rather than handing over segments that would decode as noise.
    const std::string& init = segments.empty() ? std::string() : segments.front().init_url;
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
    // What this playlist listed, even when it is live. A growing playlist grows in
    // a later copy of itself, and fetching that copy is the caller's to do; the
    // stream ends where this copy's knowledge ends.
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
    // The first segment starting after the target, stepped back one: that is the
    // segment the target falls inside.
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
  auto hasInit() const -> bool { return initSegment().has_value(); }
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
  // Keyed by the URL the playlist was fetched from, which is also what a variant
  // or a rendition names it by.
  std::map<std::string, std::shared_ptr<const hls::MediaPlaylist>> media_;
  // The order media playlists arrived in, for the case where there is no master
  // and the first one added is simply the stream.
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
      // A lone media playlist is the whole stream; nothing further is wanted, and
      // nothing at all can be said before the first one arrives.
      return wanted;
    }
    for (const std::string& url : requiredUrls()) {
      if (!media_.contains(url)) wanted.push_back(url);
    }
    return wanted;
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
  // The variant whose media playlist is the one to play, honouring the caps. The
  // variants are sorted best first, so the first that fits is the best that fits.
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

  // Renditions that go with the chosen variant and are worth reading: those with a
  // playlist of their own, in the groups the variant draws from.
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

      // One rendition per group: the members of a group are the same track in
      // different languages, and reading all of them would download every language
      // to play one.
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

  auto requiredUrls() const -> std::vector<std::string> {
    std::vector<std::string> urls;
    if (const hls::Variant* variant = chosenVariant()) urls.push_back(variant->url);
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
    if (!index || !index->hasInit()) return source;

    source.duration_ns = index->totalDurationNs();
    source.live = index->isLive();

    SegmentTrack track;
    // Nothing in a media playlist says whether it carries video, audio or both.
    // The initialization segment does, and the reader reports it once parsed.
    track.type = OM_MEDIA_NONE;
    track.alternatives.push_back(std::move(index));
    source.tracks.push_back(std::move(track));
    return source;
  }

  auto sourceFromMaster() const -> SegmentSource {
    SegmentSource source;
    const hls::Variant* chosen = chosenVariant();
    if (!chosen) return source;

    // The variants form one track's alternatives. Every variant whose playlist is
    // in hand can be switched to; the rest are not offered, because switching to
    // one would mean fetching a playlist mid-read, which this demuxer does not do.
    SegmentTrack variants;
    variants.type = OM_MEDIA_NONE; // the initialization segment knows
    size_t chosen_at = 0;
    for (const hls::Variant& variant : master_->variants) {
      if (variant.iframes_only) continue;
      if (!media_.contains(variant.url)) continue;

      IndexDescription description;
      description.id = variant.url;
      description.codecs = variant.codecs;
      description.bandwidth = variant.bandwidth;
      description.width = variant.resolution.width;
      description.height = variant.resolution.height;

      auto index = makeIndex(variant.url, std::move(description));
      if (!index || !index->hasInit()) continue;
      if (&variant == chosen) chosen_at = variants.alternatives.size();
      if (variants.alternatives.empty()) {
        source.duration_ns = index->totalDurationNs();
        source.live = index->isLive();
      }
      variants.alternatives.push_back(std::move(index));
    }
    if (variants.alternatives.empty()) return source;

    // The segment demuxer picks the first alternative that fits its caps, and the
    // caps have already been applied here against what the master said. Putting the
    // choice first makes the two agree.
    if (chosen_at != 0) {
      std::swap(variants.alternatives[0], variants.alternatives[chosen_at]);
    }
    source.tracks.push_back(std::move(variants));

    for (const hls::Rendition* rendition : chosenRenditions()) {
      IndexDescription description;
      description.id = rendition->url;
      auto index = makeIndex(rendition->url, std::move(description));
      if (!index || !index->hasInit()) continue;

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
// container: the stream it is given is the playlist, and everything it points at is
// read off the same filesystem. A caller fetching from anywhere else goes through
// createHlsPlaylists() and says how.
class HlsFormatDemuxer final : public Demuxer {
  std::unique_ptr<Demuxer> inner_;
  std::vector<Track> no_tracks_;
  Dictionary no_metadata_;

public:
  auto open(std::unique_ptr<InputStream> input) -> OMError override {
    close();
    if (!input || !input->isValid()) return OM_IO_INVALID_STREAM;

    // A playlist names its segments -- and, for a master playlist, its media
    // playlists -- relative to itself, and this wrapper reads them off the filesystem
    // beside it. A stream that cannot say where it came from leaves no "beside" to
    // read from: resolving against the working directory instead would open whatever
    // files happen to sit there. Such a playlist is opened through
    // openHlsPlaylist(), which takes its URL as an argument rather than guessing.
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

auto openHlsPlaylist(std::span<const uint8_t> document, std::string_view url,
                     const StreamOptions& options)
    -> Result<std::unique_ptr<SegmentedDemuxer>, OMError> {
  if (document.empty()) return Err(OM_COMMON_INVALID_ARGUMENT);

  auto playlists = createHlsPlaylists(options);
  if (const OMError error = playlists->add(document, url); error != OM_SUCCESS) {
    return Err(error);
  }

  // A master playlist needs a second round. With a fetch function the caller has
  // said it is content to block, so it happens here; without one, the round trip is
  // the caller's and createHlsPlaylists() is the way to make it.
  const std::vector<std::string> wanted = playlists->needed();
  if (!wanted.empty()) {
    if (!options.fetch) return Err(OM_IO_NOT_ENOUGH_DATA);
    for (const std::string& next : wanted) {
      auto bytes = options.fetch(SegmentRequest {next, 0, -1});
      if (bytes.isErr()) return Err(std::move(bytes).unwrapErr());
      const std::vector<uint8_t> fetched = std::move(bytes).unwrap();
      if (const OMError error = playlists->add(fetched, next); error != OM_SUCCESS) {
        return Err(error);
      }
    }
  }
  return playlists->open();
}

const FormatDescriptor FORMAT_HLS = {
    .container_id = OM_CONTAINER_HLS,
    .name = "hls",
    .long_name = "HTTP Live Streaming",
    .demuxer_factory = [] { return std::make_unique<HlsFormatDemuxer>(); },
    .muxer_factory = {},
};

} // namespace openmedia
