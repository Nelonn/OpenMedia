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
//
// Unlike avcC/hvcC the record has no configurationVersion byte: it opens with
// reserved '11111'b | LengthSizeMinusOne(2) | ptl_present_flag(1). In MP4 the
// record sits inside a FullBox, so the caller strips version/flags first.
inline auto parseVvcDecoderConfig(std::span<const uint8_t> body)
    -> std::optional<NalDecoderConfig> {
  if (body.size() < 2) return std::nullopt;

  ByteReader r(body);
  NalDecoderConfig config;

  const uint8_t flags = r.u8();
  config.nal_length_size = ((flags >> 1) & 0x03u) + 1u;
  const bool ptl_present = (flags & 0x01u) != 0;

  if (ptl_present) {
    // ols_idx(9) | num_sublayers(3) | constant_frame_rate(2) | chroma_format_idc(2)
    const uint16_t ols_word = r.u16be();
    const uint8_t num_sublayers = (ols_word >> 4) & 0x07u;
    r.skip(1); // bit_depth_minus8(3) | reserved(5)

    // VvcPTLRecord(num_sublayers), §11.2.4.3
    const uint8_t num_bytes_constraint_info = r.u8() & 0x3Fu;
    config.profile_idc = r.u8() >> 1; // general_profile_idc(7) | general_tier_flag(1)
    r.skip(1); // general_level_idc
    // ptl_frame_only_constraint_flag, ptl_multilayer_enabled_flag and
    // general_constraint_info share these bytes.
    r.skip(num_bytes_constraint_info);

    if (num_sublayers > 1) {
      // ptl_sublayer_level_present_flag[num_sublayers-2..0], zero-padded to a byte
      const uint8_t present = r.u8();
      for (uint8_t i = 0; i + 1u < num_sublayers; ++i) {
        if (present & (0x80u >> i)) r.skip(1); // sublayer_level_idc
      }
    }

    const uint8_t num_sub_profiles = r.u8();
    r.skip(static_cast<size_t>(num_sub_profiles) * 4u); // general_sub_profile_idc

    r.skip(2); // max_picture_width
    r.skip(2); // max_picture_height
    r.skip(2); // avg_frame_rate
    if (!r.ok()) return std::nullopt;
  }

  constexpr uint8_t VVC_OPI_NUT = 12;
  constexpr uint8_t VVC_DCI_NUT = 13;

  const uint8_t num_arrays = r.u8();
  for (uint8_t i = 0; i < num_arrays && r.ok(); ++i) {
    // array_completeness(1) | reserved(2) | NAL_unit_type(5)
    if (r.remaining() < 1) break;
    const uint8_t nal_unit_type = r.u8() & 0x1Fu;
    // OPI and DCI arrays omit num_nalus and hold exactly one NAL unit.
    uint16_t num_nalus = 1;
    if (nal_unit_type != VVC_OPI_NUT && nal_unit_type != VVC_DCI_NUT) {
      if (r.remaining() < 2) break;
      num_nalus = r.u16be();
    }

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
