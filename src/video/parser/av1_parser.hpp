#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace openmedia::video_parser {

inline constexpr uint32_t AV1_NUM_REF_FRAMES = 8;
inline constexpr uint32_t AV1_REFS_PER_FRAME = 7;
inline constexpr uint32_t AV1_TOTAL_REFS_PER_FRAME = 8;
inline constexpr uint32_t AV1_PRIMARY_REF_NONE = 7;
inline constexpr uint32_t AV1_MAX_SEGMENTS = 8;
inline constexpr uint32_t AV1_SEG_LVL_MAX = 8;
inline constexpr uint32_t AV1_MAX_TILE_COLS = 64;
inline constexpr uint32_t AV1_MAX_TILE_ROWS = 64;
inline constexpr uint32_t AV1_MAX_OPERATING_POINTS = 32;
inline constexpr uint32_t AV1_SELECT_SCREEN_CONTENT_TOOLS = 2;
inline constexpr uint32_t AV1_SELECT_INTEGER_MV = 2;
inline constexpr uint32_t AV1_SUPERRES_NUM = 8;
inline constexpr uint32_t AV1_SUPERRES_DENOM_MIN = 9;
inline constexpr uint32_t AV1_SUPERRES_DENOM_BITS = 3;

enum AV1ObuType : uint8_t {
  AV1_OBU_SEQUENCE_HEADER = 1,
  AV1_OBU_TEMPORAL_DELIMITER = 2,
  AV1_OBU_FRAME_HEADER = 3,
  AV1_OBU_TILE_GROUP = 4,
  AV1_OBU_METADATA = 5,
  AV1_OBU_FRAME = 6,
  AV1_OBU_REDUNDANT_FRAME_HEADER = 7,
  AV1_OBU_TILE_LIST = 8,
  AV1_OBU_PADDING = 15,
};

enum AV1FrameType : uint8_t {
  AV1_KEY_FRAME = 0,
  AV1_INTER_FRAME = 1,
  AV1_INTRA_ONLY_FRAME = 2,
  AV1_SWITCH_FRAME = 3,
};

enum AV1RefFrame : uint8_t {
  AV1_INTRA_FRAME = 0,
  AV1_LAST_FRAME = 1,
  AV1_LAST2_FRAME = 2,
  AV1_LAST3_FRAME = 3,
  AV1_GOLDEN_FRAME = 4,
  AV1_BWDREF_FRAME = 5,
  AV1_ALTREF2_FRAME = 6,
  AV1_ALTREF_FRAME = 7,
};

enum AV1InterpolationFilter : uint8_t {
  AV1_EIGHTTAP = 0,
  AV1_EIGHTTAP_SMOOTH = 1,
  AV1_EIGHTTAP_SHARP = 2,
  AV1_BILINEAR = 3,
  AV1_SWITCHABLE = 4,
};

enum AV1TxMode : uint8_t {
  AV1_ONLY_4X4 = 0,
  AV1_TX_MODE_LARGEST = 1,
  AV1_TX_MODE_SELECT = 2,
};

enum AV1FrameRestorationType : uint8_t {
  AV1_RESTORE_NONE = 0,
  AV1_RESTORE_WIENER = 1,
  AV1_RESTORE_SGRPROJ = 2,
  AV1_RESTORE_SWITCHABLE = 3,
};

enum AV1WarpModel : uint8_t {
  AV1_IDENTITY = 0,
  AV1_TRANSLATION = 1,
  AV1_ROTZOOM = 2,
  AV1_AFFINE = 3,
};

enum AV1MetadataType : uint8_t {
  AV1_METADATA_TYPE_HDR_CLL = 1,
  AV1_METADATA_TYPE_HDR_MDCV = 2,
  AV1_METADATA_TYPE_SCALABILITY = 3,
  AV1_METADATA_TYPE_ITUT_T35 = 4,
  AV1_METADATA_TYPE_TIMECODE = 5,
};

struct AV1Obu {
  uint8_t type = 0;
  size_t offset = 0;         // start of the OBU, including its header
  size_t size = 0;           // total OBU size, including its header
  size_t payload_offset = 0; // start of the payload
  size_t payload_size = 0;
  uint8_t temporal_id = 0;
  uint8_t spatial_id = 0;
};

struct AV1TimingInfo {
  bool present = false;
  uint32_t num_units_in_display_tick = 0;
  uint32_t time_scale = 0;
  bool equal_picture_interval = false;
  uint32_t num_ticks_per_picture = 0;
};

struct AV1ColorConfig {
  uint8_t bit_depth = 8;
  bool mono_chrome = false;
  uint8_t num_planes = 3;
  bool color_description_present_flag = false;
  uint8_t color_primaries = 2;          // CP_UNSPECIFIED
  uint8_t transfer_characteristics = 2; // TC_UNSPECIFIED
  uint8_t matrix_coefficients = 2;      // MC_UNSPECIFIED
  bool color_range = false;
  uint8_t subsampling_x = 1;
  uint8_t subsampling_y = 1;
  uint8_t chroma_sample_position = 0;
  bool separate_uv_delta_q = false;
};

struct AV1SequenceHeader {
  bool valid = false;

  uint8_t seq_profile = 0;
  bool still_picture = false;
  bool reduced_still_picture_header = false;

  AV1TimingInfo timing_info;
  bool decoder_model_info_present_flag = false;
  uint8_t buffer_delay_length = 0;
  uint32_t num_units_in_decoding_tick = 0;
  uint8_t buffer_removal_time_length = 0;
  uint8_t frame_presentation_time_length = 0;

  bool initial_display_delay_present_flag = false;
  uint8_t operating_points_cnt = 1;
  uint16_t operating_point_idc[AV1_MAX_OPERATING_POINTS] = {};
  uint8_t seq_level_idx[AV1_MAX_OPERATING_POINTS] = {};
  uint8_t seq_tier[AV1_MAX_OPERATING_POINTS] = {};
  bool decoder_model_present_for_this_op[AV1_MAX_OPERATING_POINTS] = {};

  uint8_t frame_width_bits = 1;
  uint8_t frame_height_bits = 1;
  uint32_t max_frame_width = 0;
  uint32_t max_frame_height = 0;

  bool frame_id_numbers_present_flag = false;
  uint8_t delta_frame_id_length = 0;
  uint8_t additional_frame_id_length = 0;

  bool use_128x128_superblock = false;
  bool enable_filter_intra = false;
  bool enable_intra_edge_filter = false;
  bool enable_interintra_compound = false;
  bool enable_masked_compound = false;
  bool enable_warped_motion = false;
  bool enable_dual_filter = false;
  bool enable_order_hint = false;
  bool enable_jnt_comp = false;
  bool enable_ref_frame_mvs = false;

  uint8_t seq_force_screen_content_tools = AV1_SELECT_SCREEN_CONTENT_TOOLS;
  uint8_t seq_force_integer_mv = AV1_SELECT_INTEGER_MV;
  uint8_t order_hint_bits = 0;

  bool enable_superres = false;
  bool enable_cdef = false;
  bool enable_restoration = false;

  AV1ColorConfig color_config;
  bool film_grain_params_present = false;
};

struct AV1TileInfo {
  bool uniform_tile_spacing_flag = true;
  uint32_t tile_cols_log2 = 0;
  uint32_t tile_rows_log2 = 0;
  uint32_t tile_cols = 1;
  uint32_t tile_rows = 1;
  uint32_t mi_col_starts[AV1_MAX_TILE_COLS + 1] = {};
  uint32_t mi_row_starts[AV1_MAX_TILE_ROWS + 1] = {};
  uint32_t context_update_tile_id = 0;
  uint8_t tile_size_bytes = 4;
};

struct AV1QuantizationParams {
  uint8_t base_q_idx = 0;
  int32_t delta_q_y_dc = 0;
  int32_t delta_q_u_dc = 0;
  int32_t delta_q_u_ac = 0;
  int32_t delta_q_v_dc = 0;
  int32_t delta_q_v_ac = 0;
  bool using_qmatrix = false;
  uint8_t qm_y = 0;
  uint8_t qm_u = 0;
  uint8_t qm_v = 0;
};

struct AV1SegmentationParams {
  bool enabled = false;
  bool update_map = false;
  bool temporal_update = false;
  bool update_data = false;
  bool feature_enabled[AV1_MAX_SEGMENTS][AV1_SEG_LVL_MAX] = {};
  int16_t feature_data[AV1_MAX_SEGMENTS][AV1_SEG_LVL_MAX] = {};
  bool seg_id_pre_skip = false;
  uint8_t last_active_seg_id = 0;
};

struct AV1LoopFilterParams {
  uint8_t level[4] = {};
  uint8_t sharpness = 0;
  bool delta_enabled = false;
  bool delta_update = false;
  int8_t ref_deltas[AV1_TOTAL_REFS_PER_FRAME] = {1, 0, 0, 0, -1, 0, -1, -1};
  int8_t mode_deltas[2] = {};
  bool delta_lf_present = false;
  uint8_t delta_lf_res = 0;
  bool delta_lf_multi = false;
};

struct AV1CdefParams {
  uint8_t damping = 3;
  uint8_t bits = 0;
  uint8_t y_pri_strength[8] = {};
  uint8_t y_sec_strength[8] = {};
  uint8_t uv_pri_strength[8] = {};
  uint8_t uv_sec_strength[8] = {};
};

struct AV1LoopRestorationParams {
  uint8_t frame_restoration_type[3] = {};
  uint8_t loop_restoration_size[3] = {};
  bool uses_lr = false;
};

struct AV1GlobalMotionParams {
  uint8_t type[AV1_TOTAL_REFS_PER_FRAME] = {};
  int32_t params[AV1_TOTAL_REFS_PER_FRAME][6] = {};
  bool invalid[AV1_TOTAL_REFS_PER_FRAME] = {};
};

struct AV1FilmGrainParams {
  bool apply_grain = false;
  uint16_t grain_seed = 0;
  bool update_grain = true;
  uint8_t film_grain_params_ref_idx = 0;
  uint8_t num_y_points = 0;
  uint8_t point_y_value[14] = {};
  uint8_t point_y_scaling[14] = {};
  bool chroma_scaling_from_luma = false;
  uint8_t num_cb_points = 0;
  uint8_t point_cb_value[10] = {};
  uint8_t point_cb_scaling[10] = {};
  uint8_t num_cr_points = 0;
  uint8_t point_cr_value[10] = {};
  uint8_t point_cr_scaling[10] = {};
  uint8_t grain_scaling = 8;
  uint8_t ar_coeff_lag = 0;
  uint8_t ar_coeffs_y[24] = {};
  uint8_t ar_coeffs_cb[25] = {};
  uint8_t ar_coeffs_cr[25] = {};
  uint8_t ar_coeff_shift = 6;
  uint8_t grain_scale_shift = 0;
  uint8_t cb_mult = 0;
  uint8_t cb_luma_mult = 0;
  uint16_t cb_offset = 0;
  uint8_t cr_mult = 0;
  uint8_t cr_luma_mult = 0;
  uint16_t cr_offset = 0;
  bool overlap_flag = false;
  bool clip_to_restricted_range = false;
};

// AV1 spec 5.9 uncompressed_header(), fully decoded.
struct AV1FrameHeader {
  bool valid = false;

  bool show_existing_frame = false;
  uint8_t frame_to_show_map_idx = 0;

  uint8_t frame_type = AV1_KEY_FRAME;
  bool frame_is_intra = false;
  bool show_frame = false;
  bool showable_frame = false;
  bool error_resilient_mode = false;
  bool disable_cdf_update = false;
  bool allow_screen_content_tools = false;
  bool force_integer_mv = false;
  uint32_t current_frame_id = 0;
  bool frame_size_override_flag = false;
  uint32_t order_hint = 0;
  uint8_t primary_ref_frame = AV1_PRIMARY_REF_NONE;

  uint8_t refresh_frame_flags = 0;
  uint32_t ref_order_hint[AV1_NUM_REF_FRAMES] = {};
  uint8_t ref_frame_idx[AV1_REFS_PER_FRAME] = {};
  uint32_t order_hints[AV1_TOTAL_REFS_PER_FRAME] = {};

  uint32_t frame_width = 0;
  uint32_t frame_height = 0;
  uint32_t upscaled_width = 0;
  uint32_t render_width = 0;
  uint32_t render_height = 0;
  bool use_superres = false;
  uint8_t superres_denom = AV1_SUPERRES_NUM;
  uint32_t mi_cols = 0;
  uint32_t mi_rows = 0;

  bool allow_intrabc = false;
  bool frame_refs_short_signaling = false;
  bool allow_high_precision_mv = false;
  uint8_t interpolation_filter = AV1_SWITCHABLE;
  bool is_motion_mode_switchable = false;
  bool use_ref_frame_mvs = false;
  bool disable_frame_end_update_cdf = false;

  AV1TileInfo tile_info;
  AV1QuantizationParams quantization;
  AV1SegmentationParams segmentation;
  bool delta_q_present = false;
  uint8_t delta_q_res = 0;
  AV1LoopFilterParams loop_filter;
  AV1CdefParams cdef;
  AV1LoopRestorationParams lr;

  uint8_t tx_mode = AV1_ONLY_4X4;
  bool reference_select = false;
  bool skip_mode_present = false;
  uint8_t skip_mode_frame[2] = {};
  bool allow_warped_motion = false;
  bool reduced_tx_set = false;
  AV1GlobalMotionParams global_motion;
  AV1FilmGrainParams film_grain;

  bool coded_lossless = false;
  bool all_lossless = false;
};

// HDR metadata carried in OBU_METADATA (spec 5.8.2 / 5.8.3).
struct AV1HdrMetadata {
  bool has_cll = false;
  uint16_t max_cll = 0;
  uint16_t max_fall = 0;

  bool has_mdcv = false;
  uint16_t primary_chromaticity_x[3] = {};
  uint16_t primary_chromaticity_y[3] = {};
  uint16_t white_point_chromaticity_x = 0;
  uint16_t white_point_chromaticity_y = 0;
  uint32_t luminance_max = 0;
  uint32_t luminance_min = 0;
};

struct AV1ParsedFrame {
  std::vector<uint8_t> bitstream;
  std::vector<AV1Obu> obus;
  bool has_sequence_header = false;
  bool has_frame_header = false;
  bool has_frame = false;

  AV1FrameHeader header;
  // Offset/size of the tile group data within `bitstream`.
  size_t tile_group_offset = 0;
  size_t tile_group_size = 0;
  uint32_t tile_start = 0;
  uint32_t tile_end = 0;
};

// Splits a packet into temporal units and decodes sequence/frame headers.
//
// Stateful: the sequence header persists across packets, reference slots feed
// frame_size_with_refs() and set_frame_refs(), and film grain / CDF state is
// keyed off the reference pool. Call reset() on seek.
class AV1ObuParser {
public:
  auto parse(std::span<const uint8_t> packet) -> std::vector<AV1ParsedFrame>;

  void reset();

  auto sequenceHeader() const -> const AV1SequenceHeader& { return seq_; }
  auto hdrMetadata() const -> const AV1HdrMetadata& { return hdr_; }

  // AV1's descriptor set (f/su/ns/uvlc/subexp) on top of the shared BitReader.
  // Public so the header-parsing helpers in the .cpp can take one.
  class Reader;

private:
  static auto readLeb128(std::span<const uint8_t> data, size_t& bytes_read) -> uint64_t;

  struct RefSlot {
    bool valid = false;
    uint8_t frame_type = AV1_KEY_FRAME;
    uint32_t upscaled_width = 0;
    uint32_t frame_width = 0;
    uint32_t frame_height = 0;
    uint32_t render_width = 0;
    uint32_t render_height = 0;
    uint32_t mi_cols = 0;
    uint32_t mi_rows = 0;
    uint32_t order_hint = 0;
    uint32_t frame_id = 0;
    uint8_t bit_depth = 8;
    uint8_t subsampling_x = 1;
    uint8_t subsampling_y = 1;
  };

  auto parseSequenceHeader(std::span<const uint8_t> payload) -> bool;
  auto parseFrameHeader(std::span<const uint8_t> payload, uint8_t temporal_id,
                        uint8_t spatial_id, size_t& bits_consumed) -> AV1FrameHeader;
  void parseMetadata(std::span<const uint8_t> payload);
  void updateRefSlots(const AV1FrameHeader& header);
  void setFrameRefs(AV1FrameHeader& header, uint8_t last_frame_idx, uint8_t gold_frame_idx) const;
  auto getRelativeDist(int32_t a, int32_t b) const -> int32_t;

  AV1SequenceHeader seq_;
  AV1HdrMetadata hdr_;
  RefSlot ref_slots_[AV1_NUM_REF_FRAMES] = {};
  bool ref_valid_[AV1_NUM_REF_FRAMES] = {};
  uint32_t ref_order_hint_[AV1_NUM_REF_FRAMES] = {};
  AV1FilmGrainParams ref_film_grain_[AV1_NUM_REF_FRAMES] = {};
  AV1LoopFilterParams ref_loop_filter_[AV1_NUM_REF_FRAMES] = {};
  AV1SegmentationParams ref_segmentation_[AV1_NUM_REF_FRAMES] = {};
  AV1GlobalMotionParams ref_global_motion_[AV1_NUM_REF_FRAMES] = {};
  uint32_t current_frame_id_ = 0;
};

} // namespace openmedia::video_parser
