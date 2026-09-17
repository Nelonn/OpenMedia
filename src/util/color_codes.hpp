#pragma once

#include <openmedia/video.hpp>
#include <cstdint>

// ISO/IEC 23001-8 (also ITU-T H.273) colour description codes. The same three
// tables are used by MP4 `colr`, Matroska `Colour`, H.264/H.265 VUI, AV1
// sequence headers and VP9 headers, so every demuxer and parser maps through
// here instead of keeping its own partial copy.

namespace openmedia::color_codes {

inline auto primariesFromCode(uint32_t p) -> OMColorPrimaries {
  switch (p) {
    case 1: return OM_PRIMARIES_BT709;
    case 4: return OM_PRIMARIES_BT470M;
    case 5: return OM_PRIMARIES_BT470BG;
    case 6: return OM_PRIMARIES_BT601;
    case 7: return OM_PRIMARIES_SMPTE240M;
    case 8: return OM_PRIMARIES_FILM;
    case 9: return OM_PRIMARIES_BT2020;
    case 10: return OM_PRIMARIES_SMPTE428;
    case 11: return OM_PRIMARIES_SMPTE431;
    case 12: return OM_PRIMARIES_SMPTE432;
    case 22: return OM_PRIMARIES_EBU3213;
    default: return OM_PRIMARIES_UNKNOWN;
  }
}

inline auto codeFromPrimaries(OMColorPrimaries p) -> uint32_t {
  switch (p) {
    case OM_PRIMARIES_BT709: return 1;
    case OM_PRIMARIES_BT470M: return 4;
    case OM_PRIMARIES_BT470BG: return 5;
    case OM_PRIMARIES_BT601: return 6;
    case OM_PRIMARIES_SMPTE240M: return 7;
    case OM_PRIMARIES_FILM: return 8;
    case OM_PRIMARIES_BT2020: return 9;
    case OM_PRIMARIES_SMPTE428: return 10;
    case OM_PRIMARIES_SMPTE431: return 11;
    case OM_PRIMARIES_SMPTE432: return 12;
    case OM_PRIMARIES_EBU3213: return 22;
    default: return 2; // unspecified
  }
}

inline auto transferFromCode(uint32_t t) -> OMTransferCharacteristic {
  switch (t) {
    case 1: return OM_TRANSFER_BT709;
    case 4: return OM_TRANSFER_GAMMA22;
    case 5: return OM_TRANSFER_GAMMA28;
    case 6: return OM_TRANSFER_BT601;
    case 7: return OM_TRANSFER_SMPTE240M;
    case 8: return OM_TRANSFER_LINEAR;
    case 9: return OM_TRANSFER_LOG;
    case 10: return OM_TRANSFER_LOG_SQRT;
    case 11: return OM_TRANSFER_IEC61966_2_4;
    case 12: return OM_TRANSFER_BT1361_ECG;
    case 13: return OM_TRANSFER_IEC61966_2_1;
    case 14: return OM_TRANSFER_BT2020_10;
    case 15: return OM_TRANSFER_BT2020_12;
    case 16: return OM_TRANSFER_SMPTE2084;
    case 17: return OM_TRANSFER_SMPTE428;
    case 18: return OM_TRANSFER_ARIB_STD_B67;
    default: return OM_TRANSFER_UNKNOWN;
  }
}

inline auto codeFromTransfer(OMTransferCharacteristic t) -> uint32_t {
  switch (t) {
    case OM_TRANSFER_BT709: return 1;
    case OM_TRANSFER_GAMMA22: return 4;
    case OM_TRANSFER_GAMMA28: return 5;
    case OM_TRANSFER_BT601: return 6;
    case OM_TRANSFER_SMPTE240M: return 7;
    case OM_TRANSFER_LINEAR: return 8;
    case OM_TRANSFER_LOG: return 9;
    case OM_TRANSFER_LOG_SQRT: return 10;
    case OM_TRANSFER_IEC61966_2_4: return 11;
    case OM_TRANSFER_BT1361_ECG: return 12;
    case OM_TRANSFER_IEC61966_2_1: return 13;
    case OM_TRANSFER_BT2020_10: return 14;
    case OM_TRANSFER_BT2020_12: return 15;
    case OM_TRANSFER_SMPTE2084: return 16;
    case OM_TRANSFER_SMPTE428: return 17;
    case OM_TRANSFER_ARIB_STD_B67: return 18;
    default: return 2; // unspecified
  }
}

// Matrix coefficients. Note that 0 (identity / GBR) and 8 (YCgCo) are distinct
// from "unspecified" (2), so they are mapped rather than dropped.
inline auto colorSpaceFromMatrix(uint32_t m) -> OMColorSpace {
  switch (m) {
    case 0: return OM_COLOR_SPACE_RGB;
    case 1: return OM_COLOR_SPACE_BT709;
    case 4: return OM_COLOR_SPACE_FCC;
    case 5: // BT.470BG and SMPTE 170M share the BT.601 matrix.
    case 6: return OM_COLOR_SPACE_BT601;
    case 7: return OM_COLOR_SPACE_SMPTE240M;
    case 8: return OM_COLOR_SPACE_YCGCO;
    case 9: return OM_COLOR_SPACE_BT2020;
    case 10: return OM_COLOR_SPACE_BT2020_CL;
    case 11: return OM_COLOR_SPACE_SMPTE428;
    case 12: return OM_COLOR_SPACE_CHROMA_DERIVED_NCL;
    case 13: return OM_COLOR_SPACE_CHROMA_DERIVED_CL;
    case 14: return OM_COLOR_SPACE_ICTCP;
    default: return OM_COLOR_SPACE_UNKNOWN;
  }
}

inline auto matrixFromColorSpace(OMColorSpace c) -> uint32_t {
  switch (c) {
    case OM_COLOR_SPACE_RGB: return 0;
    case OM_COLOR_SPACE_BT709: return 1;
    case OM_COLOR_SPACE_FCC: return 4;
    case OM_COLOR_SPACE_BT601: return 6;
    case OM_COLOR_SPACE_SMPTE240M: return 7;
    case OM_COLOR_SPACE_YCGCO: return 8;
    case OM_COLOR_SPACE_BT2020: return 9;
    case OM_COLOR_SPACE_BT2020_CL: return 10;
    case OM_COLOR_SPACE_SMPTE428: return 11;
    case OM_COLOR_SPACE_CHROMA_DERIVED_NCL: return 12;
    case OM_COLOR_SPACE_CHROMA_DERIVED_CL: return 13;
    case OM_COLOR_SPACE_ICTCP: return 14;
    default: return 2; // unspecified
  }
}

// When a container only describes the primaries, the matrix that normally goes
// with them is the best guess available.
inline auto colorSpaceFromPrimaries(uint32_t p) -> OMColorSpace {
  switch (p) {
    case 1: return OM_COLOR_SPACE_BT709;
    case 4:
    case 5:
    case 6: return OM_COLOR_SPACE_BT601;
    case 7: return OM_COLOR_SPACE_SMPTE240M;
    case 9: return OM_COLOR_SPACE_BT2020;
    default: return OM_COLOR_SPACE_UNKNOWN;
  }
}

} // namespace openmedia::color_codes
