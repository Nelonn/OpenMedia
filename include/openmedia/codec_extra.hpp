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
// int32 - Mapping family (0, 1, or 255)
constexpr Key OPUS_ENC_MAPPING_FAMILY = "opus.mapping_family";

// OpenH264
// int32 - Usage type (0: camera, 1: screen)
constexpr Key OPENH264_ENC_USAGE_TYPE = "openh264.usage_type";
// int32 - Complexity mode (0: low, 1: medium, 2: high)
constexpr Key OPENH264_ENC_COMPLEXITY = "openh264.complexity";
// int32 - IDR interval in frames
constexpr Key OPENH264_ENC_IDR_INTERVAL = "openh264.idr_interval";
// int32 - Number of reference frames
constexpr Key OPENH264_ENC_NUM_REF_FRAME = "openh264.num_ref_frame";
// int32 - Entropy coding mode (0: CAVLC, 1: CABAC)
constexpr Key OPENH264_ENC_ENTROPY_CODING = "openh264.entropy_coding";
// bool - Enable frame skipping
constexpr Key OPENH264_ENC_FRAME_SKIP = "openh264.frame_skip";
// int32 - Maximum QP
constexpr Key OPENH264_ENC_MAX_QP = "openh264.max_qp";
// int32 - Minimum QP
constexpr Key OPENH264_ENC_MIN_QP = "openh264.min_qp";
// bool - Enable Long Term Reference
constexpr Key OPENH264_ENC_LTR = "openh264.ltr";
// int32 - LTR marking period
constexpr Key OPENH264_ENC_LTR_PERIOD = "openh264.ltr_period";
// int32 - Number of threads (0 for auto)
constexpr Key OPENH264_ENC_THREADS = "openh264.threads";
// bool - Enable denoise
constexpr Key OPENH264_ENC_DENOISE = "openh264.denoise";
// bool - Enable background detection
constexpr Key OPENH264_ENC_BGD = "openh264.bgd";
// bool - Enable adaptive quantization
constexpr Key OPENH264_ENC_AQ = "openh264.aq";
// bool - Enable scene change detection
constexpr Key OPENH264_ENC_SCENE_CHANGE = "openh264.scene_change";
// int32 - Loop filter mode (0: on, 1: off, 2: on except slice boundaries)
constexpr Key OPENH264_ENC_LOOP_FILTER = "openh264.loop_filter";
// int32 - Slice mode (0: single, 1: fixed number, 2: raster, 3: size limited)
constexpr Key OPENH264_ENC_SLICE_MODE = "openh264.slice_mode";
// int32 - Slice number (for fixed number mode)
constexpr Key OPENH264_ENC_SLICE_NUM = "openh264.slice_num";
// int32 - Slice size constraint (for size limited mode)
constexpr Key OPENH264_ENC_SLICE_SIZE = "openh264.slice_size";

} // namespace openmedia
