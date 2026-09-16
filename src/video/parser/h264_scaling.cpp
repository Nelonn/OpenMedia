#include "h264_internal.hpp"

#include <algorithm>

namespace h264 {

// Flat_4x4_16 / Flat_8x8_16 (7.4.2.1.1.1): the value every list takes when no
// scaling matrix is signalled at all.
static constexpr int FLAT_4X4[16] = {
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
};

static constexpr int FLAT_8X8[64] = {
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
};

// Default_4x4_Intra / Default_4x4_Inter (Table 7-3) and
// Default_8x8_Intra / Default_8x8_Inter (Table 7-4). These are what
// UseDefaultScalingMatrixFlag and the Table 7-2 fallback rules select; they are
// *not* the flat lists above.
static constexpr int DEFAULT_4X4_INTRA[16] = {
    6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42,
};

static constexpr int DEFAULT_4X4_INTER[16] = {
    10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 30, 30, 34,
};

static constexpr int DEFAULT_8X8_INTRA[64] = {
    6,  10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23,
    23, 23, 23, 23, 23, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27,
    27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 31,
    31, 33, 33, 33, 33, 33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42,
};

static constexpr int DEFAULT_8X8_INTER[64] = {
    9,  13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21,
    21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 24, 24, 24, 24,
    24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27,
    27, 28, 28, 28, 28, 28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35,
};

static void copyList(int* dst, const int* src, int size) {
  std::copy_n(src, size, dst);
}

void h264FillFlatScaling(SPS& sps) {
  for (auto& list : sps.ScalingList4x4) copyList(list, FLAT_4X4, 16);
  for (auto& list : sps.ScalingList8x8) copyList(list, FLAT_8X8, 64);
}

void h264FillFlatScaling(PPS& pps) {
  for (auto& list : pps.ScalingList4x4) copyList(list, FLAT_4X4, 16);
  for (auto& list : pps.ScalingList8x8) copyList(list, FLAT_8X8, 64);
}

void h264DefaultScalingList4x4(int i, int lists[6][16]) {
  copyList(lists[i], i < 3 ? DEFAULT_4X4_INTRA : DEFAULT_4X4_INTER, 16);
}

void h264DefaultScalingList8x8(int i, int lists[6][64]) {
  copyList(lists[i], (i % 2) == 0 ? DEFAULT_8X8_INTRA : DEFAULT_8X8_INTER, 64);
}

// Table 7-2 fallback rule, shared by rule A (`intra`/`inter` are the spec
// default matrices) and rule B (they are the SPS lists).
void h264FallbackScalingList4x4(int i, const int* intra, const int* inter, int lists[6][16]) {
  switch (i) {
    case 0: copyList(lists[0], intra, 16); break;
    case 3: copyList(lists[3], inter, 16); break;
    default: copyList(lists[i], lists[i - 1], 16); break;
  }
}

void h264FallbackScalingList8x8(int i, const int* intra, const int* inter, int lists[6][64]) {
  switch (i) {
    case 0: copyList(lists[0], intra, 64); break;
    case 1: copyList(lists[1], inter, 64); break;
    default: copyList(lists[i], lists[i - 2], 64); break;
  }
}

auto h264ReadScalingList(BitReader& br, int* list, int size, int& use_default) -> bool {
  int last_scale = 8;
  int next_scale = 8;
  use_default = 0;
  for (int j = 0; j < size; ++j) {
    if (next_scale != 0) {
      const int delta_scale = br.readSE();
      if (delta_scale < -128 || delta_scale > 127) return false;
      // & 0xff, not % 256: the C++ remainder of a negative value is negative,
      // which would put out-of-range entries into the matrix handed to the
      // hardware decoder.
      next_scale = (last_scale + delta_scale + 256) & 0xff;
      if (j == 0 && next_scale == 0) {
        use_default = 1;
        return br.ok();
      }
    }
    list[j] = next_scale == 0 ? last_scale : next_scale;
    last_scale = list[j];
  }
  return br.ok();
}

void h264ApplySpsScalingFallback(SPS& sps, int i) {
  if (i < 6) h264FallbackScalingList4x4(i, DEFAULT_4X4_INTRA, DEFAULT_4X4_INTER, sps.ScalingList4x4);
  else h264FallbackScalingList8x8(i - 6, DEFAULT_8X8_INTRA, DEFAULT_8X8_INTER, sps.ScalingList8x8);
}

void h264ApplyPpsScalingFallback(PPS& pps, const SPS& sps, int i) {
  // Rule A when the SPS carries no matrix of its own, rule B when it does.
  if (i < 6) {
    if (!sps.seq_scaling_matrix_present_flag) {
      h264FallbackScalingList4x4(i, DEFAULT_4X4_INTRA, DEFAULT_4X4_INTER, pps.ScalingList4x4);
    } else {
      h264FallbackScalingList4x4(i, sps.ScalingList4x4[0], sps.ScalingList4x4[3], pps.ScalingList4x4);
    }
  } else {
    const int j = i - 6;
    if (!sps.seq_scaling_matrix_present_flag) {
      h264FallbackScalingList8x8(j, DEFAULT_8X8_INTRA, DEFAULT_8X8_INTER, pps.ScalingList8x8);
    } else {
      h264FallbackScalingList8x8(j, sps.ScalingList8x8[0], sps.ScalingList8x8[1], pps.ScalingList8x8);
    }
  }
}

} // namespace h264
