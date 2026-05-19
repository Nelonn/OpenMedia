#include "h264_internal.hpp"

namespace h264 {

static constexpr int DEFAULT_4X4[16] = {
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
};

static constexpr int DEFAULT_8X8[64] = {
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
};

void h264FillDefaultScaling(SPS& sps) {
  for (auto& list : sps.ScalingList4x4) for (int i = 0; i < 16; ++i) list[i] = DEFAULT_4X4[i];
  for (auto& list : sps.ScalingList8x8) for (int i = 0; i < 64; ++i) list[i] = DEFAULT_8X8[i];
}

void h264FillDefaultScaling(PPS& pps) {
  for (auto& list : pps.ScalingList4x4) for (int i = 0; i < 16; ++i) list[i] = DEFAULT_4X4[i];
  for (auto& list : pps.ScalingList8x8) for (int i = 0; i < 64; ++i) list[i] = DEFAULT_8X8[i];
}

void h264ReadScalingList(BitReader& br, int* list, int size, int& use_default) {
  int last_scale = 8;
  int next_scale = 8;
  use_default = 0;
  for (int j = 0; j < size; ++j) {
    if (next_scale != 0) {
      const int delta_scale = br.readSE();
      next_scale = (last_scale + delta_scale + 256) % 256;
      if (j == 0 && next_scale == 0) use_default = 1;
    }
    list[j] = next_scale == 0 ? last_scale : next_scale;
    last_scale = list[j];
  }
}

} // namespace h264
