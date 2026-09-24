#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxva.h>
#endif

#include <openmedia/video.hpp>
#include <util/bit_reader.hpp>
#include <util/color_codes.hpp>
#include <video/parser/h264_types.hpp>

namespace openmedia::dx_h264 {

template<typename T>
constexpr auto alignUp(T value, T alignment) -> T {
  if (alignment <= 1) return value;
  return ((value + alignment - 1) / alignment) * alignment;
}

inline auto isAnnexB(std::span<const uint8_t> data) -> bool {
  return data.size() >= 3 && data[0] == 0 && data[1] == 0 &&
         (data[2] == 1 || (data.size() >= 4 && data[2] == 0 && data[3] == 1));
}

struct ParsedFrame {
  std::span<const uint8_t> bitstream;
  std::vector<uint32_t> slice_offsets;
  h264::NALHeader nal = {};
  h264::SliceHeader slice = {};
  int32_t poc = 0;
  bool is_intra = false;
  bool is_reference = false;
};

struct State {
  h264::SPS sps[32] = {};
  h264::PPS pps[256] = {};
  bool sps_valid[32] = {};
  bool pps_valid[256] = {};
  bool has_sps = false;
  bool has_pps = false;
  int prev_pic_order_cnt_lsb = 0;
  int prev_pic_order_cnt_msb = 0;
  // pic_order_cnt_type 1 and 2 derive the count from frame_num instead, and
  // need the previous picture's frame_num and its wrap offset to do it.
  int prev_frame_num = 0;
  int prev_frame_num_offset = 0;
  bool have_prev_poc = false;
  openmedia::RbspBuffer rbsp_scratch;

  void resetPoc() {
    prev_pic_order_cnt_lsb = 0;
    prev_pic_order_cnt_msb = 0;
    prev_frame_num = 0;
    prev_frame_num_offset = 0;
    have_prev_poc = false;
  }

  // `nal_data` must cover exactly one NAL unit starting at its header byte:
  // read_pps uses more_rbsp_data(), which needs the rbsp_stop_one_bit of this
  // unit and not of whatever follows it in the packet.
  void storeNal(std::span<const uint8_t> nal_data) {
    if (nal_data.empty()) return;
    h264::Bitstream bs;
    bs.init(nal_data.data(), nal_data.size());
    h264::NALHeader nal;
    if (!h264::read_nal_header(nal, bs)) return;
    if (nal.type == h264::NAL_UNIT_TYPE_SPS) {
      h264::SPS parsed = {};
      if (!h264::read_sps(parsed, bs, rbsp_scratch)) return;
      sps[parsed.seq_parameter_set_id] = parsed;
      sps_valid[parsed.seq_parameter_set_id] = true;
      has_sps = true;
    } else if (nal.type == h264::NAL_UNIT_TYPE_PPS) {
      h264::PPS parsed = {};
      if (!h264::read_pps(parsed, sps, bs, rbsp_scratch)) return;
      pps[parsed.pic_parameter_set_id] = parsed;
      pps_valid[parsed.pic_parameter_set_id] = true;
      has_pps = true;
    }
  }

  void parseExtradata(std::span<const uint8_t> extradata) {
    if (extradata.empty()) return;
    if (extradata.size() >= 7 && extradata[0] == 1) {
      size_t offset = 5; // configurationVersion, profile, compat, level, lengthSize
      const uint8_t sps_count = extradata[offset++] & 0x1fu;
      for (uint8_t i = 0; i < sps_count && offset + 2 <= extradata.size(); ++i) {
        const size_t size = (static_cast<size_t>(extradata[offset]) << 8u) | extradata[offset + 1];
        offset += 2;
        if (offset + size > extradata.size()) return;
        storeNal(extradata.subspan(offset, size));
        offset += size;
      }
      if (offset >= extradata.size()) return;
      const uint8_t pps_count = extradata[offset++];
      for (uint8_t i = 0; i < pps_count && offset + 2 <= extradata.size(); ++i) {
        const size_t size = (static_cast<size_t>(extradata[offset]) << 8u) | extradata[offset + 1];
        offset += 2;
        if (offset + size > extradata.size()) return;
        storeNal(extradata.subspan(offset, size));
        offset += size;
      }
      return;
    }

    if (!isAnnexB(extradata)) return;
    const uint8_t* begin = extradata.data();
    const uint8_t* end = begin + extradata.size();
    for (const uint8_t* p = begin; p + 3 <= end;) {
      if (!(p[0] == 0 && p[1] == 0 && (p[2] == 1 || (p + 4 <= end && p[2] == 0 && p[3] == 1)))) {
        ++p;
        continue;
      }
      const uint8_t* nal_start = p + (p[2] == 1 ? 3 : 4);
      const uint8_t* nal_end = end;
      for (const uint8_t* q = nal_start; q + 3 <= end; ++q) {
        if (q[0] == 0 && q[1] == 0 && (q[2] == 1 || (q + 4 <= end && q[2] == 0 && q[3] == 1))) {
          nal_end = q;
          break;
        }
      }
      storeNal({nal_start, static_cast<size_t>(nal_end - nal_start)});
      p = nal_end;
    }
  }

  // 8.2.1. All three pic_order_cnt_types, because only type 0 keeps the count
  // in the slice header: types 1 and 2 derive it from frame_num, and returning
  // slice.pic_order_cnt_lsb for them handed back a hard 0 for every picture in
  // the stream — the field is never read when the SPS does not use type 0. A
  // constant POC leaves DXVA's CurrFieldOrderCnt and FieldOrderCntList flat,
  // which is what the driver uses to tell the pictures in the DPB apart.
  //
  // Type 2 in particular is what NVENC and the Windows game capture encoders
  // emit for their B-frame-free streams, so this was not an exotic corner.
  auto computePoc(const h264::SliceHeader& slice, bool is_idr, bool is_reference) -> int32_t {
    if (slice.pic_parameter_set_id < 0 || slice.pic_parameter_set_id >= 256 || !pps_valid[slice.pic_parameter_set_id]) {
      return slice.pic_order_cnt_lsb;
    }
    const auto& p = pps[slice.pic_parameter_set_id];
    if (p.seq_parameter_set_id < 0 || p.seq_parameter_set_id >= 32 || !sps_valid[p.seq_parameter_set_id]) {
      return slice.pic_order_cnt_lsb;
    }
    const auto& s = sps[p.seq_parameter_set_id];

    // 8.2.1.2/8.2.1.3: frame_num wraps, so both derived types carry an offset
    // that steps by MaxFrameNum every time it does.
    const auto frameNumOffset = [&]() -> int {
      if (is_idr) return 0;
      const int max_frame_num = 1 << (s.log2_max_frame_num_minus4 + 4);
      if (prev_frame_num > slice.frame_num) return prev_frame_num_offset + max_frame_num;
      return prev_frame_num_offset;
    };
    const auto rememberFrameNum = [&](int frame_num_offset) {
      prev_frame_num = slice.mmco5 ? 0 : slice.frame_num;
      prev_frame_num_offset = slice.mmco5 ? 0 : frame_num_offset;
    };

    if (s.pic_order_cnt_type == 1) {
      const int frame_num_offset = frameNumOffset();
      int abs_frame_num = 0;
      if (s.num_ref_frames_in_pic_order_cnt_cycle > 0) abs_frame_num = frame_num_offset + slice.frame_num;
      if (!is_reference && abs_frame_num > 0) abs_frame_num--;

      int expected_poc = 0;
      if (abs_frame_num > 0) {
        const int cycle_cnt = (abs_frame_num - 1) / s.num_ref_frames_in_pic_order_cnt_cycle;
        const int frame_num_in_cycle = (abs_frame_num - 1) % s.num_ref_frames_in_pic_order_cnt_cycle;
        int expected_delta_per_cycle = 0;
        for (int i = 0; i < s.num_ref_frames_in_pic_order_cnt_cycle; ++i) expected_delta_per_cycle += s.offset_for_ref_frame[i];
        expected_poc = cycle_cnt * expected_delta_per_cycle;
        for (int i = 0; i <= frame_num_in_cycle; ++i) expected_poc += s.offset_for_ref_frame[i];
      }
      if (!is_reference) expected_poc += s.offset_for_non_ref_pic;

      const int top_foc = expected_poc + slice.delta_pic_order_cnt[0];
      const int bottom_foc = top_foc + s.offset_for_top_to_bottom_field + slice.delta_pic_order_cnt[1];
      rememberFrameNum(frame_num_offset);
      return slice.mmco5 ? 0 : std::min(top_foc, bottom_foc);
    }

    if (s.pic_order_cnt_type == 2) {
      const int frame_num_offset = frameNumOffset();
      const int abs_frame_num = frame_num_offset + slice.frame_num;
      rememberFrameNum(frame_num_offset);
      if (slice.mmco5 || is_idr) return 0;
      // Decoding order is output order here, so a non-reference picture sits
      // immediately before the reference picture that follows it.
      return is_reference ? 2 * abs_frame_num : 2 * abs_frame_num - 1;
    }

    // Type 0. prevPicOrderCntMsb/Lsb come from the previous *reference* picture
    // in decoding order; letting non-reference pictures update them made the MSB
    // step at the wrong moment once pic_order_cnt_lsb wrapped, which put the
    // reordering queue and the DXVA field order counts out of step with the
    // stream.
    const int max_pic_order_cnt_lsb = 1 << (s.log2_max_pic_order_cnt_lsb_minus4 + 4);
    int pic_order_cnt_msb = 0;
    if (have_prev_poc) {
      if (slice.pic_order_cnt_lsb < prev_pic_order_cnt_lsb &&
          (prev_pic_order_cnt_lsb - slice.pic_order_cnt_lsb) >= max_pic_order_cnt_lsb / 2) {
        pic_order_cnt_msb = prev_pic_order_cnt_msb + max_pic_order_cnt_lsb;
      } else if (slice.pic_order_cnt_lsb > prev_pic_order_cnt_lsb &&
                 (slice.pic_order_cnt_lsb - prev_pic_order_cnt_lsb) > max_pic_order_cnt_lsb / 2) {
        pic_order_cnt_msb = prev_pic_order_cnt_msb - max_pic_order_cnt_lsb;
      } else {
        pic_order_cnt_msb = prev_pic_order_cnt_msb;
      }
    }
    if (is_reference) {
      prev_pic_order_cnt_lsb = slice.pic_order_cnt_lsb;
      prev_pic_order_cnt_msb = pic_order_cnt_msb;
      have_prev_poc = true;
    }
    const int top_foc = pic_order_cnt_msb + slice.pic_order_cnt_lsb;
    const int bottom_foc = top_foc + slice.delta_pic_order_cnt_bottom;
    return slice.mmco5 ? 0 : std::min(top_foc, bottom_foc);
  }

  auto parseFrame(std::span<const uint8_t> packet) -> ParsedFrame {
    ParsedFrame frame;
    frame.bitstream = packet;

    // Collect the NAL boundaries up front so each unit can be parsed within its
    // own extent instead of running to the end of the packet.
    std::vector<size_t> headers;
    {
      h264::Bitstream scan;
      scan.init(frame.bitstream.data(), frame.bitstream.size());
      while (h264::find_next_nal(scan)) headers.push_back(scan.byte_offset());
    }

    for (size_t i = 0; i < headers.size(); ++i) {
      const size_t header_pos = headers[i];
      const size_t end = (i + 1 < headers.size()) ? headers[i + 1] - 3 : frame.bitstream.size();
      if (header_pos >= end) continue;
      const auto nal_unit = frame.bitstream.subspan(header_pos, end - header_pos);

      h264::Bitstream bs;
      bs.init(nal_unit.data(), nal_unit.size());
      h264::NALHeader nal;
      if (!h264::read_nal_header(nal, bs)) continue;
      if (nal.type == h264::NAL_UNIT_TYPE_SPS || nal.type == h264::NAL_UNIT_TYPE_PPS) {
        storeNal(nal_unit);
      } else if (nal.type == h264::NAL_UNIT_TYPE_CODED_SLICE_IDR || nal.type == h264::NAL_UNIT_TYPE_CODED_SLICE_NON_IDR) {
        if (!has_sps || !has_pps) continue;
        if (!h264::read_slice_header(frame.slice, nal, pps, sps, bs, rbsp_scratch)) continue;
        frame.slice_offsets.push_back(static_cast<uint32_t>(header_pos) - 3);
        frame.nal = nal;
      }
    }

    frame.is_intra = frame.nal.type == h264::NAL_UNIT_TYPE_CODED_SLICE_IDR;
    frame.is_reference = frame.nal.idc != h264::NAL_REF_IDC_PRIORITY_DISPOSABLE;
    frame.poc = computePoc(frame.slice, frame.is_intra, frame.is_reference);
    return frame;
  }
};

// Why a parsed frame cannot be handed to the hardware, when it cannot.
enum class ParameterSetStatus {
  ok,
  unknown_pps,        // the slice names a picture parameter set never sent
  unknown_sps,        // that PPS names a sequence parameter set never sent
  unsupported_coding, // more than 8 bits per sample, or more than one slice group
};

inline auto describe(ParameterSetStatus status) -> std::string_view {
  switch (status) {
    case ParameterSetStatus::unknown_pps:
      return "the slice refers to a picture parameter set the stream has not sent";
    case ParameterSetStatus::unknown_sps:
      return "the picture parameter set refers to a sequence parameter set the stream has not sent";
    case ParameterSetStatus::unsupported_coding:
      return "this DXVA path decodes 8-bit, single-slice-group H.264 only";
    case ParameterSetStatus::ok:
      return "";
  }
  return "";
}

// The parameter sets a frame decodes against.
//
// D3D11 and D3D12 both have to resolve and vet exactly these before they can
// fill in a DXVA picture parameter structure, and a caller that leaves one of
// the checks out ends up reading a parameter set the stream never sent.
struct ActiveParameterSets {
  const h264::SPS* sps = nullptr;
  const h264::PPS* pps = nullptr;
  ParameterSetStatus status = ParameterSetStatus::ok;

  explicit operator bool() const { return status == ParameterSetStatus::ok; }
};

inline auto activeParameterSets(const State& state, const h264::SliceHeader& slice) -> ActiveParameterSets {
  const int pps_id = slice.pic_parameter_set_id;
  if (pps_id < 0 || pps_id >= 256 || !state.pps_valid[pps_id]) {
    return {.status = ParameterSetStatus::unknown_pps};
  }
  const h264::PPS& pps = state.pps[pps_id];

  const int sps_id = pps.seq_parameter_set_id;
  if (sps_id < 0 || sps_id >= 32 || !state.sps_valid[sps_id]) {
    return {.status = ParameterSetStatus::unknown_sps};
  }
  const h264::SPS& sps = state.sps[sps_id];

  if (sps.bit_depth_luma_minus8 != 0 || sps.bit_depth_chroma_minus8 != 0 ||
      pps.num_slice_groups_minus1 != 0) {
    return {.status = ParameterSetStatus::unsupported_coding};
  }
  return {.sps = &sps, .pps = &pps};
}

// Annex A Table A-1: the DPB capacity, in macroblocks, that each level
// guarantees a decoder will provide.
inline auto maxDpbMbsForLevel(int level_idc) -> uint32_t {
  switch (level_idc) {
    case 10: return 396;
    case 11: return 900;
    case 12:
    case 13:
    case 20: return 2376;
    case 21: return 4752;
    case 22:
    case 30: return 8100;
    case 31: return 18000;
    case 32: return 20480;
    case 40:
    case 41: return 32768;
    case 42: return 34816;
    case 50: return 110400;
    case 51:
    case 52: return 184320;
    default: return 696320; // level 6.x and anything newer
  }
}

// The colour description a stream states in its VUI, which is where it lives for
// H.26x: containers often say nothing, and a hardware decoder that is handed the
// bitstream whole reports nothing either. Left as it was when the stream stays
// silent, so a container that did describe its colour keeps the last word.
inline auto applyColorDescription(const State& state, VideoFormat& format) -> bool {
  for (uint32_t i = 0; i < 32; ++i) {
    if (!state.sps_valid[i]) continue;
    const h264::SPS& sps = state.sps[i];
    if (!sps.vui_parameters_present_flag) return true;
    if (sps.vui.colour_description_present_flag) {
      format.color_primaries = color_codes::primariesFromCode(sps.vui.colour_primaries);
      format.transfer_char = color_codes::transferFromCode(sps.vui.transfer_characteristics);
      format.color_space = color_codes::colorSpaceFromMatrix(sps.vui.matrix_coefficients);
    }
    if (sps.vui.video_signal_type_present_flag)
      format.color_range = sps.vui.video_full_range_flag ? OM_COLOR_RANGE_FULL : OM_COLOR_RANGE_LIMITED;
    return true;
  }
  return false;
}

// The luma bit depth of the first sequence parameter set the stream carries, or
// zero while none has been seen. A decoder has to know it before it opens, as
// 10-bit content needs a 10-bit surface to come out of.
inline auto lumaBitDepth(const State& state) -> uint8_t {
  for (uint32_t i = 0; i < 32; ++i)
    if (state.sps_valid[i]) return static_cast<uint8_t>(state.sps[i].bit_depth_luma_minus8 + 8);
  return 0;
}

// How many decoded pictures may be held back to put them into output order.
//
// A stream that carries bitstream_restriction_flag states this outright. Without
// it this used to return 0, meaning no reordering at all, so a stream with
// B-frames was emitted in decode order and its timestamps ran backwards. The
// fallback is the DPB capacity the stream's own level guarantees, which is what
// a decoder is entitled to assume when the stream stays silent.
inline auto reorderDepth(const h264::SPS& sps) -> size_t {
  if (sps.vui_parameters_present_flag && sps.vui.bitstream_restriction_flag) {
    return static_cast<size_t>(std::clamp(sps.vui.num_reorder_frames, 0, 16));
  }

  const uint32_t width_mbs = static_cast<uint32_t>(sps.pic_width_in_mbs_minus1) + 1u;
  const uint32_t height_mbs = (static_cast<uint32_t>(sps.pic_height_in_map_units_minus1) + 1u) *
                              (sps.frame_mbs_only_flag ? 1u : 2u);
  const uint32_t frame_mbs = width_mbs * height_mbs;
  if (frame_mbs == 0) return 16;

  const uint32_t frames = maxDpbMbsForLevel(sps.level_idc) / frame_mbs;
  return static_cast<size_t>(std::clamp<uint32_t>(frames, 1u, 16u));
}

struct DpbEntry {
  int32_t poc = 0;
  uint32_t frame_num = 0;
  bool is_reference = false;
  bool is_long_term = false;
  int32_t long_term_frame_idx = -1;
  // FrameNumWrap, recomputed against the current picture's frame_num. The MMCO
  // operations address short-term pictures by this, not by frame_num.
  int32_t pic_num = 0;
};

// The decoded picture buffer and the reference picture marking process of
// ITU-T H.264 8.2.5.
//
// There used to be no marking at all: slots were handed out round-robin and
// every picture decoded into one stayed flagged as a reference until the next
// IDR. Once more pictures had been decoded than the stream's max_num_ref_frames,
// the reference list handed to DXVA described pictures the stream had long since
// retired, and the driver answered by emitting a copy of a reference instead of
// a decoded picture. Streams without B-frames never got far enough into the list
// for it to matter, which is why only some clips broke.
class Dpb {
public:
  void configure(uint32_t slot_count, uint32_t max_num_ref_frames, uint32_t max_frame_num) {
    entries_.assign(slot_count, DpbEntry {});
    max_num_ref_frames_ = std::max(max_num_ref_frames, 1u);
    max_frame_num_ = std::max(max_frame_num, 1u);
  }

  void reset() {
    for (auto& entry : entries_) entry = {};
  }

  auto entries() const -> const std::vector<DpbEntry>& { return entries_; }
  auto slotCount() const -> uint32_t { return static_cast<uint32_t>(entries_.size()); }

  // 8.2.4.1: short-term pictures are addressed relative to the picture being
  // decoded, so their PicNums have to be refreshed against its frame_num before
  // any marking operation can resolve one.
  void updatePicNums(uint32_t frame_num) {
    for (auto& entry : entries_) {
      if (!entry.is_reference || entry.is_long_term) continue;
      entry.pic_num = entry.frame_num > frame_num
                          ? static_cast<int32_t>(entry.frame_num) - static_cast<int32_t>(max_frame_num_)
                          : static_cast<int32_t>(entry.frame_num);
    }
  }

  // The slot to decode into: any the DPB is not holding a reference in. The
  // picture is read back immediately, so nothing has to be reserved for output.
  auto acquireSlot() const -> uint32_t {
    for (uint32_t i = 0; i < entries_.size(); ++i) {
      if (!entries_[i].is_reference) return i;
    }
    // Every slot is spoken for, meaning the stream keeps more references live
    // than its own max_num_ref_frames allows. Evicting the oldest short-term
    // picture keeps decoding instead of failing the frame outright.
    return oldestShortTermSlot().value_or(0u);
  }

  // Runs the marking process for the picture just decoded into `slot`.
  void store(uint32_t slot, int32_t poc, const h264::SliceHeader& slice,
             bool is_idr, bool is_reference) {
    if (slot >= entries_.size()) return;

    if (is_idr) {
      markAllUnused();
      const bool long_term = slice.long_term_reference_flag != 0;
      writeEntry(slot, poc, slice, long_term, long_term ? 0 : -1);
      return;
    }

    if (!is_reference) {
      // Non-reference pictures leave the DPB alone; the slot they borrowed is
      // free again as soon as the picture has been read back.
      entries_[slot] = {};
      return;
    }

    if (slice.adaptive_ref_pic_marking_mode_flag) {
      if (applyMarkings(slot, poc, slice)) return; // operation 6 already stored it
      writeEntry(slot, poc, slice, false, -1);
      return;
    }

    slidingWindow(slot);
    writeEntry(slot, poc, slice, false, -1);
  }

private:
  void markAllUnused() {
    for (auto& entry : entries_) {
      entry.is_reference = false;
      entry.is_long_term = false;
      entry.long_term_frame_idx = -1;
    }
  }

  void writeEntry(uint32_t slot, int32_t poc, const h264::SliceHeader& slice,
                  bool long_term, int32_t long_term_frame_idx) {
    auto& entry = entries_[slot];
    entry.poc = poc;
    entry.frame_num = static_cast<uint32_t>(slice.frame_num);
    entry.pic_num = slice.frame_num;
    entry.is_reference = true;
    entry.is_long_term = long_term;
    entry.long_term_frame_idx = long_term_frame_idx;
  }

  auto referenceCount(uint32_t except_slot) const -> uint32_t {
    uint32_t count = 0;
    for (uint32_t i = 0; i < entries_.size(); ++i) {
      if (i != except_slot && entries_[i].is_reference) ++count;
    }
    return count;
  }

  auto oldestShortTermSlot() const -> std::optional<uint32_t> {
    std::optional<uint32_t> oldest;
    int32_t smallest = 0;
    for (uint32_t i = 0; i < entries_.size(); ++i) {
      const auto& entry = entries_[i];
      if (!entry.is_reference || entry.is_long_term) continue;
      if (!oldest || entry.pic_num < smallest) {
        oldest = i;
        smallest = entry.pic_num;
      }
    }
    return oldest;
  }

  // 8.2.5.3: retire the short-term picture with the smallest FrameNumWrap once
  // the buffer is full.
  void slidingWindow(uint32_t current_slot) {
    if (referenceCount(current_slot) < max_num_ref_frames_) return;
    if (auto oldest = oldestShortTermSlot()) {
      if (*oldest != current_slot) entries_[*oldest] = {};
    }
  }

  // 8.2.5.4. Returns true when operation 6 turned the current picture into a
  // long-term reference, in which case the caller must not overwrite the entry.
  auto applyMarkings(uint32_t current_slot, int32_t poc, const h264::SliceHeader& slice) -> bool {
    const int32_t curr_pic_num = slice.frame_num;
    bool current_is_long_term = false;

    for (int i = 0; i < slice.num_ref_pic_markings; ++i) {
      const auto& marking = slice.ref_pic_markings[i];
      switch (marking.operation) {
        case 1: { // short-term picture -> unused for reference
          const int32_t pic_num = curr_pic_num - (marking.difference_of_pic_nums_minus1 + 1);
          for (auto& entry : entries_) {
            if (entry.is_reference && !entry.is_long_term && entry.pic_num == pic_num) entry = {};
          }
          break;
        }
        case 2: { // long-term picture -> unused for reference
          for (auto& entry : entries_) {
            if (entry.is_reference && entry.is_long_term &&
                entry.long_term_frame_idx == marking.long_term_pic_num) {
              entry = {};
            }
          }
          break;
        }
        case 3: { // short-term picture -> long-term
          const int32_t pic_num = curr_pic_num - (marking.difference_of_pic_nums_minus1 + 1);
          for (auto& entry : entries_) {
            if (entry.is_reference && entry.is_long_term &&
                entry.long_term_frame_idx == marking.long_term_frame_idx) {
              entry = {};
            }
          }
          for (auto& entry : entries_) {
            if (entry.is_reference && !entry.is_long_term && entry.pic_num == pic_num) {
              entry.is_long_term = true;
              entry.long_term_frame_idx = marking.long_term_frame_idx;
            }
          }
          break;
        }
        case 4: { // shrink the long-term index range
          const int32_t limit = marking.max_long_term_frame_idx_plus1 - 1;
          for (auto& entry : entries_) {
            if (entry.is_reference && entry.is_long_term && entry.long_term_frame_idx > limit) {
              entry = {};
            }
          }
          break;
        }
        case 5: // everything -> unused for reference
          markAllUnused();
          break;
        case 6: { // current picture -> long-term
          for (auto& entry : entries_) {
            if (entry.is_reference && entry.is_long_term &&
                entry.long_term_frame_idx == marking.long_term_frame_idx) {
              entry = {};
            }
          }
          writeEntry(current_slot, poc, slice, true, marking.long_term_frame_idx);
          current_is_long_term = true;
          break;
        }
        default:
          break;
      }
    }
    return current_is_long_term;
  }

  std::vector<DpbEntry> entries_;
  uint32_t max_num_ref_frames_ = 4;
  uint32_t max_frame_num_ = 16;
};

// An IDR restarts the stream: the reference buffer is emptied and the picture
// counts begin again from it. The count parseFrame worked out for this frame
// was derived against the stretch that just ended, so it has to be taken again
// once the state is cleared -- which is the part a caller doing this by hand is
// most likely to leave out.
//
// `Dpb` is passed separately because `State` holds the parameter sets and the
// parser's own bookkeeping, while the reference buffer belongs to whichever
// decoder owns the surfaces.
inline void restartAtIdr(State& state, Dpb& dpb, ParsedFrame& frame) {
  dpb.reset();
  state.resetPoc();
  frame.poc = state.computePoc(frame.slice, frame.is_intra, frame.is_reference);
}

#ifdef _WIN32
// DXVA_PicParams_H264 describes the reference picture buffer with a fixed
// 16-entry list, whatever the stream's own DPB capacity is.
inline constexpr size_t MAX_REF_FRAMES = 16;

inline void fillQMatrix(const h264::SPS& sps, const h264::PPS& pps, DXVA_Qmatrix_H264& qmatrix) {
  if (!sps.seq_scaling_matrix_present_flag && !pps.pic_scaling_matrix_present_flag) {
    memset(&qmatrix, 16, sizeof(qmatrix));
    return;
  }
  static constexpr int z4[] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
  static constexpr int z8[] = {
      0, 1, 8,16, 9, 2, 3,10, 17,24,32,25,18,11, 4, 5,
      12,19,26,33,40,48,41,34, 27,20,13, 6, 7,14,21,28,
      35,42,49,56,57,50,43,36, 29,22,15,23,30,37,44,51,
      58,59,52,45,38,31,39,46, 53,60,61,54,47,55,62,63};
  for (int i = 0; i < 6; ++i) for (int j = 0; j < 16; ++j) {
    qmatrix.bScalingLists4x4[i][j] = static_cast<UCHAR>(pps.ScalingList4x4[i][z4[j]]);
  }
  for (int i = 0; i < 2; ++i) for (int j = 0; j < 64; ++j) {
    qmatrix.bScalingLists8x8[i][j] = static_cast<UCHAR>(pps.ScalingList8x8[i][z8[j]]);
  }
}

inline void fillPicParams(const h264::SPS& sps,
                          const h264::PPS& pps,
                          const h264::SliceHeader& slice,
                          const ParsedFrame& frame,
                          uint32_t current_slot,
                          const Dpb& dpb,
                          uint32_t feedback,
                          DXVA_PicParams_H264& pic) {
  pic = {};
  pic.wFrameWidthInMbsMinus1 = static_cast<USHORT>(sps.pic_width_in_mbs_minus1);
  pic.wFrameHeightInMbsMinus1 = static_cast<USHORT>(sps.pic_height_in_map_units_minus1);
  pic.IntraPicFlag = frame.is_intra ? 1 : 0;
  pic.MbaffFrameFlag = 0;
  pic.field_pic_flag = 0;
  pic.chroma_format_idc = 1;
  pic.bit_depth_chroma_minus8 = static_cast<UCHAR>(sps.bit_depth_chroma_minus8);
  pic.bit_depth_luma_minus8 = static_cast<UCHAR>(sps.bit_depth_luma_minus8);
  pic.residual_colour_transform_flag = static_cast<UCHAR>(sps.separate_colour_plane_flag);
  pic.CurrPic.AssociatedFlag = 0;
  pic.CurrPic.Index7Bits = static_cast<UCHAR>(current_slot);
  pic.CurrFieldOrderCnt[0] = frame.poc;
  pic.CurrFieldOrderCnt[1] = frame.poc;
  for (size_t i = 0; i < MAX_REF_FRAMES; ++i) {
    pic.RefFrameList[i].bPicEntry = 0xff;
    pic.FieldOrderCntList[i][0] = 0;
    pic.FieldOrderCntList[i][1] = 0;
    pic.FrameNumList[i] = 0;
  }
  // The list is packed: every entry the DPB still marks as a reference, in slot
  // order, with the unused tail left at 0xff. Long-term pictures are flagged as
  // such and carry their LongTermFrameIdx in place of a frame_num, which is what
  // the driver needs to build the slice reference lists itself.
  size_t ref_index = 0;
  for (uint32_t slot = 0; slot < dpb.slotCount() && ref_index < MAX_REF_FRAMES; ++slot) {
    const auto& entry = dpb.entries()[slot];
    if (!entry.is_reference || slot == current_slot) continue;
    pic.RefFrameList[ref_index].AssociatedFlag = entry.is_long_term ? 1 : 0;
    pic.RefFrameList[ref_index].Index7Bits = static_cast<UCHAR>(slot);
    pic.FieldOrderCntList[ref_index][0] = entry.poc;
    pic.FieldOrderCntList[ref_index][1] = entry.poc;
    pic.UsedForReferenceFlags |= 1u << (ref_index * 2 + 0);
    pic.UsedForReferenceFlags |= 1u << (ref_index * 2 + 1);
    pic.FrameNumList[ref_index] =
        static_cast<USHORT>((entry.is_long_term ? entry.long_term_frame_idx : static_cast<int32_t>(entry.frame_num)));
    ++ref_index;
  }
  pic.weighted_pred_flag = static_cast<UCHAR>(pps.weighted_pred_flag);
  pic.weighted_bipred_idc = static_cast<UCHAR>(pps.weighted_bipred_idc);
  pic.transform_8x8_mode_flag = static_cast<UCHAR>(pps.transform_8x8_mode_flag);
  pic.constrained_intra_pred_flag = static_cast<UCHAR>(pps.constrained_intra_pred_flag);
  pic.num_ref_frames = static_cast<UCHAR>(sps.num_ref_frames);
  pic.MbsConsecutiveFlag = 1;
  pic.frame_mbs_only_flag = static_cast<UCHAR>(sps.frame_mbs_only_flag);
  pic.MinLumaBipredSize8x8Flag = sps.level_idc >= 31;
  pic.RefPicFlag = frame.is_reference ? 1 : 0;
  pic.frame_num = static_cast<USHORT>(slice.frame_num);
  pic.pic_init_qp_minus26 = static_cast<CHAR>(pps.pic_init_qp_minus26);
  pic.pic_init_qs_minus26 = static_cast<CHAR>(pps.pic_init_qs_minus26);
  pic.chroma_qp_index_offset = static_cast<CHAR>(pps.chroma_qp_index_offset);
  pic.second_chroma_qp_index_offset = static_cast<CHAR>(pps.second_chroma_qp_index_offset);
  pic.log2_max_frame_num_minus4 = static_cast<UCHAR>(sps.log2_max_frame_num_minus4);
  pic.pic_order_cnt_type = static_cast<UCHAR>(sps.pic_order_cnt_type);
  pic.log2_max_pic_order_cnt_lsb_minus4 = static_cast<UCHAR>(sps.log2_max_pic_order_cnt_lsb_minus4);
  pic.delta_pic_order_always_zero_flag = static_cast<UCHAR>(sps.delta_pic_order_always_zero_flag);
  pic.direct_8x8_inference_flag = static_cast<UCHAR>(sps.direct_8x8_inference_flag);
  pic.entropy_coding_mode_flag = static_cast<UCHAR>(pps.entropy_coding_mode_flag);
  pic.pic_order_present_flag = static_cast<UCHAR>(pps.pic_order_present_flag);
  pic.num_slice_groups_minus1 = static_cast<UCHAR>(pps.num_slice_groups_minus1);
  pic.slice_group_map_type = static_cast<UCHAR>(pps.slice_group_map_type);
  pic.deblocking_filter_control_present_flag = static_cast<UCHAR>(pps.deblocking_filter_control_present_flag);
  pic.redundant_pic_cnt_present_flag = static_cast<UCHAR>(pps.redundant_pic_cnt_present_flag);
  pic.slice_group_change_rate_minus1 = static_cast<USHORT>(pps.slice_group_change_rate_minus1);
  pic.Reserved16Bits = 3;
  pic.StatusReportFeedbackNumber = feedback == 0 ? 1 : feedback;
  pic.ContinuationFlag = 1;
  pic.num_ref_idx_l0_active_minus1 = static_cast<UCHAR>(pps.num_ref_idx_l0_active_minus1);
  pic.num_ref_idx_l1_active_minus1 = static_cast<UCHAR>(pps.num_ref_idx_l1_active_minus1);
}
#endif

} // namespace openmedia::dx_h264
