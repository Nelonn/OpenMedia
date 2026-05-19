#pragma once

#include "h264_types.hpp"

#include <util/bit_reader.hpp>

namespace h264 {

using BitReader = openmedia::BitReader;

void h264FillDefaultScaling(SPS& sps);
void h264FillDefaultScaling(PPS& pps);
void h264ReadScalingList(BitReader& br, int* list, int size, int& use_default);
void h264ReadVui(BitReader& br, SPS& sps);

} // namespace h264
