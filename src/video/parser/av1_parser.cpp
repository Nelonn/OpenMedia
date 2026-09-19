#include "av1_parser.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <util/bit_reader.hpp>

namespace openmedia::video_parser {

namespace {

constexpr uint32_t kMaxTileWidth = 4096;
constexpr uint32_t kMaxTileArea = 4096 * 2304;
constexpr uint32_t kRestorationTileSizeMax = 256;
constexpr uint32_t kWarpedModelPrecBits = 16;
constexpr uint32_t kGmAbsTransBits = 12;
constexpr uint32_t kGmAbsTransOnlyBits = 9;
constexpr uint32_t kGmAbsAlphaBits = 12;
constexpr uint32_t kGmTransPrecBits = 6;
constexpr uint32_t kGmTransOnlyPrecBits = 3;
constexpr uint32_t kGmAlphaPrecBits = 15;
constexpr uint32_t kSegLvlRefFrame = 5;

constexpr uint8_t kSegFeatureBits[AV1_SEG_LVL_MAX] = {8, 6, 6, 6, 6, 3, 0, 0};
constexpr bool kSegFeatureSigned[AV1_SEG_LVL_MAX] = {true, true, true, true, true, false, false, false};
constexpr int32_t kSegFeatureMax[AV1_SEG_LVL_MAX] = {255, 63, 63, 63, 63, 7, 0, 0};

constexpr uint8_t kRemapLrType[4] = {
    AV1_RESTORE_NONE, AV1_RESTORE_SWITCHABLE, AV1_RESTORE_WIENER, AV1_RESTORE_SGRPROJ};

// Ref_Frame_List from spec 7.8 set_frame_refs().
constexpr uint8_t kRefFrameList[AV1_REFS_PER_FRAME - 2] = {
    AV1_LAST2_FRAME, AV1_LAST3_FRAME, AV1_BWDREF_FRAME, AV1_ALTREF2_FRAME, AV1_ALTREF_FRAME};

auto floorLog2(uint32_t x) -> uint32_t {
  uint32_t s = 0;
  while (x != 0) {
    x >>= 1;
    ++s;
  }
  return s == 0 ? 0 : s - 1;
}

// tile_log2(blkSize, target) (spec 5.9.15).
auto tileLog2(uint32_t blk_size, uint32_t target) -> uint32_t {
  uint32_t k = 0;
  while ((blk_size << k) < target) ++k;
  return k;
}

auto inverseRecenter(int32_t r, int32_t v) -> int32_t {
  if (v > 2 * r) return v;
  if (v & 1) return r - ((v + 1) >> 1);
  return r + (v >> 1);
}

// Div_Lut (spec 7.11.3.7): round(2^22 / (256 + i)). Written out as the formula
// because every entry of the spec's table is exactly that value.
struct DivLut {
  uint16_t values[257] = {};
  constexpr DivLut() {
    for (uint32_t i = 0; i <= 256; ++i)
      values[i] = static_cast<uint16_t>(((1u << 22) + (256u + i) / 2u) / (256u + i));
  }
};
constexpr DivLut kDivLut;
static_assert(kDivLut.values[0] == 16384 && kDivLut.values[1] == 16320 && kDivLut.values[256] == 8192);

auto round2Signed(int64_t x, uint32_t n) -> int64_t {
  if (n == 0) return x;
  const int64_t half = int64_t {1} << (n - 1);
  return x >= 0 ? (x + half) >> n : -((-x + half) >> n);
}

// resolve_divisor (spec 7.11.3.7).
auto resolveDivisor(int32_t d, uint32_t& shift) -> int32_t {
  const uint32_t magnitude = static_cast<uint32_t>(d < 0 ? -static_cast<int64_t>(d) : d);
  const uint32_t n = floorLog2(magnitude);
  const int64_t e = static_cast<int64_t>(magnitude) - (int64_t {1} << n);
  const int64_t f = n > 8 ? round2Signed(e, n - 8) : e << (8 - n);
  shift = n + 14;
  const int32_t factor = kDivLut.values[std::clamp<int64_t>(f, 0, 256)];
  return d < 0 ? -factor : factor;
}

// setup_shear (spec 7.11.3.6): whether a global motion model describes a warp
// the predictor can actually carry out. Hardware decoders are told, rather than
// left to work it out, and fall back to translation for an invalid one.
auto shearParamsValid(const int32_t (&mat)[6]) -> bool {
  if (mat[2] <= 0) return false;
  constexpr int32_t kOne = 1 << kWarpedModelPrecBits;
  const auto clip16 = [](int64_t v) { return static_cast<int32_t>(std::clamp<int64_t>(v, -32768, 32767)); };
  const auto reduce = [](int32_t v) { return static_cast<int32_t>(round2Signed(v, 6) * 64); };

  uint32_t shift = 0;
  const int32_t divisor = resolveDivisor(mat[2], shift);
  const int64_t v = static_cast<int64_t>(mat[4]) * kOne;
  const int64_t w = static_cast<int64_t>(mat[3]) * mat[4];
  const int32_t alpha = reduce(clip16(static_cast<int64_t>(mat[2]) - kOne));
  const int32_t beta = reduce(clip16(mat[3]));
  const int32_t gamma = reduce(clip16(round2Signed(v * divisor, shift)));
  const int32_t delta = reduce(clip16(static_cast<int64_t>(mat[5]) - round2Signed(w * divisor, shift) - kOne));

  if (4 * std::abs(alpha) + 7 * std::abs(beta) >= kOne) return false;
  if (4 * std::abs(gamma) + 4 * std::abs(delta) >= kOne) return false;
  return true;
}

} // namespace

// AV1's descriptor set on top of the shared MSB-first BitReader.
class AV1ObuParser::Reader {
public:
  explicit Reader(std::span<const uint8_t> data) : bits_(data) {}

  auto f(uint32_t n) -> uint32_t { return n == 0 ? 0u : bits_.readBits(n); }
  auto flag() -> bool { return bits_.readFlag(); }
  auto f64(uint32_t n) -> uint64_t { return bits_.readBits64(n); }

  // su(n): n bits, two's complement.
  auto su(uint32_t n) -> int32_t {
    if (n == 0) return 0;
    auto value = static_cast<int32_t>(bits_.readBits(n));
    const int32_t sign_mask = 1 << (n - 1);
    if (value & sign_mask) value -= 2 * sign_mask;
    return value;
  }

  // ns(n): non-symmetric unsigned encoding (spec 4.10.7).
  auto ns(uint32_t n) -> uint32_t {
    if (n <= 1) return 0;
    const uint32_t w = floorLog2(n) + 1;
    const uint32_t m = (1u << w) - n;
    const uint32_t v = f(w - 1);
    if (v < m) return v;
    const uint32_t extra = f(1);
    return (v << 1) - m + extra;
  }

  // uvlc() (spec 4.10.3).
  auto uvlc() -> uint32_t {
    uint32_t leading_zeros = 0;
    while (leading_zeros < 32 && !flag()) ++leading_zeros;
    if (leading_zeros >= 32) return UINT32_MAX;
    return f(leading_zeros) + (1u << leading_zeros) - 1;
  }

  // decode_subexp(numSyms) (spec 4.10.9).
  auto decodeSubexp(uint32_t num_syms) -> uint32_t {
    uint32_t i = 0;
    uint32_t mk = 0;
    constexpr uint32_t k = 3;
    while (ok()) {
      const uint32_t b2 = i ? k + i - 1 : k;
      const uint32_t a = 1u << b2;
      if (num_syms <= mk + 3 * a) return ns(num_syms - mk) + mk;
      if (flag()) {
        ++i;
        mk += a;
      } else {
        return f(b2) + mk;
      }
    }
    return 0;
  }

  auto decodeUnsignedSubexpWithRef(uint32_t mx, int32_t r) -> int32_t {
    const auto v = static_cast<int32_t>(decodeSubexp(mx));
    if ((r << 1) <= static_cast<int32_t>(mx)) return inverseRecenter(r, v);
    return static_cast<int32_t>(mx) - 1 - inverseRecenter(static_cast<int32_t>(mx) - 1 - r, v);
  }

  auto decodeSignedSubexpWithRef(int32_t low, int32_t high, int32_t r) -> int32_t {
    return decodeUnsignedSubexpWithRef(static_cast<uint32_t>(high - low), r - low) + low;
  }

  void byteAlign() { bits_.alignToByte(); }
  auto ok() const -> bool { return bits_.ok(); }
  auto bitPosition() const -> size_t { return bits_.bitPosition(); }

private:
  BitReader bits_;
};

// ---------------------------------------------------------------------------
// Sequence header
// ---------------------------------------------------------------------------

namespace {

void readColorConfig(AV1ObuParser::Reader& r, AV1SequenceHeader& seq) {
  auto& cc = seq.color_config;
  const bool high_bitdepth = r.flag();
  if (seq.seq_profile == 2 && high_bitdepth) {
    cc.bit_depth = r.flag() ? 12 : 10;
  } else {
    cc.bit_depth = high_bitdepth ? 10 : 8;
  }

  cc.mono_chrome = (seq.seq_profile == 1) ? false : r.flag();
  cc.num_planes = cc.mono_chrome ? 1 : 3;

  cc.color_description_present_flag = r.flag();
  if (cc.color_description_present_flag) {
    cc.color_primaries = static_cast<uint8_t>(r.f(8));
    cc.transfer_characteristics = static_cast<uint8_t>(r.f(8));
    cc.matrix_coefficients = static_cast<uint8_t>(r.f(8));
  } else {
    cc.color_primaries = 2;          // CP_UNSPECIFIED
    cc.transfer_characteristics = 2; // TC_UNSPECIFIED
    cc.matrix_coefficients = 2;      // MC_UNSPECIFIED
  }

  if (cc.mono_chrome) {
    cc.color_range = r.flag();
    cc.subsampling_x = 1;
    cc.subsampling_y = 1;
    cc.chroma_sample_position = 0;
    cc.separate_uv_delta_q = false;
    return;
  }

  if (cc.color_primaries == 1 /*CP_BT_709*/ &&
      cc.transfer_characteristics == 13 /*TC_SRGB*/ &&
      cc.matrix_coefficients == 0 /*MC_IDENTITY*/) {
    cc.color_range = true;
    cc.subsampling_x = 0;
    cc.subsampling_y = 0;
  } else {
    cc.color_range = r.flag();
    if (seq.seq_profile == 0) {
      cc.subsampling_x = 1;
      cc.subsampling_y = 1;
    } else if (seq.seq_profile == 1) {
      cc.subsampling_x = 0;
      cc.subsampling_y = 0;
    } else {
      if (cc.bit_depth == 12) {
        cc.subsampling_x = static_cast<uint8_t>(r.f(1));
        cc.subsampling_y = cc.subsampling_x ? static_cast<uint8_t>(r.f(1)) : 0;
      } else {
        cc.subsampling_x = 1;
        cc.subsampling_y = 0;
      }
    }
    if (cc.subsampling_x && cc.subsampling_y)
      cc.chroma_sample_position = static_cast<uint8_t>(r.f(2));
  }

  cc.separate_uv_delta_q = r.flag();
}

} // namespace

auto AV1ObuParser::parseSequenceHeader(std::span<const uint8_t> payload) -> bool {
  Reader r(payload);
  AV1SequenceHeader seq;

  seq.seq_profile = static_cast<uint8_t>(r.f(3));
  seq.still_picture = r.flag();
  seq.reduced_still_picture_header = r.flag();

  if (seq.reduced_still_picture_header) {
    seq.operating_points_cnt = 1;
    seq.operating_point_idc[0] = 0;
    seq.seq_level_idx[0] = static_cast<uint8_t>(r.f(5));
  } else {
    seq.timing_info.present = r.flag();
    if (seq.timing_info.present) {
      seq.timing_info.num_units_in_display_tick = r.f(32);
      seq.timing_info.time_scale = r.f(32);
      seq.timing_info.equal_picture_interval = r.flag();
      if (seq.timing_info.equal_picture_interval)
        seq.timing_info.num_ticks_per_picture = r.uvlc() + 1;

      seq.decoder_model_info_present_flag = r.flag();
      if (seq.decoder_model_info_present_flag) {
        seq.buffer_delay_length = static_cast<uint8_t>(r.f(5)) + 1;
        seq.num_units_in_decoding_tick = r.f(32);
        seq.buffer_removal_time_length = static_cast<uint8_t>(r.f(5)) + 1;
        seq.frame_presentation_time_length = static_cast<uint8_t>(r.f(5)) + 1;
      }
    }

    seq.initial_display_delay_present_flag = r.flag();
    seq.operating_points_cnt = static_cast<uint8_t>(r.f(5)) + 1;
    for (uint32_t i = 0; i < seq.operating_points_cnt && i < AV1_MAX_OPERATING_POINTS; ++i) {
      seq.operating_point_idc[i] = static_cast<uint16_t>(r.f(12));
      seq.seq_level_idx[i] = static_cast<uint8_t>(r.f(5));
      seq.seq_tier[i] = seq.seq_level_idx[i] > 7 ? static_cast<uint8_t>(r.f(1)) : 0;

      if (seq.decoder_model_info_present_flag) {
        seq.decoder_model_present_for_this_op[i] = r.flag();
        if (seq.decoder_model_present_for_this_op[i]) {
          r.f64(seq.buffer_delay_length); // decoder_buffer_delay
          r.f64(seq.buffer_delay_length); // encoder_buffer_delay
          r.f(1);                         // low_delay_mode_flag
        }
      }
      if (seq.initial_display_delay_present_flag) {
        if (r.flag()) r.f(4); // initial_display_delay_minus_1
      }
    }
  }

  seq.frame_width_bits = static_cast<uint8_t>(r.f(4)) + 1;
  seq.frame_height_bits = static_cast<uint8_t>(r.f(4)) + 1;
  seq.max_frame_width = r.f(seq.frame_width_bits) + 1;
  seq.max_frame_height = r.f(seq.frame_height_bits) + 1;

  seq.frame_id_numbers_present_flag =
      seq.reduced_still_picture_header ? false : r.flag();
  if (seq.frame_id_numbers_present_flag) {
    seq.delta_frame_id_length = static_cast<uint8_t>(r.f(4)) + 2;
    seq.additional_frame_id_length = static_cast<uint8_t>(r.f(3)) + 1;
  }

  seq.use_128x128_superblock = r.flag();
  seq.enable_filter_intra = r.flag();
  seq.enable_intra_edge_filter = r.flag();

  if (seq.reduced_still_picture_header) {
    seq.seq_force_screen_content_tools = AV1_SELECT_SCREEN_CONTENT_TOOLS;
    seq.seq_force_integer_mv = AV1_SELECT_INTEGER_MV;
    seq.order_hint_bits = 0;
  } else {
    seq.enable_interintra_compound = r.flag();
    seq.enable_masked_compound = r.flag();
    seq.enable_warped_motion = r.flag();
    seq.enable_dual_filter = r.flag();
    seq.enable_order_hint = r.flag();
    if (seq.enable_order_hint) {
      seq.enable_jnt_comp = r.flag();
      seq.enable_ref_frame_mvs = r.flag();
    }

    seq.seq_force_screen_content_tools =
        r.flag() ? static_cast<uint8_t>(AV1_SELECT_SCREEN_CONTENT_TOOLS)
                 : static_cast<uint8_t>(r.f(1));

    if (seq.seq_force_screen_content_tools > 0) {
      seq.seq_force_integer_mv = r.flag() ? static_cast<uint8_t>(AV1_SELECT_INTEGER_MV)
                                          : static_cast<uint8_t>(r.f(1));
    } else {
      seq.seq_force_integer_mv = AV1_SELECT_INTEGER_MV;
    }

    seq.order_hint_bits = seq.enable_order_hint ? static_cast<uint8_t>(r.f(3)) + 1 : 0;
  }

  seq.enable_superres = r.flag();
  seq.enable_cdef = r.flag();
  seq.enable_restoration = r.flag();
  readColorConfig(r, seq);
  seq.film_grain_params_present = r.flag();

  seq.valid = r.ok();
  if (!seq.valid) return false;
  seq_ = seq;
  return true;
}

// ---------------------------------------------------------------------------
// Frame header
// ---------------------------------------------------------------------------

auto AV1ObuParser::getRelativeDist(int32_t a, int32_t b) const -> int32_t {
  if (!seq_.enable_order_hint) return 0;
  int32_t diff = a - b;
  const int32_t m = 1 << (seq_.order_hint_bits - 1);
  diff = (diff & (m - 1)) - (diff & m);
  return diff;
}

void AV1ObuParser::setFrameRefs(AV1FrameHeader& h, uint8_t last_frame_idx,
                                uint8_t gold_frame_idx) const {
  int32_t ref_frame_idx[AV1_REFS_PER_FRAME];
  for (auto& idx : ref_frame_idx) idx = -1;
  ref_frame_idx[AV1_LAST_FRAME - AV1_LAST_FRAME] = last_frame_idx;
  ref_frame_idx[AV1_GOLDEN_FRAME - AV1_LAST_FRAME] = gold_frame_idx;

  bool used_frame[AV1_NUM_REF_FRAMES] = {};
  used_frame[last_frame_idx] = true;
  used_frame[gold_frame_idx] = true;

  const int32_t cur_frame_hint = 1 << (seq_.order_hint_bits - 1);
  int32_t shifted[AV1_NUM_REF_FRAMES];
  for (uint32_t i = 0; i < AV1_NUM_REF_FRAMES; ++i)
    shifted[i] = cur_frame_hint +
                 getRelativeDist(static_cast<int32_t>(ref_order_hint_[i]),
                                 static_cast<int32_t>(h.order_hint));

  // find_latest_backward / find_earliest_backward / find_latest_forward
  auto find = [&](bool backward, bool latest) -> int32_t {
    int32_t ref = -1;
    int32_t best = 0;
    for (uint32_t i = 0; i < AV1_NUM_REF_FRAMES; ++i) {
      if (used_frame[i]) continue;
      const int32_t hint = shifted[i];
      const bool is_backward = hint >= cur_frame_hint;
      if (is_backward != backward) continue;
      if (ref < 0 || (latest ? hint > best : hint < best)) {
        ref = static_cast<int32_t>(i);
        best = hint;
      }
    }
    return ref;
  };

  int32_t ref = find(/*backward=*/true, /*latest=*/true);
  if (ref >= 0) {
    ref_frame_idx[AV1_ALTREF_FRAME - AV1_LAST_FRAME] = ref;
    used_frame[ref] = true;
  }
  ref = find(true, false);
  if (ref >= 0) {
    ref_frame_idx[AV1_BWDREF_FRAME - AV1_LAST_FRAME] = ref;
    used_frame[ref] = true;
  }
  ref = find(true, false);
  if (ref >= 0) {
    ref_frame_idx[AV1_ALTREF2_FRAME - AV1_LAST_FRAME] = ref;
    used_frame[ref] = true;
  }

  for (uint32_t i = 0; i < AV1_REFS_PER_FRAME - 2; ++i) {
    const uint8_t ref_frame = kRefFrameList[i];
    if (ref_frame_idx[ref_frame - AV1_LAST_FRAME] >= 0) continue;
    ref = find(/*backward=*/false, /*latest=*/true);
    if (ref >= 0) {
      ref_frame_idx[ref_frame - AV1_LAST_FRAME] = ref;
      used_frame[ref] = true;
    }
  }

  // Anything still unset takes the earliest reference overall.
  int32_t earliest = -1;
  int32_t earliest_hint = 0;
  for (uint32_t i = 0; i < AV1_NUM_REF_FRAMES; ++i) {
    const int32_t hint = shifted[i];
    if (earliest < 0 || hint < earliest_hint) {
      earliest = static_cast<int32_t>(i);
      earliest_hint = hint;
    }
  }
  for (auto& idx : ref_frame_idx)
    if (idx < 0) idx = earliest < 0 ? 0 : earliest;

  for (uint32_t i = 0; i < AV1_REFS_PER_FRAME; ++i)
    h.ref_frame_idx[i] = static_cast<uint8_t>(ref_frame_idx[i]);
}

auto AV1ObuParser::parseFrameHeader(std::span<const uint8_t> payload,
                                    uint8_t temporal_id, uint8_t spatial_id,
                                    size_t& bits_consumed) -> AV1FrameHeader {
  AV1FrameHeader h;
  bits_consumed = 0;
  if (!seq_.valid) return h;

  Reader r(payload);
  const auto& cc = seq_.color_config;
  const uint32_t id_len =
      seq_.frame_id_numbers_present_flag
          ? static_cast<uint32_t>(seq_.additional_frame_id_length + seq_.delta_frame_id_length)
          : 0;
  constexpr uint8_t kAllFrames = (1u << AV1_NUM_REF_FRAMES) - 1;

  if (seq_.reduced_still_picture_header) {
    h.frame_type = AV1_KEY_FRAME;
    h.frame_is_intra = true;
    h.show_frame = true;
  } else {
    h.show_existing_frame = r.flag();
    if (h.show_existing_frame) {
      h.frame_to_show_map_idx = static_cast<uint8_t>(r.f(3));
      if (seq_.decoder_model_info_present_flag && !seq_.timing_info.equal_picture_interval)
        r.f(seq_.frame_presentation_time_length); // temporal_point_info
      if (seq_.frame_id_numbers_present_flag) r.f(id_len); // display_frame_id

      const auto& slot = ref_slots_[h.frame_to_show_map_idx];
      h.frame_type = slot.frame_type;
      h.refresh_frame_flags = (h.frame_type == AV1_KEY_FRAME) ? kAllFrames : 0;
      h.frame_width = slot.frame_width;
      h.frame_height = slot.frame_height;
      h.upscaled_width = slot.upscaled_width;
      h.render_width = slot.render_width;
      h.render_height = slot.render_height;
      h.mi_cols = slot.mi_cols;
      h.mi_rows = slot.mi_rows;
      h.order_hint = slot.order_hint;
      h.show_frame = true;
      if (seq_.film_grain_params_present)
        h.film_grain = ref_film_grain_[h.frame_to_show_map_idx];
      // Showing a key frame runs the reference frame loading process (7.21)
      // and then refreshes every slot with it, so the state saved alongside
      // the frame has to come along or the slots end up with defaults.
      h.loop_filter = ref_loop_filter_[h.frame_to_show_map_idx];
      h.segmentation = ref_segmentation_[h.frame_to_show_map_idx];
      h.global_motion = ref_global_motion_[h.frame_to_show_map_idx];
      r.byteAlign();
      bits_consumed = r.bitPosition();
      h.valid = r.ok();
      return h;
    }

    h.frame_type = static_cast<uint8_t>(r.f(2));
    h.frame_is_intra =
        (h.frame_type == AV1_INTRA_ONLY_FRAME || h.frame_type == AV1_KEY_FRAME);
    h.show_frame = r.flag();
    if (h.show_frame && seq_.decoder_model_info_present_flag &&
        !seq_.timing_info.equal_picture_interval)
      r.f(seq_.frame_presentation_time_length); // temporal_point_info
    h.showable_frame = h.show_frame ? (h.frame_type != AV1_KEY_FRAME) : r.flag();

    if (h.frame_type == AV1_SWITCH_FRAME ||
        (h.frame_type == AV1_KEY_FRAME && h.show_frame)) {
      h.error_resilient_mode = true;
    } else {
      h.error_resilient_mode = r.flag();
    }
  }

  if (h.frame_type == AV1_KEY_FRAME && h.show_frame) {
    for (uint32_t i = 0; i < AV1_NUM_REF_FRAMES; ++i) {
      ref_valid_[i] = false;
      ref_order_hint_[i] = 0;
    }
  }

  h.disable_cdf_update = r.flag();
  h.allow_screen_content_tools =
      (seq_.seq_force_screen_content_tools == AV1_SELECT_SCREEN_CONTENT_TOOLS)
          ? r.flag()
          : seq_.seq_force_screen_content_tools != 0;

  if (h.allow_screen_content_tools) {
    h.force_integer_mv = (seq_.seq_force_integer_mv == AV1_SELECT_INTEGER_MV)
                             ? r.flag()
                             : seq_.seq_force_integer_mv != 0;
  }
  if (h.frame_is_intra) h.force_integer_mv = true;

  if (seq_.frame_id_numbers_present_flag) {
    h.current_frame_id = r.f(id_len);
    current_frame_id_ = h.current_frame_id;
  }

  if (h.frame_type == AV1_SWITCH_FRAME) {
    h.frame_size_override_flag = true;
  } else if (seq_.reduced_still_picture_header) {
    h.frame_size_override_flag = false;
  } else {
    h.frame_size_override_flag = r.flag();
  }

  h.order_hint = r.f(seq_.order_hint_bits);
  h.primary_ref_frame = (h.frame_is_intra || h.error_resilient_mode)
                            ? static_cast<uint8_t>(AV1_PRIMARY_REF_NONE)
                            : static_cast<uint8_t>(r.f(3));

  if (seq_.decoder_model_info_present_flag) {
    if (r.flag()) { // buffer_removal_time_present_flag
      for (uint32_t op = 0; op < seq_.operating_points_cnt && op < AV1_MAX_OPERATING_POINTS; ++op) {
        if (!seq_.decoder_model_present_for_this_op[op]) continue;
        const uint16_t idc = seq_.operating_point_idc[op];
        const bool in_temporal = ((idc >> temporal_id) & 1) != 0;
        const bool in_spatial = ((idc >> (spatial_id + 8)) & 1) != 0;
        if (idc == 0 || (in_temporal && in_spatial))
          r.f(seq_.buffer_removal_time_length);
      }
    }
  }

  if (h.frame_type == AV1_SWITCH_FRAME ||
      (h.frame_type == AV1_KEY_FRAME && h.show_frame)) {
    h.refresh_frame_flags = kAllFrames;
  } else {
    h.refresh_frame_flags = static_cast<uint8_t>(r.f(8));
  }

  if (!h.frame_is_intra || h.refresh_frame_flags != kAllFrames) {
    if (h.error_resilient_mode && seq_.enable_order_hint) {
      for (uint32_t i = 0; i < AV1_NUM_REF_FRAMES; ++i) {
        h.ref_order_hint[i] = r.f(seq_.order_hint_bits);
        if (h.ref_order_hint[i] != ref_order_hint_[i]) ref_valid_[i] = false;
      }
    }
  }

  // ---- frame size / references -----------------------------------------
  auto superres_params = [&]() {
    h.use_superres = seq_.enable_superres ? r.flag() : false;
    if (h.use_superres) {
      h.coded_denom = static_cast<uint8_t>(r.f(AV1_SUPERRES_DENOM_BITS));
      h.superres_denom = static_cast<uint8_t>(h.coded_denom + AV1_SUPERRES_DENOM_MIN);
    } else {
      h.coded_denom = 0;
      h.superres_denom = static_cast<uint8_t>(AV1_SUPERRES_NUM);
    }
    h.upscaled_width = h.frame_width;
    h.frame_width = (h.upscaled_width * AV1_SUPERRES_NUM + (h.superres_denom / 2)) / h.superres_denom;
  };
  auto compute_image_size = [&]() {
    h.mi_cols = 2 * ((h.frame_width + 7) >> 3);
    h.mi_rows = 2 * ((h.frame_height + 7) >> 3);
  };
  auto frame_size = [&]() {
    if (h.frame_size_override_flag) {
      h.frame_width = r.f(seq_.frame_width_bits) + 1;
      h.frame_height = r.f(seq_.frame_height_bits) + 1;
    } else {
      h.frame_width = seq_.max_frame_width;
      h.frame_height = seq_.max_frame_height;
    }
    superres_params();
    compute_image_size();
  };
  auto render_size = [&]() {
    h.render_and_frame_size_different = r.flag();
    if (h.render_and_frame_size_different) {
      h.render_width = r.f(16) + 1;
      h.render_height = r.f(16) + 1;
    } else {
      h.render_width = h.upscaled_width;
      h.render_height = h.frame_height;
    }
  };

  if (h.frame_is_intra) {
    frame_size();
    render_size();
    if (h.allow_screen_content_tools && h.upscaled_width == h.frame_width)
      h.allow_intrabc = r.flag();
  } else {
    h.frame_refs_short_signaling = false;
    if (seq_.enable_order_hint) {
      h.frame_refs_short_signaling = r.flag();
      if (h.frame_refs_short_signaling) {
        const auto last_frame_idx = static_cast<uint8_t>(r.f(3));
        const auto gold_frame_idx = static_cast<uint8_t>(r.f(3));
        setFrameRefs(h, last_frame_idx, gold_frame_idx);
      }
    }

    for (uint32_t i = 0; i < AV1_REFS_PER_FRAME; ++i) {
      if (!h.frame_refs_short_signaling) h.ref_frame_idx[i] = static_cast<uint8_t>(r.f(3));
      if (seq_.frame_id_numbers_present_flag) r.f(seq_.delta_frame_id_length);
    }

    if (h.frame_size_override_flag && !h.error_resilient_mode) {
      // frame_size_with_refs()
      bool found_ref = false;
      for (uint32_t i = 0; i < AV1_REFS_PER_FRAME; ++i) {
        found_ref = r.flag();
        if (!found_ref) continue;
        const auto& slot = ref_slots_[h.ref_frame_idx[i]];
        h.upscaled_width = slot.upscaled_width;
        h.frame_width = h.upscaled_width;
        h.frame_height = slot.frame_height;
        h.render_width = slot.render_width;
        h.render_height = slot.render_height;
        break;
      }
      if (!found_ref) {
        frame_size();
        render_size();
      } else {
        superres_params();
        compute_image_size();
      }
    } else {
      frame_size();
      render_size();
    }

    h.allow_high_precision_mv = h.force_integer_mv ? false : r.flag();

    // read_interpolation_filter()
    h.interpolation_filter = r.flag() ? static_cast<uint8_t>(AV1_SWITCHABLE)
                                      : static_cast<uint8_t>(r.f(2));

    h.is_motion_mode_switchable = r.flag();
    h.use_ref_frame_mvs =
        (h.error_resilient_mode || !seq_.enable_ref_frame_mvs) ? false : r.flag();

    for (uint32_t i = 0; i < AV1_REFS_PER_FRAME; ++i)
      h.order_hints[AV1_LAST_FRAME + i] = ref_order_hint_[h.ref_frame_idx[i]];
  }

  h.disable_frame_end_update_cdf =
      (seq_.reduced_still_picture_header || h.disable_cdf_update) ? true : r.flag();

  // Inherit the previous frame's deltas when a primary reference is in use.
  if (h.primary_ref_frame != AV1_PRIMARY_REF_NONE) {
    const uint8_t slot = h.ref_frame_idx[h.primary_ref_frame];
    h.loop_filter = ref_loop_filter_[slot];
    h.segmentation = ref_segmentation_[slot];
    h.global_motion = ref_global_motion_[slot];
  }

  // ---- tile_info() ------------------------------------------------------
  {
    auto& ti = h.tile_info;
    const uint32_t sb_shift = seq_.use_128x128_superblock ? 5 : 4;
    const uint32_t sb_size = sb_shift + 2;
    const uint32_t sb_cols = seq_.use_128x128_superblock ? ((h.mi_cols + 31) >> 5)
                                                         : ((h.mi_cols + 15) >> 4);
    const uint32_t sb_rows = seq_.use_128x128_superblock ? ((h.mi_rows + 31) >> 5)
                                                         : ((h.mi_rows + 15) >> 4);
    const uint32_t max_tile_width_sb = kMaxTileWidth >> sb_size;
    uint32_t max_tile_area_sb = kMaxTileArea >> (2 * sb_size);
    const uint32_t min_log2_tile_cols = tileLog2(max_tile_width_sb, sb_cols);
    const uint32_t max_log2_tile_cols = tileLog2(1, std::min(sb_cols, AV1_MAX_TILE_COLS));
    const uint32_t max_log2_tile_rows = tileLog2(1, std::min(sb_rows, AV1_MAX_TILE_ROWS));
    const uint32_t min_log2_tiles =
        std::max(min_log2_tile_cols, tileLog2(max_tile_area_sb, sb_rows * sb_cols));

    ti.uniform_tile_spacing_flag = r.flag();
    if (ti.uniform_tile_spacing_flag) {
      ti.tile_cols_log2 = min_log2_tile_cols;
      while (ti.tile_cols_log2 < max_log2_tile_cols) {
        if (!r.flag()) break;
        ++ti.tile_cols_log2;
      }
      const uint32_t tile_width_sb =
          (sb_cols + (1u << ti.tile_cols_log2) - 1) >> ti.tile_cols_log2;
      uint32_t i = 0;
      for (uint32_t start_sb = 0; start_sb < sb_cols && i < AV1_MAX_TILE_COLS;
           start_sb += tile_width_sb) {
        ti.mi_col_starts[i++] = start_sb << sb_shift;
      }
      ti.mi_col_starts[i] = h.mi_cols;
      ti.tile_cols = i;

      const uint32_t min_log2_tile_rows =
          min_log2_tiles > ti.tile_cols_log2 ? min_log2_tiles - ti.tile_cols_log2 : 0;
      ti.tile_rows_log2 = min_log2_tile_rows;
      while (ti.tile_rows_log2 < max_log2_tile_rows) {
        if (!r.flag()) break;
        ++ti.tile_rows_log2;
      }
      const uint32_t tile_height_sb =
          (sb_rows + (1u << ti.tile_rows_log2) - 1) >> ti.tile_rows_log2;
      i = 0;
      for (uint32_t start_sb = 0; start_sb < sb_rows && i < AV1_MAX_TILE_ROWS;
           start_sb += tile_height_sb) {
        ti.mi_row_starts[i++] = start_sb << sb_shift;
      }
      ti.mi_row_starts[i] = h.mi_rows;
      ti.tile_rows = i;
    } else {
      uint32_t widest_tile_sb = 0;
      uint32_t start_sb = 0;
      uint32_t i = 0;
      for (; start_sb < sb_cols && i < AV1_MAX_TILE_COLS; ++i) {
        ti.mi_col_starts[i] = start_sb << sb_shift;
        const uint32_t max_width = std::min(sb_cols - start_sb, max_tile_width_sb);
        const uint32_t size_sb = r.ns(max_width) + 1;
        widest_tile_sb = std::max(size_sb, widest_tile_sb);
        start_sb += size_sb;
      }
      ti.mi_col_starts[i] = h.mi_cols;
      ti.tile_cols = i;
      ti.tile_cols_log2 = tileLog2(1, ti.tile_cols);

      if (min_log2_tiles > 0) max_tile_area_sb = (sb_rows * sb_cols) >> (min_log2_tiles + 1);
      else max_tile_area_sb = sb_rows * sb_cols;
      const uint32_t max_tile_height_sb =
          std::max(widest_tile_sb ? max_tile_area_sb / widest_tile_sb : 1u, 1u);

      start_sb = 0;
      i = 0;
      for (; start_sb < sb_rows && i < AV1_MAX_TILE_ROWS; ++i) {
        ti.mi_row_starts[i] = start_sb << sb_shift;
        const uint32_t max_height = std::min(sb_rows - start_sb, max_tile_height_sb);
        const uint32_t size_sb = r.ns(max_height) + 1;
        start_sb += size_sb;
      }
      ti.mi_row_starts[i] = h.mi_rows;
      ti.tile_rows = i;
      ti.tile_rows_log2 = tileLog2(1, ti.tile_rows);
    }

    if (ti.tile_cols_log2 > 0 || ti.tile_rows_log2 > 0) {
      ti.context_update_tile_id = r.f(ti.tile_rows_log2 + ti.tile_cols_log2);
      ti.tile_size_bytes = static_cast<uint8_t>(r.f(2)) + 1;
    } else {
      ti.context_update_tile_id = 0;
    }
  }

  // ---- quantization_params() -------------------------------------------
  {
    auto& q = h.quantization;
    auto read_delta_q = [&]() -> int32_t { return r.flag() ? r.su(7) : 0; };

    q.base_q_idx = static_cast<uint8_t>(r.f(8));
    q.delta_q_y_dc = read_delta_q();
    if (cc.num_planes > 1) {
      const bool diff_uv_delta = cc.separate_uv_delta_q ? r.flag() : false;
      q.delta_q_u_dc = read_delta_q();
      q.delta_q_u_ac = read_delta_q();
      if (diff_uv_delta) {
        q.delta_q_v_dc = read_delta_q();
        q.delta_q_v_ac = read_delta_q();
      } else {
        q.delta_q_v_dc = q.delta_q_u_dc;
        q.delta_q_v_ac = q.delta_q_u_ac;
      }
    }
    q.using_qmatrix = r.flag();
    if (q.using_qmatrix) {
      q.qm_y = static_cast<uint8_t>(r.f(4));
      q.qm_u = static_cast<uint8_t>(r.f(4));
      q.qm_v = cc.separate_uv_delta_q ? static_cast<uint8_t>(r.f(4)) : q.qm_u;
    }
  }

  // ---- segmentation_params() -------------------------------------------
  {
    auto& seg = h.segmentation;
    seg.enabled = r.flag();
    if (seg.enabled) {
      if (h.primary_ref_frame == AV1_PRIMARY_REF_NONE) {
        seg.update_map = true;
        seg.temporal_update = false;
        seg.update_data = true;
      } else {
        seg.update_map = r.flag();
        seg.temporal_update = seg.update_map ? r.flag() : false;
        seg.update_data = r.flag();
      }
      if (seg.update_data) {
        for (uint32_t i = 0; i < AV1_MAX_SEGMENTS; ++i) {
          for (uint32_t j = 0; j < AV1_SEG_LVL_MAX; ++j) {
            int32_t clipped = 0;
            const bool enabled = r.flag();
            seg.feature_enabled[i][j] = enabled;
            if (enabled) {
              const uint8_t bits = kSegFeatureBits[j];
              const int32_t limit = kSegFeatureMax[j];
              if (kSegFeatureSigned[j]) {
                clipped = std::clamp(r.su(bits + 1), -limit, limit);
              } else {
                clipped = std::clamp(static_cast<int32_t>(r.f(bits)), 0, limit);
              }
            }
            seg.feature_data[i][j] = static_cast<int16_t>(clipped);
          }
        }
      }
    } else {
      seg = {};
    }

    seg.seg_id_pre_skip = false;
    seg.last_active_seg_id = 0;
    for (uint32_t i = 0; i < AV1_MAX_SEGMENTS; ++i) {
      for (uint32_t j = 0; j < AV1_SEG_LVL_MAX; ++j) {
        if (!seg.feature_enabled[i][j]) continue;
        seg.last_active_seg_id = static_cast<uint8_t>(i);
        if (j >= kSegLvlRefFrame) seg.seg_id_pre_skip = true;
      }
    }
  }

  // ---- delta_q_params() / delta_lf_params() -----------------------------
  h.delta_q_present = h.quantization.base_q_idx > 0 ? r.flag() : false;
  if (h.delta_q_present) h.delta_q_res = static_cast<uint8_t>(r.f(2));

  // delta_lf_params() starts by zeroing these. Leaving them inherited from the
  // primary reference frame (loaded above) would switch on per-superblock loop
  // filter deltas for a frame that never coded any.
  h.loop_filter.delta_lf_present = false;
  h.loop_filter.delta_lf_res = 0;
  h.loop_filter.delta_lf_multi = false;
  if (h.delta_q_present) {
    if (!h.allow_intrabc) h.loop_filter.delta_lf_present = r.flag();
    if (h.loop_filter.delta_lf_present) {
      h.loop_filter.delta_lf_res = static_cast<uint8_t>(r.f(2));
      h.loop_filter.delta_lf_multi = r.flag();
    }
  }

  // CodedLossless / AllLossless (spec 5.9.12), needed by the filter sections.
  {
    const auto& q = h.quantization;
    h.coded_lossless = true;
    for (uint32_t segment = 0; segment < AV1_MAX_SEGMENTS; ++segment) {
      int32_t qindex = q.base_q_idx;
      if (h.segmentation.enabled && h.segmentation.feature_enabled[segment][0]) {
        qindex = q.base_q_idx + h.segmentation.feature_data[segment][0];
      }
      qindex = std::clamp(qindex, 0, 255);
      const bool lossless = qindex == 0 && q.delta_q_y_dc == 0 && q.delta_q_u_ac == 0 &&
                            q.delta_q_u_dc == 0 && q.delta_q_v_ac == 0 && q.delta_q_v_dc == 0;
      if (!lossless) {
        h.coded_lossless = false;
        break;
      }
    }
    h.all_lossless = h.coded_lossless && (h.frame_width == h.upscaled_width);
  }

  // ---- loop_filter_params() ---------------------------------------------
  {
    auto& lf = h.loop_filter;
    if (h.coded_lossless || h.allow_intrabc) {
      lf.level[0] = 0;
      lf.level[1] = 0;
      const int8_t defaults[AV1_TOTAL_REFS_PER_FRAME] = {1, 0, 0, 0, -1, 0, -1, -1};
      std::memcpy(lf.ref_deltas, defaults, sizeof(defaults));
      lf.mode_deltas[0] = 0;
      lf.mode_deltas[1] = 0;
    } else {
      lf.level[0] = static_cast<uint8_t>(r.f(6));
      lf.level[1] = static_cast<uint8_t>(r.f(6));
      if (cc.num_planes > 1 && (lf.level[0] || lf.level[1])) {
        lf.level[2] = static_cast<uint8_t>(r.f(6));
        lf.level[3] = static_cast<uint8_t>(r.f(6));
      }
      lf.sharpness = static_cast<uint8_t>(r.f(3));
      lf.delta_enabled = r.flag();
      if (lf.delta_enabled) {
        lf.delta_update = r.flag();
        if (lf.delta_update) {
          for (uint32_t i = 0; i < AV1_TOTAL_REFS_PER_FRAME; ++i)
            if (r.flag()) lf.ref_deltas[i] = static_cast<int8_t>(r.su(7));
          for (auto& mode_delta : lf.mode_deltas)
            if (r.flag()) mode_delta = static_cast<int8_t>(r.su(7));
        }
      }
    }
  }

  // ---- cdef_params() -----------------------------------------------------
  {
    auto& cdef = h.cdef;
    if (h.coded_lossless || h.allow_intrabc || !seq_.enable_cdef) {
      cdef.bits = 0;
      cdef.y_pri_strength[0] = 0;
      cdef.y_sec_strength[0] = 0;
      cdef.uv_pri_strength[0] = 0;
      cdef.uv_sec_strength[0] = 0;
      cdef.damping = 3;
    } else {
      cdef.damping = static_cast<uint8_t>(r.f(2)) + 3;
      cdef.bits = static_cast<uint8_t>(r.f(2));
      for (uint32_t i = 0; i < (1u << cdef.bits); ++i) {
        // These are kept as the coded syntax elements (0..3). The spec turns a
        // coded 3 into a strength of 4, but that is a decoder-internal
        // derivation: DXVA and the Vulkan StdVideo structures both want the
        // two-bit value straight from the bitstream.
        cdef.y_pri_strength[i] = static_cast<uint8_t>(r.f(4));
        cdef.y_sec_strength[i] = static_cast<uint8_t>(r.f(2));
        if (cc.num_planes > 1) {
          cdef.uv_pri_strength[i] = static_cast<uint8_t>(r.f(4));
          cdef.uv_sec_strength[i] = static_cast<uint8_t>(r.f(2));
        }
      }
    }
  }

  // ---- lr_params() -------------------------------------------------------
  {
    auto& lr = h.lr;
    if (h.all_lossless || h.allow_intrabc || !seq_.enable_restoration) {
      lr.frame_restoration_type[0] = AV1_RESTORE_NONE;
      lr.frame_restoration_type[1] = AV1_RESTORE_NONE;
      lr.frame_restoration_type[2] = AV1_RESTORE_NONE;
      lr.uses_lr = false;
    } else {
      lr.uses_lr = false;
      bool uses_chroma_lr = false;
      for (uint32_t i = 0; i < cc.num_planes; ++i) {
        lr.frame_restoration_type[i] = kRemapLrType[r.f(2) & 3u];
        if (lr.frame_restoration_type[i] != AV1_RESTORE_NONE) {
          lr.uses_lr = true;
          if (i > 0) uses_chroma_lr = true;
        }
      }
      if (lr.uses_lr) {
        uint32_t lr_unit_shift = 0;
        if (seq_.use_128x128_superblock) {
          lr_unit_shift = r.f(1) + 1;
        } else {
          lr_unit_shift = r.f(1);
          if (lr_unit_shift) lr_unit_shift += r.f(1);
        }
        lr.loop_restoration_size[0] =
            static_cast<uint16_t>(kRestorationTileSizeMax >> (2 - lr_unit_shift));
        const uint32_t lr_uv_shift =
            (cc.subsampling_x && cc.subsampling_y && uses_chroma_lr) ? r.f(1) : 0;
        lr.loop_restoration_size[1] =
            static_cast<uint16_t>(lr.loop_restoration_size[0] >> lr_uv_shift);
        lr.loop_restoration_size[2] = lr.loop_restoration_size[1];

        // log2(RESTORATION_TILESIZE_MAX) is 8; accelerators want the log2 form.
        lr.loop_restoration_size_log2[0] = static_cast<uint8_t>(6 + lr_unit_shift);
        lr.loop_restoration_size_log2[1] =
            static_cast<uint8_t>(lr.loop_restoration_size_log2[0] - lr_uv_shift);
        lr.loop_restoration_size_log2[2] = lr.loop_restoration_size_log2[1];
      }
    }
  }

  // ---- read_tx_mode() / frame_reference_mode() --------------------------
  h.tx_mode = h.coded_lossless ? static_cast<uint8_t>(AV1_ONLY_4X4)
                               : (r.flag() ? static_cast<uint8_t>(AV1_TX_MODE_SELECT)
                                           : static_cast<uint8_t>(AV1_TX_MODE_LARGEST));
  h.reference_select = h.frame_is_intra ? false : r.flag();

  // ---- skip_mode_params() -----------------------------------------------
  {
    bool skip_mode_allowed = false;
    if (!h.frame_is_intra && h.reference_select && seq_.enable_order_hint) {
      int32_t forward_idx = -1, backward_idx = -1;
      int32_t forward_hint = 0, backward_hint = 0;
      for (uint32_t i = 0; i < AV1_REFS_PER_FRAME; ++i) {
        const auto ref_hint = static_cast<int32_t>(ref_order_hint_[h.ref_frame_idx[i]]);
        if (getRelativeDist(ref_hint, static_cast<int32_t>(h.order_hint)) < 0) {
          if (forward_idx < 0 || getRelativeDist(ref_hint, forward_hint) > 0) {
            forward_idx = static_cast<int32_t>(i);
            forward_hint = ref_hint;
          }
        } else if (getRelativeDist(ref_hint, static_cast<int32_t>(h.order_hint)) > 0) {
          if (backward_idx < 0 || getRelativeDist(ref_hint, backward_hint) < 0) {
            backward_idx = static_cast<int32_t>(i);
            backward_hint = ref_hint;
          }
        }
      }
      if (forward_idx >= 0) {
        if (backward_idx >= 0) {
          skip_mode_allowed = true;
          h.skip_mode_frame[0] = static_cast<uint8_t>(AV1_LAST_FRAME + std::min(forward_idx, backward_idx));
          h.skip_mode_frame[1] = static_cast<uint8_t>(AV1_LAST_FRAME + std::max(forward_idx, backward_idx));
        } else {
          int32_t second_forward_idx = -1;
          int32_t second_forward_hint = 0;
          for (uint32_t i = 0; i < AV1_REFS_PER_FRAME; ++i) {
            const auto ref_hint = static_cast<int32_t>(ref_order_hint_[h.ref_frame_idx[i]]);
            if (getRelativeDist(ref_hint, forward_hint) < 0) {
              if (second_forward_idx < 0 || getRelativeDist(ref_hint, second_forward_hint) > 0) {
                second_forward_idx = static_cast<int32_t>(i);
                second_forward_hint = ref_hint;
              }
            }
          }
          if (second_forward_idx >= 0) {
            skip_mode_allowed = true;
            h.skip_mode_frame[0] = static_cast<uint8_t>(AV1_LAST_FRAME + std::min(forward_idx, second_forward_idx));
            h.skip_mode_frame[1] = static_cast<uint8_t>(AV1_LAST_FRAME + std::max(forward_idx, second_forward_idx));
          }
        }
      }
    }
    h.skip_mode_present = skip_mode_allowed ? r.flag() : false;
  }

  h.allow_warped_motion =
      (h.frame_is_intra || h.error_resilient_mode || !seq_.enable_warped_motion) ? false
                                                                                 : r.flag();
  h.reduced_tx_set = r.flag();

  // ---- global_motion_params() -------------------------------------------
  {
    auto& gm = h.global_motion;

    // PrevGmParams: setup_past_independence() seeds it with the *identity*
    // warp, not zeros. load_previous() replaces it with the primary reference
    // frame's saved parameters (already loaded into gm above). Getting this
    // wrong does not desync the bitstream — the subexp codes are the same
    // length either way — it just decodes every global motion parameter to the
    // wrong value, which shows up as drifting/warped inter frames.
    AV1GlobalMotionParams prev;
    for (uint32_t ref = AV1_LAST_FRAME; ref <= AV1_ALTREF_FRAME; ++ref) {
      for (uint32_t i = 0; i < 6; ++i)
        prev.params[ref][i] = (i % 3 == 2) ? (1 << kWarpedModelPrecBits) : 0;
    }
    if (h.primary_ref_frame != AV1_PRIMARY_REF_NONE) prev = gm;

    for (uint32_t ref = AV1_LAST_FRAME; ref <= AV1_ALTREF_FRAME; ++ref) {
      gm.type[ref] = AV1_IDENTITY;
      for (uint32_t i = 0; i < 6; ++i)
        gm.params[ref][i] = (i % 3 == 2) ? (1 << kWarpedModelPrecBits) : 0;
    }
    if (!h.frame_is_intra) {
      auto read_global_param = [&](uint8_t type, uint32_t ref, uint32_t idx) {
        uint32_t abs_bits = kGmAbsAlphaBits;
        uint32_t prec_bits = kGmAlphaPrecBits;
        if (idx < 2) {
          if (type == AV1_TRANSLATION) {
            abs_bits = kGmAbsTransOnlyBits - (h.allow_high_precision_mv ? 0u : 1u);
            prec_bits = kGmTransOnlyPrecBits - (h.allow_high_precision_mv ? 0u : 1u);
          } else {
            abs_bits = kGmAbsTransBits;
            prec_bits = kGmTransPrecBits;
          }
        }
        const uint32_t prec_diff = kWarpedModelPrecBits - prec_bits;
        const int32_t round = (idx % 3) == 2 ? (1 << kWarpedModelPrecBits) : 0;
        const int32_t sub = (idx % 3) == 2 ? (1 << prec_bits) : 0;
        const int32_t mx = 1 << abs_bits;
        const int32_t ref_value = (prev.params[ref][idx] >> prec_diff) - sub;
        gm.params[ref][idx] =
            (r.decodeSignedSubexpWithRef(-mx, mx + 1, ref_value) << prec_diff) + round;
      };

      for (uint32_t ref = AV1_LAST_FRAME; ref <= AV1_ALTREF_FRAME; ++ref) {
        uint8_t type = AV1_IDENTITY;
        if (r.flag()) { // is_global
          if (r.flag()) {
            type = AV1_ROTZOOM;
          } else {
            type = r.flag() ? AV1_TRANSLATION : AV1_AFFINE;
          }
        }
        gm.type[ref] = type;

        if (type >= AV1_ROTZOOM) {
          read_global_param(type, ref, 2);
          read_global_param(type, ref, 3);
          if (type == AV1_AFFINE) {
            read_global_param(type, ref, 4);
            read_global_param(type, ref, 5);
          } else {
            gm.params[ref][4] = -gm.params[ref][3];
            gm.params[ref][5] = gm.params[ref][2];
          }
        }
        if (type >= AV1_TRANSLATION) {
          read_global_param(type, ref, 0);
          read_global_param(type, ref, 1);
        }
      }
    }
    for (uint32_t ref = AV1_LAST_FRAME; ref <= AV1_ALTREF_FRAME; ++ref)
      gm.invalid[ref] = !shearParamsValid(gm.params[ref]);
  }

  // ---- film_grain_params() ----------------------------------------------
  {
    auto& fg = h.film_grain;
    if (!seq_.film_grain_params_present || (!h.show_frame && !h.showable_frame)) {
      fg = {};
    } else {
      fg.apply_grain = r.flag();
      if (!fg.apply_grain) {
        fg = {};
      } else {
        fg.grain_seed = static_cast<uint16_t>(r.f(16));
        fg.update_grain = (h.frame_type == AV1_INTER_FRAME) ? r.flag() : true;
        if (!fg.update_grain) {
          fg.film_grain_params_ref_idx = static_cast<uint8_t>(r.f(3));
          const uint16_t temp_seed = fg.grain_seed;
          fg = ref_film_grain_[fg.film_grain_params_ref_idx];
          fg.grain_seed = temp_seed;
          fg.update_grain = false;
        } else {
          fg.num_y_points = static_cast<uint8_t>(r.f(4));
          for (uint32_t i = 0; i < fg.num_y_points && i < 14; ++i) {
            fg.point_y_value[i] = static_cast<uint8_t>(r.f(8));
            fg.point_y_scaling[i] = static_cast<uint8_t>(r.f(8));
          }
          fg.chroma_scaling_from_luma = cc.mono_chrome ? false : r.flag();

          if (cc.mono_chrome || fg.chroma_scaling_from_luma ||
              (cc.subsampling_x == 1 && cc.subsampling_y == 1 && fg.num_y_points == 0)) {
            fg.num_cb_points = 0;
            fg.num_cr_points = 0;
          } else {
            fg.num_cb_points = static_cast<uint8_t>(r.f(4));
            for (uint32_t i = 0; i < fg.num_cb_points && i < 10; ++i) {
              fg.point_cb_value[i] = static_cast<uint8_t>(r.f(8));
              fg.point_cb_scaling[i] = static_cast<uint8_t>(r.f(8));
            }
            fg.num_cr_points = static_cast<uint8_t>(r.f(4));
            for (uint32_t i = 0; i < fg.num_cr_points && i < 10; ++i) {
              fg.point_cr_value[i] = static_cast<uint8_t>(r.f(8));
              fg.point_cr_scaling[i] = static_cast<uint8_t>(r.f(8));
            }
          }

          fg.grain_scaling = static_cast<uint8_t>(r.f(2)) + 8;
          fg.ar_coeff_lag = static_cast<uint8_t>(r.f(2));
          const uint32_t num_pos_luma = 2 * fg.ar_coeff_lag * (fg.ar_coeff_lag + 1);
          uint32_t num_pos_chroma = num_pos_luma;
          if (fg.num_y_points) {
            num_pos_chroma = num_pos_luma + 1;
            for (uint32_t i = 0; i < num_pos_luma && i < 24; ++i)
              fg.ar_coeffs_y[i] = static_cast<uint8_t>(r.f(8));
          }
          if (fg.chroma_scaling_from_luma || fg.num_cb_points)
            for (uint32_t i = 0; i < num_pos_chroma && i < 25; ++i)
              fg.ar_coeffs_cb[i] = static_cast<uint8_t>(r.f(8));
          if (fg.chroma_scaling_from_luma || fg.num_cr_points)
            for (uint32_t i = 0; i < num_pos_chroma && i < 25; ++i)
              fg.ar_coeffs_cr[i] = static_cast<uint8_t>(r.f(8));

          fg.ar_coeff_shift = static_cast<uint8_t>(r.f(2)) + 6;
          fg.grain_scale_shift = static_cast<uint8_t>(r.f(2));
          if (fg.num_cb_points) {
            fg.cb_mult = static_cast<uint8_t>(r.f(8));
            fg.cb_luma_mult = static_cast<uint8_t>(r.f(8));
            fg.cb_offset = static_cast<uint16_t>(r.f(9));
          }
          if (fg.num_cr_points) {
            fg.cr_mult = static_cast<uint8_t>(r.f(8));
            fg.cr_luma_mult = static_cast<uint8_t>(r.f(8));
            fg.cr_offset = static_cast<uint16_t>(r.f(9));
          }
          fg.overlap_flag = r.flag();
          fg.clip_to_restricted_range = r.flag();
        }
      }
    }
  }

  bits_consumed = r.bitPosition();
  h.valid = r.ok() && h.frame_width > 0 && h.frame_height > 0;
  return h;
}

// ---------------------------------------------------------------------------
// Metadata (HDR)
// ---------------------------------------------------------------------------

void AV1ObuParser::parseMetadata(std::span<const uint8_t> payload) {
  if (payload.empty()) return;

  // metadata_type is a leb128; in practice it is a single byte for the types
  // that matter here.
  size_t pos = 0;
  uint64_t metadata_type = 0;
  for (uint32_t i = 0; i < 8 && pos < payload.size(); ++i) {
    const uint8_t byte = payload[pos++];
    metadata_type |= static_cast<uint64_t>(byte & 0x7F) << (i * 7);
    if ((byte & 0x80) == 0) break;
  }

  auto body = payload.subspan(std::min(pos, payload.size()));
  Reader r(body);

  if (metadata_type == AV1_METADATA_TYPE_HDR_CLL) {
    if (body.size() < 4) return;
    hdr_.max_cll = static_cast<uint16_t>(r.f(16));
    hdr_.max_fall = static_cast<uint16_t>(r.f(16));
    hdr_.has_cll = r.ok();
  } else if (metadata_type == AV1_METADATA_TYPE_HDR_MDCV) {
    if (body.size() < 24) return;
    for (uint32_t i = 0; i < 3; ++i) {
      hdr_.primary_chromaticity_x[i] = static_cast<uint16_t>(r.f(16));
      hdr_.primary_chromaticity_y[i] = static_cast<uint16_t>(r.f(16));
    }
    hdr_.white_point_chromaticity_x = static_cast<uint16_t>(r.f(16));
    hdr_.white_point_chromaticity_y = static_cast<uint16_t>(r.f(16));
    hdr_.luminance_max = r.f(32);
    hdr_.luminance_min = r.f(32);
    hdr_.has_mdcv = r.ok();
  }
}

// ---------------------------------------------------------------------------
// Reference state
// ---------------------------------------------------------------------------

void AV1ObuParser::updateRefSlots(const AV1FrameHeader& h) {
  if (!h.valid) return;

  RefSlot updated;
  updated.valid = true;
  updated.frame_type = h.frame_type;
  updated.upscaled_width = h.upscaled_width;
  updated.frame_width = h.frame_width;
  updated.frame_height = h.frame_height;
  updated.render_width = h.render_width;
  updated.render_height = h.render_height;
  updated.mi_cols = h.mi_cols;
  updated.mi_rows = h.mi_rows;
  updated.order_hint = h.order_hint;
  updated.frame_id = h.current_frame_id;
  updated.bit_depth = seq_.color_config.bit_depth;
  updated.subsampling_x = seq_.color_config.subsampling_x;
  updated.subsampling_y = seq_.color_config.subsampling_y;

  for (uint32_t i = 0; i < AV1_NUM_REF_FRAMES; ++i) {
    if ((h.refresh_frame_flags & (1u << i)) == 0) continue;
    ref_slots_[i] = updated;
    ref_valid_[i] = true;
    ref_order_hint_[i] = h.order_hint;
    ref_film_grain_[i] = h.film_grain;
    ref_loop_filter_[i] = h.loop_filter;
    ref_segmentation_[i] = h.segmentation;
    ref_global_motion_[i] = h.global_motion;
  }
}

void AV1ObuParser::restart() {
  const AV1SequenceHeader seq = seq_;
  const AV1HdrMetadata hdr = hdr_;
  reset();
  seq_ = seq;
  hdr_ = hdr;
}

void AV1ObuParser::reset() {
  seq_ = {};
  hdr_ = {};
  for (uint32_t i = 0; i < AV1_NUM_REF_FRAMES; ++i) {
    ref_slots_[i] = {};
    ref_valid_[i] = false;
    ref_order_hint_[i] = 0;
    ref_film_grain_[i] = {};
    ref_loop_filter_[i] = {};
    ref_segmentation_[i] = {};
    ref_global_motion_[i] = {};
  }
  current_frame_id_ = 0;
}

// ---------------------------------------------------------------------------
// OBU layer
// ---------------------------------------------------------------------------

auto AV1ObuParser::readLeb128(std::span<const uint8_t> data, size_t& bytes_read) -> uint64_t {
  uint64_t value = 0;
  bytes_read = 0;
  for (uint32_t i = 0; i < 8; ++i) {
    if (bytes_read >= data.size()) return 0;
    const uint8_t byte = data[bytes_read++];
    value |= static_cast<uint64_t>(byte & 0x7F) << (i * 7);
    if ((byte & 0x80) == 0) break;
  }
  return value;
}

auto AV1ObuParser::parse(std::span<const uint8_t> packet) -> std::vector<AV1ParsedFrame> {
  std::vector<AV1ParsedFrame> frames;
  if (packet.empty()) return frames;

  // A temporal unit may carry more than one frame — most commonly a coded
  // frame with show_frame=0 followed by a show_existing_frame that displays an
  // earlier one. Each frame header therefore starts a new output entry;
  // accumulating them into a single entry would silently drop all but the last.
  AV1ParsedFrame current;
  current.bitstream.assign(packet.begin(), packet.end());
  bool have_frame_header = false;

  auto start_new_frame = [&]() {
    frames.push_back(std::move(current));
    current = {};
    current.bitstream.assign(packet.begin(), packet.end());
    have_frame_header = false;
  };

  size_t pos = 0;
  while (pos < packet.size()) {
    const size_t obu_start = pos;
    const uint8_t header_byte = packet[pos];
    const auto obu_type = static_cast<uint8_t>((header_byte >> 3) & 0x0F);
    const bool extension_flag = (header_byte >> 2) & 1;
    const bool has_size_field = (header_byte >> 1) & 1;
    ++pos;

    uint8_t temporal_id = 0;
    uint8_t spatial_id = 0;
    if (extension_flag) {
      if (pos >= packet.size()) break;
      const uint8_t ext = packet[pos++];
      temporal_id = static_cast<uint8_t>((ext >> 5) & 0x07);
      spatial_id = static_cast<uint8_t>((ext >> 3) & 0x03);
    }

    size_t payload_size = 0;
    if (has_size_field) {
      size_t leb_bytes = 0;
      payload_size = static_cast<size_t>(readLeb128(packet.subspan(pos), leb_bytes));
      if (leb_bytes == 0) break;
      pos += leb_bytes;
    } else {
      payload_size = packet.size() - pos;
    }
    if (pos + payload_size > packet.size()) break;

    const auto payload = packet.subspan(pos, payload_size);

    // A second frame header in the same temporal unit belongs to a new frame.
    if ((obu_type == AV1_OBU_FRAME_HEADER || obu_type == AV1_OBU_FRAME) && have_frame_header)
      start_new_frame();

    AV1Obu obu;
    obu.type = obu_type;
    obu.offset = obu_start;
    obu.payload_offset = pos;
    obu.payload_size = payload_size;
    obu.size = (pos + payload_size) - obu_start;
    obu.temporal_id = temporal_id;
    obu.spatial_id = spatial_id;
    current.obus.push_back(obu);

    switch (obu_type) {
      case AV1_OBU_SEQUENCE_HEADER:
        if (parseSequenceHeader(payload)) current.has_sequence_header = true;
        break;

      case AV1_OBU_METADATA:
        parseMetadata(payload);
        break;

      case AV1_OBU_FRAME_HEADER:
      case AV1_OBU_REDUNDANT_FRAME_HEADER: {
        if (obu_type == AV1_OBU_REDUNDANT_FRAME_HEADER && have_frame_header) break;
        size_t bits = 0;
        current.header = parseFrameHeader(payload, temporal_id, spatial_id, bits);
        current.has_frame_header = current.header.valid;
        have_frame_header = current.has_frame_header;
        if (current.header.valid) updateRefSlots(current.header);
        break;
      }

      case AV1_OBU_FRAME: {
        size_t bits = 0;
        current.header = parseFrameHeader(payload, temporal_id, spatial_id, bits);
        current.has_frame_header = current.header.valid;
        current.has_frame = true;
        have_frame_header = current.has_frame_header;
        if (current.header.valid) updateRefSlots(current.header);
        // The tile group follows the frame header, byte aligned within the OBU.
        const size_t header_bytes = (bits + 7) / 8;
        if (header_bytes <= payload_size) {
          current.tile_group_offset = pos + header_bytes;
          current.tile_group_size = payload_size - header_bytes;
        }
        break;
      }

      case AV1_OBU_TILE_GROUP:
        current.tile_group_offset = pos;
        current.tile_group_size = payload_size;
        current.has_frame = current.has_frame_header;
        break;

      default:
        break;
    }

    pos += payload_size;
  }

  if (current.has_frame_header || current.has_sequence_header)
    frames.push_back(std::move(current));
  return frames;
}

} // namespace openmedia::video_parser
