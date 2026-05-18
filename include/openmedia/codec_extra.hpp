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
// int32 - Temporal layer count (1-4)
constexpr Key OPENH264_ENC_TEMPORAL_LAYERS = "openh264.temporal_layers";
// int32 - Number of reference frames
constexpr Key OPENH264_ENC_NUM_REF_FRAME = "openh264.num_ref_frame";
// int32 - SPS/PPS id strategy (0=constant, 1=increasing, 2=list SPS, 3=list SPS + increasing PPS, 6=list SPS/PPS)
constexpr Key OPENH264_ENC_SPS_PPS_ID_STRATEGY = "openh264.sps_pps_id_strategy";
// bool - Enable prefix NAL units
constexpr Key OPENH264_ENC_PREFIX_NAL = "openh264.prefix_nal";
// bool - Enable subsequence SEI
constexpr Key OPENH264_ENC_SSEI = "openh264.ssei";
// bool - Use simulcast AVC syntax for multiple spatial layers
constexpr Key OPENH264_ENC_SIMULCAST_AVC = "openh264.simulcast_avc";
// int32 - Padding flag (0=disabled, 1=enabled)
constexpr Key OPENH264_ENC_PADDING = "openh264.padding";
// int32 - Entropy coding mode (0: CAVLC, 1: CABAC)
constexpr Key OPENH264_ENC_ENTROPY_CODING = "openh264.entropy_coding";
// bool - Enable frame skipping
constexpr Key OPENH264_ENC_FRAME_SKIP = "openh264.frame_skip";
// int32 - Maximum bitrate in bps
constexpr Key OPENH264_ENC_MAX_BITRATE = "openh264.max_bitrate";
// int32 - Maximum QP
constexpr Key OPENH264_ENC_MAX_QP = "openh264.max_qp";
// int32 - Minimum QP
constexpr Key OPENH264_ENC_MIN_QP = "openh264.min_qp";
// int32 - Maximum NAL unit size in bytes
constexpr Key OPENH264_ENC_MAX_NAL_SIZE = "openh264.max_nal_size";
// bool - Enable Long Term Reference
constexpr Key OPENH264_ENC_LTR = "openh264.ltr";
// int32 - Number of LTR reference frames
constexpr Key OPENH264_ENC_LTR_REF_NUM = "openh264.ltr_ref_num";
// int32 - LTR marking period
constexpr Key OPENH264_ENC_LTR_PERIOD = "openh264.ltr_period";
// int32 - Number of threads (0 for auto)
constexpr Key OPENH264_ENC_THREADS = "openh264.threads";
// bool - Enable load balancing for multi-thread slicing
constexpr Key OPENH264_ENC_LOAD_BALANCING = "openh264.load_balancing";
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
// int32 - Loop filter alpha offset (-6 to 6)
constexpr Key OPENH264_ENC_LOOP_FILTER_ALPHA = "openh264.loop_filter_alpha";
// int32 - Loop filter beta offset (-6 to 6)
constexpr Key OPENH264_ENC_LOOP_FILTER_BETA = "openh264.loop_filter_beta";
// bool - Enable frame cropping flag
constexpr Key OPENH264_ENC_FRAME_CROPPING = "openh264.frame_cropping";
// bool - Enable lossless link mode
constexpr Key OPENH264_ENC_LOSSLESS_LINK = "openh264.lossless_link";
// bool - Enable rate-control overshoot fix
constexpr Key OPENH264_ENC_FIX_RC_OVERSHOOT = "openh264.fix_rc_overshoot";
// int32 - IDR bitrate ratio in percent
constexpr Key OPENH264_ENC_IDR_BITRATE_RATIO = "openh264.idr_bitrate_ratio";
// bool - Enable Y-plane PSNR stats
constexpr Key OPENH264_ENC_PSNR_Y = "openh264.psnr_y";
// bool - Enable U-plane PSNR stats
constexpr Key OPENH264_ENC_PSNR_U = "openh264.psnr_u";
// bool - Enable V-plane PSNR stats
constexpr Key OPENH264_ENC_PSNR_V = "openh264.psnr_v";
// int32 - Dependency-layer QP for spatial layer 0
constexpr Key OPENH264_ENC_LAYER_QP = "openh264.layer_qp";
// int32 - Slice mode (0: single, 1: fixed number, 2: raster, 3: size limited)
constexpr Key OPENH264_ENC_SLICE_MODE = "openh264.slice_mode";
// int32 - Slice number (for fixed number mode)
constexpr Key OPENH264_ENC_SLICE_NUM = "openh264.slice_num";
// array<int32> - Slice macroblock counts (for raster slice mode)
constexpr Key OPENH264_ENC_SLICE_MB_NUM = "openh264.slice_mb_num";
// int32 - Slice size constraint (for size limited mode)
constexpr Key OPENH264_ENC_SLICE_SIZE = "openh264.slice_size";
// bool - Write video signal type into VUI
constexpr Key OPENH264_ENC_VIDEO_SIGNAL_TYPE_PRESENT = "openh264.video_signal_type_present";
// int32 - VUI video format (0=component, 1=PAL, 2=NTSC, 3=SECAM, 4=MAC, 5=undefined)
constexpr Key OPENH264_ENC_VIDEO_FORMAT = "openh264.video_format";
// bool - Set VUI full-range flag
constexpr Key OPENH264_ENC_FULL_RANGE = "openh264.full_range";
// bool - Write color description into VUI
constexpr Key OPENH264_ENC_COLOR_DESCRIPTION_PRESENT = "openh264.color_description_present";
// int32 - VUI color primaries
constexpr Key OPENH264_ENC_COLOR_PRIMARIES = "openh264.color_primaries";
// int32 - VUI transfer characteristics
constexpr Key OPENH264_ENC_TRANSFER_CHARACTERISTICS = "openh264.transfer_characteristics";
// int32 - VUI color matrix
constexpr Key OPENH264_ENC_COLOR_MATRIX = "openh264.color_matrix";
// bool - Write aspect ratio into VUI
constexpr Key OPENH264_ENC_ASPECT_RATIO_PRESENT = "openh264.aspect_ratio_present";
// int32 - VUI sample aspect ratio idc
constexpr Key OPENH264_ENC_ASPECT_RATIO = "openh264.aspect_ratio";
// int32 - Extended SAR width (used when aspect_ratio is 255)
constexpr Key OPENH264_ENC_ASPECT_RATIO_EXT_WIDTH = "openh264.aspect_ratio_ext_width";
// int32 - Extended SAR height (used when aspect_ratio is 255)
constexpr Key OPENH264_ENC_ASPECT_RATIO_EXT_HEIGHT = "openh264.aspect_ratio_ext_height";

} // namespace openmedia
