#include "vp9_parser.hpp"

#include <algorithm>
#include <iterator>
#include <util/bit_reader.hpp>

namespace openmedia::video_parser {

namespace {

constexpr uint8_t kFrameSyncByte0 = 0x49;
constexpr uint8_t kFrameSyncByte1 = 0x83;
constexpr uint8_t kFrameSyncByte2 = 0x42;

// VP9 spec 9.3: tile sizing limits, in units of 64x64 superblocks.
constexpr uint32_t kMaxTileWidthB64 = 64;
constexpr uint32_t kMinTileWidthB64 = 4;

// segmentation_feature_bits / segmentation_feature_signed (spec 7.2.10).
constexpr uint8_t kSegFeatureBits[VP9_SEG_LVL_MAX] = {8, 6, 2, 0};
constexpr bool kSegFeatureSigned[VP9_SEG_LVL_MAX] = {true, true, false, false};

// literal_to_type mapping for read_interpolation_filter() (spec 6.2.6).
constexpr uint8_t kLiteralToFilterType[4] = {
    VP9_EIGHTTAP_SMOOTH, VP9_EIGHTTAP, VP9_EIGHTTAP_SHARP, VP9_BILINEAR};

// VP9 su(n): n magnitude bits followed by a sign bit.
auto readSignedLiteral(BitReader& reader, uint32_t bits) -> int32_t {
  const auto value = static_cast<int32_t>(reader.readBits(bits));
  return reader.readFlag() ? -value : value;
}

// read_prob() (spec 6.2.11): an 8-bit probability, or 255 when not coded.
auto readProb(BitReader& reader) -> uint8_t {
  if (!reader.readFlag()) return 255;
  return static_cast<uint8_t>(reader.readBits(8));
}

// read_delta_q() (spec 6.2.9).
auto readDeltaQ(BitReader& reader) -> int8_t {
  if (!reader.readFlag()) return 0;
  return static_cast<int8_t>(readSignedLiteral(reader, 4));
}

auto readFrameSyncCode(BitReader& reader) -> bool {
  const uint32_t b0 = reader.readBits(8);
  const uint32_t b1 = reader.readBits(8);
  const uint32_t b2 = reader.readBits(8);
  return b0 == kFrameSyncByte0 && b1 == kFrameSyncByte1 && b2 == kFrameSyncByte2;
}

void readColorConfig(BitReader& reader, VP9FrameHeader& h) {
  if (h.profile >= 2) {
    h.bit_depth = reader.readFlag() ? 12 : 10;
  } else {
    h.bit_depth = 8;
  }

  h.color_space = static_cast<uint8_t>(reader.readBits(3));
  if (h.color_space != VP9_CS_RGB) {
    h.color_range = reader.readFlag();
    if (h.profile == 1 || h.profile == 3) {
      h.subsampling_x = static_cast<uint8_t>(reader.readBit());
      h.subsampling_y = static_cast<uint8_t>(reader.readBit());
      reader.skipBits(1); // reserved_zero
    } else {
      h.subsampling_x = 1;
      h.subsampling_y = 1;
    }
  } else {
    // 4:4:4 RGB is always full range.
    h.color_range = true;
    if (h.profile == 1 || h.profile == 3) {
      h.subsampling_x = 0;
      h.subsampling_y = 0;
      reader.skipBits(1); // reserved_zero
    }
  }
}

// compute_image_size() (spec 7.2.6).
void computeImageSize(VP9FrameHeader& h) {
  h.mi_cols = (h.frame_width + 7) >> 3;
  h.mi_rows = (h.frame_height + 7) >> 3;
  h.sb64_cols = (h.mi_cols + 7) >> 3;
  h.sb64_rows = (h.mi_rows + 7) >> 3;
}

void readFrameSize(BitReader& reader, VP9FrameHeader& h) {
  h.frame_width = reader.readBits(16) + 1;
  h.frame_height = reader.readBits(16) + 1;
  computeImageSize(h);
}

void readRenderSize(BitReader& reader, VP9FrameHeader& h) {
  if (reader.readFlag()) {
    h.render_width = reader.readBits(16) + 1;
    h.render_height = reader.readBits(16) + 1;
  } else {
    h.render_width = h.frame_width;
    h.render_height = h.frame_height;
  }
}

void readInterpolationFilter(BitReader& reader, VP9FrameHeader& h) {
  if (reader.readFlag()) {
    h.interpolation_filter = VP9_SWITCHABLE;
    return;
  }
  h.interpolation_filter = kLiteralToFilterType[reader.readBits(2) & 3u];
}

void readLoopFilterParams(BitReader& reader, VP9FrameHeader& h) {
  auto& lf = h.loop_filter;
  lf.level = static_cast<uint8_t>(reader.readBits(6));
  lf.sharpness = static_cast<uint8_t>(reader.readBits(3));
  lf.delta_enabled = reader.readFlag();
  if (!lf.delta_enabled) return;

  lf.delta_update = reader.readFlag();
  if (!lf.delta_update) return;

  for (uint32_t i = 0; i < VP9_MAX_LOOP_FILTER_REF_DELTAS; ++i) {
    lf.update_ref_delta[i] = reader.readFlag();
    if (lf.update_ref_delta[i])
      lf.ref_deltas[i] = static_cast<int8_t>(readSignedLiteral(reader, 6));
  }
  for (uint32_t i = 0; i < VP9_MAX_LOOP_FILTER_MODE_DELTAS; ++i) {
    lf.update_mode_delta[i] = reader.readFlag();
    if (lf.update_mode_delta[i])
      lf.mode_deltas[i] = static_cast<int8_t>(readSignedLiteral(reader, 6));
  }
}

void readQuantizationParams(BitReader& reader, VP9FrameHeader& h) {
  auto& q = h.quantization;
  q.base_q_idx = static_cast<uint8_t>(reader.readBits(8));
  q.delta_q_y_dc = readDeltaQ(reader);
  q.delta_q_uv_dc = readDeltaQ(reader);
  q.delta_q_uv_ac = readDeltaQ(reader);
  q.lossless = q.base_q_idx == 0 && q.delta_q_y_dc == 0 &&
               q.delta_q_uv_dc == 0 && q.delta_q_uv_ac == 0;
}

void readSegmentationParams(BitReader& reader, VP9FrameHeader& h) {
  auto& seg = h.segmentation;
  seg.enabled = reader.readFlag();
  if (!seg.enabled) return;

  seg.update_map = reader.readFlag();
  if (seg.update_map) {
    for (auto& prob : seg.tree_probs) prob = readProb(reader);
    seg.temporal_update = reader.readFlag();
    for (auto& prob : seg.pred_probs)
      prob = seg.temporal_update ? readProb(reader) : 255;
  }

  seg.update_data = reader.readFlag();
  if (!seg.update_data) return;

  seg.abs_or_delta_update = reader.readFlag();
  for (uint32_t i = 0; i < VP9_MAX_SEGMENTS; ++i) {
    for (uint32_t j = 0; j < VP9_SEG_LVL_MAX; ++j) {
      int32_t value = 0;
      const bool enabled = reader.readFlag();
      seg.feature_enabled[i][j] = enabled;
      if (enabled) {
        const uint8_t bits = kSegFeatureBits[j];
        if (bits > 0) value = static_cast<int32_t>(reader.readBits(bits));
        if (kSegFeatureSigned[j] && reader.readFlag()) value = -value;
      }
      seg.feature_data[i][j] = static_cast<int16_t>(value);
    }
  }
}

// calc_min_log2_tile_cols() / calc_max_log2_tile_cols() (spec 6.2.14).
auto calcMinLog2TileCols(uint32_t sb64_cols) -> uint32_t {
  uint32_t min_log2 = 0;
  while ((kMaxTileWidthB64 << min_log2) < sb64_cols) ++min_log2;
  return min_log2;
}

auto calcMaxLog2TileCols(uint32_t sb64_cols) -> uint32_t {
  uint32_t max_log2 = 1;
  while ((sb64_cols >> max_log2) >= kMinTileWidthB64) ++max_log2;
  return max_log2 - 1;
}

void readTileInfo(BitReader& reader, VP9FrameHeader& h) {
  const uint32_t min_log2 = calcMinLog2TileCols(h.sb64_cols);
  const uint32_t max_log2 = calcMaxLog2TileCols(h.sb64_cols);

  uint32_t tile_cols_log2 = min_log2;
  while (tile_cols_log2 < max_log2) {
    if (!reader.readFlag()) break;
    ++tile_cols_log2;
  }
  h.tile_cols_log2 = static_cast<uint8_t>(tile_cols_log2);

  uint32_t tile_rows_log2 = reader.readBit();
  if (tile_rows_log2 != 0) tile_rows_log2 += reader.readBit();
  h.tile_rows_log2 = static_cast<uint8_t>(tile_rows_log2);
}

} // namespace

void VP9FrameParser::reset() {
  for (auto& slot : ref_slots_) slot = {};
  color_config_ = {};
  loop_filter_state_ = {};
  segmentation_state_ = {};
}

auto VP9FrameParser::parseUncompressedHeader(std::span<const uint8_t> frame) -> VP9FrameHeader {
  VP9FrameHeader h;
  BitReader reader(frame);

  if (reader.readBits(2) != 2) return h; // frame_marker

  const uint32_t profile_low = reader.readBit();
  const uint32_t profile_high = reader.readBit();
  h.profile = static_cast<uint8_t>((profile_high << 1) | profile_low);
  if (h.profile == 3) reader.skipBits(1); // reserved_zero

  h.show_existing_frame = reader.readFlag();
  if (h.show_existing_frame) {
    h.frame_to_show_map_idx = static_cast<uint8_t>(reader.readBits(3));
    // A show_existing_frame header stops here: it re-displays a frame already
    // in the reference pool, refreshes nothing and carries no partition data.
    const auto& slot = ref_slots_[h.frame_to_show_map_idx];
    h.frame_width = slot.width;
    h.frame_height = slot.height;
    h.render_width = slot.width;
    h.render_height = slot.height;
    h.bit_depth = slot.bit_depth;
    h.subsampling_x = slot.subsampling_x;
    h.subsampling_y = slot.subsampling_y;
    h.show_frame = true;
    computeImageSize(h);
    h.uncompressed_header_size = static_cast<uint32_t>((reader.bitPosition() + 7) / 8);
    h.valid = reader.ok();
    return h;
  }

  h.frame_type = static_cast<uint8_t>(reader.readBit());
  h.show_frame = reader.readFlag();
  h.error_resilient_mode = reader.readFlag();

  if (h.frame_type == VP9_KEY_FRAME) {
    if (!readFrameSyncCode(reader)) return h;
    readColorConfig(reader, h);
    readFrameSize(reader, h);
    readRenderSize(reader, h);
    h.refresh_frame_flags = 0xFF;
    h.frame_is_intra = true;
  } else {
    h.intra_only = h.show_frame ? false : reader.readFlag();
    h.frame_is_intra = h.intra_only;

    h.reset_frame_context =
        h.error_resilient_mode ? 0 : static_cast<uint8_t>(reader.readBits(2));

    if (h.intra_only) {
      if (!readFrameSyncCode(reader)) return h;
      if (h.profile > 0) {
        readColorConfig(reader, h);
      } else {
        // Profile 0 intra-only is always 8-bit 4:2:0 BT.601.
        h.color_space = VP9_CS_BT_601;
        h.subsampling_x = 1;
        h.subsampling_y = 1;
        h.bit_depth = 8;
      }
      h.refresh_frame_flags = static_cast<uint8_t>(reader.readBits(8));
      readFrameSize(reader, h);
      readRenderSize(reader, h);
    } else {
      h.refresh_frame_flags = static_cast<uint8_t>(reader.readBits(8));
      for (uint32_t i = 0; i < VP9_REFS_PER_FRAME; ++i) {
        h.ref_frame_idx[i] = static_cast<uint8_t>(reader.readBits(3));
        h.ref_frame_sign_bias[VP9_LAST_FRAME + i] = reader.readFlag();
      }

      // frame_size_with_refs(): the size may be inherited from a reference.
      bool found_ref = false;
      for (uint32_t i = 0; i < VP9_REFS_PER_FRAME; ++i) {
        found_ref = reader.readFlag();
        if (!found_ref) continue;
        const auto& slot = ref_slots_[h.ref_frame_idx[i]];
        h.frame_width = slot.width;
        h.frame_height = slot.height;
        break;
      }
      if (!found_ref) {
        readFrameSize(reader, h);
      } else {
        computeImageSize(h);
      }
      readRenderSize(reader, h);

      h.allow_high_precision_mv = reader.readFlag();
      readInterpolationFilter(reader, h);
    }
  }

  // Inter frames never re-send color_config(); they keep whatever the last key
  // or intra-only frame established.
  if (h.frame_type == VP9_KEY_FRAME || h.intra_only) {
    color_config_ = {h.bit_depth, h.color_space, h.color_range,
                     h.subsampling_x, h.subsampling_y};
  } else {
    h.bit_depth = color_config_.bit_depth;
    h.color_space = color_config_.color_space;
    h.color_range = color_config_.color_range;
    h.subsampling_x = color_config_.subsampling_x;
    h.subsampling_y = color_config_.subsampling_y;
  }

  if (!h.error_resilient_mode) {
    h.refresh_frame_context = reader.readFlag();
    h.frame_parallel_decoding_mode = reader.readFlag();
  } else {
    h.refresh_frame_context = false;
    h.frame_parallel_decoding_mode = true;
  }

  h.frame_context_idx = static_cast<uint8_t>(reader.readBits(2));

  // setup_past_independence(): intra and error resilient frames start from the
  // defaults, everything else from what the previous frame left behind.
  VP9LoopFilterParams loop_filter = loop_filter_state_;
  VP9SegmentationParams segmentation = segmentation_state_;
  if (h.frame_is_intra || h.error_resilient_mode) {
    loop_filter = {};
    segmentation = {};
  }
  h.loop_filter = {};
  std::copy(std::begin(loop_filter.ref_deltas), std::end(loop_filter.ref_deltas), h.loop_filter.ref_deltas);
  std::copy(std::begin(loop_filter.mode_deltas), std::end(loop_filter.mode_deltas), h.loop_filter.mode_deltas);
  // Only the per-frame switches start from scratch; the tables they gate carry
  // over when the frame does not re-send them.
  h.segmentation = segmentation;
  h.segmentation.enabled = false;
  h.segmentation.update_map = false;
  h.segmentation.temporal_update = false;
  h.segmentation.update_data = false;

  readLoopFilterParams(reader, h);
  readQuantizationParams(reader, h);
  readSegmentationParams(reader, h);
  readTileInfo(reader, h);

  h.header_size_in_bytes = static_cast<uint16_t>(reader.readBits(16));

  // The compressed header begins at the next byte boundary.
  reader.alignToByte();
  h.uncompressed_header_size = static_cast<uint32_t>(reader.bytePosition());

  h.valid = reader.ok() && h.frame_width > 0 && h.frame_height > 0;
  if (h.valid) {
    loop_filter_state_ = h.loop_filter;
    segmentation_state_ = h.segmentation;
  }
  return h;
}

void VP9FrameParser::updateRefSlots(const VP9FrameHeader& header) {
  if (!header.valid || header.show_existing_frame) return;

  RefSlot updated;
  updated.width = header.frame_width;
  updated.height = header.frame_height;
  updated.bit_depth = header.bit_depth;
  updated.subsampling_x = header.subsampling_x;
  updated.subsampling_y = header.subsampling_y;
  updated.valid = true;

  for (uint32_t i = 0; i < VP9_NUM_REF_FRAMES; ++i) {
    if (header.refresh_frame_flags & (1u << i)) ref_slots_[i] = updated;
  }
}

auto VP9FrameParser::parse(std::span<const uint8_t> packet) -> std::vector<VP9ParsedFrame> {
  std::vector<VP9ParsedFrame> frames;
  if (packet.empty()) return frames;

  auto emit = [&](std::span<const uint8_t> frame) {
    VP9ParsedFrame parsed;
    parsed.bitstream = frame;
    parsed.header = parseUncompressedHeader(frame);
    parsed.profile = parsed.header.profile;
    parsed.key_frame = parsed.header.valid && !parsed.header.show_existing_frame &&
                       parsed.header.frame_type == VP9_KEY_FRAME;
    updateRefSlots(parsed.header);
    frames.push_back(std::move(parsed));
  };

  const uint8_t marker = packet.back();
  if ((marker & 0xe0u) == 0xc0u) {
    const size_t length_size = ((marker >> 3u) & 0x03u) + 1u;
    const size_t frame_count = (marker & 0x07u) + 1u;
    const size_t index_size = 2 + length_size * frame_count;
    if (packet.size() >= index_size && packet[packet.size() - index_size] == marker) {
      size_t pos = packet.size() - index_size + 1;
      size_t frame_offset = 0;
      bool ok = true;
      for (size_t i = 0; i < frame_count && ok; ++i) {
        size_t frame_size = 0;
        for (size_t j = 0; j < length_size; ++j)
          frame_size |= static_cast<size_t>(packet[pos++]) << (j * 8u);
        if (frame_size == 0 || frame_offset + frame_size > packet.size() - index_size) {
          ok = false;
          break;
        }
        emit(packet.subspan(frame_offset, frame_size));
        frame_offset += frame_size;
      }
      if (!frames.empty()) return frames;
    }
  }

  emit(packet);
  return frames;
}

} // namespace openmedia::video_parser
