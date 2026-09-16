#pragma once

#include "h264_types.hpp"

#include <util/bit_reader.hpp>

namespace h264 {

using BitReader = openmedia::BitReader;

void h264FillFlatScaling(SPS& sps);
void h264FillFlatScaling(PPS& pps);

// Reads scaling_list() (7.3.2.1.1.1). Returns false on an out-of-range
// delta_scale or a truncated list.
auto h264ReadScalingList(BitReader& br, int* list, int size, int& use_default) -> bool;

void h264DefaultScalingList4x4(int i, int lists[6][16]);
void h264DefaultScalingList8x8(int i, int lists[6][64]);

// Table 7-2 fallback for list `i` (0..5 are the 4x4 lists, 6..11 the 8x8 ones).
void h264ApplySpsScalingFallback(SPS& sps, int i);
void h264ApplyPpsScalingFallback(PPS& pps, const SPS& sps, int i);

auto h264ReadVui(BitReader& br, SPS& sps) -> bool;

// ChromaArrayType (7.4.2.1.1): 0 when the colour planes are coded separately.
inline auto chromaArrayType(const SPS& sps) -> int {
  return sps.separate_colour_plane_flag ? 0 : sps.chroma_format_idc;
}

} // namespace h264
