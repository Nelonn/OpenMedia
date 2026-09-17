#pragma once

#include <openmedia/video.hpp>
#include <cstdint>
#include <span>
#include <vector>

namespace openmedia::hdr_sei {

// HDR10 static metadata lives in SEI messages inside the elementary stream:
// mastering_display_colour_volume (payload type 137, ST.2086) and
// content_light_level_info (payload type 144, CEA-861.3). Hardware decoders
// hand the bitstream straight to the GPU without telling us what was in it, so
// the messages have to be picked out here or the metadata is lost.
//
// Both H.264 and H.265 use the same payload layout; only the NAL header differs.

inline constexpr uint8_t SEI_MASTERING_DISPLAY_COLOUR_VOLUME = 137;
inline constexpr uint8_t SEI_CONTENT_LIGHT_LEVEL_INFO = 144;

namespace detail {

inline auto readBE16(const uint8_t* p) -> uint16_t {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

inline auto readBE32(const uint8_t* p) -> uint32_t {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// Strips emulation prevention bytes so the payload can be read directly.
inline void toRbsp(std::span<const uint8_t> nal, std::vector<uint8_t>& out) {
  out.clear();
  out.reserve(nal.size());
  uint32_t zeros = 0;
  for (size_t i = 0; i < nal.size(); ++i) {
    const uint8_t byte = nal[i];
    if (zeros >= 2 && byte == 0x03) {
      zeros = 0;
      continue; // emulation prevention byte
    }
    out.push_back(byte);
    zeros = (byte == 0) ? zeros + 1 : 0;
  }
}

// One SEI NAL holds a list of messages, each prefixed by a type and a size that
// are coded as a run of 0xFF bytes followed by the remainder.
inline void parseMessages(std::span<const uint8_t> rbsp,
                          OMMasteringDisplayMetadata& mastering,
                          OMContentLightLevel& light) {
  size_t pos = 0;
  while (pos < rbsp.size()) {
    uint32_t type = 0;
    while (pos < rbsp.size() && rbsp[pos] == 0xFF) {
      type += 255;
      ++pos;
    }
    if (pos >= rbsp.size()) return;
    type += rbsp[pos++];

    uint32_t size = 0;
    while (pos < rbsp.size() && rbsp[pos] == 0xFF) {
      size += 255;
      ++pos;
    }
    if (pos >= rbsp.size()) return;
    size += rbsp[pos++];

    if (pos + size > rbsp.size()) return;
    const uint8_t* payload = rbsp.data() + pos;

    if (type == SEI_MASTERING_DISPLAY_COLOUR_VOLUME && size >= 24) {
      // Primaries are stored in the order the SEI uses: green, blue, red.
      for (uint32_t i = 0; i < 3; ++i) {
        mastering.display_primaries[i][0] = readBE16(payload + i * 4);
        mastering.display_primaries[i][1] = readBE16(payload + i * 4 + 2);
      }
      mastering.white_point[0] = readBE16(payload + 12);
      mastering.white_point[1] = readBE16(payload + 14);
      mastering.max_display_mastering_luminance = readBE32(payload + 16);
      mastering.min_display_mastering_luminance = readBE32(payload + 20);
      mastering.has_value = true;
    } else if (type == SEI_CONTENT_LIGHT_LEVEL_INFO && size >= 4) {
      light.max_content_light_level = readBE16(payload);
      light.max_pic_average_light_level = readBE16(payload + 2);
      light.has_value = true;
    }

    pos += size;
    // A trailing rbsp_stop_one_bit ends the list.
    if (pos < rbsp.size() && rbsp[pos] == 0x80) return;
  }
}

} // namespace detail

// Scans an Annex-B access unit for HDR10 SEI and updates the two structures in
// place. Values already present are left alone when the frame carries none, so
// metadata sent once on a keyframe keeps applying.
inline void parseAnnexB(std::span<const uint8_t> data, bool hevc,
                        OMMasteringDisplayMetadata& mastering,
                        OMContentLightLevel& light) {
  if (data.size() < 4) return;

  std::vector<uint8_t> rbsp;
  size_t pos = 0;
  while (pos + 3 <= data.size()) {
    if (data[pos] != 0 || data[pos + 1] != 0 || data[pos + 2] != 1) {
      ++pos;
      continue;
    }
    const size_t nal_start = pos + 3;
    if (nal_start >= data.size()) return;

    // Find the next start code to bound this NAL.
    size_t next = data.size();
    for (size_t i = nal_start; i + 3 <= data.size(); ++i) {
      if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
        next = i;
        break;
      }
    }
    size_t nal_end = next;
    while (nal_end > nal_start && data[nal_end - 1] == 0) --nal_end;

    const uint8_t header = data[nal_start];
    const bool is_sei = hevc ? (((header >> 1) & 0x3F) == 39 || ((header >> 1) & 0x3F) == 40)
                             : ((header & 0x1F) == 6);
    if (is_sei) {
      const size_t header_bytes = hevc ? 2u : 1u;
      if (nal_start + header_bytes < nal_end) {
        detail::toRbsp(data.subspan(nal_start + header_bytes, nal_end - nal_start - header_bytes), rbsp);
        detail::parseMessages(rbsp, mastering, light);
      }
    }
    pos = next;
  }
}

} // namespace openmedia::hdr_sei
