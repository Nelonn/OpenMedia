#pragma once

// Constant tables of ATSC A/52 (AC-3) and ETSI TS 102 366 Annex E (E-AC-3).

#include <array>
#include <cstdint>

namespace openmedia::ac3 {

inline constexpr int BLOCK_SIZE = 256;      // new samples per audio block
inline constexpr int MAX_BLOCKS = 6;        // audio blocks per syncframe
inline constexpr int MAX_COEFS = 256;       // transform coefficients per block
inline constexpr int CRITICAL_BANDS = 50;   // bit allocation bands
inline constexpr int MAX_FBW_CHANNELS = 5;
inline constexpr int MAX_CHANNELS = 7;      // coupling + 5 full bandwidth + LFE

// Sample rates for fscod (A/52 Table 5.6).
inline constexpr uint32_t SAMPLE_RATES[3] = {48000, 44100, 32000};

// AC-3 nominal bit rates for frmsizecod >> 1, in kbit/s (A/52 Table 5.18).
inline constexpr uint16_t BIT_RATES[19] = {
  32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640,
};

// AC-3 syncframe size in 16-bit words, [frmsizecod][fscod] (A/52 Table 5.18).
inline constexpr uint16_t FRAME_SIZE_WORDS[38][3] = {
  {64, 69, 96},     {64, 70, 96},     {80, 87, 120},    {80, 88, 120},    {96, 104, 144},
  {96, 105, 144},   {112, 121, 168},  {112, 122, 168},  {128, 139, 192},  {128, 140, 192},
  {160, 174, 240},  {160, 175, 240},  {192, 208, 288},  {192, 209, 288},  {224, 243, 336},
  {224, 244, 336},  {256, 278, 384},  {256, 279, 384},  {320, 348, 480},  {320, 349, 480},
  {384, 417, 576},  {384, 418, 576},  {448, 487, 672},  {448, 488, 672},  {512, 557, 768},
  {512, 558, 768},  {640, 696, 960},  {640, 697, 960},  {768, 835, 1152}, {768, 836, 1152},
  {896, 975, 1344}, {896, 976, 1344}, {1024, 1114, 1536}, {1024, 1115, 1536}, {1152, 1253, 1728},
  {1152, 1254, 1728}, {1280, 1393, 1920}, {1280, 1394, 1920},
};

// Full bandwidth channels per acmod (A/52 Table 5.8).
inline constexpr uint8_t FBW_CHANNELS[8] = {2, 1, 2, 3, 3, 4, 4, 5};

// E-AC-3 audio blocks per syncframe for numblkscod.
inline constexpr uint8_t EAC3_BLOCKS[4] = {1, 2, 3, 6};

// First bin of each rematrixing band (A/52 Section 7.5.2).
inline constexpr uint8_t REMATRIX_BANDS[5] = {13, 25, 37, 61, 253};

// First bin of each bit allocation band (A/52 Table 7.12, bndtab), plus the end.
inline constexpr uint8_t BAND_START[CRITICAL_BANDS + 1] = {
  0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11,  12,  13,  14,  15,  16,
  17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,  31,  34,  37,  40,  43,
  46, 49, 55, 61, 67, 73, 79, 85, 97, 109, 121, 133, 157, 181, 205, 229, 253,
};

// Bit allocation band containing each bin (A/52 Table 7.13, masktab).
inline constexpr auto BIN_TO_BAND = [] {
  std::array<uint8_t, MAX_COEFS> table {};
  int band = 0;
  for (int bin = 0; bin < MAX_COEFS; ++bin) {
    while (band < CRITICAL_BANDS - 1 && bin >= BAND_START[band + 1]) ++band;
    table[bin] = static_cast<uint8_t>(band);
  }
  return table;
}();

// Log-addition table (A/52 Table 7.14, latab).
inline constexpr uint8_t LOG_ADD[260] = {
  0x40, 0x3f, 0x3e, 0x3d, 0x3c, 0x3b, 0x3a, 0x39, 0x38, 0x37, 0x36, 0x35, 0x34, 0x34, 0x33, 0x32,
  0x31, 0x30, 0x2f, 0x2f, 0x2e, 0x2d, 0x2c, 0x2c, 0x2b, 0x2a, 0x29, 0x29, 0x28, 0x27, 0x26, 0x26,
  0x25, 0x24, 0x24, 0x23, 0x23, 0x22, 0x21, 0x21, 0x20, 0x20, 0x1f, 0x1e, 0x1e, 0x1d, 0x1d, 0x1c,
  0x1c, 0x1b, 0x1b, 0x1a, 0x1a, 0x19, 0x19, 0x18, 0x18, 0x17, 0x17, 0x16, 0x16, 0x15, 0x15, 0x15,
  0x14, 0x14, 0x13, 0x13, 0x13, 0x12, 0x12, 0x12, 0x11, 0x11, 0x11, 0x10, 0x10, 0x10, 0x0f, 0x0f,
  0x0f, 0x0e, 0x0e, 0x0e, 0x0d, 0x0d, 0x0d, 0x0d, 0x0c, 0x0c, 0x0c, 0x0c, 0x0b, 0x0b, 0x0b, 0x0b,
  0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x09, 0x09, 0x09, 0x09, 0x09, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
  0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x05, 0x05,
  0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
  0x04, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x02,
  0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
  0x02, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
  0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
  0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
};

// Hearing threshold per band and fscod (A/52 Table 7.15, hth).
inline constexpr uint16_t HEARING_THRESHOLD[CRITICAL_BANDS][3] = {
  {0x04d0, 0x04f0, 0x0580}, {0x04d0, 0x04f0, 0x0580}, {0x0440, 0x0460, 0x04b0}, {0x0400, 0x0410, 0x0450},
  {0x03e0, 0x03e0, 0x0420}, {0x03c0, 0x03d0, 0x03f0}, {0x03b0, 0x03c0, 0x03e0}, {0x03b0, 0x03b0, 0x03d0},
  {0x03a0, 0x03b0, 0x03c0}, {0x03a0, 0x03a0, 0x03b0}, {0x03a0, 0x03a0, 0x03b0}, {0x03a0, 0x03a0, 0x03b0},
  {0x03a0, 0x03a0, 0x03a0}, {0x0390, 0x03a0, 0x03a0}, {0x0390, 0x0390, 0x03a0}, {0x0390, 0x0390, 0x03a0},
  {0x0380, 0x0390, 0x03a0}, {0x0380, 0x0380, 0x03a0}, {0x0370, 0x0380, 0x03a0}, {0x0370, 0x0380, 0x03a0},
  {0x0360, 0x0370, 0x0390}, {0x0360, 0x0370, 0x0390}, {0x0350, 0x0360, 0x0390}, {0x0350, 0x0360, 0x0390},
  {0x0340, 0x0350, 0x0380}, {0x0340, 0x0350, 0x0380}, {0x0330, 0x0340, 0x0380}, {0x0320, 0x0340, 0x0370},
  {0x0310, 0x0320, 0x0360}, {0x0300, 0x0310, 0x0350}, {0x02f0, 0x0300, 0x0340}, {0x02f0, 0x02f0, 0x0330},
  {0x02f0, 0x02f0, 0x0320}, {0x02f0, 0x02f0, 0x0310}, {0x0300, 0x02f0, 0x0300}, {0x0310, 0x0300, 0x02f0},
  {0x0340, 0x0320, 0x02f0}, {0x0390, 0x0350, 0x02f0}, {0x03e0, 0x0390, 0x0300}, {0x0420, 0x03e0, 0x0310},
  {0x0460, 0x0420, 0x0330}, {0x0490, 0x0450, 0x0350}, {0x04a0, 0x04a0, 0x03c0}, {0x0460, 0x0490, 0x0410},
  {0x0440, 0x0460, 0x0470}, {0x0440, 0x0440, 0x04a0}, {0x0520, 0x0480, 0x0460}, {0x0800, 0x0630, 0x0440},
  {0x0840, 0x0840, 0x0450}, {0x0840, 0x0840, 0x04e0},
};

// Bit allocation pointer per address (A/52 Table 7.16, baptab).
inline constexpr uint8_t BAP_TABLE[64] = {
  0,  1,  1,  1,  1,  1,  2,  2,  3,  3,  3,  4,  4,  5,  5,  6,  6,  6,  6,  7,  7,  7,
  7,  8,  8,  8,  8,  9,  9,  9,  9,  10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 13,
  13, 13, 13, 14, 14, 14, 14, 14, 14, 14, 14, 15, 15, 15, 15, 15, 15, 15, 15, 15,
};

// High efficiency bit allocation pointer per address (TS 102 366 Table E3.1).
inline constexpr uint8_t HEBAP_TABLE[64] = {
  0,  1,  2,  3,  4,  5,  6,  7,  8,  8,  8,  8,  9,  9,  9,  10, 10, 10, 10, 11, 11, 11,
  11, 12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 15, 16, 16, 16, 16, 17,
  17, 17, 17, 18, 18, 18, 18, 18, 18, 18, 18, 19, 19, 19, 19, 19, 19, 19, 19, 19,
};

// Bit allocation parameter tables (A/52 Table 7.6 to 7.11).
inline constexpr int SLOW_DECAY[4] = {0x0f, 0x11, 0x13, 0x15};
inline constexpr int FAST_DECAY[4] = {0x3f, 0x53, 0x67, 0x7b};
inline constexpr int SLOW_GAIN[4] = {0x540, 0x4d8, 0x478, 0x410};
inline constexpr int DB_PER_BIT[4] = {0x000, 0x700, 0x900, 0xb00};
inline constexpr int FLOOR_TABLE[8] = {0x2f0, 0x2b0, 0x270, 0x230, 0x1f0, 0x170, 0x0f0, -2048};
inline constexpr int FAST_GAIN[8] = {0x080, 0x100, 0x180, 0x200, 0x280, 0x300, 0x380, 0x400};

// Mantissa bits for the asymmetric quantizers of baps 6..15 (A/52 Table 7.18).
inline constexpr uint8_t BAP_BITS[16] = {0, 0, 0, 0, 0, 0, 5, 6, 7, 8, 9, 10, 11, 12, 14, 16};

// Mantissa bits per hebap: VQ index bits for 1..7, scalar bits for 8..19 (TS 102 366 Table E3.2).
inline constexpr uint8_t HEBAP_BITS[20] = {0, 2, 3, 4, 5, 7, 8, 9, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 16};

// AHT inverse quantization remapping, Q15 (TS 102 366 Table E3.6), indexed by hebap - 8.
inline constexpr int16_t GAQ_REMAP_GAIN1[12] = {4681, 2185, 1057, 520, 258, 129, 64, 32, 16, 8, 2, 0};
// [hebap - 8][0: Gk = 2, 1: Gk = 4]
inline constexpr int16_t GAQ_REMAP_A[9][2] = {
  {-10923, -4681}, {-14043, -6554}, {-15292, -7399}, {-15855, -7802}, {-16124, -7998},
  {-16255, -8096}, {-16320, -8144}, {-16352, -8168}, {-16368, -8180},
};
// Offset b for negative large mantissas; positive ones use 0.5 (Gk = 2) or 0.25 (Gk = 4).
inline constexpr int16_t GAQ_REMAP_NEG_B[9][2] = {
  {-5461, -1170},  {-11703, -4915}, {-14199, -6606}, {-15327, -7412}, {-15864, -7805},
  {-16126, -7999}, {-16255, -8096}, {-16320, -8144}, {-16352, -8168},
};

// AHT VQ codebooks for hebap 1..7 (TS 102 366 Tables E4.1-E4.7), Q15. Index 0 is null.
extern const int16_t (*const AHT_VQ_CODEBOOKS[8])[6];

enum ExpStrategy : uint8_t {
  EXP_REUSE = 0,
  EXP_D15 = 1,
  EXP_D25 = 2,
  EXP_D45 = 3,
};

// Frame exponent strategy combinations for frmchexpstr (TS 102 366 Table E2.10).
inline constexpr uint8_t FRAME_EXP_STRATEGIES[32][6] = {
  {1, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 3}, {1, 0, 0, 0, 2, 0}, {1, 0, 0, 0, 3, 3},
  {2, 0, 0, 2, 0, 0}, {2, 0, 0, 2, 0, 3}, {2, 0, 0, 3, 2, 0}, {2, 0, 0, 3, 3, 3},
  {2, 0, 1, 0, 0, 0}, {2, 0, 2, 0, 0, 3}, {2, 0, 2, 0, 2, 0}, {2, 0, 2, 0, 3, 3},
  {2, 0, 3, 2, 0, 0}, {2, 0, 3, 2, 0, 3}, {2, 0, 3, 3, 2, 0}, {2, 0, 3, 3, 3, 3},
  {3, 1, 0, 0, 0, 0}, {3, 1, 0, 0, 0, 3}, {3, 2, 0, 0, 2, 0}, {3, 2, 0, 0, 3, 3},
  {3, 2, 0, 2, 0, 0}, {3, 2, 0, 2, 0, 3}, {3, 2, 0, 3, 2, 0}, {3, 2, 0, 3, 3, 3},
  {3, 3, 1, 0, 0, 0}, {3, 3, 2, 0, 0, 3}, {3, 3, 2, 0, 2, 0}, {3, 3, 2, 0, 3, 3},
  {3, 3, 3, 2, 0, 0}, {3, 3, 3, 2, 0, 3}, {3, 3, 3, 3, 2, 0}, {3, 3, 3, 3, 3, 3},
};

// Default coupling and spectral extension band structures (TS 102 366 Tables E2.16, E2.15).
inline constexpr uint8_t DEFAULT_CPL_BAND_STRUCT[18] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1};
inline constexpr uint8_t DEFAULT_SPX_BAND_STRUCT[17] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};

} // namespace openmedia::ac3
