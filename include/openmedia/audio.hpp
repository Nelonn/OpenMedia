#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <openmedia/buffer.hpp>

OM_ENUM(OMSampleFormat, uint8_t) {
  OM_SAMPLE_UNKNOWN = 0,
  OM_SAMPLE_U8,
  OM_SAMPLE_S16,
  OM_SAMPLE_S32,
  OM_SAMPLE_F32,
  OM_SAMPLE_F64,
};

namespace openmedia {

constexpr auto getBytesPerSample(OMSampleFormat sample_format) noexcept -> size_t {
  switch (sample_format) {
    case OM_SAMPLE_UNKNOWN: return 0;
    case OM_SAMPLE_U8: return 1;
    case OM_SAMPLE_S16: return 2;
    case OM_SAMPLE_S32:
    case OM_SAMPLE_F32: return 4;
    case OM_SAMPLE_F64: return 8;
    default: return 0;
  }
}

struct OPENMEDIA_ABI AudioFormat {
  OMSampleFormat sample_format = OM_SAMPLE_UNKNOWN;
  uint8_t bits_per_sample = 0;
  uint32_t sample_rate = 0;
  uint32_t channels = 0;
  bool planar = false;
};

constexpr auto operator==(const AudioFormat& lhs, const AudioFormat& rhs) noexcept -> bool {
  return lhs.sample_format == rhs.sample_format &&
         lhs.sample_rate == rhs.sample_rate &&
         lhs.channels == rhs.channels &&
         lhs.planar == rhs.planar;
}

struct OPENMEDIA_ABI AudioSamples {
  AudioFormat format;
  uint32_t nb_samples = 0; // samples per channel
  std::shared_ptr<Buffer> buffer;
  PlaneSpan<8> planes;

  AudioSamples() = default;

  AudioSamples(const AudioFormat& fmt, uint32_t samples)
      : format(fmt), nb_samples(samples) {
    allocate();
  }

  void allocate() {
    size_t bps = getBytesPerSample(format.sample_format);
    size_t total_size = static_cast<size_t>(nb_samples) * format.channels * bps;

    buffer = BufferPool::getInstance().get(total_size);

    uint8_t* raw_ptr = buffer->bytes().data();
    planes.count = 0;
    if (format.planar) {
      size_t plane_samples = static_cast<size_t>(nb_samples) * bps;
      for (uint32_t i = 0; i < format.channels && i < 8; ++i) {
        planes.setData(i, raw_ptr + i * plane_samples, static_cast<uint32_t>(plane_samples));
      }
    } else {
      size_t plane_size = static_cast<size_t>(nb_samples) * bps;
      for (uint32_t i = 0; i < format.channels && i < 8; ++i) {
        planes.setData(i, raw_ptr + i * plane_size, static_cast<uint32_t>(plane_size));
      }
    }
  }

  void unref() {
    buffer.reset();
    planes = PlaneSpan<8> {};
    nb_samples = 0;
  }
};

} // namespace openmedia
