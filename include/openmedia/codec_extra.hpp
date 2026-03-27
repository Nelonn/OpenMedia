#pragma once

#include <cstdint>
#include <openmedia/dictionary.hpp>

namespace openmedia {

// int32 - Encoding application mode (VOIP(2048)/AUDIO(2049)/RESTRICTED_LOWDELAY(2051))
constexpr Key OPUS_ENC_APPLICATION = "opus.application";
// int32 - Target bitrate in bps (OPUS_AUTO or specific value)
constexpr Key OPUS_ENC_BITRATE = "opus.bitrate";
// int32 - VBR mode (0=CBR, 1=VBR, 2=constrained VBR, 3=hybrid VBR)
constexpr Key OPUS_ENC_VBR = "opus.vbr";
// int32 - Encoder complexity (0-10, default 10)
constexpr Key OPUS_ENC_COMPLEXITY = "opus.complexity";
// int32 - Frame size in samples
constexpr Key OPUS_ENC_FRAME_SIZE = "opus.frame_size";
// int32 - Force stereo/mono (-1=auto)
constexpr Key OPUS_ENC_FORCE_CHANNELS = "opus.force_channels";
// int32 - Signal type hint (MUSIC/VOICE)
constexpr Key OPUS_ENC_SIGNAL_TYPE = "opus.signal_type";
// int32 - Audio bandwidth (NARROWBAND to FULLBAND)
constexpr Key OPUS_ENC_BANDWIDTH = "opus.bandwidth";
// int32 - Expected packet loss percentage (0-100)
constexpr Key OPUS_ENC_PACKET_LOSS_PERC = "opus.packet_loss_perc";
// int32 - Enable inband FEC (0 or 1)
constexpr Key OPUS_ENC_FEC = "opus.fec";
// int32 - Enable DTX (0 or 1)
constexpr Key OPUS_ENC_DTX = "opus.dtx";
// int32 - LSB depth for quantization (4-24 bits)
constexpr Key OPUS_ENC_LSB_DEPTH = "opus.lsb_depth";
// int32 - Encoder lookahead delay in samples
constexpr Key OPUS_ENC_LOOKAHEAD = "opus.lookahead";

} // namespace openmedia
