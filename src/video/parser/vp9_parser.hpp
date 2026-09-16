#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace openmedia::video_parser {

inline constexpr uint32_t VP9_NUM_REF_FRAMES = 8;
inline constexpr uint32_t VP9_REFS_PER_FRAME = 3;
inline constexpr uint32_t VP9_MAX_SEGMENTS = 8;
inline constexpr uint32_t VP9_SEG_LVL_MAX = 4;
inline constexpr uint32_t VP9_MAX_LOOP_FILTER_REF_DELTAS = 4;
inline constexpr uint32_t VP9_MAX_LOOP_FILTER_MODE_DELTAS = 2;
inline constexpr uint32_t VP9_MAX_MODE_LF_DELTAS = 2;

enum VP9FrameType : uint8_t {
  VP9_KEY_FRAME = 0,
  VP9_NON_KEY_FRAME = 1,
};

// VP9 spec 7.2.2 color_space
enum VP9ColorSpace : uint8_t {
  VP9_CS_UNKNOWN = 0,
  VP9_CS_BT_601 = 1,
  VP9_CS_BT_709 = 2,
  VP9_CS_SMPTE_170 = 3,
  VP9_CS_SMPTE_240 = 4,
  VP9_CS_BT_2020 = 5,
  VP9_CS_RESERVED = 6,
  VP9_CS_RGB = 7,
};

enum VP9InterpolationFilter : uint8_t {
  VP9_EIGHTTAP_SMOOTH = 0,
  VP9_EIGHTTAP = 1,
  VP9_EIGHTTAP_SHARP = 2,
  VP9_BILINEAR = 3,
  VP9_SWITCHABLE = 4,
};

// Reference frame slots, as indexed by ref_frame_sign_bias.
enum VP9RefFrame : uint8_t {
  VP9_INTRA_FRAME = 0,
  VP9_LAST_FRAME = 1,
  VP9_GOLDEN_FRAME = 2,
  VP9_ALTREF_FRAME = 3,
};

struct VP9LoopFilterParams {
  uint8_t level = 0;
  uint8_t sharpness = 0;
  bool delta_enabled = false;
  bool delta_update = false;
  bool update_ref_delta[VP9_MAX_LOOP_FILTER_REF_DELTAS] = {};
  int8_t ref_deltas[VP9_MAX_LOOP_FILTER_REF_DELTAS] = {1, 0, -1, -1};
  bool update_mode_delta[VP9_MAX_LOOP_FILTER_MODE_DELTAS] = {};
  int8_t mode_deltas[VP9_MAX_LOOP_FILTER_MODE_DELTAS] = {};
};

struct VP9QuantizationParams {
  uint8_t base_q_idx = 0;
  int8_t delta_q_y_dc = 0;
  int8_t delta_q_uv_dc = 0;
  int8_t delta_q_uv_ac = 0;
  bool lossless = false;
};

struct VP9SegmentationParams {
  bool enabled = false;
  bool update_map = false;
  bool temporal_update = false;
  bool update_data = false;
  bool abs_or_delta_update = false;
  uint8_t tree_probs[7] = {255, 255, 255, 255, 255, 255, 255};
  uint8_t pred_probs[3] = {255, 255, 255};
  bool feature_enabled[VP9_MAX_SEGMENTS][VP9_SEG_LVL_MAX] = {};
  int16_t feature_data[VP9_MAX_SEGMENTS][VP9_SEG_LVL_MAX] = {};
};

// VP9 spec 6.2 uncompressed_header(), fully decoded.
//
// This carries everything DXVA_PicParams_VP9, VADecPictureParameterBufferVP9
// and StdVideoDecodeVP9PictureInfo need; the accelerators only differ in how
// the fields are packed.
struct VP9FrameHeader {
  bool valid = false;

  uint8_t profile = 0;

  bool show_existing_frame = false;
  uint8_t frame_to_show_map_idx = 0;

  uint8_t frame_type = VP9_KEY_FRAME;
  bool show_frame = false;
  bool error_resilient_mode = false;
  bool intra_only = false;
  bool frame_is_intra = false;
  uint8_t reset_frame_context = 0;

  uint8_t refresh_frame_flags = 0;
  uint8_t ref_frame_idx[VP9_REFS_PER_FRAME] = {};
  // Indexed by VP9RefFrame; entry 0 (intra) is unused.
  bool ref_frame_sign_bias[VP9_REFS_PER_FRAME + 1] = {};

  bool allow_high_precision_mv = false;
  uint8_t interpolation_filter = VP9_SWITCHABLE;

  bool refresh_frame_context = false;
  bool frame_parallel_decoding_mode = true;
  uint8_t frame_context_idx = 0;

  // color_config()
  uint8_t bit_depth = 8;
  uint8_t color_space = VP9_CS_UNKNOWN;
  bool color_range = false; // false = studio swing, true = full swing
  uint8_t subsampling_x = 1;
  uint8_t subsampling_y = 1;

  uint32_t frame_width = 0;
  uint32_t frame_height = 0;
  uint32_t render_width = 0;
  uint32_t render_height = 0;

  uint32_t mi_cols = 0;
  uint32_t mi_rows = 0;
  uint32_t sb64_cols = 0;
  uint32_t sb64_rows = 0;

  VP9LoopFilterParams loop_filter;
  VP9QuantizationParams quantization;
  VP9SegmentationParams segmentation;

  uint8_t tile_cols_log2 = 0;
  uint8_t tile_rows_log2 = 0;

  // first_partition_size: length of the compressed header that follows.
  uint16_t header_size_in_bytes = 0;
  // Bytes of the frame consumed by the uncompressed header itself, i.e. the
  // offset at which the compressed header starts.
  uint32_t uncompressed_header_size = 0;
};

struct VP9ParsedFrame {
  std::span<const uint8_t> bitstream;
  std::vector<uint8_t> storage;
  bool key_frame = false;
  uint8_t profile = 0;
  VP9FrameHeader header;
};

// Splits superframes and decodes each frame's uncompressed header.
//
// The parser is stateful: frame_size_with_refs() takes the frame dimensions
// from a reference slot, so the sizes stored in those slots have to be tracked
// across frames. Call reset() on seek.
class VP9FrameParser {
public:
  auto parse(std::span<const uint8_t> packet) -> std::vector<VP9ParsedFrame>;

  void reset();

  struct RefSlot {
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t bit_depth = 8;
    uint8_t subsampling_x = 1;
    uint8_t subsampling_y = 1;
    bool valid = false;
  };

  auto refSlot(size_t index) const -> const RefSlot& { return ref_slots_[index]; }

private:
  auto parseUncompressedHeader(std::span<const uint8_t> frame) -> VP9FrameHeader;
  void updateRefSlots(const VP9FrameHeader& header);

  RefSlot ref_slots_[VP9_NUM_REF_FRAMES] = {};

  // color_config() only appears on key and intra-only frames; inter frames
  // carry the configuration established by the last one that did.
  struct ColorConfig {
    uint8_t bit_depth = 8;
    uint8_t color_space = VP9_CS_UNKNOWN;
    bool color_range = false;
    uint8_t subsampling_x = 1;
    uint8_t subsampling_y = 1;
  } color_config_;
};

} // namespace openmedia::video_parser
