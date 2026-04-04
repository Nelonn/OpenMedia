#include <algorithm>
#include <codecs.hpp>
#include <openmedia/codec_registry.hpp>

namespace openmedia {

CodecRegistry::CodecRegistry() = default;
CodecRegistry::~CodecRegistry() = default;

auto CodecRegistry::registerCodec(const CodecDescriptor* descriptor) noexcept -> bool {
  if (!descriptor ||
      descriptor->codec_id == OM_CODEC_NONE ||
      descriptor->type == OM_MEDIA_NONE ||
      descriptor->name.empty() ||
      name_table.contains(descriptor->name)) return false;
  codec_table.emplace(descriptor->codec_id, descriptor);
  name_table.emplace(descriptor->name, descriptor);
  return true;
}

auto CodecRegistry::getCodec(OMCodecId codec_id) const noexcept -> const CodecDescriptor* {
  auto it = codec_table.find(codec_id);
  return (it != codec_table.end()) ? it->second : nullptr;
}

auto CodecRegistry::getCodecByName(std::string_view name) const noexcept -> const CodecDescriptor* {
  auto it = name_table.find(name);
  return (it != name_table.end()) ? it->second : nullptr;
}

auto CodecRegistry::getAllCodecs() const -> std::vector<const CodecDescriptor*> {
  std::vector<const CodecDescriptor*> result;
  result.reserve(codec_table.size());
  for (const auto& [id, descriptor] : codec_table) {
    result.push_back(descriptor);
  }
  return result;
}

auto CodecRegistry::getCodecsByType(OMMediaType type) const -> std::vector<const CodecDescriptor*> {
  std::vector<const CodecDescriptor*> result;
  for (const auto& [id, descriptor] : codec_table) {
    if (descriptor->type == type) {
      result.push_back(descriptor);
    }
  }
  return result;
}

auto CodecRegistry::getCodecsByCodecId(OMCodecId codec_id) const -> std::vector<const CodecDescriptor*> {
  std::vector<const CodecDescriptor*> result;
  auto [begin, end] = codec_table.equal_range(codec_id);
  for (auto it = begin; it != end; ++it) {
    result.push_back(it->second);
  }
  return result;
}

auto CodecRegistry::createDecoder(OMCodecId codec_id) const noexcept -> std::unique_ptr<Decoder> {
  auto [begin, end] = codec_table.equal_range(codec_id);
  for (auto it = begin; it != end; ++it) {
    const auto* descriptor = it->second;
    if (descriptor->isDecoding()) {
      return descriptor->decoder_factory();
    }
  }
  return {};
}

auto CodecRegistry::createEncoder(OMCodecId codec_id) const noexcept -> std::unique_ptr<Encoder> {
  auto [begin, end] = codec_table.equal_range(codec_id);
  for (auto it = begin; it != end; ++it) {
    const auto* descriptor = it->second;
    if (descriptor->isEncoding()) {
      return descriptor->encoder_factory();
    }
  }
  return {};
}

auto CodecRegistry::hasCodec(OMCodecId codec_id) const noexcept -> bool {
  return codec_table.contains(codec_id);
}

auto CodecRegistry::hasDecoder(OMCodecId codec_id) const noexcept -> bool {
  auto [begin, end] = codec_table.equal_range(codec_id);
  return std::any_of(begin, end, [](const auto& pair) {
    return pair.second->isDecoding();
  });
}

auto CodecRegistry::hasEncoder(OMCodecId codec_id) const noexcept -> bool {
  auto [begin, end] = codec_table.equal_range(codec_id);
  return std::any_of(begin, end, [](const auto& pair) {
    return pair.second->isEncoding();
  });
}

void registerBuiltInCodecs(CodecRegistry* registry) noexcept {
  if (!registry) return;

  // Audio codecs
  registry->registerCodec(&CODEC_PCM_S16LE);
  registry->registerCodec(&CODEC_PCM_F32LE);
  registry->registerCodec(&CODEC_ALAC);
#if defined(OPENMEDIA_FDK_AAC)
  registry->registerCodec(&CODEC_FDK_AAC);
#endif
  registry->registerCodec(&CODEC_MP3);
  registry->registerCodec(&CODEC_FLAC);
  registry->registerCodec(&CODEC_VORBIS);
  registry->registerCodec(&CODEC_OPUS);
#if defined(_WIN32)
  registry->registerCodec(&CODEC_WMF_AAC);
  registry->registerCodec(&CODEC_WMF_MP3);
#endif

  // Video - Software
#if defined(OPENMEDIA_DAV1D)
  registry->registerCodec(&CODEC_DAV1D);
#endif
#if defined(OPENMEDIA_OPENH264)
  registry->registerCodec(&CODEC_OPENH264);
#endif
#if defined(OPENMEDIA_VVDEC)
  registry->registerCodec(&CODEC_VVDEC);
#endif
#if defined(OPENMEDIA_XEVD)
  registry->registerCodec(&CODEC_XEVD);
#endif
#if defined(OPENMEDIA_XEVE)
  registry->registerCodec(&CODEC_XEVE);
#endif

#if defined(_WIN32)
  //registry->registerCodec(&CODEC_WMF_VIDEO_H264);
  //registry->registerCodec(&CODEC_WMF_VIDEO_H265);
  //registry->registerCodec(&CODEC_WMF_VIDEO_AV1);
#endif

#if defined(OPENMEDIA_AVCODEC)
  registry->registerCodec(&CODEC_FFMPEG_H264);
  registry->registerCodec(&CODEC_FFMPEG_H265);
  registry->registerCodec(&CODEC_FFMPEG_H266);
  registry->registerCodec(&CODEC_FFMPEG_EVC);
  registry->registerCodec(&CODEC_FFMPEG_VP8);
  registry->registerCodec(&CODEC_FFMPEG_VP9);
  registry->registerCodec(&CODEC_FFMPEG_AV1);
#endif

  // Video - DirectX11
#if defined(OPENMEDIA_DX11_VIDEO)
  registry->registerCodec(&CODEC_DX11_H264);
#endif

  // Video - DirectX12
#if defined(OPENMEDIA_DX12_VIDEO)
  //registry->registerCodec(&CODEC_DX12_H264);
  //registry->registerCodec(&CODEC_DX12_H265);
  //registry->registerCodec(&CODEC_DX12_VP9);
  //registry->registerCodec(&CODEC_DX12_AV1);
#endif

  // Video - AMD AMF
  //registry->registerCodec(&CODEC_AMF_H264);
  //registry->registerCodec(&CODEC_AMF_H265);
  //registry->registerCodec(&CODEC_AMF_AV1);

  // Image codecs
  registry->registerCodec(&CODEC_PNG);
  registry->registerCodec(&CODEC_JPEG);
  registry->registerCodec(&CODEC_WEBP);
  registry->registerCodec(&CODEC_GIF);
  registry->registerCodec(&CODEC_TGA);
  registry->registerCodec(&CODEC_BMP);
  registry->registerCodec(&CODEC_TIFF);
}

} // namespace openmedia
