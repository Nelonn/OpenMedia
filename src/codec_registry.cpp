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
#if defined(__APPLE__)
  registry->registerCodec(&CODEC_AUDIO_TOOLBOX_ALAC);
#else
  registry->registerCodec(&CODEC_ALAC);
#endif
#if defined(OPENMEDIA_FDK_AAC)
  registry->registerCodec(&CODEC_FDK_AAC);
#endif
#if defined(__APPLE__)
  registry->registerCodec(&CODEC_AUDIO_TOOLBOX_MP3);
#else
  registry->registerCodec(&CODEC_MP3);
#endif
  registry->registerCodec(&CODEC_FLAC);
  registry->registerCodec(&CODEC_VORBIS);
#if defined(OPENMEDIA_OPUS)
  registry->registerCodec(&CODEC_OPUS);
#endif
#if defined(_WIN32)
  registry->registerCodec(&CODEC_WMF_AAC);
  registry->registerCodec(&CODEC_WMF_MP3);
#endif
#if defined(__APPLE__)
  registry->registerCodec(&CODEC_AUDIO_TOOLBOX_AAC);
  registry->registerCodec(&CODEC_AUDIO_TOOLBOX_AC3);
  registry->registerCodec(&CODEC_AUDIO_TOOLBOX_EAC3);
#endif

  // Video - Software
#if defined(OPENMEDIA_DAV1D)
  registry->registerCodec(&CODEC_DAV1D);
#endif
#if defined(OPENMEDIA_VPX)
  registry->registerCodec(&CODEC_VP8);
  registry->registerCodec(&CODEC_VP9);
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

#if defined(__APPLE__)
  registry->registerCodec(&CODEC_VIDEOTOOLBOX_H263);
  registry->registerCodec(&CODEC_VIDEOTOOLBOX_H264);
  registry->registerCodec(&CODEC_VIDEOTOOLBOX_H265);
  registry->registerCodec(&CODEC_VIDEOTOOLBOX_MPEG2);
  registry->registerCodec(&CODEC_VIDEOTOOLBOX_MPEG4);
  // CoreMedia defines a VP9 codec type, but VideoToolbox cannot create a VP9
  // decompression session on macOS. Keep VP9 on software/FFmpeg backends.
  registry->registerCodec(&CODEC_VIDEOTOOLBOX_AV1);
  registry->registerCodec(&CODEC_VIDEOTOOLBOX_PRORES);
#endif

  // Video - DirectX11
#if defined(OPENMEDIA_DX11_VIDEO)
  registry->registerCodec(&CODEC_DX11_H264);
  registry->registerCodec(&CODEC_DX11_H265);
  registry->registerCodec(&CODEC_DX11_ENC_H264);
#endif

  // Video - DirectX12
#if defined(OPENMEDIA_DX12_VIDEO)
  registry->registerCodec(&CODEC_DX12_H264);
  registry->registerCodec(&CODEC_DX12_H265);
  registry->registerCodec(&CODEC_DX12_VP9);
  registry->registerCodec(&CODEC_DX12_AV1);
  registry->registerCodec(&CODEC_DX12_ENC_H264);
  registry->registerCodec(&CODEC_DX12_ENC_H265);
#endif

  // Video - AMD AMF
#if defined(OPENMEDIA_AMF)
  registry->registerCodec(&CODEC_AMF_H264);
  registry->registerCodec(&CODEC_AMF_H265);
  registry->registerCodec(&CODEC_AMF_AV1);
  registry->registerCodec(&CODEC_AMF_VP9);
#endif

#if defined(OPENMEDIA_VAAPI)
  registry->registerCodec(&CODEC_VAAPI_H264);
  registry->registerCodec(&CODEC_VAAPI_H265);
  registry->registerCodec(&CODEC_VAAPI_VP9);
  registry->registerCodec(&CODEC_VAAPI_AV1);
#endif

#if defined(OPENMEDIA_VULKAN)
  registry->registerCodec(&CODEC_VULKAN_H264);
  registry->registerCodec(&CODEC_VULKAN_H265);
  //registry->registerCodec(&CODEC_VULKAN_AV1);
  //registry->registerCodec(&CODEC_VULKAN_VP9);
#endif

#if defined(OPENMEDIA_NVIDIA)
  registry->registerCodec(&CODEC_NVDEC_H264);
  registry->registerCodec(&CODEC_NVDEC_H265);
  registry->registerCodec(&CODEC_NVDEC_VP9);
  registry->registerCodec(&CODEC_NVDEC_AV1);
  registry->registerCodec(&CODEC_NVENC_H264);
  registry->registerCodec(&CODEC_NVENC_H265);
  registry->registerCodec(&CODEC_NVENC_AV1);
#endif

#if defined(__ANDROID__)
  registry->registerCodec(&CODEC_MEDIACODEC_H264);
  registry->registerCodec(&CODEC_MEDIACODEC_H265);
  registry->registerCodec(&CODEC_MEDIACODEC_VP8);
  registry->registerCodec(&CODEC_MEDIACODEC_VP9);
  registry->registerCodec(&CODEC_MEDIACODEC_AV1);
  registry->registerCodec(&CODEC_MEDIACODEC_AAC);
#endif

  // Image codecs
  registry->registerCodec(&CODEC_PNG);
  registry->registerCodec(&CODEC_JPEG);
  registry->registerCodec(&CODEC_WEBP);
  registry->registerCodec(&CODEC_GIF);
  registry->registerCodec(&CODEC_TGA);
  registry->registerCodec(&CODEC_BMP);
  registry->registerCodec(&CODEC_TIFF);

#if defined(OPENMEDIA_AVCODEC)
  registerFFmpegCodecs(registry);
#endif
}

} // namespace openmedia
