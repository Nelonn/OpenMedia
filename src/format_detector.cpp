#include <util/io_util.hpp>
#include <openmedia/format_detector.hpp>

namespace openmedia {

FormatDetector::FormatDetector() = default;

auto FormatDetector::detect(std::span<const uint8_t> data) const -> DetectedFormat {
  if (data.size() < 4) return DetectedFormat::unknown();
  for (const auto& detector : detectors_) {
    DetectedFormat format = detector(data);
    if (!format.isUnknown()) return format;
  }
  return DetectedFormat::unknown();
}

void FormatDetector::addDetector(FormatDetectFn detector) {
  detectors_.push_back(std::move(detector));
}

void FormatDetector::addStandardContainers() {
  addDetector([](std::span<const uint8_t> data) -> DetectedFormat {
    const uint32_t v0 = load_u32(data.data());
    if (v0 == magic_u32(0x1A, 0x45, 0xDF, 0xA3)) return DetectedFormat::fromContainer(OM_CONTAINER_MKV);
    if (data.size() >= 8 && load_u32(data.data() + 4) == magic_u32('f', 't', 'y', 'p')) {
      const uint32_t brand = data.size() >= 12 ? load_u32(data.data() + 8) : 0;
      if (brand == magic_u32('w', 'e', 'b', 'm')) return DetectedFormat::fromContainer(OM_CONTAINER_WEBM);
      return DetectedFormat::fromContainer(OM_CONTAINER_MP4);
    }
    if (v0 == magic_u32('O', 'g', 'g', 'S')) return DetectedFormat::fromContainer(OM_CONTAINER_OGG);
    if (v0 == magic_u32('R', 'I', 'F', 'F') && data.size() >= 12) {
      uint32_t v8 = load_u32(data.data() + 8);
      if (v8 == magic_u32('A', 'V', 'I', ' ')) return DetectedFormat::fromContainer(OM_CONTAINER_AVI);
    }
    if (v0 == magic_u32(0x00, 0x00, 0x01, 0xBA)) return DetectedFormat::fromContainer(OM_CONTAINER_MPEG_PS);
    if (v0 == 0x47 && data.size() >= 189 && data[188] == 0x47) return DetectedFormat::fromContainer(OM_CONTAINER_MPEG_TS);
    if (v0 == magic_u32(0x30, 0x26, 0xB2, 0x75)) return DetectedFormat::fromContainer(OM_CONTAINER_ASF);
    if (v0 == magic_u32('F', 'L', 'V', 0x01)) return DetectedFormat::fromContainer(OM_CONTAINER_FLV);
    if (v0 == magic_u32('N', 'U', 'T', 'S')) return DetectedFormat::fromContainer(OM_CONTAINER_NUT);
    return DetectedFormat::unknown();
  });
}

static auto skipId3v2(std::span<const uint8_t> data) -> size_t {
  if (data.size() < 10) return 0;
  if (data[0] != 'I' || data[1] != 'D' || data[2] != '3') return 0;
  if (data[3] == 0xFF || data[4] == 0xFF) return 0;
  if (data[6] & 0x80 || data[7] & 0x80 || data[8] & 0x80 || data[9] & 0x80) return 0;
  size_t tag_size = (static_cast<size_t>(data[6] & 0x7F) << 21) |
                    (static_cast<size_t>(data[7] & 0x7F) << 14) |
                    (static_cast<size_t>(data[8] & 0x7F) << 7) |
                    static_cast<size_t>(data[9] & 0x7F);
  tag_size += 10;
  if (data[5] & 0x10) tag_size += 10;
  return tag_size;
}

void FormatDetector::addStandardAudio() {
  addDetector([](std::span<const uint8_t> data) -> DetectedFormat {
    const uint32_t v0 = load_u32(data.data());
    if (v0 == magic_u32('f', 'L', 'a', 'C')) return DetectedFormat::fromContainer(OM_CONTAINER_FLAC);
    if (v0 == magic_u32('R', 'I', 'F', 'F') && data.size() >= 12 && load_u32(data.data() + 8) == magic_u32('W', 'A', 'V', 'E')) return DetectedFormat::fromContainer(OM_CONTAINER_WAV);
    if ((v0 & 0x00FFFFFF) == magic_u32('I', 'D', '3', 0)) {
      size_t skip = skipId3v2(data);
      if (skip > 0 && skip + 4 <= data.size()) {
        uint32_t v_after = load_u32(data.data() + skip);
        if (v_after == magic_u32('f', 'L', 'a', 'C')) return DetectedFormat::fromContainer(OM_CONTAINER_FLAC);
      }
      return DetectedFormat::fromContainer(OM_CONTAINER_MP3);
    }
    if ((v0 & 0xFFFE) == 0xFFFA) return DetectedFormat::fromContainer(OM_CONTAINER_MP3);
    if (data.size() >= 8 && load_u32(data.data() + 4) == magic_u32('f', 't', 'y', 'p')) {
      uint32_t v8 = data.size() >= 12 ? load_u32(data.data() + 8) : 0;
      if (v8 == magic_u32('M', '4', 'A', ' ')) return DetectedFormat::fromContainer(OM_CONTAINER_M4A);
    }
    if (v0 == magic_u32('M', 'a', 'c', ' ')) return DetectedFormat::fromContainer(OM_CONTAINER_APE);
    return DetectedFormat::unknown();
  });
}

void FormatDetector::addStandardImages() {
  addDetector([](std::span<const uint8_t> data) -> DetectedFormat {
    const uint32_t v0 = load_u32(data.data());
    if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) return DetectedFormat::fromContainer(OM_CONTAINER_JPEG);
    if (v0 == magic_u32(0x89, 'P', 'N', 'G')) return DetectedFormat::fromContainer(OM_CONTAINER_PNG);
    if (v0 == magic_u32('R', 'I', 'F', 'F') && data.size() >= 12 && load_u32(data.data() + 8) == magic_u32('W', 'E', 'B', 'P')) return DetectedFormat::fromContainer(OM_CONTAINER_WEBP);
    if ((v0 & 0xFFFF) == magic_u32('B', 'M', 0, 0)) return DetectedFormat::fromContainer(OM_CONTAINER_BMP);
    if (v0 == magic_u32('G', 'I', 'F', '8')) return DetectedFormat::fromContainer(OM_CONTAINER_GIF);
    if (v0 == magic_u32(0x49, 0x49, 0x2A, 0x00) || v0 == magic_u32(0x4D, 0x4D, 0x00, 0x2A)) return DetectedFormat::fromContainer(OM_CONTAINER_TIFF);
    if (v0 == magic_u32(0x00, 0x00, 0x01, 0x00)) return DetectedFormat::fromContainer(OM_CONTAINER_ICO);
    if (v0 == magic_u32(0x76, 0x2F, 0x31, 0x01)) return DetectedFormat::fromContainer(OM_CONTAINER_EXR);
    if (v0 == magic_u32('i', 'c', 'n', 's')) return DetectedFormat::fromContainer(OM_CONTAINER_ICNS);
    if (data.size() >= 12 && load_u32(data.data() + 4) == magic_u32('f', 't', 'y', 'p')) {
      uint32_t brand = load_u32(data.data() + 8);
      if (brand == magic_u32('a', 'v', 'i', 'f')) return DetectedFormat::fromContainer(OM_CONTAINER_HEIF);
      if (brand == magic_u32('h', 'e', 'i', 'c') || brand == magic_u32('h', 'e', 'v', 'c')) return DetectedFormat::fromContainer(OM_CONTAINER_HEIF);
    }
    return DetectedFormat::unknown();
  });
}

void FormatDetector::addStandardVideoBitstreams() {
  addDetector([](std::span<const uint8_t> data) -> DetectedFormat {
    const uint32_t v0 = load_u32(data.data());
    if (v0 == magic_u32(0x00, 0x00, 0x00, 0x01) || (v0 & 0x00FFFFFF) == magic_u32(0x00, 0x00, 0x01, 0)) {
      uint8_t nalu = (v0 == 0x01000000) ? data[4] : data[3];
      if ((nalu & 0x1F) == 7) return DetectedFormat::fromCodec(OM_CODEC_H264);
      if (((nalu >> 1) & 0x3F) == 32) return DetectedFormat::fromCodec(OM_CODEC_H265);
    }
    return DetectedFormat::unknown();
  });
}

void FormatDetector::addAllStandard() {
  addStandardContainers();
  addStandardAudio();
  addStandardImages();
  addStandardVideoBitstreams();
}

} // namespace openmedia
