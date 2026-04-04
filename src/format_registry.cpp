#include <openmedia/format_registry.hpp>
#include <formats.hpp>

namespace openmedia {

FormatRegistry::FormatRegistry() = default;
FormatRegistry::~FormatRegistry() = default;

auto FormatRegistry::registerFormat(const FormatDescriptor* descriptor, bool replace) noexcept -> bool {
  if (!descriptor || descriptor->container_id == OM_CONTAINER_NONE) return false;
  auto& place = format_table[descriptor->container_id];
  if (place != nullptr && !replace) return false;
  place = descriptor;
  return true;
}

auto FormatRegistry::getFormat(OMContainerId format_id) const noexcept -> const FormatDescriptor* {
  auto it = format_table.find(format_id);
  return (it != format_table.end()) ? it->second : nullptr;
}

auto FormatRegistry::getAllFormats() const -> std::vector<const FormatDescriptor*> {
  std::vector<const FormatDescriptor*> result;
  result.reserve(format_table.size());
  for (const auto& [id, descriptor] : format_table) {
    result.push_back(descriptor);
  }
  return result;
}

auto FormatRegistry::createDecoder(OMContainerId format_id) const noexcept -> std::unique_ptr<Demuxer> {
  auto [begin, end] = format_table.equal_range(format_id);
  for (auto it = begin; it != end; ++it) {
    const auto* descriptor = it->second;
    if (descriptor->isDemuxing()) {
      return descriptor->demuxer_factory();
    }
  }
  return {};
}

auto FormatRegistry::createEncoder(OMContainerId format_id) const noexcept -> std::unique_ptr<Muxer> {
  auto [begin, end] = format_table.equal_range(format_id);
  for (auto it = begin; it != end; ++it) {
    const auto* descriptor = it->second;
    if (descriptor->isMuxing()) {
      return descriptor->muxer_factory();
    }
  }
  return {};
}

auto FormatRegistry::hasFormat(OMContainerId format_id) const noexcept -> bool {
  return format_table.contains(format_id);
}

auto FormatRegistry::hasDemuxer(OMContainerId format_id) const noexcept -> bool {
  auto [begin, end] = format_table.equal_range(format_id);
  return std::any_of(begin, end, [](const auto& pair) {
    return pair.second->isDemuxing();
  });
}

auto FormatRegistry::hasMuxer(OMContainerId format_id) const noexcept -> bool {
  auto [begin, end] = format_table.equal_range(format_id);
  return std::any_of(begin, end, [](const auto& pair) {
    return pair.second->isMuxing();
  });
}

void registerBuiltInFormats(FormatRegistry* registry) noexcept {
  if (!registry) return;

  // Container formats
  registry->registerFormat(&FORMAT_BMFF);
#if defined(OPENMEDIA_MATROSKA)
  registry->registerFormat(&FORMAT_MATROSKA);
#endif
#if defined(OPENMEDIA_AVFORMAT)
  registry->registerFormat(&FORMAT_FFMPEG_BMFF);
  registry->registerFormat(&FORMAT_FFMPEG_MATROSKA);
#endif

  // Audio formats
  registry->registerFormat(&FORMAT_MP3);
  registry->registerFormat(&FORMAT_WAV);
  registry->registerFormat(&FORMAT_FLAC);
  registry->registerFormat(&FORMAT_OGG);

  // Image formats
  registry->registerFormat(&FORMAT_PNG);
  registry->registerFormat(&FORMAT_JPEG);
  registry->registerFormat(&FORMAT_WEBP);
  registry->registerFormat(&FORMAT_GIF);
  registry->registerFormat(&FORMAT_TGA);
  registry->registerFormat(&FORMAT_BMP);
  registry->registerFormat(&FORMAT_TIFF);
}

} // namespace openmedia
