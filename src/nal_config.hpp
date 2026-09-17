#pragma once

#include <annexb.hpp>
#include <util/byte_reader.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace openmedia {

// Parameter sets carried by an ISO-BMFF decoder configuration record
// (avcC / hvcC / vvcC), rewritten as an Annex-B byte stream.
//
// MP4 keeps these records in the sample entry and Matroska in CodecPrivate, so
// both demuxers parse the same bytes; the parsers live here rather than in
// either one.
struct NalDecoderConfig {
  std::vector<uint8_t> annexb_extradata;
  uint8_t nal_length_size = 4;
  uint8_t profile_idc = 0;           // 0 when the record carries none
  bool constrained_baseline = false; // AVC only
};

// AVCDecoderConfigurationRecord, ISO/IEC 14496-15 §5.3.3.1.
inline auto parseAvcDecoderConfig(std::span<const uint8_t> body)
    -> std::optional<NalDecoderConfig> {
  if (body.size() < 7) return std::nullopt;
  ByteReader r(body);

  r.skip(1); // configurationVersion
  const uint8_t profile_idc = r.u8(); // AVCProfileIndication
  const uint8_t profile_compat = r.u8(); // profile_compatibility
  r.skip(1); // AVCLevelIndication

  NalDecoderConfig config;
  config.profile_idc = profile_idc;
  config.constrained_baseline = profile_idc == 66 && (profile_compat & 0x40u) != 0;
  config.nal_length_size = (r.u8() & 0x03u) + 1u;

  auto extract_nals = [&](uint8_t count) {
    for (uint8_t i = 0; i < count; ++i) {
      uint16_t nal_size = r.u16be();
      if (r.remaining() < nal_size) break;
      config.annexb_extradata.insert(config.annexb_extradata.end(),
                                     AnnexBFilter::START_CODE_LONG,
                                     AnnexBFilter::START_CODE_LONG + 4);
      const uint8_t* ptr = r.cur();
      config.annexb_extradata.insert(config.annexb_extradata.end(), ptr, ptr + nal_size);
      r.skip(nal_size);
    }
  };

  uint8_t num_sps = r.u8() & 0x1Fu;
  extract_nals(num_sps);
  if (r.remaining() > 0) {
    uint8_t num_pps = r.u8();
    extract_nals(num_pps);
  }

  return config;
}

// HEVCDecoderConfigurationRecord, ISO/IEC 14496-15 §8.3.3.1.
inline auto parseHevcDecoderConfig(std::span<const uint8_t> body)
    -> std::optional<NalDecoderConfig> {
  if (body.size() < 23) return std::nullopt;

  ByteReader r(body);

  r.skip(1); // configurationVersion
  const uint8_t ptl_byte = r.u8();
  const uint8_t profile_idc = ptl_byte & 0x1Fu; // general_profile_idc
  r.skip(4); // general_profile_compatibility_flags
  r.skip(6); // general_constraint_indicator_flags (48 bit)
  r.skip(1); // general_level_idc
  r.skip(2); // min_spatial_segmentation_idc
  r.skip(1); // parallelismType
  r.skip(1); // chromaFormat
  r.skip(1); // bitDepthLumaMinus8
  r.skip(1); // bitDepthChromaMinus8
  r.skip(2); // avgFrameRate

  NalDecoderConfig config;
  config.profile_idc = profile_idc;
  config.nal_length_size = (r.u8() & 0x03u) + 1u;

  const uint8_t num_arrays = r.u8();

  for (uint8_t i = 0; i < num_arrays; ++i) {
    if (r.remaining() < 3) break;

    r.skip(1);
    const uint16_t num_nalus = r.u16be();

    for (uint16_t j = 0; j < num_nalus; ++j) {
      if (r.remaining() < 2) break;
      const uint16_t nal_size = r.u16be();
      if (r.remaining() < nal_size) break;

      config.annexb_extradata.insert(config.annexb_extradata.end(),
                                     AnnexBFilter::START_CODE_LONG,
                                     AnnexBFilter::START_CODE_LONG + 4);
      const uint8_t* ptr = r.cur();
      config.annexb_extradata.insert(config.annexb_extradata.end(), ptr, ptr + nal_size);
      r.skip(nal_size);
    }
  }

  if (config.annexb_extradata.empty()) return std::nullopt;

  return config;
}

// VvcDecoderConfigurationRecord, ISO/IEC 14496-15:2022 §11.2.4.2.
inline auto parseVvcDecoderConfig(std::span<const uint8_t> body)
    -> std::optional<NalDecoderConfig> {
  if (body.size() < 2) return std::nullopt;

  ByteReader r(body);

  // byte 0: configurationVersion (must be 1)
  const uint8_t config_version = r.u8();
  if (config_version != 1) {
    return std::nullopt;
  }

  NalDecoderConfig config;

  // byte 1: lengthSizeMinusOne(2) | ptl_present_flag(1) | reserved(5)
  const uint8_t flags       = r.u8();
  config.nal_length_size    = ((flags >> 5) & 0x03u) + 1u;  // bits [6:5]
  const bool    ptl_present = (flags & 0x10u) != 0;          // bit 4

  if (ptl_present) {
    // bytes [2..3]:
    //   ols_idx(9 bits) | num_sublayers(3 bits) | constant_frame_rate(2 bits)
    //   | chroma_format_idc(2 bits)
    // byte [4]: bit_depth_minus8(3) | reserved(5)
    if (r.remaining() < 3) {
      return std::nullopt;
    }
    r.skip(1); // b0 = ols_idx[8:1]
    const uint8_t b1 = r.u8();
    // b1[7] = ols_idx[0], b1[6:4] = num_sublayers,
    // b1[3:2] = constant_frame_rate, b1[1:0] = chroma_format_idc
    const uint8_t num_sublayers = (b1 >> 4) & 0x07u;
    r.skip(1);  // bit_depth_minus8(3) | reserved(5)

    // --- VvcPTL() — ISO 14496-15:2022 §11.2.4.3 ---

    // byte: general_profile_idc(7) | general_tier_flag(1)
    if (r.remaining() < 1) {
      return std::nullopt;
    }
    const uint8_t ptl_b0 = r.u8();
    config.profile_idc   = ptl_b0 >> 1;   // bits [7:1]

    // byte: general_level_idc(8)
    if (r.remaining() < 1) {
      return std::nullopt;
    }
    r.skip(1);

    // byte: ptl_frame_only_constraint_flag(1) | ptl_multi_layer_enabled_flag(1)
    //       | gci_present_flag(1) | reserved(5)
    if (r.remaining() < 1) {
      return std::nullopt;
    }
    const uint8_t constraint_byte = r.u8();
    const bool    gci_present     = (constraint_byte >> 5) & 0x01u;  // bit 5

    if (gci_present) {
      // general_constraint_info() is exactly 12 bytes in the stored record
      // (ISO 14496-15 §11.2 specifies the in-file form is byte-aligned to 12 B)
      if (r.remaining() < 12) {
        return std::nullopt;
      }
      r.skip(12);
    }

    // ptl_sublayer_level_present_flag[i] for i in [num_sublayers-2 .. 0]
    // That is (num_sublayers - 1) flags, packed MSB-first then byte-padded.
    if (num_sublayers > 1) {
      const uint32_t flag_count = num_sublayers - 1u;
      const uint32_t flag_bytes = (flag_count + 7u) / 8u;
      if (r.remaining() < flag_bytes) {
        return std::nullopt;
      }

      uint8_t present_count = 0;
      for (uint32_t fb = 0; fb < flag_bytes; ++fb) {
        const uint8_t fbyte      = r.u8();
        const uint32_t bits_used = (fb == flag_bytes - 1u)
            ? flag_count - fb * 8u
            : 8u;
        for (uint32_t bit = 0; bit < bits_used; ++bit) {
          if ((fbyte >> (7u - bit)) & 0x01u) ++present_count;
        }
      }

      // Each flagged sublayer has one level_idc byte
      if (r.remaining() < present_count) {
        return std::nullopt;
      }
      r.skip(present_count);
    }

    // ptl_num_sub_profiles(8) followed by N × 4-byte sub-profile IDCs
    if (r.remaining() < 1) {
      return std::nullopt;
    }
    const uint8_t num_sub_profiles   = r.u8();
    const size_t  sub_profile_bytes  = static_cast<size_t>(num_sub_profiles) * 4u;
    if (r.remaining() < sub_profile_bytes) {
      return std::nullopt;
    }
    r.skip(sub_profile_bytes);
  }

  // num_of_arrays(8)
  if (r.remaining() < 1) {
    return std::nullopt;
  }
  const uint8_t num_arrays = r.u8();

  for (uint8_t i = 0; i < num_arrays; ++i) {
    // array_completeness(1) | reserved(1) | nal_unit_type(6)
    if (r.remaining() < 3) break;
    r.skip(1);
    const uint16_t num_nalus = r.u16be();

    for (uint16_t j = 0; j < num_nalus; ++j) {
      if (r.remaining() < 2) break;
      const uint16_t nal_size = r.u16be();
      if (nal_size == 0) continue;
      if (r.remaining() < nal_size) break;

      config.annexb_extradata.insert(config.annexb_extradata.end(),
                                     AnnexBFilter::START_CODE_LONG,
                                     AnnexBFilter::START_CODE_LONG + 4);
      const uint8_t* ptr = r.cur();
      config.annexb_extradata.insert(config.annexb_extradata.end(), ptr, ptr + nal_size);
      r.skip(nal_size);
    }
  }

  if (config.annexb_extradata.empty()) {
    return std::nullopt;
  }

  return config;
}

} // namespace openmedia
