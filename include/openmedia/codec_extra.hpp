#pragma once

#include <cstdint>
#include <openmedia/dictionary.hpp>

namespace openmedia {

// Generic (software codecs)
// int32 - Number of worker threads (0 = auto). Absent = codec default
constexpr Key CODEC_THREADS = "threads";
// bool - Minimize decoder output delay at the cost of frame-level parallelism
constexpr Key CODEC_DEC_LOW_DELAY = "low_delay";
// bool - Apply film grain synthesis on output frames (default true)
constexpr Key CODEC_DEC_APPLY_FILM_GRAIN = "apply_film_grain";

// dav1d
// int32 - Max frame delay (0 = auto, 1 = low latency). Overrides low_delay
constexpr Key DAV1D_DEC_MAX_FRAME_DELAY = "dav1d.max_frame_delay";
// int32 - Operating point to decode (0-31)
constexpr Key DAV1D_DEC_OPERATING_POINT = "dav1d.operating_point";
// bool - Output all spatial layers instead of only the highest one
constexpr Key DAV1D_DEC_ALL_LAYERS = "dav1d.all_layers";
// int32 - Max frame size in pixels (0 = unlimited)
constexpr Key DAV1D_DEC_FRAME_SIZE_LIMIT = "dav1d.frame_size_limit";

// AC-3 / E-AC-3
// float - Dynamic range control scale: 0 ignores the transmitted gain words,
// 1 applies them as transmitted (default)
constexpr Key AC3_DEC_DRC_SCALE = "ac3.drc_scale";

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

// VA-API
// int32 - Encoder speed/quality trade-off: 1 is the best quality, larger values
// are faster, up to what the driver reports (VAConfigAttribEncQualityRange).
// Absent or 0 = driver default
constexpr Key VAAPI_ENC_QUALITY_LEVEL = "vaapi.quality_level";

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

// WebP
// bool - Lossless compression (default false)
constexpr Key WEBP_ENC_LOSSLESS = "webp.lossless";
// float - Quality factor 0-100. Lossy: 0 is the smallest file, 100 the best
// picture. Lossless: the effort spent compressing, 0 fastest to 100 smallest.
// Overrides the rate control quality when present
constexpr Key WEBP_ENC_QUALITY = "webp.quality";
// int32 - Lossless preset level (0 fastest .. 9 smallest). Implies lossless and
// overrides quality and method
constexpr Key WEBP_ENC_LOSSLESS_PRESET = "webp.lossless_preset";
// int32 - Source-type preset (0=default, 1=picture, 2=photo, 3=drawing, 4=icon, 5=text)
constexpr Key WEBP_ENC_PRESET = "webp.preset";
// int32 - Compression method, speed/size trade-off (0 fast .. 6 slower-better)
constexpr Key WEBP_ENC_METHOD = "webp.method";
// int32 - Image hint, lossless only (0=default, 1=picture, 2=photo, 3=graph)
constexpr Key WEBP_ENC_IMAGE_HINT = "webp.image_hint";
// int32 - Near-lossless quality (0 = max loss, 100 = off). Lossless only
constexpr Key WEBP_ENC_NEAR_LOSSLESS = "webp.near_lossless";
// bool - Preserve the exact RGB values of fully transparent pixels
constexpr Key WEBP_ENC_EXACT = "webp.exact";
// int32 - Desired output size in bytes (0 = off). Takes precedence over quality
constexpr Key WEBP_ENC_TARGET_SIZE = "webp.target_size";
// float - Minimal distortion to try to achieve in dB (0 = off). Takes
// precedence over target_size
constexpr Key WEBP_ENC_TARGET_PSNR = "webp.target_psnr";
// int32 - Alpha plane compression (0 = none, 1 = WebP lossless)
constexpr Key WEBP_ENC_ALPHA_COMPRESSION = "webp.alpha_compression";
// int32 - Alpha predictive filtering (0 = none, 1 = fast, 2 = best)
constexpr Key WEBP_ENC_ALPHA_FILTERING = "webp.alpha_filtering";
// int32 - Alpha quality (0 smallest .. 100 lossless)
constexpr Key WEBP_ENC_ALPHA_QUALITY = "webp.alpha_quality";
// int32 - Number of segments to use (1-4)
constexpr Key WEBP_ENC_SEGMENTS = "webp.segments";
// int32 - Spatial noise shaping strength (0 = off .. 100 = maximum)
constexpr Key WEBP_ENC_SNS_STRENGTH = "webp.sns_strength";
// int32 - Deblocking filter strength (0 = off .. 100 = strongest)
constexpr Key WEBP_ENC_FILTER_STRENGTH = "webp.filter_strength";
// int32 - Filter sharpness (0 = off .. 7 = least sharp)
constexpr Key WEBP_ENC_FILTER_SHARPNESS = "webp.filter_sharpness";
// int32 - Filter type (0 = simple, 1 = strong)
constexpr Key WEBP_ENC_FILTER_TYPE = "webp.filter_type";
// bool - Auto-adjust the filter strength
constexpr Key WEBP_ENC_AUTOFILTER = "webp.autofilter";
// int32 - Number of entropy-analysis passes (1-10)
constexpr Key WEBP_ENC_PASS = "webp.pass";
// int32 - Preprocessing filter (0 = none, 1 = segment-smooth, 2 = pseudo-random dithering)
constexpr Key WEBP_ENC_PREPROCESSING = "webp.preprocessing";
// int32 - log2 of the token partition count (0-3)
constexpr Key WEBP_ENC_PARTITIONS = "webp.partitions";
// int32 - Quality degradation allowed to fit the 512k partition-0 limit (0-100)
constexpr Key WEBP_ENC_PARTITION_LIMIT = "webp.partition_limit";
// bool - Remap the parameters to match the size JPEG would have produced
constexpr Key WEBP_ENC_EMULATE_JPEG_SIZE = "webp.emulate_jpeg_size";
// bool - Reduce memory usage at the cost of CPU time
constexpr Key WEBP_ENC_LOW_MEMORY = "webp.low_memory";
// bool - Use the sharp (and slow) RGB->YUV conversion
constexpr Key WEBP_ENC_SHARP_YUV = "webp.sharp_yuv";
// int32 - Minimum permissible quality factor (0-100)
constexpr Key WEBP_ENC_QMIN = "webp.qmin";
// int32 - Maximum permissible quality factor (0-100)
constexpr Key WEBP_ENC_QMAX = "webp.qmax";

// PNG
// int32 - zlib compression level (0 = stored, 1 fastest .. 9 smallest)
constexpr Key PNG_ENC_COMPRESSION_LEVEL = "png.compression_level";
// int32 - zlib strategy (0 = default, 1 = filtered, 2 = Huffman only, 3 = RLE, 4 = fixed)
constexpr Key PNG_ENC_COMPRESSION_STRATEGY = "png.compression_strategy";
// int32 - zlib memory level (1-9)
constexpr Key PNG_ENC_COMPRESSION_MEM_LEVEL = "png.compression_mem_level";
// int32 - zlib window size in bits (8-15)
constexpr Key PNG_ENC_COMPRESSION_WINDOW_BITS = "png.compression_window_bits";
// int32 - Row filters libpng may choose between, as ORed PNG_FILTER_* bits:
// 0x00 none, 0x08 None, 0x10 Sub, 0x20 Up, 0x40 Avg, 0x80 Paeth, 0xF8 all
constexpr Key PNG_ENC_FILTERS = "png.filters";
// bool - Write the image interlaced (Adam7)
constexpr Key PNG_ENC_INTERLACE = "png.interlace";
// bool - Discard the alpha channel and write RGB instead of RGBA
constexpr Key PNG_ENC_STRIP_ALPHA = "png.strip_alpha";
// bool - Describe colour with cICP/mDCV/cLLI when the picture carries something
// sRGB cannot express, such as PQ, HLG, BT.2020 or DCI-P3 (default true)
constexpr Key PNG_ENC_COLOR_CHUNKS = "png.color_chunks";

} // namespace openmedia
