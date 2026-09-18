#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxva.h>
#endif

#include <openmedia/frame.hpp>
#include <video/parser/h265_parser.hpp>
#include "dx_h264.hpp"
#include "reorder_queue.hpp"

namespace openmedia::dx_h265 {

using Sps = video_parser::H265AccessUnitParser::Sps;
using Pps = video_parser::H265AccessUnitParser::Pps;
using SliceHeader = video_parser::H265SliceHeader;
using ParsedFrame = video_parser::H265ParsedFrame;
using StRefPicSet = video_parser::H265StRefPicSet;

// How many pictures a stream may hold back before the earliest of them can be shown. HEVC states
// it outright in the SPS, per temporal sub-layer; the highest sub-layer is the one a decoder taking
// the whole stream has to allow for. Zero means none, and a stream that says so is emitted as it
// is decoded.
//
// Without this the pictures come out in decode order, which for anything with B-frames means their
// timestamps run backwards -- the same fault the H.264 path had; see dx_h264::reorderDepth.
inline auto reorderDepth(const Sps& sps) -> size_t {
  const int layer = std::clamp(sps.max_sub_layers_minus1, 0, 7);
  return static_cast<size_t>(std::clamp(sps.sps_max_num_reorder_pics[layer], 0, 16));
}

inline auto isIdr(int nal_type) noexcept -> bool {
  return nal_type == video_parser::NAL_IDR_W_RADL || nal_type == video_parser::NAL_IDR_N_LP;
}

inline auto isIrap(int nal_type) noexcept -> bool {
  return nal_type >= video_parser::NAL_BLA_W_LP && nal_type <= video_parser::NAL_CRA_NUT;
}

inline auto activeStRefPicSet(const Sps& sps, const SliceHeader& sh) -> const StRefPicSet& {
  if (!sh.short_term_ref_pic_set_sps_flag) return sh.st_ref_pic_set;
  const int idx = std::clamp(sh.short_term_ref_pic_set_idx, 0, video_parser::H265_MAX_SHORT_TERM_REF_PIC_SETS - 1);
  return sps.st_ref_pic_set[idx];
}

// `foll` is a picture this one may not predict from but a later one still can;
// it is in the set only so that it is not thrown away.
enum class RefList : uint8_t { st_curr_before, st_curr_after, lt_curr, foll };

struct WantedPicture {
  int32_t poc = 0;
  RefList list = RefList::foll;
  bool long_term = false;
  // A long term entry whose msb the slice header left out is identified by the
  // low bits of its count alone (8.3.2), so `poc` then holds only those bits.
  bool by_lsb_only = false;
};

// ITU-T H.265 8.3.2. Every picture re-states the whole set, so what the list
// leaves out stops being a reference right there -- which is what makes this
// also the answer to "which surface is free to decode into".
struct ReferenceSet {
  static constexpr size_t kCapacity =
      static_cast<size_t>(video_parser::H265_MAX_DELTA_POCS + video_parser::H265_MAX_LONG_TERM_REF_PICS);

  WantedPicture pictures[kCapacity] = {};
  size_t count = 0;
  // MaxPicOrderCntLsb - 1, for the long term entries matched on the low bits.
  int32_t poc_lsb_mask = 0;

  void add(const WantedPicture& picture) {
    if (count < kCapacity) pictures[count++] = picture;
  }

  auto matches(const WantedPicture& wanted, int32_t poc) const -> bool {
    return wanted.by_lsb_only ? (poc & poc_lsb_mask) == wanted.poc : poc == wanted.poc;
  }
};

inline auto referenceSet(const Sps& sps, const SliceHeader& sh, int32_t poc, int nal_type) -> ReferenceSet {
  ReferenceSet set;
  set.poc_lsb_mask = (1 << (sps.log2_max_pic_order_cnt_lsb_minus4 + 4)) - 1;
  if (isIdr(nal_type)) return set;

  const StRefPicSet& st = activeStRefPicSet(sps, sh);
  // The order within each of the two short term runs is the order DXVA wants
  // RefPicSetStCurrBefore and RefPicSetStCurrAfter in, so keep it.
  for (int i = 0; i < st.num_negative_pics; ++i) {
    set.add({poc + st.delta_poc_s0[i],
             st.used_by_curr_pic_s0_flag[i] ? RefList::st_curr_before : RefList::foll, false, false});
  }
  for (int i = 0; i < st.num_positive_pics; ++i) {
    set.add({poc + st.delta_poc_s1[i],
             st.used_by_curr_pic_s1_flag[i] ? RefList::st_curr_after : RefList::foll, false, false});
  }

  for (int i = 0; i < sh.num_long_term_refs; ++i) {
    const auto& lt = sh.long_term_ref[i];
    WantedPicture wanted = {};
    wanted.long_term = true;
    wanted.list = lt.used_by_curr_pic_lt_flag ? RefList::lt_curr : RefList::foll;
    if (lt.delta_poc_msb_present_flag) {
      // With the msb the stream states the whole count, relative to this one.
      wanted.poc = lt.poc_lsb_lt + poc - lt.delta_poc_msb_cycle_lt * (set.poc_lsb_mask + 1) -
                   (poc & set.poc_lsb_mask);
    } else {
      wanted.poc = lt.poc_lsb_lt;
      wanted.by_lsb_only = true;
    }
    set.add(wanted);
  }
  return set;
}

struct DpbEntry {
  int32_t poc = 0;
  bool is_reference = false;
  bool is_long_term = false;
};

// Slots used to be handed out round-robin and a picture stayed flagged as a
// reference until the next IRAP, so nothing stopped the decoder from writing a
// picture over a surface that very picture was predicting from. How far back a
// stream referred decided whether it happened at all, which made it a
// clip-by-clip fault. The H.264 path grew the same marking; see dx_h264::Dpb.
class Dpb {
public:
  void configure(uint32_t slot_count) { entries_.assign(slot_count, DpbEntry {}); }

  void reset() {
    for (auto& entry : entries_) entry = {};
  }

  auto entries() const -> const std::vector<DpbEntry>& { return entries_; }

  // 8.3.2.
  void applyReferenceSet(const ReferenceSet& set) {
    for (auto& entry : entries_) {
      if (!entry.is_reference) continue;
      const WantedPicture* wanted = lookup(set, entry.poc);
      if (wanted == nullptr) {
        entry = {};
        continue;
      }
      entry.is_long_term = wanted->long_term;
    }
  }

  auto findSlot(const ReferenceSet& set, const WantedPicture& wanted) const -> std::optional<uint32_t> {
    for (uint32_t slot = 0; slot < entries_.size(); ++slot) {
      if (entries_[slot].is_reference && set.matches(wanted, entries_[slot].poc)) return slot;
    }
    return std::nullopt;
  }

  // Pictures are read back as they are decoded, so none has to be held for output.
  auto acquireSlot() const -> uint32_t {
    if (entries_.empty()) return 0;
    for (uint32_t slot = 0; slot < entries_.size(); ++slot) {
      if (!entries_[slot].is_reference) return slot;
    }
    // The stream is keeping more references live than it has surfaces. Dropping
    // the earliest keeps the picture decoding rather than losing it outright.
    uint32_t oldest = 0;
    for (uint32_t slot = 1; slot < entries_.size(); ++slot) {
      if (entries_[slot].poc < entries_[oldest].poc) oldest = slot;
    }
    return oldest;
  }

  void store(uint32_t slot, int32_t poc, bool is_reference) {
    if (slot >= entries_.size()) return;
    entries_[slot] = {poc, is_reference, false};
  }

  // Expected for a moment after a seek into the middle of a sequence. Any other
  // time a reference has been lost, and the blocks that wanted it come out
  // carrying whatever was left in the surface.
  auto missingReferences(const ReferenceSet& set) const -> size_t {
    size_t missing = 0;
    for (size_t i = 0; i < set.count; ++i) {
      if (set.pictures[i].list == RefList::foll) continue;
      if (!findSlot(set, set.pictures[i]).has_value()) ++missing;
    }
    return missing;
  }

private:
  static auto lookup(const ReferenceSet& set, int32_t poc) -> const WantedPicture* {
    for (size_t i = 0; i < set.count; ++i) {
      if (set.matches(set.pictures[i], poc)) return &set.pictures[i];
    }
    return nullptr;
  }

  std::vector<DpbEntry> entries_;
};

// Retiring references before taking a surface, and not after, is what keeps a
// decoder from writing a picture over one of its own references. That ordering
// is the whole reason this lives here rather than in each decoder.
struct PictureStart {
  ReferenceSet reference_set;
  uint32_t slot = 0;
  size_t missing_references = 0;
};

inline auto beginPicture(const ParsedFrame& frame, const Sps& sps, const SliceHeader& sh,
                         Dpb& dpb, ReorderQueue& reorder, std::vector<Frame>& output) -> PictureStart {
  if (isIrap(frame.nal_unit_type)) {
    // The counts start over here, so ordering what is still held back against the
    // new ones would put it among pictures it has nothing to do with.
    for (auto& held : reorder.drain()) output.push_back(std::move(held));
    dpb.reset();
  }

  PictureStart start;
  start.reference_set = referenceSet(sps, sh, frame.poc, frame.nal_unit_type);
  dpb.applyReferenceSet(start.reference_set);
  start.missing_references = dpb.missingReferences(start.reference_set);
  start.slot = dpb.acquireSlot();
  return start;
}

#ifdef _WIN32
// DXVA_PicParams_HEVC carries a fixed 15-entry reference picture list, and
// each of the reference sets that index into it holds at most 8.
inline constexpr uint8_t MAX_REF_PICS = 15;
inline constexpr uint8_t MAX_REF_SET_ENTRIES = 8;

struct SliceData {
  std::vector<uint8_t> bitstream;
  std::vector<DXVA_Slice_HEVC_Short> slices;
};

inline auto isStartCode(const std::vector<uint8_t>& data, size_t offset) noexcept -> size_t {
  if (offset + 3 <= data.size() && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 1) return 3;
  if (offset + 4 <= data.size() && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 0 && data[offset + 3] == 1) return 4;
  return 0;
}

inline auto findNextStartCode(const std::vector<uint8_t>& data, size_t offset) noexcept -> size_t {
  for (size_t i = offset; i + 3 <= data.size(); ++i) {
    if (isStartCode(data, i) != 0) return i;
  }
  return data.size();
}

inline auto buildSliceData(const ParsedFrame& frame) -> SliceData {
  SliceData out;
  out.slices.reserve(frame.slice_offsets.size());
  out.bitstream.reserve(frame.bitstream.size());

  for (const uint32_t offset : frame.slice_offsets) {
    if (offset >= frame.bitstream.size()) continue;
    const size_t start_code_size = isStartCode(frame.bitstream, offset);
    if (start_code_size == 0) continue;

    const size_t end = findNextStartCode(frame.bitstream, offset + start_code_size);
    const uint32_t location = static_cast<uint32_t>(out.bitstream.size());
    const uint32_t bytes = static_cast<uint32_t>(end - offset);
    out.bitstream.insert(out.bitstream.end(), frame.bitstream.begin() + static_cast<ptrdiff_t>(offset), frame.bitstream.begin() + static_cast<ptrdiff_t>(end));

    DXVA_Slice_HEVC_Short slice = {};
    slice.BSNALunitDataLocation = location;
    slice.SliceBytesInBuffer = bytes;
    slice.wBadSliceChopping = 0;
    out.slices.push_back(slice);
  }

  return out;
}

inline void fillQMatrix(const Sps& sps, const Pps& pps, DXVA_Qmatrix_HEVC& qmatrix) {
  memset(&qmatrix, 16, sizeof(qmatrix));
  if (!sps.scaling_list_enabled_flag) return;
  if (!pps.pps_scaling_list_data_present_flag && !sps.sps_scaling_list_data_present_flag) return;

  const auto& sl = pps.pps_scaling_list_data_present_flag ? pps.scaling_list_data : sps.scaling_list_data;
  memcpy(qmatrix.ucScalingLists0, sl.scaling_list_4x4, sizeof(qmatrix.ucScalingLists0));
  memcpy(qmatrix.ucScalingLists1, sl.scaling_list_8x8, sizeof(qmatrix.ucScalingLists1));
  memcpy(qmatrix.ucScalingLists2, sl.scaling_list_16x16, sizeof(qmatrix.ucScalingLists2));
  memcpy(qmatrix.ucScalingLists3[0], sl.scaling_list_32x32[0], sizeof(qmatrix.ucScalingLists3[0]));
  memcpy(qmatrix.ucScalingLists3[1], sl.scaling_list_32x32[1], sizeof(qmatrix.ucScalingLists3[1]));
  memcpy(qmatrix.ucScalingListDCCoefSizeID2, sl.scaling_list_dc_coef_16x16, sizeof(qmatrix.ucScalingListDCCoefSizeID2));
  qmatrix.ucScalingListDCCoefSizeID3[0] = sl.scaling_list_dc_coef_32x32[0];
  qmatrix.ucScalingListDCCoefSizeID3[1] = sl.scaling_list_dc_coef_32x32[1];
}

// Every picture still held as a reference goes in, not only the ones this one
// predicts from: the driver reads surface lifetime out of the list, so anything
// missing from it is a surface it may reuse.
inline void fillReferenceLists(const Dpb& dpb, const ReferenceSet& set, uint32_t current_slot,
                               DXVA_PicParams_HEVC& pic) {
  for (int i = 0; i < MAX_REF_PICS; ++i) {
    pic.RefPicList[i].bPicEntry = 0xff;
    pic.PicOrderCntValList[i] = 0;
  }
  for (int i = 0; i < MAX_REF_SET_ENTRIES; ++i) {
    pic.RefPicSetStCurrBefore[i] = 0xff;
    pic.RefPicSetStCurrAfter[i] = 0xff;
    pic.RefPicSetLtCurr[i] = 0xff;
  }

  // Index7Bits is seven bits wide, so no surface past 127 can be named at all.
  uint8_t index_of_slot[128];
  memset(index_of_slot, 0xff, sizeof(index_of_slot));

  uint8_t ref_count = 0;
  const auto& entries = dpb.entries();
  const size_t addressable = std::min(entries.size(), sizeof(index_of_slot));
  for (uint32_t slot = 0; slot < addressable && ref_count < MAX_REF_PICS; ++slot) {
    if (!entries[slot].is_reference || slot == current_slot) continue;
    pic.RefPicList[ref_count].Index7Bits = static_cast<UCHAR>(slot);
    pic.RefPicList[ref_count].AssociatedFlag = entries[slot].is_long_term ? 1 : 0;
    pic.PicOrderCntValList[ref_count] = entries[slot].poc;
    index_of_slot[slot] = ref_count;
    ++ref_count;
  }

  uint8_t filled[3] = {0, 0, 0};
  UCHAR* const lists[3] = {pic.RefPicSetStCurrBefore, pic.RefPicSetStCurrAfter, pic.RefPicSetLtCurr};
  for (size_t i = 0; i < set.count; ++i) {
    const WantedPicture& wanted = set.pictures[i];
    if (wanted.list == RefList::foll) continue;
    const auto which = static_cast<size_t>(wanted.list);
    if (filled[which] >= MAX_REF_SET_ENTRIES) continue;
    // A reference the decoder does not hold is left out, as a software decoder
    // does with a picture it cannot supply.
    const auto slot = dpb.findSlot(set, wanted);
    if (!slot.has_value() || *slot >= addressable || index_of_slot[*slot] == 0xff) continue;
    lists[which][filled[which]++] = index_of_slot[*slot];
  }
}

inline void fillPicParams(const Sps& sps,
                          const Pps& pps,
                          const SliceHeader& sh,
                          const ParsedFrame& frame,
                          int32_t poc,
                          uint32_t current_slot,
                          const Dpb& dpb,
                          const ReferenceSet& set,
                          uint32_t feedback,
                          DXVA_PicParams_HEVC& pic) {
  pic = {};
  const int min_cb_log2_size_y = sps.log2_min_luma_coding_block_size_minus3 + 3;
  pic.PicWidthInMinCbsY = static_cast<USHORT>(sps.pic_width_in_luma_samples >> min_cb_log2_size_y);
  pic.PicHeightInMinCbsY = static_cast<USHORT>(sps.pic_height_in_luma_samples >> min_cb_log2_size_y);
  pic.chroma_format_idc = static_cast<USHORT>(sps.chroma_format_idc);
  pic.separate_colour_plane_flag = static_cast<USHORT>(sps.separate_colour_plane_flag);
  pic.bit_depth_luma_minus8 = static_cast<USHORT>(sps.bit_depth_luma_minus8);
  pic.bit_depth_chroma_minus8 = static_cast<USHORT>(sps.bit_depth_chroma_minus8);
  pic.log2_max_pic_order_cnt_lsb_minus4 = static_cast<USHORT>(sps.log2_max_pic_order_cnt_lsb_minus4);
  pic.NoPicReorderingFlag = 0;
  pic.NoBiPredFlag = 0;

  pic.CurrPic.Index7Bits = static_cast<UCHAR>(current_slot);
  pic.CurrPic.AssociatedFlag = 0;
  pic.CurrPicOrderCntVal = poc;
  fillReferenceLists(dpb, set, current_slot, pic);

  pic.sps_max_dec_pic_buffering_minus1 = static_cast<UCHAR>(sps.sps_max_dec_pic_buffering_minus1[sps.max_sub_layers_minus1]);
  pic.log2_min_luma_coding_block_size_minus3 = static_cast<UCHAR>(sps.log2_min_luma_coding_block_size_minus3);
  pic.log2_diff_max_min_luma_coding_block_size = static_cast<UCHAR>(sps.log2_diff_max_min_luma_coding_block_size);
  pic.log2_min_transform_block_size_minus2 = static_cast<UCHAR>(sps.log2_min_luma_transform_block_size_minus2);
  pic.log2_diff_max_min_transform_block_size = static_cast<UCHAR>(sps.log2_diff_max_min_luma_transform_block_size);
  pic.max_transform_hierarchy_depth_inter = static_cast<UCHAR>(sps.max_transform_hierarchy_depth_inter);
  pic.max_transform_hierarchy_depth_intra = static_cast<UCHAR>(sps.max_transform_hierarchy_depth_intra);
  pic.num_short_term_ref_pic_sets = static_cast<UCHAR>(sps.num_short_term_ref_pic_sets);
  pic.num_long_term_ref_pics_sps = static_cast<UCHAR>(sps.num_long_term_ref_pics_sps);
  pic.scaling_list_enabled_flag = static_cast<UINT>(sps.scaling_list_enabled_flag);
  pic.amp_enabled_flag = static_cast<UINT>(sps.amp_enabled_flag);
  pic.sample_adaptive_offset_enabled_flag = static_cast<UINT>(sps.sample_adaptive_offset_enabled_flag);
  pic.pcm_enabled_flag = static_cast<UINT>(sps.pcm_enabled_flag);
  pic.pcm_sample_bit_depth_luma_minus1 = static_cast<UCHAR>(sps.pcm_sample_bit_depth_luma_minus1);
  pic.pcm_sample_bit_depth_chroma_minus1 = static_cast<UCHAR>(sps.pcm_sample_bit_depth_chroma_minus1);
  pic.log2_min_pcm_luma_coding_block_size_minus3 = static_cast<UCHAR>(sps.log2_min_pcm_luma_coding_block_size_minus3);
  pic.log2_diff_max_min_pcm_luma_coding_block_size = static_cast<UCHAR>(sps.log2_diff_max_min_pcm_luma_coding_block_size);
  pic.pcm_loop_filter_disabled_flag = static_cast<UINT>(sps.pcm_loop_filter_disabled_flag);
  pic.long_term_ref_pics_present_flag = static_cast<UINT>(sps.long_term_ref_pics_present_flag);
  pic.sps_temporal_mvp_enabled_flag = static_cast<UINT>(sps.sps_temporal_mvp_enabled_flag);
  pic.strong_intra_smoothing_enabled_flag = static_cast<UINT>(sps.strong_intra_smoothing_enabled_flag);

  pic.num_ref_idx_l0_default_active_minus1 = static_cast<UCHAR>(pps.num_ref_idx_l0_default_active_minus1);
  pic.num_ref_idx_l1_default_active_minus1 = static_cast<UCHAR>(pps.num_ref_idx_l1_default_active_minus1);
  pic.init_qp_minus26 = static_cast<CHAR>(pps.init_qp_minus26);
  pic.dependent_slice_segments_enabled_flag = static_cast<UINT>(pps.dependent_slice_segments_enabled_flag);
  pic.output_flag_present_flag = static_cast<UINT>(pps.output_flag_present_flag);
  pic.num_extra_slice_header_bits = static_cast<UINT>(pps.num_extra_slice_header_bits);
  pic.sign_data_hiding_enabled_flag = static_cast<UINT>(pps.sign_data_hiding_enabled_flag);
  pic.cabac_init_present_flag = static_cast<UINT>(pps.cabac_init_present_flag);
  pic.constrained_intra_pred_flag = static_cast<UINT>(pps.constrained_intra_pred_flag);
  pic.transform_skip_enabled_flag = static_cast<UINT>(pps.transform_skip_enabled_flag);
  pic.cu_qp_delta_enabled_flag = static_cast<UINT>(pps.cu_qp_delta_enabled_flag);
  pic.pps_slice_chroma_qp_offsets_present_flag = static_cast<UINT>(pps.pps_slice_chroma_qp_offsets_present_flag);
  pic.weighted_pred_flag = static_cast<UINT>(pps.weighted_pred_flag);
  pic.weighted_bipred_flag = static_cast<UINT>(pps.weighted_bipred_flag);
  pic.transquant_bypass_enabled_flag = static_cast<UINT>(pps.transquant_bypass_enabled_flag);
  pic.tiles_enabled_flag = static_cast<UINT>(pps.tiles_enabled_flag);
  pic.entropy_coding_sync_enabled_flag = static_cast<UINT>(pps.entropy_coding_sync_enabled_flag);
  pic.uniform_spacing_flag = static_cast<UINT>(pps.uniform_spacing_flag);
  // Inferred to be 1 without tiles, but ffmpeg and Chromium both report 0 there,
  // so no driver has ever been given the inferred value.
  pic.loop_filter_across_tiles_enabled_flag =
      static_cast<UINT>(pps.tiles_enabled_flag && pps.loop_filter_across_tiles_enabled_flag);
  pic.pps_loop_filter_across_slices_enabled_flag = static_cast<UINT>(pps.pps_loop_filter_across_slices_enabled_flag);
  pic.deblocking_filter_override_enabled_flag = static_cast<UINT>(pps.deblocking_filter_override_enabled_flag);
  pic.pps_deblocking_filter_disabled_flag = static_cast<UINT>(pps.pps_deblocking_filter_disabled_flag);
  pic.lists_modification_present_flag = static_cast<UINT>(pps.lists_modification_present_flag);
  pic.slice_segment_header_extension_present_flag = static_cast<UINT>(pps.slice_segment_header_extension_present_flag);
  pic.pps_cb_qp_offset = static_cast<CHAR>(pps.pps_cb_qp_offset);
  pic.pps_cr_qp_offset = static_cast<CHAR>(pps.pps_cr_qp_offset);
  pic.diff_cu_qp_delta_depth = static_cast<UCHAR>(pps.diff_cu_qp_delta_depth);
  pic.pps_beta_offset_div2 = static_cast<CHAR>(pps.pps_beta_offset_div2);
  pic.pps_tc_offset_div2 = static_cast<CHAR>(pps.pps_tc_offset_div2);
  pic.log2_parallel_merge_level_minus2 = static_cast<UCHAR>(pps.log2_parallel_merge_level_minus2);
  pic.num_tile_columns_minus1 = static_cast<UCHAR>(pps.tiles_enabled_flag ? pps.num_tile_columns_minus1 : 0);
  pic.num_tile_rows_minus1 = static_cast<UCHAR>(pps.tiles_enabled_flag ? pps.num_tile_rows_minus1 : 0);
  if (pps.tiles_enabled_flag && !pps.uniform_spacing_flag) {
    for (int i = 0; i <= pps.num_tile_columns_minus1 && i < video_parser::H265_MAX_TILE_COLUMNS; ++i) {
      pic.column_width_minus1[i] = static_cast<USHORT>(pps.column_width_minus1[i]);
    }
    for (int i = 0; i <= pps.num_tile_rows_minus1 && i < video_parser::H265_MAX_TILE_ROWS; ++i) {
      pic.row_height_minus1[i] = static_cast<USHORT>(pps.row_height_minus1[i]);
    }
  }

  // How the hardware finds its way past a set written out in the slice header.
  // A set the slice coded outright predicts from nothing, so the count is zero;
  // passing the set's own NumDeltaPocs, as this used to, describes a different
  // set than the one in the bitstream.
  const StRefPicSet& st = activeStRefPicSet(sps, sh);
  pic.ucNumDeltaPocsOfRefRpsIdx =
      sh.short_term_ref_pic_set_sps_flag ? 0 : static_cast<UCHAR>(st.num_delta_pocs_of_ref_rps);
  pic.wNumBitsForShortTermRPSInSlice = sh.short_term_ref_pic_set_sps_flag ? 0 : static_cast<USHORT>(sh.st_rps_bits);
  pic.IrapPicFlag = isIrap(frame.nal_unit_type) ? 1 : 0;
  pic.IdrPicFlag = isIdr(frame.nal_unit_type) ? 1 : 0;
  pic.IntraPicFlag = isIrap(frame.nal_unit_type) ? 1 : 0;
  pic.StatusReportFeedbackNumber = feedback == 0 ? 1 : feedback;
}

#endif

} // namespace openmedia::dx_h265
