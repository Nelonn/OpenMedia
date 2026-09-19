#include "vaapi_codecs.hpp"

#include "av1_quant.hpp"
#include "vaapi_common.hpp"

#include <openmedia/codec_extra.hpp>
#include <util/bit_writer.hpp>
#include <util/color_codes.hpp>

#include <va/va.h>
#include <va/va_enc_av1.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>
#include <va/va_enc_vp9.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <optional>
#include <tuple>
#include <span>
#include <string_view>
#include <vector>

// Encoding goes IDR/key frame, then P frames that each predict from the one
// before, until the next key frame. No B frames: every picture is emitted as
// soon as it is encoded, in presentation order, so DTS equals PTS and a single
// reconstructed reference is all the state carried between frames.
//
// The H.264, HEVC and AV1 headers are written here and handed to the driver as
// packed headers wherever it accepts them -- which is what puts VUI colour
// information and timing into the stream; a driver that takes none writes its
// own. VP9 headers are always the driver's.

namespace openmedia {

namespace {

// ---------------------------------------------------------------------------
// Bitstream helpers
// ---------------------------------------------------------------------------

// Bits written so far, and how many of them count: a packed slice header may
// stop in the middle of a byte, and the driver carries on from there.
struct PackedBits {
  std::vector<uint8_t> bytes;
  size_t bits = 0;
};

class HeaderWriter {
public:
  HeaderWriter() : writer_(buffer_) {}
  HeaderWriter(const HeaderWriter&) = delete;
  HeaderWriter& operator=(const HeaderWriter&) = delete;

  auto w() -> BitWriter& { return writer_; }
  auto position() const -> size_t { return writer_.bitPosition(); }

  // rbsp_trailing_bits() for H.26x, trailing_bits() for AV1: a one, then zeros
  // to the byte boundary.
  void trailing() {
    writer_.bit(true);
    writer_.align();
  }

  auto finish() -> PackedBits {
    PackedBits out;
    out.bits = writer_.bitPosition();
    writer_.align();
    writer_.flush();
    out.bytes = std::move(buffer_);
    return out;
  }

private:
  std::vector<uint8_t> buffer_;
  BitWriter writer_;
};

// A NAL unit in Annex B form: a four byte start code, then the unit with
// emulation prevention bytes inserted. The partly written last byte of a slice
// header takes part in the scan too; an escape it causes that turns out not to
// be needed once the driver has filled the byte is harmless, because decoders
// drop every 0x03 that follows two zero bytes.
auto annexB(const PackedBits& nal) -> PackedBits {
  PackedBits out;
  out.bytes = {0, 0, 0, 1};
  const size_t bytes = (nal.bits + 7) / 8;
  size_t zeros = 0;
  size_t inserted = 0;
  for (size_t i = 0; i < bytes; ++i) {
    const uint8_t byte = nal.bytes[i];
    if (zeros >= 2 && byte <= 3) {
      out.bytes.push_back(3);
      ++inserted;
      zeros = 0;
    }
    out.bytes.push_back(byte);
    zeros = byte == 0 ? zeros + 1 : 0;
  }
  out.bits = 32 + nal.bits + inserted * 8;
  return out;
}

void append(PackedBits& into, const PackedBits& more) {
  // Only whole-byte units are concatenated.
  into.bytes.insert(into.bytes.end(), more.bytes.begin(), more.bytes.end());
  into.bits = into.bytes.size() * 8 - ((8 - more.bits % 8) % 8);
}

void writeLeb128Fixed(uint8_t* out, uint32_t value, uint32_t length) {
  for (uint32_t i = 0; i < length; ++i) {
    uint8_t byte = value & 0x7f;
    value >>= 7;
    if (i + 1 < length) byte |= 0x80;
    out[i] = byte;
  }
}

auto leb128(uint64_t value) -> std::vector<uint8_t> {
  std::vector<uint8_t> out;
  do {
    uint8_t byte = value & 0x7f;
    value >>= 7;
    if (value) byte |= 0x80;
    out.push_back(byte);
  } while (value);
  return out;
}

// ---------------------------------------------------------------------------
// Levels
// ---------------------------------------------------------------------------

struct StreamShape {
  uint32_t width = 0;
  uint32_t height = 0;
  double fps = 30.0;
  uint64_t bitrate = 0; // 0 when unknown (constant quality modes)
};

// Table A-1 (MaxMBPS, MaxFS, MaxBR in kbit/s).
auto guessH264Level(const StreamShape& s) -> uint8_t {
  struct Level { uint8_t idc; uint32_t mbps; uint32_t fs; uint32_t br; };
  static constexpr Level kLevels[] = {
      {10, 1485, 99, 64},          {11, 3000, 396, 192},        {12, 6000, 396, 384},
      {13, 11880, 396, 768},       {20, 11880, 396, 2000},      {21, 19800, 792, 4000},
      {22, 20250, 1620, 4000},     {30, 40500, 1620, 10000},    {31, 108000, 3600, 14000},
      {32, 216000, 5120, 20000},   {40, 245760, 8192, 20000},   {41, 245760, 8192, 50000},
      {42, 522240, 8704, 50000},   {50, 589824, 22080, 135000}, {51, 983040, 36864, 240000},
      {52, 2073600, 36864, 240000}, {60, 4177920, 139264, 240000}, {61, 8355840, 139264, 480000},
      {62, 16711680, 139264, 800000},
  };
  const uint64_t w = (s.width + 15) / 16;
  const uint64_t h = (s.height + 15) / 16;
  const uint64_t fs = w * h;
  const double mbps = static_cast<double>(fs) * s.fps;
  for (const auto& level : kLevels) {
    if (fs > level.fs || w * w > 8ull * level.fs || h * h > 8ull * level.fs) continue;
    if (mbps > level.mbps) continue;
    if (s.bitrate > static_cast<uint64_t>(level.br) * 1000) continue;
    return level.idc;
  }
  return 62;
}

// Table A.8 (MaxLumaPs, MaxLumaSr, main tier MaxBR in kbit/s); level_idc is
// 30 times the level number.
auto guessH265Level(const StreamShape& s) -> uint8_t {
  struct Level { uint8_t idc; uint64_t ps; uint64_t sr; uint64_t br; };
  static constexpr Level kLevels[] = {
      {30, 36864, 552960, 128},           {60, 122880, 3686400, 1500},
      {63, 245760, 7372800, 3000},        {90, 552960, 16588800, 6000},
      {93, 983040, 33177600, 10000},      {120, 2228224, 66846720, 12000},
      {123, 2228224, 133693440, 20000},   {150, 8912896, 267386880, 25000},
      {153, 8912896, 534773760, 40000},   {156, 8912896, 1069547520, 60000},
      {180, 35651584, 1069547520, 60000}, {183, 35651584, 2139095040, 120000},
      {186, 35651584, 4278190080, 240000},
  };
  const uint64_t ps = static_cast<uint64_t>(s.width) * s.height;
  const double sr = static_cast<double>(ps) * s.fps;
  for (const auto& level : kLevels) {
    const uint64_t max_dim = static_cast<uint64_t>(std::sqrt(8.0 * static_cast<double>(level.ps)));
    if (ps > level.ps || s.width > max_dim || s.height > max_dim) continue;
    if (sr > static_cast<double>(level.sr)) continue;
    if (s.bitrate > level.br * 1000) continue;
    return level.idc;
  }
  return 186;
}

// Annex A.3 (seq_level_idx, MaxPicSize, MaxHSize, MaxVSize, MaxDisplayRate,
// main tier MaxBitrate in Mbit/s).
auto guessAV1Level(const StreamShape& s) -> uint8_t {
  struct Level { uint8_t idx; uint64_t pic; uint32_t hsize; uint32_t vsize; uint64_t rate; double mbps; };
  static constexpr Level kLevels[] = {
      {0, 147456, 2048, 1152, 4423680, 1.5},          {1, 278784, 2816, 1584, 8363520, 3.0},
      {4, 665856, 4352, 2448, 19975680, 6.0},         {5, 1065024, 5504, 3096, 31950720, 10.0},
      {8, 2359296, 6144, 3456, 70778880, 12.0},       {9, 2359296, 6144, 3456, 141557760, 20.0},
      {12, 8912896, 8192, 4352, 267386880, 30.0},     {13, 8912896, 8192, 4352, 534773760, 40.0},
      {14, 8912896, 8192, 4352, 1069547520, 60.0},    {16, 35651584, 16384, 8704, 1069547520, 60.0},
      {17, 35651584, 16384, 8704, 2139095040, 100.0}, {18, 35651584, 16384, 8704, 4278190080, 160.0},
  };
  const uint64_t pic = static_cast<uint64_t>(s.width) * s.height;
  const double rate = static_cast<double>(pic) * s.fps;
  for (const auto& level : kLevels) {
    if (pic > level.pic || s.width > level.hsize || s.height > level.vsize) continue;
    if (rate > static_cast<double>(level.rate)) continue;
    if (static_cast<double>(s.bitrate) > level.mbps * 1e6) continue;
    return level.idx;
  }
  return 31; // no constraints
}

// ---------------------------------------------------------------------------
// Settings shared by the header writers
// ---------------------------------------------------------------------------

struct ColourInfo {
  uint32_t primaries = 2;
  uint32_t transfer = 2;
  uint32_t matrix = 2;
  bool full_range = false;
  bool range_known = false;

  auto described() const -> bool { return primaries != 2 || transfer != 2 || matrix != 2; }
};

struct Timing {
  uint32_t num = 30; // frames
  uint32_t den = 1;  // per this many seconds
};

// ---------------------------------------------------------------------------
// H.264 (ITU-T H.264 7.3.2)
// ---------------------------------------------------------------------------

struct H264Headers {
  uint8_t profile_idc = 100;
  bool constraint_set1 = false;
  bool constraint_set3 = false;
  bool constraint_set4 = true;
  bool constraint_set5 = true;
  uint8_t level_idc = 40;
  uint32_t width_mbs = 0;
  uint32_t height_mbs = 0;
  uint32_t crop_right = 0;  // in chroma sample pairs, as frame_crop_*_offset
  uint32_t crop_bottom = 0;
  uint32_t bit_depth = 8;
  uint32_t max_num_ref_frames = 1;
  bool cabac = true;
  bool transform_8x8 = true;
  int init_qp = 26;
  ColourInfo colour;
  Timing timing;

  static constexpr uint32_t kLog2MaxFrameNumMinus4 = 4; // frame_num wraps at 256

  auto highProfile() const -> bool { return profile_idc >= 100; }

  auto sps() const -> PackedBits {
    HeaderWriter h;
    auto& w = h.w();
    w.bits(0, 1).bits(3, 2).bits(7, 5); // nal_ref_idc 3, SPS
    w.bits(profile_idc, 8);
    w.bit(false).bit(constraint_set1).bit(false).bit(constraint_set3).bit(constraint_set4).bit(constraint_set5);
    w.bits(0, 2);
    w.bits(level_idc, 8);
    w.ue(0); // seq_parameter_set_id
    if (highProfile()) {
      w.ue(1); // chroma_format_idc
      w.ue(bit_depth - 8);
      w.ue(bit_depth - 8);
      w.bit(false); // qpprime_y_zero_transform_bypass_flag
      w.bit(false); // seq_scaling_matrix_present_flag
    }
    w.ue(kLog2MaxFrameNumMinus4);
    w.ue(2); // pic_order_cnt_type: output order is decoding order
    w.ue(max_num_ref_frames);
    w.bit(false); // gaps_in_frame_num_value_allowed_flag
    w.ue(width_mbs - 1);
    w.ue(height_mbs - 1);
    w.bit(true); // frame_mbs_only_flag
    w.bit(true); // direct_8x8_inference_flag
    const bool crop = crop_right || crop_bottom;
    w.bit(crop);
    if (crop) w.ue(0).ue(crop_right).ue(0).ue(crop_bottom);

    w.bit(true); // vui_parameters_present_flag
    w.bit(false); // aspect_ratio_info_present_flag
    w.bit(false); // overscan_info_present_flag
    const bool signal = colour.described() || colour.range_known;
    w.bit(signal);
    if (signal) {
      w.bits(5, 3); // video_format: unspecified
      w.bit(colour.full_range);
      w.bit(colour.described());
      if (colour.described()) w.bits(colour.primaries, 8).bits(colour.transfer, 8).bits(colour.matrix, 8);
    }
    w.bit(false); // chroma_loc_info_present_flag
    w.bit(true);  // timing_info_present_flag
    w.bits(timing.den, 32).bits(2 * timing.num, 32);
    w.bit(true); // fixed_frame_rate_flag
    w.bit(false); // nal_hrd_parameters_present_flag
    w.bit(false); // vcl_hrd_parameters_present_flag
    w.bit(false); // pic_struct_present_flag
    w.bit(true);  // bitstream_restriction_flag
    w.bit(true);  // motion_vectors_over_pic_boundaries_flag
    w.ue(0).ue(0).ue(15).ue(15); // max_bytes_per_pic_denom .. log2_max_mv_length_vertical
    w.ue(0);                     // max_num_reorder_frames
    w.ue(max_num_ref_frames);    // max_dec_frame_buffering
    h.trailing();
    return annexB(h.finish());
  }

  auto pps() const -> PackedBits {
    HeaderWriter h;
    auto& w = h.w();
    w.bits(0, 1).bits(3, 2).bits(8, 5); // PPS
    w.ue(0).ue(0);
    w.bit(cabac);
    w.bit(false); // bottom_field_pic_order_in_frame_present_flag
    w.ue(0);      // num_slice_groups_minus1
    w.ue(0).ue(0); // num_ref_idx_l0/l1_default_active_minus1
    w.bit(false).bits(0, 2); // weighted_pred_flag, weighted_bipred_idc
    w.se(init_qp - 26);
    w.se(0).se(0); // pic_init_qs_minus26, chroma_qp_index_offset
    w.bit(false); // deblocking_filter_control_present_flag
    w.bit(false); // constrained_intra_pred_flag
    w.bit(false); // redundant_pic_cnt_present_flag
    if (highProfile()) {
      w.bit(transform_8x8);
      w.bit(false); // pic_scaling_matrix_present_flag
      w.se(0);      // second_chroma_qp_index_offset
    }
    h.trailing();
    return annexB(h.finish());
  }

  // Everything up to slice_data(); CABAC alignment and the data itself are the
  // driver's.
  auto sliceHeader(bool idr, uint32_t frame_num, uint32_t idr_pic_id, int slice_qp_delta) const -> PackedBits {
    HeaderWriter h;
    auto& w = h.w();
    w.bits(0, 1).bits(idr ? 3 : 2, 2).bits(idr ? 5 : 1, 5);
    w.ue(0);            // first_mb_in_slice
    w.ue(idr ? 7 : 5);  // slice_type: all I / all P
    w.ue(0);            // pic_parameter_set_id
    w.bits(frame_num & ((1u << (kLog2MaxFrameNumMinus4 + 4)) - 1), kLog2MaxFrameNumMinus4 + 4);
    if (idr) w.ue(idr_pic_id);
    if (!idr) {
      w.bit(false); // num_ref_idx_active_override_flag
      w.bit(false); // ref_pic_list_modification_flag_l0
    }
    // dec_ref_pic_marking(): the sliding window does all there is to do.
    if (idr) w.bit(false).bit(false);
    else w.bit(false);
    if (cabac && !idr) w.ue(0); // cabac_init_idc
    w.se(slice_qp_delta);
    return annexB(h.finish());
  }
};

// ---------------------------------------------------------------------------
// H.265 (ITU-T H.265 7.3.2 / 7.3.6)
// ---------------------------------------------------------------------------

struct H265Headers {
  uint8_t profile_idc = 1;
  uint8_t level_idc = 120;
  uint32_t width = 0;  // pic_width_in_luma_samples
  uint32_t height = 0;
  uint32_t crop_right = 0; // conformance window, chroma units
  uint32_t crop_bottom = 0;
  uint32_t bit_depth = 8;
  uint32_t max_dec_pic_buffering_minus1 = 1;
  uint32_t log2_min_cb_minus3 = 0;
  uint32_t log2_diff_max_min_cb = 2;
  uint32_t log2_min_tb_minus2 = 0;
  uint32_t log2_diff_max_min_tb = 3;
  uint32_t max_th_depth_inter = 3;
  uint32_t max_th_depth_intra = 3;
  bool amp = true;
  bool sao = false;
  bool temporal_mvp = false;
  bool transform_skip = false;
  bool cu_qp_delta = false;
  uint32_t diff_cu_qp_delta_depth = 0;
  int init_qp = 26;
  ColourInfo colour;
  Timing timing;

  static constexpr uint32_t kLog2MaxPocLsbMinus4 = 8;

  static void nalHeader(BitWriter& w, uint32_t type) {
    w.bits(0, 1).bits(type, 6).bits(0, 6).bits(1, 3);
  }

  void profileTierLevel(BitWriter& w) const {
    w.bits(0, 2);    // general_profile_space
    w.bit(false);    // general_tier_flag
    w.bits(profile_idc, 5);
    for (uint32_t j = 0; j < 32; ++j) {
      // Main 10 decoders take Main streams too, and say so.
      const bool compatible = j == profile_idc || (profile_idc == 1 && j == 2);
      w.bit(compatible);
    }
    w.bit(true);  // general_progressive_source_flag
    w.bit(false); // general_interlaced_source_flag
    w.bit(true);  // general_non_packed_constraint_flag
    w.bit(true);  // general_frame_only_constraint_flag
    w.bits(0, 32).bits(0, 11); // general_reserved_zero_43bits
    w.bit(false);              // general_inbld_flag
    w.bits(level_idc, 8);
  }

  void timingInfo(BitWriter& w) const {
    w.bits(timing.den, 32).bits(timing.num, 32);
    w.bit(true); // poc_proportional_to_timing_flag
    w.ue(0);     // num_ticks_poc_diff_one_minus1
  }

  auto vps() const -> PackedBits {
    HeaderWriter h;
    auto& w = h.w();
    nalHeader(w, 32);
    w.bits(0, 4);           // vps_video_parameter_set_id
    w.bit(true).bit(true);  // base layer internal / available
    w.bits(0, 6);           // vps_max_layers_minus1
    w.bits(0, 3);           // vps_max_sub_layers_minus1
    w.bit(true);            // vps_temporal_id_nesting_flag
    w.bits(0xffff, 16);
    profileTierLevel(w);
    w.bit(false); // vps_sub_layer_ordering_info_present_flag
    w.ue(max_dec_pic_buffering_minus1).ue(0).ue(0);
    w.bits(0, 6); // vps_max_layer_id
    w.ue(0);      // vps_num_layer_sets_minus1
    w.bit(true);  // vps_timing_info_present_flag
    timingInfo(w);
    w.ue(0);      // vps_num_hrd_parameters
    w.bit(false); // vps_extension_flag
    h.trailing();
    return annexB(h.finish());
  }

  auto sps() const -> PackedBits {
    HeaderWriter h;
    auto& w = h.w();
    nalHeader(w, 33);
    w.bits(0, 4); // sps_video_parameter_set_id
    w.bits(0, 3); // sps_max_sub_layers_minus1
    w.bit(true);  // sps_temporal_id_nesting_flag
    profileTierLevel(w);
    w.ue(0); // sps_seq_parameter_set_id
    w.ue(1); // chroma_format_idc
    w.ue(width).ue(height);
    const bool crop = crop_right || crop_bottom;
    w.bit(crop);
    if (crop) w.ue(0).ue(crop_right).ue(0).ue(crop_bottom);
    w.ue(bit_depth - 8).ue(bit_depth - 8);
    w.ue(kLog2MaxPocLsbMinus4);
    w.bit(false); // sps_sub_layer_ordering_info_present_flag
    w.ue(max_dec_pic_buffering_minus1).ue(0).ue(0);
    w.ue(log2_min_cb_minus3).ue(log2_diff_max_min_cb);
    w.ue(log2_min_tb_minus2).ue(log2_diff_max_min_tb);
    w.ue(max_th_depth_inter).ue(max_th_depth_intra);
    w.bit(false); // scaling_list_enabled_flag
    w.bit(amp);
    w.bit(sao);
    w.bit(false); // pcm_enabled_flag
    w.ue(0);      // num_short_term_ref_pic_sets
    w.bit(false); // long_term_ref_pics_present_flag
    w.bit(temporal_mvp);
    w.bit(false); // strong_intra_smoothing_enabled_flag

    w.bit(true);  // vui_parameters_present_flag
    w.bit(false); // aspect_ratio_info_present_flag
    w.bit(false); // overscan_info_present_flag
    const bool signal = colour.described() || colour.range_known;
    w.bit(signal);
    if (signal) {
      w.bits(5, 3);
      w.bit(colour.full_range);
      w.bit(colour.described());
      if (colour.described()) w.bits(colour.primaries, 8).bits(colour.transfer, 8).bits(colour.matrix, 8);
    }
    w.bit(false); // chroma_loc_info_present_flag
    w.bit(false); // neutral_chroma_indication_flag
    w.bit(false); // field_seq_flag
    w.bit(false); // frame_field_info_present_flag
    w.bit(false); // default_display_window_flag
    w.bit(true);  // vui_timing_info_present_flag
    timingInfo(w);
    w.bit(false); // vui_hrd_parameters_present_flag
    w.bit(true);  // bitstream_restriction_flag
    w.bit(false); // tiles_fixed_structure_flag
    w.bit(true);  // motion_vectors_over_pic_boundaries_flag
    w.bit(true);  // restricted_ref_pic_lists_flag
    w.ue(0).ue(0).ue(0).ue(15).ue(15);

    w.bit(false); // sps_extension_present_flag
    h.trailing();
    return annexB(h.finish());
  }

  auto pps() const -> PackedBits {
    HeaderWriter h;
    auto& w = h.w();
    nalHeader(w, 34);
    w.ue(0).ue(0);
    w.bit(false); // dependent_slice_segments_enabled_flag
    w.bit(false); // output_flag_present_flag
    w.bits(0, 3); // num_extra_slice_header_bits
    w.bit(false); // sign_data_hiding_enabled_flag
    w.bit(false); // cabac_init_present_flag
    w.ue(0).ue(0);
    w.se(init_qp - 26);
    w.bit(false); // constrained_intra_pred_flag
    w.bit(transform_skip);
    w.bit(cu_qp_delta);
    if (cu_qp_delta) w.ue(diff_cu_qp_delta_depth);
    w.se(0).se(0); // pps_cb_qp_offset, pps_cr_qp_offset
    w.bit(false);  // pps_slice_chroma_qp_offsets_present_flag
    w.bit(false).bit(false); // weighted_pred_flag, weighted_bipred_flag
    w.bit(false);  // transquant_bypass_enabled_flag
    w.bit(false);  // tiles_enabled_flag
    w.bit(false);  // entropy_coding_sync_enabled_flag
    w.bit(true);   // pps_loop_filter_across_slices_enabled_flag
    w.bit(false);  // deblocking_filter_control_present_flag
    w.bit(false);  // pps_scaling_list_data_present_flag
    w.bit(false);  // lists_modification_present_flag
    w.ue(0);       // log2_parallel_merge_level_minus2
    w.bit(false);  // slice_segment_header_extension_present_flag
    w.bit(false);  // pps_extension_present_flag
    h.trailing();
    return annexB(h.finish());
  }

  // slice_type: 2 = I, 1 = P, 0 = B (a P frame in disguise for drivers that
  // only take B slices; both lists then hold the previous picture).
  auto sliceHeader(bool idr, uint32_t poc, int slice_type, int slice_qp_delta) const -> PackedBits {
    HeaderWriter h;
    auto& w = h.w();
    const uint32_t nal_type = idr ? 19 /* IDR_W_RADL */ : 1 /* TRAIL_R */;
    nalHeader(w, nal_type);
    w.bit(true); // first_slice_segment_in_pic_flag
    if (idr) w.bit(false); // no_output_of_prior_pics_flag
    w.ue(0);     // slice_pic_parameter_set_id
    w.ue(static_cast<uint32_t>(slice_type));
    if (!idr) {
      w.bits(poc & ((1u << (kLog2MaxPocLsbMinus4 + 4)) - 1), kLog2MaxPocLsbMinus4 + 4);
      w.bit(false); // short_term_ref_pic_set_sps_flag
      // st_ref_pic_set(0): the previous picture, one before this one.
      w.ue(1).ue(0);          // num_negative_pics, num_positive_pics
      w.ue(0).bit(true);      // delta_poc_s0_minus1, used_by_curr_pic_s0_flag
      if (temporal_mvp) w.bit(true); // slice_temporal_mvp_enabled_flag
    }
    if (sao) w.bit(true).bit(true); // slice_sao_luma_flag, slice_sao_chroma_flag
    if (slice_type != 2) {
      w.bit(false); // num_ref_idx_active_override_flag
      if (slice_type == 0) w.bit(false); // mvd_l1_zero_flag
      if (temporal_mvp && slice_type == 0) w.bit(true); // collocated_from_l0_flag
      w.ue(0);      // five_minus_max_num_merge_cand
    }
    w.se(slice_qp_delta);
    // pps_loop_filter_across_slices_enabled_flag is set and the deblocking
    // filter is on, so the slice says whether it filters across its edges.
    w.bit(false); // slice_loop_filter_across_slices_enabled_flag
    h.trailing(); // byte_alignment()
    return annexB(h.finish());
  }
};

// ---------------------------------------------------------------------------
// AV1 (AV1 bitstream specification 5.5 / 5.9)
// ---------------------------------------------------------------------------

struct AV1Tiles {
  uint32_t cols = 1;
  uint32_t rows = 1;
  uint32_t cols_log2 = 0;
  uint32_t rows_log2 = 0;
  uint32_t min_log2_cols = 0;
  uint32_t max_log2_cols = 0;
  uint32_t min_log2_rows = 0;
  uint32_t max_log2_rows = 0;
  uint16_t width_sb[64] = {};
  uint16_t height_sb[64] = {};
};

auto av1TileLog2(uint32_t blk, uint32_t target) -> uint32_t {
  uint32_t k = 0;
  while ((blk << k) < target) ++k;
  return k;
}

// The smallest uniform tiling tile_info() allows (5.9.15), which is what a
// single tile group without any tiling preference of our own comes down to.
auto av1Tiles(uint32_t width, uint32_t height) -> AV1Tiles {
  AV1Tiles t;
  const uint32_t mi_cols = 2 * ((width + 7) >> 3);
  const uint32_t mi_rows = 2 * ((height + 7) >> 3);
  const uint32_t sb_cols = (mi_cols + 15) >> 4; // 64x64 superblocks
  const uint32_t sb_rows = (mi_rows + 15) >> 4;
  constexpr uint32_t kSbSize = 6;
  const uint32_t max_tile_width_sb = 4096 >> kSbSize;
  const uint32_t max_tile_area_sb = (4096 * 2304) >> (2 * kSbSize);
  t.min_log2_cols = av1TileLog2(max_tile_width_sb, sb_cols);
  t.max_log2_cols = av1TileLog2(1, std::min(sb_cols, 64u));
  t.max_log2_rows = av1TileLog2(1, std::min(sb_rows, 64u));
  const uint32_t min_log2_tiles = std::max(t.min_log2_cols, av1TileLog2(max_tile_area_sb, sb_rows * sb_cols));

  t.cols_log2 = t.min_log2_cols;
  const uint32_t tile_width_sb = (sb_cols + (1u << t.cols_log2) - 1) >> t.cols_log2;
  t.cols = (sb_cols + tile_width_sb - 1) / tile_width_sb;
  for (uint32_t i = 0; i < t.cols; ++i) {
    t.width_sb[i] = static_cast<uint16_t>(i + 1 < t.cols ? tile_width_sb : sb_cols - i * tile_width_sb);
  }

  t.min_log2_rows = min_log2_tiles > t.cols_log2 ? min_log2_tiles - t.cols_log2 : 0;
  t.rows_log2 = t.min_log2_rows;
  const uint32_t tile_height_sb = (sb_rows + (1u << t.rows_log2) - 1) >> t.rows_log2;
  t.rows = (sb_rows + tile_height_sb - 1) / tile_height_sb;
  for (uint32_t i = 0; i < t.rows; ++i) {
    t.height_sb[i] = static_cast<uint16_t>(i + 1 < t.rows ? tile_height_sb : sb_rows - i * tile_height_sb);
  }
  return t;
}

// Where the driver has to patch the frame header when it runs the rate
// control itself, in bits from the start of the frame header OBU.
struct AV1FrameHeaderLayout {
  uint32_t qindex = 0;
  uint32_t loop_filter = 0;
  uint32_t cdef = 0;
  uint32_t cdef_size = 0;
};

// Loop filter and CDEF strengths as a frame header codes them. All zero when
// the driver's rate control writes its own in.
struct AV1Filters {
  uint8_t level[4] = {};
  uint8_t cdef_damping_minus_3 = 0;
  uint8_t cdef_bits = 0;
  uint8_t cdef_y_pri[8] = {};
  uint8_t cdef_y_sec[8] = {};
  uint8_t cdef_uv_pri[8] = {};
  uint8_t cdef_uv_sec[8] = {};
};

// With fixed quantisers nobody else picks the filters. The loop filter level
// is libaom's LPF_PICK_FROM_Q fit against the AC quantiser; the CDEF strength
// set is the one Chromium's VA-API AV1 encoder hands the hardware, which then
// chooses among the eight per superblock.
auto av1FiltersForQ(uint32_t qindex, bool key, uint32_t bit_depth) -> AV1Filters {
  AV1Filters f;
  const int64_t q = av1_quant::acQuant(static_cast<int>(qindex), bit_depth);
  const auto round2 = [](int64_t v, int n) { return v <= 0 ? int64_t {0} : (v + (int64_t {1} << (n - 1))) >> n; };
  int64_t level = 0;
  if (bit_depth > 8) {
    level = round2(q * 20723 + 4060632, 20) - (key ? 4 : 0);
  } else {
    level = key ? round2(q * 17563 - 421574, 18) : round2(q * 12034 + 650707, 18);
  }
  const auto clamped = static_cast<uint8_t>(std::clamp<int64_t>(level, 0, 63));
  for (auto& l : f.level) l = clamped;

  static constexpr uint8_t kPrimary[8] = {9, 12, 0, 6, 2, 4, 1, 2};
  static constexpr uint8_t kSecondary[8] = {0, 2, 0, 0, 0, 1, 0, 1};
  f.cdef_damping_minus_3 = 5 - 3;
  f.cdef_bits = 3;
  for (int i = 0; i < 8; ++i) {
    f.cdef_y_pri[i] = f.cdef_uv_pri[i] = kPrimary[i];
    f.cdef_y_sec[i] = f.cdef_uv_sec[i] = kSecondary[i];
  }
  return f;
}

struct AV1Headers {
  uint8_t level_idx = 8;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t bit_depth = 8;
  bool enable_cdef = false;
  uint32_t obu_size_bytes = 4; // fixed length of obu_size, so the driver can rewrite it
  uint32_t tile_size_bytes_minus1 = 3;
  bool tx_mode_select = true;
  AV1Tiles tiles;
  ColourInfo colour;

  static constexpr uint32_t kOrderHintBits = 8;

  auto obu(uint8_t type, const PackedBits& payload) const -> PackedBits {
    PackedBits out;
    out.bytes.push_back(static_cast<uint8_t>((type << 3) | 0x02)); // obu_has_size_field
    out.bytes.resize(1 + obu_size_bytes);
    writeLeb128Fixed(out.bytes.data() + 1, static_cast<uint32_t>(payload.bytes.size()), obu_size_bytes);
    out.bytes.insert(out.bytes.end(), payload.bytes.begin(), payload.bytes.end());
    out.bits = out.bytes.size() * 8;
    return out;
  }

  auto sequenceHeader() const -> PackedBits {
    HeaderWriter h;
    auto& w = h.w();
    w.bits(0, 3);  // seq_profile: Main
    w.bit(false);  // still_picture
    w.bit(false);  // reduced_still_picture_header
    w.bit(false);  // timing_info_present_flag
    w.bit(false);  // initial_display_delay_present_flag
    w.bits(0, 5);  // operating_points_cnt_minus_1
    w.bits(0, 12); // operating_point_idc[0]
    w.bits(level_idx, 5);
    if (level_idx > 7) w.bit(false); // seq_tier[0]
    const uint32_t width_bits = static_cast<uint32_t>(std::bit_width(width - 1)) > 0
                                    ? static_cast<uint32_t>(std::bit_width(width - 1)) : 1u;
    const uint32_t height_bits = static_cast<uint32_t>(std::bit_width(height - 1)) > 0
                                     ? static_cast<uint32_t>(std::bit_width(height - 1)) : 1u;
    w.bits(width_bits - 1, 4).bits(height_bits - 1, 4);
    w.bits(width - 1, width_bits).bits(height - 1, height_bits);
    w.bit(false); // frame_id_numbers_present_flag
    w.bit(false); // use_128x128_superblock
    w.bit(false); // enable_filter_intra
    w.bit(false); // enable_intra_edge_filter
    w.bit(false); // enable_interintra_compound
    w.bit(false); // enable_masked_compound
    w.bit(false); // enable_warped_motion
    w.bit(false); // enable_dual_filter
    w.bit(true);  // enable_order_hint
    w.bit(false); // enable_jnt_comp
    w.bit(false); // enable_ref_frame_mvs
    w.bit(false); // seq_choose_screen_content_tools
    w.bit(false); // seq_force_screen_content_tools -> integer mv is then SELECT
    w.bits(kOrderHintBits - 1, 3);
    w.bit(false); // enable_superres
    w.bit(enable_cdef);
    w.bit(false); // enable_restoration
    // color_config()
    w.bit(bit_depth > 8); // high_bitdepth
    w.bit(false);         // mono_chrome
    w.bit(colour.described());
    if (colour.described()) w.bits(colour.primaries, 8).bits(colour.transfer, 8).bits(colour.matrix, 8);
    w.bit(colour.full_range); // color_range
    w.bits(0, 2);             // chroma_sample_position: unknown
    w.bit(false);             // separate_uv_delta_q
    w.bit(false);             // film_grain_params_present
    h.trailing();
    return obu(1, h.finish());
  }

  // uncompressed_header() for the two kinds of frame this encoder makes: a
  // shown key frame, and a shown inter frame predicting from slot 0 that
  // refreshes slot 0.
  auto frameHeader(bool key, uint32_t order_hint, uint32_t base_q_idx, const AV1Filters& filters,
                   AV1FrameHeaderLayout& layout) const -> PackedBits {
    HeaderWriter h;
    auto& w = h.w();
    const size_t base = 8u * (1u + obu_size_bytes);
    w.bit(false);                  // show_existing_frame
    w.bits(key ? 0 : 1, 2);        // frame_type
    w.bit(true);                   // show_frame
    if (!key) w.bit(false);        // error_resilient_mode (implied for shown key frames)
    w.bit(false);                  // disable_cdf_update
    w.bit(false);                  // frame_size_override_flag
    w.bits(order_hint & ((1u << kOrderHintBits) - 1), kOrderHintBits);
    if (!key) w.bits(0, 3);        // primary_ref_frame: slot 0 through LAST
    if (!key) w.bits(0x01, 8);     // refresh_frame_flags
    if (!key) {
      w.bit(false); // frame_refs_short_signaling
      for (int i = 0; i < 7; ++i) w.bits(0, 3); // ref_frame_idx: all slot 0
    }
    w.bit(false); // render_and_frame_size_different_flag
    if (!key) {
      w.bit(false); // allow_high_precision_mv
      w.bit(false); // is_filter_switchable
      w.bits(0, 2); // interpolation_filter: EIGHTTAP
      w.bit(false); // is_motion_mode_switchable
    }
    w.bit(false); // disable_frame_end_update_cdf

    // tile_info()
    w.bit(true); // uniform_tile_spacing_flag
    if (tiles.min_log2_cols < tiles.max_log2_cols) w.bit(false); // cols_log2 stays at the minimum
    if (tiles.min_log2_rows < tiles.max_log2_rows) w.bit(false);
    if (tiles.cols_log2 > 0 || tiles.rows_log2 > 0) {
      w.bits(0, tiles.cols_log2 + tiles.rows_log2); // context_update_tile_id
      w.bits(tile_size_bytes_minus1, 2);
    }

    // quantization_params()
    layout.qindex = static_cast<uint32_t>(base + h.position());
    w.bits(base_q_idx, 8);
    w.bit(false);             // DeltaQYDc coded
    w.bit(false).bit(false);  // DeltaQUDc, DeltaQUAc coded
    w.bit(false);             // using_qmatrix
    w.bit(false);             // segmentation_enabled
    if (base_q_idx > 0) w.bit(false); // delta_q_present

    // loop_filter_params(); the driver rewrites it when it controls rate.
    layout.loop_filter = static_cast<uint32_t>(base + h.position());
    w.bits(filters.level[0], 6).bits(filters.level[1], 6);
    if (filters.level[0] || filters.level[1]) w.bits(filters.level[2], 6).bits(filters.level[3], 6);
    w.bits(0, 3);  // loop_filter_sharpness
    w.bit(false);  // loop_filter_delta_enabled

    if (enable_cdef) {
      layout.cdef = static_cast<uint32_t>(base + h.position());
      w.bits(filters.cdef_damping_minus_3, 2);
      w.bits(filters.cdef_bits, 2);
      for (uint32_t i = 0; i < (1u << filters.cdef_bits); ++i) {
        w.bits(filters.cdef_y_pri[i], 4).bits(filters.cdef_y_sec[i], 2);
        w.bits(filters.cdef_uv_pri[i], 4).bits(filters.cdef_uv_sec[i], 2);
      }
      layout.cdef_size = static_cast<uint32_t>(base + h.position()) - layout.cdef;
    }

    w.bit(tx_mode_select); // tx_mode_select
    if (!key) w.bit(false); // reference_select
    w.bit(false);           // reduced_tx_set
    if (!key) {
      for (int i = 0; i < 7; ++i) w.bit(false); // is_global
    }
    h.trailing();
    return obu(3, h.finish());
  }
};

// ---------------------------------------------------------------------------
// HDR metadata
// ---------------------------------------------------------------------------

// SEI payloads for mastering display colour volume (137) and content light
// level (144); both codecs code them the same, in the units the Om structs use.
auto hdrSeiPayloads(const OMMasteringDisplayMetadata& md, const OMContentLightLevel& cll) -> std::vector<uint8_t> {
  std::vector<uint8_t> out;
  const auto put16 = [&](uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
  };
  const auto put32 = [&](uint32_t v) {
    put16(v >> 16);
    put16(v & 0xffff);
  };
  if (md.has_value) {
    out.push_back(137);
    out.push_back(24);
    for (int c = 0; c < 3; ++c) {
      put16(md.display_primaries[c][0]);
      put16(md.display_primaries[c][1]);
    }
    put16(md.white_point[0]);
    put16(md.white_point[1]);
    put32(md.max_display_mastering_luminance);
    put32(md.min_display_mastering_luminance);
  }
  if (cll.has_value) {
    out.push_back(144);
    out.push_back(4);
    put16(cll.max_content_light_level);
    put16(cll.max_pic_average_light_level);
  }
  return out;
}

auto hdrSeiNal(bool hevc, const OMMasteringDisplayMetadata& md, const OMContentLightLevel& cll) -> std::vector<uint8_t> {
  const auto payloads = hdrSeiPayloads(md, cll);
  if (payloads.empty()) return {};
  HeaderWriter h;
  auto& w = h.w();
  if (hevc) {
    H265Headers::nalHeader(w, 39); // PREFIX_SEI
  } else {
    w.bits(0, 1).bits(0, 2).bits(6, 5);
  }
  for (uint8_t b : payloads) w.bits(b, 8);
  h.trailing();
  return annexB(h.finish()).bytes;
}

// OBU_METADATA for the same, in AV1's units (0.16 chromaticity in RGB order,
// 24.8 / 18.14 luminance).
auto av1HdrMetadata(const OMMasteringDisplayMetadata& md, const OMContentLightLevel& cll) -> std::vector<uint8_t> {
  std::vector<uint8_t> out;
  const auto emit = [&](uint8_t type, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> payload = leb128(type);
    payload.insert(payload.end(), body.begin(), body.end());
    payload.push_back(0x80); // trailing_bits
    out.push_back(static_cast<uint8_t>((5 << 3) | 0x02));
    const auto size = leb128(payload.size());
    out.insert(out.end(), size.begin(), size.end());
    out.insert(out.end(), payload.begin(), payload.end());
  };
  const auto be = [](std::vector<uint8_t>& v, uint32_t value, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) v.push_back(static_cast<uint8_t>(value >> (8 * i)));
  };
  if (cll.has_value) {
    std::vector<uint8_t> body;
    be(body, cll.max_content_light_level, 2);
    be(body, cll.max_pic_average_light_level, 2);
    emit(1, body);
  }
  if (md.has_value) {
    std::vector<uint8_t> body;
    const auto chroma = [](uint32_t v) { return std::min<uint32_t>((v * 65536u + 25000u) / 50000u, 65535u); };
    constexpr int kSeiIndexForRgb[3] = {2, 0, 1}; // the SEI order is green, blue, red
    for (int c = 0; c < 3; ++c) {
      be(body, chroma(md.display_primaries[kSeiIndexForRgb[c]][0]), 2);
      be(body, chroma(md.display_primaries[kSeiIndexForRgb[c]][1]), 2);
    }
    be(body, chroma(md.white_point[0]), 2);
    be(body, chroma(md.white_point[1]), 2);
    be(body, static_cast<uint32_t>((static_cast<uint64_t>(md.max_display_mastering_luminance) * 256 + 5000) / 10000), 4);
    be(body, static_cast<uint32_t>((static_cast<uint64_t>(md.min_display_mastering_luminance) * 16384 + 5000) / 10000), 4);
    emit(2, body);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Rate control
// ---------------------------------------------------------------------------

struct RateControl {
  uint32_t va_mode = VA_RC_CQP;
  uint32_t bits_per_second = 0; // peak for VBR
  uint32_t target_percentage = 100;
  uint32_t quality = 0;         // ICQ / QVBR quality factor
  int qp_i = 26;                // CQP, codec-native scale
  int qp_p = 28;
  uint32_t min_qp = 0;
  uint32_t max_qp = 0;
  uint32_t hrd_size = 0;
  uint32_t hrd_initial = 0;

  auto hasBitrate() const -> bool { return va_mode != VA_RC_CQP && va_mode != VA_RC_ICQ; }
};

// ---------------------------------------------------------------------------
// The encoder
// ---------------------------------------------------------------------------

class VAAPIEncoder final : public Encoder {
public:
  VAAPIEncoder() = default;
  ~VAAPIEncoder() override { release(); }

  auto configure(const EncoderOptions& options) -> OMError override {
    release();
    codec_id_ = options.format.codec_id;
    if (codec_id_ != OM_CODEC_H264 && codec_id_ != OM_CODEC_H265 && codec_id_ != OM_CODEC_VP9 &&
        codec_id_ != OM_CODEC_AV1) {
      return OM_CODEC_NOT_SUPPORTED;
    }
    auto& libva = LibVA::getInstance();
    if (!libva.load()) return OM_CODEC_HWACCEL_FAILED;

    width_ = options.format.video.width ? options.format.video.width : options.video_format.width;
    height_ = options.format.video.height ? options.format.video.height : options.video_format.height;
    if (width_ == 0 || height_ == 0) return OM_CODEC_INVALID_PARAMS;

    switch (options.video_format.format) {
      case OM_FORMAT_NV12:
      case OM_FORMAT_YUV420P:
      case OM_FORMAT_YUVJ420P: bit_depth_ = 8; break;
      case OM_FORMAT_P010:
      case OM_FORMAT_YUV420P10: bit_depth_ = 10; break;
      case OM_FORMAT_UNKNOWN: bit_depth_ = 8; break;
      default:
        return fail(OM_CODEC_NOT_SUPPORTED, "input pixel format is not 4:2:0 8/10-bit");
    }
    input_format_ = options.video_format;

    if (options.hw_device && options.hw_device->type == HWDeviceType::VAAPI && options.hw_device->context) {
      display_ = static_cast<OMVAAPIContext*>(options.hw_device->context)->display;
    } else {
      owned_display_ = vaapi::openDisplay();
      if (!owned_display_) return OM_CODEC_HWACCEL_FAILED;
      display_ = owned_display_->display;
    }

    if (const OMError error = pickProfile(options.format.profile); error != OM_SUCCESS) return error;
    queryCapabilities();

    // Timing and colour for the headers.
    const auto& video = options.format.video;
    if (video.framerate.num > 0 && video.framerate.den > 0) {
      timing_.num = static_cast<uint32_t>(video.framerate.num);
      timing_.den = static_cast<uint32_t>(video.framerate.den);
    }
    colour_.primaries = color_codes::codeFromPrimaries(video.color_primaries);
    colour_.transfer = color_codes::codeFromTransfer(video.transfer_char);
    colour_.matrix = color_codes::matrixFromColorSpace(video.color_space);
    colour_.full_range = video.color_range == OM_COLOR_RANGE_FULL || input_format_.format == OM_FORMAT_YUVJ420P;
    colour_.range_known = video.color_range != OM_COLOR_RANGE_UNSPECIFIED || input_format_.format == OM_FORMAT_YUVJ420P;
    mastering_display_ = video.mastering_display.has_value ? video.mastering_display : input_format_.mastering_display;
    content_light_level_ = video.content_light_level.has_value ? video.content_light_level : input_format_.content_light_level;

    quality_level_ = 0;
    if (const auto* value = options.extra.get(VAAPI_ENC_QUALITY_LEVEL)) {
      if (const auto n = value->getInt64()) quality_level_ = static_cast<uint32_t>(std::max<int64_t>(*n, 0));
    }

    gop_ = 0;
    if (const auto* value = options.extra.get("gop_size")) {
      if (const auto n = value->getInt64()) gop_ = static_cast<uint32_t>(std::max<int64_t>(*n, 0));
    }
    if (gop_ == 0) gop_ = std::max<uint32_t>(1, (timing_.num + timing_.den - 1) / timing_.den * 2);
    if (max_refs_l0_ == 0) gop_ = 1; // the driver only does intra frames

    rc_ = chooseRateControl(options.rate_control);

    if (const OMError error = createSession(); error != OM_SUCCESS) {
      release();
      return error;
    }
    setupHeaders(options.format.level);

    log(OM_CATEGORY_HARDWARE, OM_LEVEL_INFO,
        "[VAAPI] encoding {} {}x{} {}-bit, {}, {} rate control, key frame every {} frames, packed headers 0x{:x}",
        codecName(), width_, height_, bit_depth_, entrypoint_ == VAEntrypointEncSliceLP ? "low power" : "full",
        rcName(rc_.va_mode), gop_, packed_headers_);
    initialized_ = true;
    return OM_SUCCESS;
  }

  auto getInfo() -> EncodingInfo override {
    EncodingInfo info = {};
    info.extradata = extradata_;
    info.mastering_display = mastering_display_;
    info.content_light_level = content_light_level_;
    return info;
  }

  auto encode(const Frame& frame) -> Result<std::vector<Packet>, OMError> override {
    if (!initialized_) return Err(OM_COMMON_NOT_INITIALIZED);
    std::vector<Packet> packets;
    // One picture is kept in flight: this call submits its frame and collects
    // the one before, so the GPU encodes while the caller prepares the next.
    // An empty frame drains what is left.
    std::optional<InFlight> previous = std::exchange(in_flight_, std::nullopt);
    const auto* pic = std::get_if<Picture>(&frame.data);
    bool submitted = true;
    if (pic && pic->width != 0) {
      const bool key = frames_since_key_ == 0 || frames_since_key_ >= gop_ || pic->is_keyframe || !have_reference_;
      submitted = submit(frame, *pic, key);
    }
    if (previous) {
      if (auto packet = collect(*previous)) packets.push_back(std::move(*packet));
    }
    if (!submitted && packets.empty()) return Err(OM_CODEC_ENCODE_FAILED);
    return Ok(std::move(packets));
  }

  auto updateBitrate(const RateControlParams& params) -> OMError override {
    if (!initialized_) return OM_COMMON_NOT_INITIALIZED;
    RateControl next = chooseRateControl(params);
    if (next.va_mode != rc_.va_mode) {
      // The mode is part of the VA config; only the numbers can change live.
      log(OM_CATEGORY_HARDWARE, OM_LEVEL_WARNING,
          "[VAAPI] rate control mode cannot change mid-stream; keeping {}", rcName(rc_.va_mode));
      next.va_mode = rc_.va_mode;
    }
    rc_ = next;
    rc_changed_ = true;
    return OM_SUCCESS;
  }

private:
  template<typename... Args>
  auto fail(OMError error, std::format_string<Args...> fmt, Args&&... args) -> OMError {
    log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "[VAAPI] {} encoder: {}", codecName(),
        std::format(fmt, std::forward<Args>(args)...));
    return error;
  }

  auto codecName() const -> std::string_view {
    switch (codec_id_) {
      case OM_CODEC_H264: return "H.264";
      case OM_CODEC_H265: return "H.265";
      case OM_CODEC_VP9: return "VP9";
      case OM_CODEC_AV1: return "AV1";
      default: return "?";
    }
  }

  static auto rcName(uint32_t mode) -> std::string_view {
    switch (mode) {
      case VA_RC_CQP: return "CQP";
      case VA_RC_CBR: return "CBR";
      case VA_RC_VBR: return "VBR";
      case VA_RC_ICQ: return "ICQ";
      case VA_RC_QVBR: return "QVBR";
      case VA_RC_AVBR: return "AVBR";
      default: return "?";
    }
  }

  // ---------------------------------------------------------------------------
  // Capabilities
  // ---------------------------------------------------------------------------

  auto pickProfile(OMProfile requested) -> OMError {
    const auto profiles = vaapi::queryProfiles(display_);
    const auto has = [&](VAProfile p) { return std::find(profiles.begin(), profiles.end(), p) != profiles.end(); };
    std::vector<VAProfile> candidates;
    if (codec_id_ == OM_CODEC_H264) {
      if (bit_depth_ > 8) {
        candidates = {VAProfileH264High10};
      } else if (requested == OM_PROFILE_H264_BASELINE || requested == OM_PROFILE_H264_CONSTRAINED_BASELINE) {
        candidates = {VAProfileH264ConstrainedBaseline, VAProfileH264Main, VAProfileH264High};
      } else if (requested == OM_PROFILE_H264_MAIN) {
        candidates = {VAProfileH264Main, VAProfileH264High};
      } else {
        candidates = {VAProfileH264High, VAProfileH264Main, VAProfileH264ConstrainedBaseline};
      }
    } else if (codec_id_ == OM_CODEC_H265) {
      candidates = {bit_depth_ > 8 ? VAProfileHEVCMain10 : VAProfileHEVCMain};
    } else if (codec_id_ == OM_CODEC_VP9) {
      candidates = {bit_depth_ > 8 ? VAProfileVP9Profile2 : VAProfileVP9Profile0};
    } else {
      candidates = {VAProfileAV1Profile0};
    }
    for (VAProfile p : candidates) {
      if (!has(p)) continue;
      // Low power (fixed function) encoding is all some GPUs have for newer
      // codecs; the full path is preferred where both exist.
      for (VAEntrypoint e : {VAEntrypointEncSlice, VAEntrypointEncSliceLP}) {
        if (vaapi::supportsEntrypoint(display_, p, e)) {
          profile_ = p;
          entrypoint_ = e;
          return OM_SUCCESS;
        }
      }
    }
    log(OM_CATEGORY_HARDWARE, OM_LEVEL_INFO, "[VAAPI] the driver cannot encode {} at {}-bit", codecName(), bit_depth_);
    return OM_CODEC_NOT_SUPPORTED;
  }

  auto attribute(VAConfigAttribType type) const -> uint32_t {
    VAConfigAttrib attrib = {};
    attrib.type = type;
    if (LibVA::getInstance().vaGetConfigAttributes(display_, profile_, entrypoint_, &attrib, 1) != VA_STATUS_SUCCESS)
      return VA_ATTRIB_NOT_SUPPORTED;
    return attrib.value;
  }

  void queryCapabilities() {
    rc_modes_ = attribute(VAConfigAttribRateControl);
    if (rc_modes_ == VA_ATTRIB_NOT_SUPPORTED) rc_modes_ = VA_RC_CQP;

    // All or nothing, as Chromium does it: a slice header of ours behind a
    // parameter set the driver wrote itself (or the other way round) only
    // agrees by luck -- the picture order count type alone differs between
    // encoders.
    const uint32_t packed_attr = attribute(VAConfigAttribEncPackedHeaders);
    const uint32_t packed = packed_attr == VA_ATTRIB_NOT_SUPPORTED ? 0 : packed_attr;
    packed_headers_ = 0;
    switch (codec_id_) {
      case OM_CODEC_H264:
      case OM_CODEC_H265: {
        constexpr uint32_t kNeeded = VA_ENC_PACKED_HEADER_SEQUENCE | VA_ENC_PACKED_HEADER_SLICE;
        // With the picture flag too, the PPS goes in a packed header of its own.
        if ((packed & kNeeded) == kNeeded) packed_headers_ = kNeeded | (packed & VA_ENC_PACKED_HEADER_PICTURE);
        break;
      }
      case OM_CODEC_AV1: {
        constexpr uint32_t kNeeded = VA_ENC_PACKED_HEADER_SEQUENCE | VA_ENC_PACKED_HEADER_PICTURE;
        if ((packed & kNeeded) == kNeeded) packed_headers_ = kNeeded;
        break;
      }
      default: break;
    }

    quality_range_ = attribute(VAConfigAttribEncQualityRange);
    if (quality_range_ == VA_ATTRIB_NOT_SUPPORTED) quality_range_ = 0;

    const uint32_t refs = attribute(VAConfigAttribEncMaxRefFrames);
    max_refs_l0_ = refs == VA_ATTRIB_NOT_SUPPORTED ? 1 : (refs & 0xffff);
    const uint32_t refs_l1 = refs == VA_ATTRIB_NOT_SUPPORTED ? 0 : ((refs >> 16) & 0xffff);

    // Some fixed-function HEVC encoders have no P slices at all and want a B
    // slice with the same picture in both lists instead.
    p_as_b_ = false;
    const uint32_t direction = attribute(VAConfigAttribPredictionDirection);
    if (direction != VA_ATTRIB_NOT_SUPPORTED && (direction & VA_PREDICTION_DIRECTION_BI_NOT_EMPTY) &&
        max_refs_l0_ > 0 && refs_l1 > 0) {
      p_as_b_ = true;
    }

    hevc_features_ = codec_id_ == OM_CODEC_H265 ? attribute(VAConfigAttribEncHEVCFeatures) : VA_ATTRIB_NOT_SUPPORTED;
    hevc_block_sizes_ = codec_id_ == OM_CODEC_H265 ? attribute(VAConfigAttribEncHEVCBlockSizes) : VA_ATTRIB_NOT_SUPPORTED;
    av1_ext2_ = codec_id_ == OM_CODEC_AV1 ? attribute(VAConfigAttribEncAV1Ext2) : VA_ATTRIB_NOT_SUPPORTED;
  }

  // Maps the generic rate control onto what the driver offers, falling back
  // towards the modes every driver has.
  auto chooseRateControl(const RateControlParams& params) const -> RateControl {
    RateControl rc;
    const auto supports = [&](uint32_t mode) { return (rc_modes_ & mode) != 0; };
    const bool qindex_scale = codec_id_ == OM_CODEC_VP9 || codec_id_ == OM_CODEC_AV1;
    // VP9 and AV1 quantisers run to 255. Values up to 63 are taken to be on
    // the familiar 0..51/63 scale and spread over that range.
    const auto nativeQp = [&](int qp) {
      if (!qindex_scale) return std::clamp(qp, 1, 51);
      return qp <= 63 ? std::clamp(qp * 4, 1, 255) : std::clamp(qp, 1, 255);
    };
    const auto setBitrate = [&](const BitrateParams& b, bool peak) {
      const uint64_t target = static_cast<uint64_t>(std::max<int64_t>(b.target_bitrate, 1));
      const uint64_t max = peak && b.max_bitrate ? static_cast<uint64_t>(std::max(*b.max_bitrate, b.target_bitrate))
                                                 : (peak ? target * 3 / 2 : target);
      rc.bits_per_second = static_cast<uint32_t>(std::min<uint64_t>(max, UINT32_MAX));
      rc.target_percentage = static_cast<uint32_t>(std::clamp<uint64_t>(target * 100 / std::max<uint64_t>(max, 1), 1, 100));
      const uint64_t buffer = b.vbv ? static_cast<uint64_t>(std::max<int64_t>(b.vbv->buffer_size, 1)) : max;
      rc.hrd_size = static_cast<uint32_t>(std::min<uint64_t>(buffer, UINT32_MAX));
      rc.hrd_initial = b.vbv && b.vbv->buffer_initial_fullness
                           ? static_cast<uint32_t>(std::min<int64_t>(*b.vbv->buffer_initial_fullness, rc.hrd_size))
                           : rc.hrd_size / 4 * 3;
    };
    const auto cqp = [&](int qp_i, int qp_p) {
      rc.va_mode = VA_RC_CQP;
      rc.qp_i = nativeQp(qp_i);
      rc.qp_p = nativeQp(qp_p);
    };
    const auto quality = [&](float value) {
      // Constant quality; ICQ where the driver has it, else fixed quantisers.
      const int q = value > 0 ? static_cast<int>(std::lround(value)) : 23;
      if (supports(VA_RC_ICQ)) {
        rc.va_mode = VA_RC_ICQ;
        rc.quality = static_cast<uint32_t>(std::clamp(q, 1, 51));
      } else {
        cqp(q, q + 2);
      }
    };
    const auto vbr = [&](const BitrateParams& b) {
      setBitrate(b, true);
      rc.va_mode = supports(VA_RC_VBR) ? VA_RC_VBR : (supports(VA_RC_CBR) ? VA_RC_CBR : VA_RC_CQP);
    };
    const auto cbr = [&](const BitrateParams& b) {
      setBitrate(b, false);
      rc.va_mode = supports(VA_RC_CBR) ? VA_RC_CBR : (supports(VA_RC_VBR) ? VA_RC_VBR : VA_RC_CQP);
    };

    switch (params.getMode()) {
      case RateControlMode::CQP: {
        const auto& p = std::get<CqpParams>(params.params);
        cqp(p.qp_i, p.qp_p);
        break;
      }
      case RateControlMode::CBR: cbr(std::get<CbrParams>(params.params).bitrate); break;
      case RateControlMode::VBR: vbr(std::get<VbrParams>(params.params).bitrate); break;
      case RateControlMode::HQCBR: cbr(std::get<HqcbrParams>(params.params).bitrate); break;
      case RateControlMode::HQVBR: vbr(std::get<HqvbrParams>(params.params).bitrate); break;
      case RateControlMode::VBR_LAT: vbr(std::get<VbrLatParams>(params.params).bitrate); break;
      case RateControlMode::ABR: {
        BitrateParams b = {};
        b.target_bitrate = std::get<AbrParams>(params.params).target_bitrate;
        setBitrate(b, false);
        rc.va_mode = supports(VA_RC_AVBR) ? VA_RC_AVBR : (supports(VA_RC_VBR) ? VA_RC_VBR : VA_RC_CBR);
        if (!supports(rc.va_mode)) cqp(26, 28);
        break;
      }
      case RateControlMode::QVBR: {
        const auto& p = std::get<QvbrParams>(params.params);
        if (supports(VA_RC_QVBR)) {
          setBitrate(p.bitrate, true);
          rc.va_mode = VA_RC_QVBR;
          rc.quality = static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(p.quality)), 1, 51));
        } else {
          vbr(p.bitrate);
        }
        break;
      }
      case RateControlMode::CRF:
      case RateControlMode::ICQ:
      default: quality(std::get<CrfParams>(params.params).quality); break;
    }
    if (!supports(rc.va_mode)) cqp(26, 28);
    if (params.min_qp) rc.min_qp = static_cast<uint32_t>(std::max(*params.min_qp, 0));
    if (params.max_qp) rc.max_qp = static_cast<uint32_t>(std::max(*params.max_qp, 0));
    return rc;
  }

  // ---------------------------------------------------------------------------
  // Session
  // ---------------------------------------------------------------------------

  auto surfaceAlignment() const -> uint32_t {
    switch (codec_id_) {
      case OM_CODEC_H265: return std::max<uint32_t>(16, 1u << (hevcMinCbLog2()));
      case OM_CODEC_VP9:
      case OM_CODEC_AV1: return 64;
      default: return 16;
    }
  }

  auto hevcMinCbLog2() const -> uint32_t {
    if (hevc_block_sizes_ == VA_ATTRIB_NOT_SUPPORTED) return 4; // 16x16, what ffmpeg assumes
    VAConfigAttribValEncHEVCBlockSizes bs = {};
    bs.value = hevc_block_sizes_;
    return bs.bits.log2_min_luma_coding_block_size_minus3 + 3;
  }

  auto createSession() -> OMError {
    auto& libva = LibVA::getInstance();
    const uint32_t rt_format = bit_depth_ > 8 ? VA_RT_FORMAT_YUV420_10 : VA_RT_FORMAT_YUV420;

    const uint32_t rt_supported = attribute(VAConfigAttribRTFormat);
    if (rt_supported != VA_ATTRIB_NOT_SUPPORTED && (rt_supported & rt_format) == 0) {
      return fail(OM_CODEC_NOT_SUPPORTED, "the driver does not encode from {}-bit 4:2:0", bit_depth_);
    }

    VAConfigAttrib attribs[3] = {};
    int count = 0;
    attribs[count].type = VAConfigAttribRTFormat;
    attribs[count++].value = rt_format;
    attribs[count].type = VAConfigAttribRateControl;
    attribs[count++].value = rc_.va_mode;
    if (packed_headers_ != 0) {
      attribs[count].type = VAConfigAttribEncPackedHeaders;
      attribs[count++].value = packed_headers_;
    }
    VAStatus status = libva.vaCreateConfig(display_, profile_, entrypoint_, attribs, count, &config_);
    if (status != VA_STATUS_SUCCESS) {
      config_ = VA_INVALID_ID;
      return fail(OM_CODEC_OPEN_FAILED, "vaCreateConfig failed: {}", vaapi::describe(status));
    }

    const uint32_t align = surfaceAlignment();
    surface_width_ = vaapi::alignUp(width_, align);
    surface_height_ = vaapi::alignUp(height_, align);

    // Two reconstructed pictures (the reference and the one being made); two
    // input pictures and coded buffers, one for the picture in flight and one
    // for the picture being submitted.
    recon_ = vaapi::SurfacePool::create(owned_display_, display_, rt_format, surface_width_, surface_height_, 2);
    input_ = vaapi::SurfacePool::create(owned_display_, display_, rt_format, surface_width_, surface_height_, 2);
    if (!recon_ || !input_) return fail(OM_CODEC_OPEN_FAILED, "could not allocate surfaces");

    std::vector<VASurfaceID> targets = recon_->surfaces();
    const auto inputs = input_->surfaces();
    targets.insert(targets.end(), inputs.begin(), inputs.end());
    status = libva.vaCreateContext(display_, config_, static_cast<int>(surface_width_), static_cast<int>(surface_height_),
                                   VA_PROGRESSIVE, targets.data(), static_cast<int>(targets.size()), &context_);
    if (status != VA_STATUS_SUCCESS) {
      context_ = VA_INVALID_ID;
      return fail(OM_CODEC_OPEN_FAILED, "vaCreateContext failed: {}", vaapi::describe(status));
    }

    // Room for an intra frame at a quality nobody asks of a hardware encoder.
    const size_t coded_size = std::max<size_t>(1u << 20, static_cast<size_t>(surface_width_) * surface_height_ *
                                                             (bit_depth_ > 8 ? 3 : 2));
    for (auto& buffer : coded_buffers_) {
      status = libva.vaCreateBuffer(display_, context_, VAEncCodedBufferType, static_cast<unsigned int>(coded_size), 1,
                                    nullptr, &buffer);
      if (status != VA_STATUS_SUCCESS) {
        buffer = VA_INVALID_ID;
        return fail(OM_CODEC_OPEN_FAILED, "could not create a {} byte coded buffer: {}", coded_size,
                    vaapi::describe(status));
      }
    }
    return OM_SUCCESS;
  }

  void release() {
    auto& libva = LibVA::getInstance();
    if (libva.isLoaded() && display_) {
      // Whatever was still being encoded has to finish before its buffers go.
      if (in_flight_) libva.vaSyncSurface(display_, in_flight_->input);
      for (VABufferID buffer : coded_buffers_) {
        if (buffer != VA_INVALID_ID) libva.vaDestroyBuffer(display_, buffer);
      }
      if (upload_image_.image_id != VA_INVALID_ID) libva.vaDestroyImage(display_, upload_image_.image_id);
      if (context_ != VA_INVALID_ID) libva.vaDestroyContext(display_, context_);
      if (config_ != VA_INVALID_ID) libva.vaDestroyConfig(display_, config_);
    }
    in_flight_.reset();
    coded_buffers_[0] = coded_buffers_[1] = VA_INVALID_ID;
    coded_buffer_ = VA_INVALID_ID;
    next_slot_ = 0;
    upload_image_ = {};
    upload_image_.image_id = VA_INVALID_ID;
    context_ = VA_INVALID_ID;
    config_ = VA_INVALID_ID;
    recon_.reset();
    input_.reset();
    owned_display_.reset();
    display_ = nullptr;
    extradata_.clear();
    initialized_ = false;
    have_reference_ = false;
    frames_since_key_ = 0;
    frame_num_ = 0;
    idr_pic_id_ = 0;
    current_recon_ = 0;
    rc_changed_ = false;
  }

  // ---------------------------------------------------------------------------
  // Headers
  // ---------------------------------------------------------------------------

  void setupHeaders(int32_t level) {
    StreamShape shape;
    shape.width = width_;
    shape.height = height_;
    shape.fps = static_cast<double>(timing_.num) / std::max<uint32_t>(timing_.den, 1);
    shape.bitrate = rc_.hasBitrate() ? rc_.bits_per_second : 0;

    const int init_qp = rc_.va_mode == VA_RC_CQP ? rc_.qp_i : 26;

    if (codec_id_ == OM_CODEC_H264) {
      auto& h = h264_;
      h.width_mbs = surface_width_ / 16;
      h.height_mbs = surface_height_ / 16;
      h.crop_right = (surface_width_ - width_) / 2;
      h.crop_bottom = (surface_height_ - height_) / 2;
      h.bit_depth = bit_depth_;
      h.max_num_ref_frames = gop_ == 1 ? 0 : 1;
      switch (profile_) {
        case VAProfileH264ConstrainedBaseline:
          h.profile_idc = 66;
          h.constraint_set1 = true;
          h.constraint_set4 = false;
          h.constraint_set5 = false;
          h.cabac = false;
          h.transform_8x8 = false;
          break;
        case VAProfileH264Main:
          h.profile_idc = 77;
          h.constraint_set1 = true;
          h.transform_8x8 = false;
          break;
        case VAProfileH264High10: h.profile_idc = 110; break;
        default: h.profile_idc = 100; break;
      }
      h.constraint_set3 = h.highProfile() && gop_ == 1; // intra profiles
      h.level_idc = level > 0 ? static_cast<uint8_t>(level) : guessH264Level(shape);
      h.init_qp = init_qp;
      h.colour = colour_;
      h.timing = timing_;
    } else if (codec_id_ == OM_CODEC_H265) {
      auto& h = h265_;
      h.profile_idc = bit_depth_ > 8 ? 2 : 1;
      h.width = surface_width_;
      h.height = surface_height_;
      h.crop_right = (surface_width_ - width_) / 2;
      h.crop_bottom = (surface_height_ - height_) / 2;
      h.bit_depth = bit_depth_;
      h.max_dec_pic_buffering_minus1 = gop_ == 1 ? 0 : 1;
      h.level_idc = level > 0 ? static_cast<uint8_t>(level) : guessH265Level(shape);
      h.init_qp = init_qp;
      h.colour = colour_;
      h.timing = timing_;

      // Defaults from the first VA HEVC encoder (i965, Skylake), for drivers
      // that do not say what they can do.
      uint32_t ctu_log2 = 5;
      h.log2_min_cb_minus3 = 1;
      h.log2_diff_max_min_cb = 1;
      h.log2_min_tb_minus2 = 0;
      h.log2_diff_max_min_tb = 3;
      h.max_th_depth_inter = 3;
      h.max_th_depth_intra = 3;
      if (hevc_block_sizes_ != VA_ATTRIB_NOT_SUPPORTED) {
        VAConfigAttribValEncHEVCBlockSizes bs = {};
        bs.value = hevc_block_sizes_;
        ctu_log2 = bs.bits.log2_max_coding_tree_block_size_minus3 + 3;
        h.log2_min_cb_minus3 = bs.bits.log2_min_luma_coding_block_size_minus3;
        h.log2_diff_max_min_cb = ctu_log2 - (h.log2_min_cb_minus3 + 3);
        h.log2_min_tb_minus2 = bs.bits.log2_min_luma_transform_block_size_minus2;
        h.log2_diff_max_min_tb = bs.bits.log2_max_luma_transform_block_size_minus2 - bs.bits.log2_min_luma_transform_block_size_minus2;
        h.max_th_depth_inter = bs.bits.max_max_transform_hierarchy_depth_inter;
        h.max_th_depth_intra = bs.bits.max_max_transform_hierarchy_depth_intra;
      }
      hevc_ctu_log2_ = ctu_log2;
      h.amp = true;
      h.sao = false;
      h.temporal_mvp = false;
      if (hevc_features_ != VA_ATTRIB_NOT_SUPPORTED) {
        VAConfigAttribValEncHEVCFeatures f = {};
        f.value = hevc_features_;
        h.amp = f.bits.amp != 0;
        h.sao = f.bits.sao != 0;
        h.temporal_mvp = f.bits.temporal_mvp != 0;
        h.transform_skip = f.bits.transform_skip != 0;
        if (rc_.va_mode != VA_RC_CQP) h.cu_qp_delta = f.bits.cu_qp_delta != 0;
      } else {
        h.cu_qp_delta = rc_.va_mode != VA_RC_CQP;
      }
      // cu_qp_delta at any depth short of the maximum would leave it unused.
      h.diff_cu_qp_delta_depth = h.cu_qp_delta ? h.log2_diff_max_min_cb : 0;
    } else if (codec_id_ == OM_CODEC_AV1) {
      auto& h = av1_;
      h.width = width_;
      h.height = height_;
      h.bit_depth = bit_depth_;
      h.level_idx = level >= 0 && level <= 31 && level != 0 ? static_cast<uint8_t>(level) : guessAV1Level(shape);
      // Under the driver's rate control it fills the CDEF strengths in; with
      // fixed quantisers the encoder picks them (av1FiltersForQ).
      h.enable_cdef = true;
      h.colour = colour_;
      // An identity matrix would mean RGB, which profile 0 cannot carry.
      if (h.colour.matrix == 0) h.colour.matrix = 2;
      h.tiles = av1Tiles(width_, height_);
      if (av1_ext2_ != VA_ATTRIB_NOT_SUPPORTED) {
        VAConfigAttribValEncAV1Ext2 ext = {};
        ext.value = av1_ext2_;
        h.obu_size_bytes = ext.bits.obu_size_bytes_minus1 + 1;
        h.tile_size_bytes_minus1 = ext.bits.tile_size_bytes_minus1;
        h.tx_mode_select = (ext.bits.tx_mode_support & 0x04) != 0 || (ext.bits.tx_mode_support & 0x02) == 0;
      }
    }
  }

  // ---------------------------------------------------------------------------
  // Input
  // ---------------------------------------------------------------------------

  // The surface to encode from: a VA-API picture on our own display as it is,
  // anything else copied into our input surface.
  auto inputSurface(const Picture& pic) -> VASurfaceID {
    if (const auto* hardware = std::get_if<std::shared_ptr<HardwarePicture>>(&pic.buffer)) {
      if (*hardware && (*hardware)->getType() == HWDeviceType::VAAPI) {
        const auto va = std::static_pointer_cast<VAAPIHardwarePicture>(*hardware);
        if (va->display() == display_) return va->surface();
      }
      fail(OM_CODEC_INVALID_PARAMS, "hardware input must be a VA-API surface on the encoder's display");
      return VA_INVALID_SURFACE;
    }
    const VASurfaceID surface = input_->surface(next_slot_);
    return upload(pic, surface) ? surface : VA_INVALID_SURFACE;
  }

  auto upload(const Picture& pic, VASurfaceID surface) -> bool {
    auto& libva = LibVA::getInstance();
    const bool nv12_input = pic.format == OM_FORMAT_NV12 || pic.format == OM_FORMAT_P010;
    const bool planar_input = pic.format == OM_FORMAT_YUV420P || pic.format == OM_FORMAT_YUVJ420P ||
                              pic.format == OM_FORMAT_YUV420P10;
    const bool high = pic.format == OM_FORMAT_P010 || pic.format == OM_FORMAT_YUV420P10;
    if ((!nv12_input && !planar_input) || high != (bit_depth_ > 8)) {
      fail(OM_CODEC_INVALID_PARAMS, "picture format does not match the encoder's input format");
      return false;
    }

    // Mapping the surface itself avoids a second copy; drivers that cannot
    // derive one get a linear image and vaPutImage.
    VAImage image = {};
    image.image_id = VA_INVALID_ID;
    bool derived = false;
    if (derive_works_ && libva.vaDeriveImage(display_, surface, &image) == VA_STATUS_SUCCESS) {
      if (image.format.fourcc == input_->fourcc()) {
        derived = true;
      } else {
        libva.vaDestroyImage(display_, image.image_id);
        derive_works_ = false;
      }
    } else {
      derive_works_ = false;
    }
    if (!derived) {
      if (upload_image_.image_id == VA_INVALID_ID) {
        VAImageFormat format = {};
        format.fourcc = input_->fourcc();
        format.byte_order = VA_LSB_FIRST;
        format.bits_per_pixel = high ? 24 : 12;
        if (libva.vaCreateImage(display_, &format, static_cast<int>(surface_width_), static_cast<int>(surface_height_),
                                &upload_image_) != VA_STATUS_SUCCESS) {
          upload_image_.image_id = VA_INVALID_ID;
          fail(OM_CODEC_ENCODE_FAILED, "could not create an upload image");
          return false;
        }
      }
      image = upload_image_;
    }

    void* mapped = nullptr;
    if (libva.vaMapBuffer(display_, image.buf, &mapped) != VA_STATUS_SUCCESS || !mapped) {
      if (derived) libva.vaDestroyImage(display_, image.image_id);
      return false;
    }
    auto* base = static_cast<uint8_t*>(mapped);
    const uint32_t w = std::min(pic.width, surface_width_);
    const uint32_t h = std::min(pic.height, surface_height_);
    const uint32_t cw = (w + 1) / 2;
    const uint32_t ch = (h + 1) / 2;
    const size_t bpp = high ? 2 : 1;

    uint8_t* dst_y = base + image.offsets[0];
    uint8_t* dst_uv = base + image.offsets[1];
    const uint8_t* src_y = pic.planes.getData(0);
    const size_t src_y_stride = pic.planes.getLinesize(0);
    const bool shift = pic.format == OM_FORMAT_YUV420P10; // LSB-aligned -> MSB-aligned

    for (uint32_t row = 0; row < h; ++row) {
      uint8_t* d = dst_y + static_cast<size_t>(row) * image.pitches[0];
      const uint8_t* s = src_y + row * src_y_stride;
      if (!shift) {
        std::memcpy(d, s, w * bpp);
      } else {
        for (uint32_t x = 0; x < w; ++x) {
          uint16_t v;
          std::memcpy(&v, s + 2 * x, 2);
          v = static_cast<uint16_t>(v << 6);
          std::memcpy(d + 2 * x, &v, 2);
        }
      }
    }
    if (nv12_input) {
      const uint8_t* src_uv = pic.planes.getData(1);
      const size_t src_uv_stride = pic.planes.getLinesize(1);
      for (uint32_t row = 0; row < ch; ++row) {
        std::memcpy(dst_uv + static_cast<size_t>(row) * image.pitches[1], src_uv + row * src_uv_stride, 2 * cw * bpp);
      }
    } else {
      const uint8_t* src_u = pic.planes.getData(1);
      const uint8_t* src_v = pic.planes.getData(2);
      const size_t su = pic.planes.getLinesize(1);
      const size_t sv = pic.planes.getLinesize(2);
      for (uint32_t row = 0; row < ch; ++row) {
        uint8_t* d = dst_uv + static_cast<size_t>(row) * image.pitches[1];
        const uint8_t* u = src_u + row * su;
        const uint8_t* v = src_v + row * sv;
        if (bpp == 1) {
          for (uint32_t x = 0; x < cw; ++x) {
            d[2 * x] = u[x];
            d[2 * x + 1] = v[x];
          }
        } else {
          for (uint32_t x = 0; x < cw; ++x) {
            uint16_t a;
            uint16_t b;
            std::memcpy(&a, u + 2 * x, 2);
            std::memcpy(&b, v + 2 * x, 2);
            if (shift) {
              a = static_cast<uint16_t>(a << 6);
              b = static_cast<uint16_t>(b << 6);
            }
            std::memcpy(d + 4 * x, &a, 2);
            std::memcpy(d + 4 * x + 2, &b, 2);
          }
        }
      }
    }

    libva.vaUnmapBuffer(display_, image.buf);
    if (derived) {
      libva.vaDestroyImage(display_, image.image_id);
      return true;
    }
    const VAStatus status = libva.vaPutImage(display_, surface, image.image_id, 0, 0, surface_width_, surface_height_,
                                             0, 0, surface_width_, surface_height_);
    if (status != VA_STATUS_SUCCESS) {
      fail(OM_CODEC_ENCODE_FAILED, "vaPutImage failed: {}", vaapi::describe(status));
      return false;
    }
    return true;
  }

  // ---------------------------------------------------------------------------
  // One picture
  // ---------------------------------------------------------------------------

  // Parameter buffers of one picture, destroyed together.
  class Buffers {
  public:
    Buffers(VADisplay display, VAContextID context) : display_(display), context_(context) {}
    ~Buffers() {
      auto& libva = LibVA::getInstance();
      for (VABufferID id : ids_) libva.vaDestroyBuffer(display_, id);
    }
    Buffers(const Buffers&) = delete;
    Buffers& operator=(const Buffers&) = delete;

    auto add(VABufferType type, const void* data, size_t size) -> bool {
      VABufferID id = VA_INVALID_ID;
      if (LibVA::getInstance().vaCreateBuffer(display_, context_, type, static_cast<unsigned int>(size), 1,
                                              const_cast<void*>(data), &id) != VA_STATUS_SUCCESS) {
        ok_ = false;
        return false;
      }
      ids_.push_back(id);
      return true;
    }

    template<typename T>
    auto misc(VAEncMiscParameterType type, const T& value) -> bool {
      std::vector<uint8_t> data(sizeof(VAEncMiscParameterBuffer) + sizeof(T));
      auto* header = reinterpret_cast<VAEncMiscParameterBuffer*>(data.data());
      header->type = type;
      std::memcpy(data.data() + sizeof(VAEncMiscParameterBuffer), &value, sizeof(T));
      return add(VAEncMiscParameterBufferType, data.data(), data.size());
    }

    auto packed(uint32_t type, const PackedBits& bits, bool emulation_bytes) -> bool {
      VAEncPackedHeaderParameterBuffer param = {};
      param.type = type;
      param.bit_length = static_cast<uint32_t>(bits.bits);
      param.has_emulation_bytes = emulation_bytes ? 1 : 0;
      return add(VAEncPackedHeaderParameterBufferType, &param, sizeof(param)) &&
             add(VAEncPackedHeaderDataBufferType, bits.bytes.data(), bits.bytes.size());
    }

    auto ok() const -> bool { return ok_; }
    auto ids() -> std::vector<VABufferID>& { return ids_; }

  private:
    VADisplay display_;
    VAContextID context_;
    std::vector<VABufferID> ids_;
    bool ok_ = true;
  };

  void addRateControl(Buffers& buffers, bool reset) {
    if (rc_.va_mode != VA_RC_CQP) {
      VAEncMiscParameterRateControl rc = {};
      rc.bits_per_second = rc_.bits_per_second;
      rc.target_percentage = rc_.target_percentage;
      rc.window_size = 1000;
      rc.initial_qp = 0;
      rc.min_qp = rc_.min_qp;
      rc.max_qp = rc_.max_qp;
      rc.basic_unit_size = 0;
      rc.ICQ_quality_factor = rc_.quality;
      rc.quality_factor = rc_.quality;
      rc.rc_flags.bits.reset = reset ? 1 : 0;
      rc.rc_flags.bits.mb_rate_control = 0; // driver default
      // One packet per frame and a reference chain that stays intact: a
      // skipped frame would break both, so the rate control may not drop one.
      rc.rc_flags.bits.disable_frame_skip = 1;
      buffers.misc(VAEncMiscParameterTypeRateControl, rc);
      if (rc_.hasBitrate()) {
        VAEncMiscParameterHRD hrd = {};
        hrd.buffer_size = rc_.hrd_size;
        hrd.initial_buffer_fullness = rc_.hrd_initial;
        buffers.misc(VAEncMiscParameterTypeHRD, hrd);
      }
    }
    VAEncMiscParameterFrameRate fr = {};
    fr.framerate = (timing_.den << 16) | (timing_.num & 0xffff);
    if (timing_.num > 0xffff || timing_.den > 0xffff) {
      // The field packs two 16-bit halves; a large rational goes in rounded.
      fr.framerate = static_cast<uint32_t>(std::lround(static_cast<double>(timing_.num) / timing_.den));
    }
    buffers.misc(VAEncMiscParameterTypeFrameRate, fr);

    if (quality_level_ > 0 && quality_range_ > 0) {
      VAEncMiscParameterBufferQualityLevel quality = {};
      quality.quality_level = std::min(quality_level_, quality_range_);
      buffers.misc(VAEncMiscParameterTypeQualityLevel, quality);
    }
  }

  // Everything needed to finish a picture that has been submitted.
  struct InFlight {
    VASurfaceID input = VA_INVALID_SURFACE;
    VABufferID coded = VA_INVALID_ID;
    int64_t pts = 0;
    bool key = false;
    // A caller's VA-API surface has to outlive its encoding.
    std::shared_ptr<HardwarePicture> hold;
  };

  auto submit(const Frame& frame, const Picture& pic, bool key) -> bool {
    auto& libva = LibVA::getInstance();
    const VASurfaceID input = inputSurface(pic);
    if (input == VA_INVALID_SURFACE) return false;

    const int recon_index = have_reference_ ? 1 - current_recon_ : current_recon_;
    const VASurfaceID recon = recon_->surface(recon_index);
    const VASurfaceID reference = have_reference_ && !key ? recon_->surface(current_recon_) : VA_INVALID_SURFACE;
    coded_buffer_ = coded_buffers_[next_slot_];

    if (key) {
      frames_since_key_ = 0;
      frame_num_ = 0;
    }

    Buffers buffers(display_, context_);
    bool built = false;
    switch (codec_id_) {
      case OM_CODEC_H264: built = buildH264(buffers, key, recon, reference); break;
      case OM_CODEC_H265: built = buildH265(buffers, key, recon, reference); break;
      case OM_CODEC_VP9: built = buildVP9(buffers, key, recon, reference); break;
      case OM_CODEC_AV1: built = buildAV1(buffers, key, recon, reference); break;
      default: break;
    }
    if (!built || !buffers.ok()) {
      fail(OM_CODEC_ENCODE_FAILED, "could not create the parameter buffers");
      return false;
    }

    VAStatus status = libva.vaBeginPicture(display_, context_, input);
    if (status != VA_STATUS_SUCCESS) {
      fail(OM_CODEC_ENCODE_FAILED, "vaBeginPicture failed: {}", vaapi::describe(status));
      return false;
    }
    auto& ids = buffers.ids();
    const VAStatus render = libva.vaRenderPicture(display_, context_, ids.data(), static_cast<int>(ids.size()));
    status = libva.vaEndPicture(display_, context_);
    if (render != VA_STATUS_SUCCESS || status != VA_STATUS_SUCCESS) {
      fail(OM_CODEC_ENCODE_FAILED, "submitting the picture failed: {}",
           vaapi::describe(render != VA_STATUS_SUCCESS ? render : status));
      // The reference chain has a hole now; the next frame starts over.
      have_reference_ = false;
      return false;
    }

    // The picture just submitted is the reference for the next one; the driver
    // orders the two encodes itself.
    current_recon_ = recon_index;
    have_reference_ = max_refs_l0_ > 0;
    ++frames_since_key_;
    ++frame_num_;
    rc_changed_ = false;

    InFlight job;
    job.input = input;
    job.coded = coded_buffer_;
    job.pts = frame.pts;
    job.key = key;
    if (const auto* hardware = std::get_if<std::shared_ptr<HardwarePicture>>(&pic.buffer)) job.hold = *hardware;
    in_flight_ = std::move(job);
    next_slot_ ^= 1;
    return true;
  }

  auto collect(const InFlight& job) -> std::optional<Packet> {
    auto& libva = LibVA::getInstance();
    VAStatus status = libva.vaSyncSurface(display_, job.input);
    if (status != VA_STATUS_SUCCESS) {
      fail(OM_CODEC_ENCODE_FAILED, "vaSyncSurface failed: {}", vaapi::describe(status));
      return std::nullopt;
    }

    std::vector<uint8_t> coded;
    void* mapped = nullptr;
    status = libva.vaMapBuffer(display_, job.coded, &mapped);
    if (status != VA_STATUS_SUCCESS || !mapped) {
      fail(OM_CODEC_ENCODE_FAILED, "could not map the coded buffer: {}", vaapi::describe(status));
      return std::nullopt;
    }
    for (auto* segment = static_cast<VACodedBufferSegment*>(mapped); segment;
         segment = static_cast<VACodedBufferSegment*>(segment->next)) {
      if (segment->status & VA_CODED_BUF_STATUS_SLICE_OVERFLOW_MASK) {
        log(OM_CATEGORY_HARDWARE, OM_LEVEL_WARNING, "[VAAPI] coded buffer overflowed; the frame is truncated");
      }
      const auto* data = static_cast<const uint8_t*>(segment->buf);
      if (data && segment->size) coded.insert(coded.end(), data, data + segment->size);
    }
    libva.vaUnmapBuffer(display_, job.coded);
    if (coded.empty()) {
      fail(OM_CODEC_ENCODE_FAILED, "the driver produced no data");
      return std::nullopt;
    }

    if (job.key) {
      addHdrMetadata(coded);
      if (extradata_.empty()) captureExtradata(coded);
    }

    Packet packet;
    packet.allocate(coded.size());
    std::memcpy(packet.bytes.data(), coded.data(), coded.size());
    packet.is_keyframe = job.key;
    packet.pts = job.pts;
    packet.dts = job.pts;
    return packet;
  }

  static void invalidate(VAPictureH264& pic) {
    pic = {};
    pic.picture_id = VA_INVALID_SURFACE;
    pic.flags = VA_PICTURE_H264_INVALID;
  }

  static void invalidate(VAPictureHEVC& pic) {
    pic = {};
    pic.picture_id = VA_INVALID_SURFACE;
    pic.flags = VA_PICTURE_HEVC_INVALID;
  }

  auto sliceQpDelta(bool key, int init_qp) const -> int {
    if (rc_.va_mode != VA_RC_CQP) return 0;
    return (key ? rc_.qp_i : rc_.qp_p) - init_qp;
  }

  auto buildH264(Buffers& b, bool key, VASurfaceID recon, VASurfaceID reference) -> bool {
    const auto& h = h264_;
    if (key) {
      ++idr_pic_id_;
      VAEncSequenceParameterBufferH264 seq = {};
      seq.seq_parameter_set_id = 0;
      seq.level_idc = h.level_idc;
      seq.intra_period = gop_;
      seq.intra_idr_period = gop_;
      seq.ip_period = 1;
      seq.bits_per_second = rc_.hasBitrate() ? rc_.bits_per_second : 0;
      seq.max_num_ref_frames = h.max_num_ref_frames;
      seq.picture_width_in_mbs = static_cast<uint16_t>(h.width_mbs);
      seq.picture_height_in_mbs = static_cast<uint16_t>(h.height_mbs);
      seq.seq_fields.bits.chroma_format_idc = 1;
      seq.seq_fields.bits.frame_mbs_only_flag = 1;
      seq.seq_fields.bits.direct_8x8_inference_flag = 1;
      seq.seq_fields.bits.log2_max_frame_num_minus4 = H264Headers::kLog2MaxFrameNumMinus4;
      seq.seq_fields.bits.pic_order_cnt_type = 2;
      seq.bit_depth_luma_minus8 = static_cast<uint8_t>(bit_depth_ - 8);
      seq.bit_depth_chroma_minus8 = static_cast<uint8_t>(bit_depth_ - 8);
      seq.frame_cropping_flag = (h.crop_right || h.crop_bottom) ? 1 : 0;
      seq.frame_crop_right_offset = h.crop_right;
      seq.frame_crop_bottom_offset = h.crop_bottom;
      seq.vui_parameters_present_flag = 1;
      seq.vui_fields.bits.timing_info_present_flag = 1;
      seq.vui_fields.bits.bitstream_restriction_flag = 1;
      seq.vui_fields.bits.log2_max_mv_length_horizontal = 15;
      seq.vui_fields.bits.log2_max_mv_length_vertical = 15;
      seq.vui_fields.bits.fixed_frame_rate_flag = 1;
      seq.vui_fields.bits.motion_vectors_over_pic_boundaries_flag = 1;
      seq.num_units_in_tick = timing_.den;
      seq.time_scale = 2 * timing_.num;
      b.add(VAEncSequenceParameterBufferType, &seq, sizeof(seq));
    }
    if (key || rc_changed_) addRateControl(b, rc_changed_ && !key);

    const uint32_t frame_num = frame_num_ & ((1u << (H264Headers::kLog2MaxFrameNumMinus4 + 4)) - 1);
    const int32_t poc = static_cast<int32_t>(2 * frames_since_key_);

    VAEncPictureParameterBufferH264 pic = {};
    pic.CurrPic.picture_id = recon;
    pic.CurrPic.frame_idx = frame_num;
    pic.CurrPic.flags = 0;
    pic.CurrPic.TopFieldOrderCnt = poc;
    pic.CurrPic.BottomFieldOrderCnt = poc;
    for (auto& ref : pic.ReferenceFrames) invalidate(ref);
    VAPictureH264 ref = {};
    invalidate(ref);
    if (!key) {
      ref.picture_id = reference;
      ref.frame_idx = (frame_num_ - 1) & ((1u << (H264Headers::kLog2MaxFrameNumMinus4 + 4)) - 1);
      ref.flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
      ref.TopFieldOrderCnt = poc - 2;
      ref.BottomFieldOrderCnt = poc - 2;
      pic.ReferenceFrames[0] = ref;
    }
    pic.coded_buf = coded_buffer_;
    pic.pic_parameter_set_id = 0;
    pic.seq_parameter_set_id = 0;
    pic.last_picture = 0;
    pic.frame_num = static_cast<uint16_t>(frame_num);
    pic.pic_init_qp = static_cast<uint8_t>(h.init_qp);
    pic.num_ref_idx_l0_active_minus1 = 0;
    pic.num_ref_idx_l1_active_minus1 = 0;
    pic.pic_fields.bits.idr_pic_flag = key ? 1 : 0;
    pic.pic_fields.bits.reference_pic_flag = 1;
    pic.pic_fields.bits.entropy_coding_mode_flag = h.cabac ? 1 : 0;
    pic.pic_fields.bits.transform_8x8_mode_flag = h.transform_8x8 ? 1 : 0;
    pic.pic_fields.bits.deblocking_filter_control_present_flag = 0;
    b.add(VAEncPictureParameterBufferType, &pic, sizeof(pic));

    const int qp_delta = sliceQpDelta(key, h.init_qp);
    if (key && (packed_headers_ & VA_ENC_PACKED_HEADER_SEQUENCE)) {
      if (packed_headers_ & VA_ENC_PACKED_HEADER_PICTURE) {
        b.packed(VAEncPackedHeaderSequence, h.sps(), true);
        b.packed(VAEncPackedHeaderPicture, h.pps(), true);
      } else {
        PackedBits sets = h.sps();
        append(sets, h.pps());
        b.packed(VAEncPackedHeaderSequence, sets, true);
      }
    }
    if (packed_headers_ & VA_ENC_PACKED_HEADER_SLICE) {
      b.packed(VAEncPackedHeaderSlice, h.sliceHeader(key, frame_num, idr_pic_id_ & 0xffff, qp_delta), true);
    }

    VAEncSliceParameterBufferH264 slice = {};
    slice.macroblock_address = 0;
    slice.num_macroblocks = h.width_mbs * h.height_mbs;
    slice.macroblock_info = VA_INVALID_ID;
    slice.slice_type = key ? 2 : 0;
    slice.pic_parameter_set_id = 0;
    slice.idr_pic_id = static_cast<uint16_t>(idr_pic_id_ & 0xffff);
    slice.pic_order_cnt_lsb = 0;
    slice.direct_spatial_mv_pred_flag = 1;
    slice.num_ref_idx_active_override_flag = 0;
    for (auto& r : slice.RefPicList0) invalidate(r);
    for (auto& r : slice.RefPicList1) invalidate(r);
    if (!key) slice.RefPicList0[0] = ref;
    slice.cabac_init_idc = 0;
    slice.slice_qp_delta = static_cast<int8_t>(qp_delta);
    slice.disable_deblocking_filter_idc = 0;
    b.add(VAEncSliceParameterBufferType, &slice, sizeof(slice));
    return true;
  }

  auto buildH265(Buffers& b, bool key, VASurfaceID recon, VASurfaceID reference) -> bool {
    const auto& h = h265_;
    if (key) {
      VAEncSequenceParameterBufferHEVC seq = {};
      seq.general_profile_idc = h.profile_idc;
      seq.general_level_idc = h.level_idc;
      seq.general_tier_flag = 0;
      seq.intra_period = gop_;
      seq.intra_idr_period = gop_;
      seq.ip_period = 1;
      seq.bits_per_second = rc_.hasBitrate() ? rc_.bits_per_second : 0;
      seq.pic_width_in_luma_samples = static_cast<uint16_t>(h.width);
      seq.pic_height_in_luma_samples = static_cast<uint16_t>(h.height);
      auto& sf = seq.seq_fields.bits;
      sf.chroma_format_idc = 1;
      sf.bit_depth_luma_minus8 = bit_depth_ - 8;
      sf.bit_depth_chroma_minus8 = bit_depth_ - 8;
      sf.amp_enabled_flag = h.amp;
      sf.sample_adaptive_offset_enabled_flag = h.sao;
      sf.sps_temporal_mvp_enabled_flag = h.temporal_mvp;
      sf.low_delay_seq = 1;
      seq.log2_min_luma_coding_block_size_minus3 = static_cast<uint8_t>(h.log2_min_cb_minus3);
      seq.log2_diff_max_min_luma_coding_block_size = static_cast<uint8_t>(h.log2_diff_max_min_cb);
      seq.log2_min_transform_block_size_minus2 = static_cast<uint8_t>(h.log2_min_tb_minus2);
      seq.log2_diff_max_min_transform_block_size = static_cast<uint8_t>(h.log2_diff_max_min_tb);
      seq.max_transform_hierarchy_depth_inter = static_cast<uint8_t>(h.max_th_depth_inter);
      seq.max_transform_hierarchy_depth_intra = static_cast<uint8_t>(h.max_th_depth_intra);
      seq.vui_parameters_present_flag = 0;
      b.add(VAEncSequenceParameterBufferType, &seq, sizeof(seq));
    }
    if (key || rc_changed_) addRateControl(b, rc_changed_ && !key);

    const auto poc = static_cast<int32_t>(frames_since_key_);
    const int slice_type = key ? 2 : (p_as_b_ ? 0 : 1);

    VAEncPictureParameterBufferHEVC pic = {};
    pic.decoded_curr_pic.picture_id = recon;
    pic.decoded_curr_pic.pic_order_cnt = poc;
    pic.decoded_curr_pic.flags = 0;
    for (auto& ref : pic.reference_frames) invalidate(ref);
    VAPictureHEVC ref = {};
    invalidate(ref);
    if (!key) {
      ref.picture_id = reference;
      ref.pic_order_cnt = poc - 1;
      ref.flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
      pic.reference_frames[0] = ref;
    }
    pic.coded_buf = coded_buffer_;
    pic.collocated_ref_pic_index = h.temporal_mvp && !key ? 0 : 0xff;
    pic.last_picture = 0;
    pic.pic_init_qp = static_cast<uint8_t>(h.init_qp);
    pic.diff_cu_qp_delta_depth = static_cast<uint8_t>(h.diff_cu_qp_delta_depth);
    pic.num_ref_idx_l0_default_active_minus1 = 0;
    pic.num_ref_idx_l1_default_active_minus1 = 0;
    pic.slice_pic_parameter_set_id = 0;
    pic.nal_unit_type = key ? 19 : 1;
    auto& pf = pic.pic_fields.bits;
    pf.idr_pic_flag = key ? 1 : 0;
    pf.coding_type = key ? 1 : 2;
    pf.reference_pic_flag = 1;
    pf.transform_skip_enabled_flag = h.transform_skip;
    pf.cu_qp_delta_enabled_flag = h.cu_qp_delta;
    pf.pps_loop_filter_across_slices_enabled_flag = 1;
    b.add(VAEncPictureParameterBufferType, &pic, sizeof(pic));

    const int qp_delta = sliceQpDelta(key, h.init_qp);
    if (key && (packed_headers_ & VA_ENC_PACKED_HEADER_SEQUENCE)) {
      PackedBits sets = h.vps();
      append(sets, h.sps());
      if (packed_headers_ & VA_ENC_PACKED_HEADER_PICTURE) {
        b.packed(VAEncPackedHeaderSequence, sets, true);
        b.packed(VAEncPackedHeaderPicture, h.pps(), true);
      } else {
        append(sets, h.pps());
        b.packed(VAEncPackedHeaderSequence, sets, true);
      }
    }
    if (packed_headers_ & VA_ENC_PACKED_HEADER_SLICE) {
      b.packed(VAEncPackedHeaderSlice, h.sliceHeader(key, static_cast<uint32_t>(poc), slice_type, qp_delta), true);
    }

    const uint32_t ctu = 1u << hevc_ctu_log2_;
    VAEncSliceParameterBufferHEVC slice = {};
    slice.slice_segment_address = 0;
    slice.num_ctu_in_slice = ((h.width + ctu - 1) / ctu) * ((h.height + ctu - 1) / ctu);
    slice.slice_type = static_cast<uint8_t>(slice_type);
    slice.slice_pic_parameter_set_id = 0;
    slice.num_ref_idx_l0_active_minus1 = 0;
    slice.num_ref_idx_l1_active_minus1 = 0;
    for (auto& r : slice.ref_pic_list0) invalidate(r);
    for (auto& r : slice.ref_pic_list1) invalidate(r);
    if (!key) {
      slice.ref_pic_list0[0] = ref;
      if (slice_type == 0) slice.ref_pic_list1[0] = ref;
    }
    slice.max_num_merge_cand = 5;
    slice.slice_qp_delta = static_cast<int8_t>(qp_delta);
    auto& fl = slice.slice_fields.bits;
    fl.last_slice_of_pic_flag = 1;
    fl.slice_temporal_mvp_enabled_flag = h.temporal_mvp && !key;
    fl.slice_sao_luma_flag = h.sao;
    fl.slice_sao_chroma_flag = h.sao;
    fl.collocated_from_l0_flag = 1;
    b.add(VAEncSliceParameterBufferType, &slice, sizeof(slice));
    return true;
  }

  auto buildVP9(Buffers& b, bool key, VASurfaceID recon, VASurfaceID reference) -> bool {
    if (key) {
      VAEncSequenceParameterBufferVP9 seq = {};
      seq.max_frame_width = width_;
      seq.max_frame_height = height_;
      seq.kf_auto = 0;
      seq.bits_per_second = rc_.hasBitrate() ? rc_.bits_per_second : 0;
      seq.intra_period = gop_;
      b.add(VAEncSequenceParameterBufferType, &seq, sizeof(seq));
    }
    if (key || rc_changed_) addRateControl(b, rc_changed_ && !key);

    VAEncPictureParameterBufferVP9 pic = {};
    pic.frame_width_src = width_;
    pic.frame_height_src = height_;
    pic.frame_width_dst = width_;
    pic.frame_height_dst = height_;
    pic.reconstructed_frame = recon;
    for (auto& r : pic.reference_frames) r = VA_INVALID_SURFACE;
    pic.coded_buf = coded_buffer_;
    // Every frame refreshes all eight slots, and inter frames predict from
    // slot 0 as their LAST reference.
    pic.refresh_frame_flags = 0xff;
    if (key) {
      pic.ref_flags.bits.force_kf = 1;
    } else {
      pic.reference_frames[0] = reference;
      pic.ref_flags.bits.ref_frame_ctrl_l0 = 1; // LAST
      pic.ref_flags.bits.ref_last_idx = 0;
      pic.ref_flags.bits.ref_last_sign_bias = 1;
    }
    pic.pic_flags.bits.frame_type = key ? 0 : 1;
    pic.pic_flags.bits.show_frame = 1;
    pic.luma_ac_qindex = static_cast<uint8_t>(rc_.va_mode == VA_RC_CQP ? (key ? rc_.qp_i : rc_.qp_p) : 100);
    pic.filter_level = 16;
    pic.sharpness_level = 4;
    const uint32_t tile_columns = (width_ + 4095) / 4096;
    pic.log2_tile_columns = tile_columns == 1 ? 0 : static_cast<uint8_t>(std::bit_width(tile_columns - 1));
    b.add(VAEncPictureParameterBufferType, &pic, sizeof(pic));
    return true;
  }

  auto buildAV1(Buffers& b, bool key, VASurfaceID recon, VASurfaceID reference) -> bool {
    const auto& h = av1_;
    PackedBits sequence;
    if (key) {
      sequence = h.sequenceHeader();
      VAEncSequenceParameterBufferAV1 seq = {};
      seq.seq_profile = 0;
      seq.seq_level_idx = h.level_idx;
      seq.seq_tier = 0;
      seq.intra_period = gop_;
      seq.ip_period = 1;
      seq.bits_per_second = rc_.hasBitrate() ? rc_.bits_per_second : 0;
      seq.seq_fields.bits.enable_order_hint = 1;
      seq.seq_fields.bits.enable_cdef = h.enable_cdef;
      seq.seq_fields.bits.bit_depth_minus8 = bit_depth_ - 8;
      seq.seq_fields.bits.subsampling_x = 1;
      seq.seq_fields.bits.subsampling_y = 1;
      seq.order_hint_bits_minus_1 = AV1Headers::kOrderHintBits - 1;
      b.add(VAEncSequenceParameterBufferType, &seq, sizeof(seq));
    }
    if (key || rc_changed_) addRateControl(b, rc_changed_ && !key);

    const uint32_t order_hint = frames_since_key_ & ((1u << AV1Headers::kOrderHintBits) - 1);
    const uint32_t qindex = rc_.va_mode == VA_RC_CQP ? static_cast<uint32_t>(key ? rc_.qp_i : rc_.qp_p) : 128;
    const AV1Filters filters = rc_.va_mode == VA_RC_CQP ? av1FiltersForQ(qindex, key, bit_depth_) : AV1Filters {};
    AV1FrameHeaderLayout layout;
    const PackedBits frame_header = h.frameHeader(key, order_hint, qindex, filters, layout);

    VAEncPictureParameterBufferAV1 pic = {};
    pic.frame_width_minus_1 = static_cast<uint16_t>(width_ - 1);
    pic.frame_height_minus_1 = static_cast<uint16_t>(height_ - 1);
    pic.reconstructed_frame = recon;
    pic.coded_buf = coded_buffer_;
    for (auto& r : pic.reference_frames) r = VA_INVALID_SURFACE;
    if (!key) {
      pic.reference_frames[0] = reference;
      pic.ref_frame_ctrl_l0.fields.search_idx0 = 1; // LAST_FRAME
    }
    for (auto& idx : pic.ref_frame_idx) idx = 0;
    pic.primary_ref_frame = key ? 7 : 0; // PRIMARY_REF_NONE for key frames
    pic.order_hint = static_cast<uint8_t>(order_hint);
    pic.refresh_frame_flags = key ? 0xff : 0x01;
    auto& pf = pic.picture_flags.bits;
    pf.frame_type = key ? 0 : 1;
    pf.error_resilient_mode = key ? 1 : 0;
    pf.enable_frame_obu = 0;
    pf.reduced_tx_set = 0;
    pic.base_qindex = static_cast<uint8_t>(qindex);
    pic.filter_level[0] = filters.level[0];
    pic.filter_level[1] = filters.level[1];
    pic.filter_level_u = filters.level[2];
    pic.filter_level_v = filters.level[3];
    pic.cdef_damping_minus_3 = filters.cdef_damping_minus_3;
    pic.cdef_bits = filters.cdef_bits;
    for (uint32_t i = 0; i < (1u << filters.cdef_bits); ++i) {
      pic.cdef_y_strengths[i] = static_cast<uint8_t>(filters.cdef_y_pri[i] * 4 + filters.cdef_y_sec[i]);
      pic.cdef_uv_strengths[i] = static_cast<uint8_t>(filters.cdef_uv_pri[i] * 4 + filters.cdef_uv_sec[i]);
    }
    pic.mode_control_flags.bits.tx_mode = h.tx_mode_select ? 2 : 1;
    pic.mode_control_flags.bits.reference_mode = 0;
    pic.tile_cols = static_cast<uint8_t>(h.tiles.cols);
    pic.tile_rows = static_cast<uint8_t>(h.tiles.rows);
    for (uint32_t i = 0; i < h.tiles.cols; ++i) pic.width_in_sbs_minus_1[i] = static_cast<uint16_t>(h.tiles.width_sb[i] - 1);
    for (uint32_t i = 0; i < h.tiles.rows; ++i) pic.height_in_sbs_minus_1[i] = static_cast<uint16_t>(h.tiles.height_sb[i] - 1);
    pic.tile_group_obu_hdr_info.bits.obu_has_size_field = 1;
    pic.num_tile_groups_minus1 = 0;
    for (int i = 0; i < 8; ++i) {
      static constexpr int8_t kDefaultRefDeltas[8] = {1, 0, 0, 0, -1, 0, -1, -1};
      pic.ref_deltas[i] = kDefaultRefDeltas[i];
    }
    if (rc_.va_mode != VA_RC_CQP) {
      // The driver picks the quantiser, loop filter and CDEF strengths and
      // writes them into the frame header at these places.
      pic.min_base_qindex = static_cast<uint8_t>(std::clamp<uint32_t>(rc_.min_qp ? rc_.min_qp : 1, 1, 255));
      pic.max_base_qindex = static_cast<uint8_t>(std::clamp<uint32_t>(rc_.max_qp ? rc_.max_qp : 255, 1, 255));
      pic.bit_offset_qindex = layout.qindex;
      pic.bit_offset_loopfilter_params = layout.loop_filter;
      pic.bit_offset_cdef_params = layout.cdef;
      pic.size_in_bits_cdef_params = layout.cdef_size;
      pic.size_in_bits_frame_hdr_obu = static_cast<uint32_t>(frame_header.bits);
      pic.byte_offset_frame_hdr_obu_size = static_cast<uint32_t>((key ? sequence.bytes.size() : 0) + 1);
    }
    b.add(VAEncPictureParameterBufferType, &pic, sizeof(pic));

    if (packed_headers_ & VA_ENC_PACKED_HEADER_SEQUENCE && key) {
      b.packed(VAEncPackedHeaderSequence, sequence, false);
    }
    if (packed_headers_ & VA_ENC_PACKED_HEADER_PICTURE) {
      b.packed(VAEncPackedHeaderPicture, frame_header, false);
    }

    VAEncTileGroupBufferAV1 tg = {};
    tg.tg_start = 0;
    tg.tg_end = static_cast<uint8_t>(h.tiles.cols * h.tiles.rows - 1);
    b.add(VAEncSliceParameterBufferType, &tg, sizeof(tg));
    return true;
  }

  // ---------------------------------------------------------------------------
  // Output
  // ---------------------------------------------------------------------------

  // Annex B NAL units of an access unit: (offset of the NAL header, type).
  auto annexBUnits(const std::vector<uint8_t>& data) const -> std::vector<std::pair<size_t, int>> {
    std::vector<std::pair<size_t, int>> units;
    for (size_t i = 0; i + 3 < data.size(); ++i) {
      if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
        const size_t header = i + 3;
        const int type = codec_id_ == OM_CODEC_H265 ? (data[header] >> 1) & 0x3f : data[header] & 0x1f;
        units.emplace_back(header, type);
        i += 2;
      }
    }
    return units;
  }

  auto isVcl(int type) const -> bool {
    return codec_id_ == OM_CODEC_H265 ? type < 32 : (type >= 1 && type <= 5);
  }

  auto isParameterSet(int type) const -> bool {
    return codec_id_ == OM_CODEC_H265 ? (type >= 32 && type <= 34) : (type == 7 || type == 8);
  }

  // Start code position of a NAL whose header is at `header`.
  static auto startOf(const std::vector<uint8_t>& data, size_t header) -> size_t {
    size_t start = header - 3;
    if (start > 0 && data[start - 1] == 0) --start;
    return start;
  }

  // The OBUs of a temporal unit: (offset, type, total size).
  static auto obus(const std::vector<uint8_t>& data) -> std::vector<std::tuple<size_t, int, size_t>> {
    std::vector<std::tuple<size_t, int, size_t>> out;
    size_t pos = 0;
    while (pos < data.size()) {
      const uint8_t header = data[pos];
      const int type = (header >> 3) & 0x0f;
      size_t cursor = pos + 1 + ((header & 0x04) ? 1 : 0);
      if (!(header & 0x02)) {
        out.emplace_back(pos, type, data.size() - pos);
        break;
      }
      uint64_t size = 0;
      for (uint32_t i = 0; i < 8 && cursor < data.size(); ++i) {
        const uint8_t byte = data[cursor++];
        size |= static_cast<uint64_t>(byte & 0x7f) << (7 * i);
        if (!(byte & 0x80)) break;
      }
      if (cursor + size > data.size()) break;
      out.emplace_back(pos, type, cursor + size - pos);
      pos = cursor + size;
    }
    return out;
  }

  // HDR10 static metadata goes with every key frame, where a player that
  // starts or seeks there picks it up.
  void addHdrMetadata(std::vector<uint8_t>& coded) const {
    if (!mastering_display_.has_value && !content_light_level_.has_value) return;
    if (codec_id_ == OM_CODEC_H264 || codec_id_ == OM_CODEC_H265) {
      const auto sei = hdrSeiNal(codec_id_ == OM_CODEC_H265, mastering_display_, content_light_level_);
      if (sei.empty()) return;
      for (const auto& [header, type] : annexBUnits(coded)) {
        if (!isVcl(type)) continue;
        const size_t at = startOf(coded, header);
        coded.insert(coded.begin() + static_cast<ptrdiff_t>(at), sei.begin(), sei.end());
        return;
      }
    } else if (codec_id_ == OM_CODEC_AV1) {
      const auto metadata = av1HdrMetadata(mastering_display_, content_light_level_);
      if (metadata.empty()) return;
      // After the temporal delimiter and sequence header, before the frame.
      size_t at = 0;
      for (const auto& [offset, type, size] : obus(coded)) {
        if (type == 1 || type == 2) {
          at = offset + size;
          continue;
        }
        break;
      }
      coded.insert(coded.begin() + static_cast<ptrdiff_t>(at), metadata.begin(), metadata.end());
    }
  }

  // Whatever parameter sets the key frame carries, whoever wrote them: the
  // muxer wants them out of band as well.
  void captureExtradata(const std::vector<uint8_t>& coded) {
    if (codec_id_ == OM_CODEC_H264 || codec_id_ == OM_CODEC_H265) {
      const auto units = annexBUnits(coded);
      for (size_t i = 0; i < units.size(); ++i) {
        if (!isParameterSet(units[i].second)) continue;
        const size_t begin = units[i].first;
        size_t end = i + 1 < units.size() ? startOf(coded, units[i + 1].first) : coded.size();
        while (end > begin && coded[end - 1] == 0) --end;
        static constexpr uint8_t kStartCode[4] = {0, 0, 0, 1};
        extradata_.insert(extradata_.end(), std::begin(kStartCode), std::end(kStartCode));
        extradata_.insert(extradata_.end(), coded.begin() + static_cast<ptrdiff_t>(begin),
                          coded.begin() + static_cast<ptrdiff_t>(end));
      }
    } else if (codec_id_ == OM_CODEC_AV1) {
      for (const auto& [offset, type, size] : obus(coded)) {
        if (type != 1) continue;
        extradata_.assign(coded.begin() + static_cast<ptrdiff_t>(offset),
                          coded.begin() + static_cast<ptrdiff_t>(offset + size));
        break;
      }
    }
  }

  // ---------------------------------------------------------------------------
  // State
  // ---------------------------------------------------------------------------

  bool initialized_ = false;
  OMCodecId codec_id_ = OM_CODEC_NONE;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t surface_width_ = 0;
  uint32_t surface_height_ = 0;
  uint32_t bit_depth_ = 8;
  VideoFormat input_format_ = {};
  ColourInfo colour_;
  Timing timing_;
  OMMasteringDisplayMetadata mastering_display_ = {};
  OMContentLightLevel content_light_level_ = {};
  uint32_t gop_ = 60;

  vaapi::OwnedDisplay owned_display_;
  VADisplay display_ = nullptr;
  VAProfile profile_ = VAProfileNone;
  VAEntrypoint entrypoint_ = VAEntrypointEncSlice;
  VAConfigID config_ = VA_INVALID_ID;
  VAContextID context_ = VA_INVALID_ID;
  VABufferID coded_buffers_[2] = {VA_INVALID_ID, VA_INVALID_ID};
  VABufferID coded_buffer_ = VA_INVALID_ID; // where the picture being built writes to
  int next_slot_ = 0;                       // input surface / coded buffer of the next picture
  std::optional<InFlight> in_flight_;
  uint32_t quality_range_ = 0;
  uint32_t quality_level_ = 0;
  std::shared_ptr<vaapi::SurfacePool> recon_;
  std::shared_ptr<vaapi::SurfacePool> input_;
  VAImage upload_image_ = {VA_INVALID_ID};
  bool derive_works_ = true;

  uint32_t rc_modes_ = VA_RC_CQP;
  uint32_t packed_headers_ = 0;
  uint32_t max_refs_l0_ = 1;
  bool p_as_b_ = false;
  uint32_t hevc_features_ = VA_ATTRIB_NOT_SUPPORTED;
  uint32_t hevc_block_sizes_ = VA_ATTRIB_NOT_SUPPORTED;
  uint32_t hevc_ctu_log2_ = 5;
  uint32_t av1_ext2_ = VA_ATTRIB_NOT_SUPPORTED;
  RateControl rc_;
  bool rc_changed_ = false;

  H264Headers h264_;
  H265Headers h265_;
  AV1Headers av1_;

  // Sequence position.
  bool have_reference_ = false;
  int current_recon_ = 0; // index of the reconstructed reference in recon_
  uint32_t frames_since_key_ = 0;
  uint32_t frame_num_ = 0;
  uint32_t idr_pic_id_ = 0;

  std::vector<uint8_t> extradata_;
};

} // namespace

auto createVAAPIEncoder() -> std::unique_ptr<Encoder> {
  return std::make_unique<VAAPIEncoder>();
}

} // namespace openmedia
