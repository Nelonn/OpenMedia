#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <dxva.h>
#endif

#include <h264.h>

namespace openmedia::dx_h264 {

template<typename T>
static auto alignUp(T value, T alignment) -> T {
  if (alignment <= 1) return value;
  return ((value + alignment - 1) / alignment) * alignment;
}

static auto isAnnexB(std::span<const uint8_t> data) -> bool {
  return data.size() >= 3 && data[0] == 0 && data[1] == 0 &&
         (data[2] == 1 || (data.size() >= 4 && data[2] == 0 && data[3] == 1));
}

struct ParsedFrame {
  std::span<const uint8_t> bitstream;
  std::vector<uint8_t> owned_bitstream;
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
  uint8_t nal_length_size = 0;
  int prev_pic_order_cnt_lsb = 0;
  int prev_pic_order_cnt_msb = 0;
  bool have_prev_poc = false;

  void resetPoc() {
    prev_pic_order_cnt_lsb = 0;
    prev_pic_order_cnt_msb = 0;
    have_prev_poc = false;
  }

  void storeNal(std::span<const uint8_t> nal_data) {
    if (nal_data.empty()) return;
    h264::Bitstream bs;
    bs.init(nal_data.data(), nal_data.size());
    h264::NALHeader nal;
    if (!h264::read_nal_header(&nal, &bs)) return;
    if (nal.type == h264::NAL_UNIT_TYPE_SPS) {
      h264::SPS parsed = {};
      h264::read_sps(&parsed, &bs);
      if (parsed.seq_parameter_set_id >= 0 && parsed.seq_parameter_set_id < 32) {
        sps[parsed.seq_parameter_set_id] = parsed;
        sps_valid[parsed.seq_parameter_set_id] = true;
        has_sps = true;
      }
    } else if (nal.type == h264::NAL_UNIT_TYPE_PPS) {
      h264::PPS parsed = {};
      h264::read_pps(&parsed, &bs);
      if (parsed.pic_parameter_set_id >= 0 && parsed.pic_parameter_set_id < 256) {
        pps[parsed.pic_parameter_set_id] = parsed;
        pps_valid[parsed.pic_parameter_set_id] = true;
        has_pps = true;
      }
    }
  }

  void parseExtradata(std::span<const uint8_t> extradata) {
    if (extradata.empty()) return;
    if (extradata.size() >= 7 && extradata[0] == 1) {
      nal_length_size = static_cast<uint8_t>((extradata[4] & 0x03u) + 1);
      size_t offset = 5;
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

  auto computePoc(const h264::SliceHeader& slice) -> int32_t {
    if (slice.pic_parameter_set_id < 0 || slice.pic_parameter_set_id >= 256 || !pps_valid[slice.pic_parameter_set_id]) {
      return slice.pic_order_cnt_lsb;
    }
    const auto& p = pps[slice.pic_parameter_set_id];
    if (p.seq_parameter_set_id < 0 || p.seq_parameter_set_id >= 32 || !sps_valid[p.seq_parameter_set_id]) {
      return slice.pic_order_cnt_lsb;
    }
    const auto& s = sps[p.seq_parameter_set_id];
    if (s.pic_order_cnt_type != 0) return slice.pic_order_cnt_lsb;
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
    prev_pic_order_cnt_lsb = slice.pic_order_cnt_lsb;
    prev_pic_order_cnt_msb = pic_order_cnt_msb;
    have_prev_poc = true;
    return pic_order_cnt_msb + slice.pic_order_cnt_lsb;
  }

  auto parseFrame(std::span<const uint8_t> packet) -> ParsedFrame {
    ParsedFrame frame;
    frame.bitstream = packet;

    h264::Bitstream bs;
    bs.init(frame.bitstream.data(), frame.bitstream.size());
    while (h264::find_next_nal(&bs)) {
      const uint32_t nal_offset = static_cast<uint32_t>(bs.byte_offset()) - 3;
      h264::NALHeader nal;
      if (!h264::read_nal_header(&nal, &bs)) continue;
      if (nal.type == h264::NAL_UNIT_TYPE_SPS || nal.type == h264::NAL_UNIT_TYPE_PPS) {
        const size_t start = static_cast<size_t>(bs.byte_offset()) - 1;
        storeNal(frame.bitstream.subspan(start));
      } else if (nal.type == h264::NAL_UNIT_TYPE_CODED_SLICE_IDR || nal.type == h264::NAL_UNIT_TYPE_CODED_SLICE_NON_IDR) {
        if (!has_sps || !has_pps) continue;
        h264::read_slice_header(&frame.slice, &nal, pps, sps, &bs);
        frame.slice_offsets.push_back(nal_offset);
        frame.nal = nal;
      }
    }

    frame.is_intra = frame.nal.type == h264::NAL_UNIT_TYPE_CODED_SLICE_IDR;
    frame.is_reference = frame.nal.idc != h264::NAL_REF_IDC_PRIORITY_DISPOSABLE;
    frame.poc = computePoc(frame.slice);
    return frame;
  }
};

struct DpbEntry {
  int32_t poc = 0;
  uint32_t frame_num = 0;
  bool is_reference = false;
};

#ifdef _WIN32
static void fillQMatrix(const h264::SPS& sps, const h264::PPS& pps, DXVA_Qmatrix_H264& qmatrix) {
  if (!sps.seq_scaling_matrix_present_flag && !pps.pic_scaling_matrix_present_flag) {
    std::memset(&qmatrix, 16, sizeof(qmatrix));
    return;
  }
  static constexpr int z4[] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
  static constexpr int z8[] = {
      0, 1, 8,16, 9, 2, 3,10, 17,24,32,25,18,11, 4, 5,
      12,19,26,33,40,48,41,34, 27,20,13, 6, 7,14,21,28,
      35,42,49,56,57,50,43,36, 29,22,15,23,30,37,44,51,
      58,59,52,45,38,31,39,46, 53,60,61,54,47,55,62,63};
  for (int i = 0; i < 6; ++i) for (int j = 0; j < 16; ++j) qmatrix.bScalingLists4x4[i][j] = (UCHAR)pps.ScalingList4x4[i][z4[j]];
  for (int i = 0; i < 2; ++i) for (int j = 0; j < 64; ++j) qmatrix.bScalingLists8x8[i][j] = (UCHAR)pps.ScalingList8x8[i][z8[j]];
}

static void fillPicParams(const h264::SPS& sps,
                          const h264::PPS& pps,
                          const h264::SliceHeader& slice,
                          const ParsedFrame& frame,
                          uint32_t current_slot,
                          const std::vector<uint8_t>& reference_usage,
                          const std::vector<DpbEntry>& dpb,
                          uint32_t feedback,
                          DXVA_PicParams_H264& pic) {
  pic = {};
  pic.wFrameWidthInMbsMinus1 = (USHORT)sps.pic_width_in_mbs_minus1;
  pic.wFrameHeightInMbsMinus1 = (USHORT)sps.pic_height_in_map_units_minus1;
  pic.IntraPicFlag = frame.is_intra ? 1 : 0;
  pic.MbaffFrameFlag = 0;
  pic.field_pic_flag = 0;
  pic.chroma_format_idc = 1;
  pic.bit_depth_chroma_minus8 = (UCHAR)sps.bit_depth_chroma_minus8;
  pic.bit_depth_luma_minus8 = (UCHAR)sps.bit_depth_luma_minus8;
  pic.residual_colour_transform_flag = (UCHAR)sps.separate_colour_plane_flag;
  pic.CurrPic.AssociatedFlag = 0;
  pic.CurrPic.Index7Bits = (UCHAR)current_slot;
  pic.CurrFieldOrderCnt[0] = frame.poc;
  pic.CurrFieldOrderCnt[1] = frame.poc;
  for (uint32_t i = 0; i < 16; ++i) {
    pic.RefFrameList[i].bPicEntry = 0xff;
    pic.FieldOrderCntList[i][0] = 0;
    pic.FieldOrderCntList[i][1] = 0;
    pic.FrameNumList[i] = 0;
  }
  for (size_t i = 0; i < reference_usage.size() && i < 16; ++i) {
    const uint32_t ref_slot = reference_usage[i];
    if (ref_slot >= dpb.size() || ref_slot == current_slot || !dpb[ref_slot].is_reference) continue;
    pic.RefFrameList[i].AssociatedFlag = 0;
    pic.RefFrameList[i].Index7Bits = (UCHAR)ref_slot;
    pic.FieldOrderCntList[i][0] = dpb[ref_slot].poc;
    pic.FieldOrderCntList[i][1] = dpb[ref_slot].poc;
    pic.UsedForReferenceFlags |= 1 << (i * 2 + 0);
    pic.UsedForReferenceFlags |= 1 << (i * 2 + 1);
    pic.FrameNumList[i] = (USHORT)dpb[ref_slot].frame_num;
  }
  pic.weighted_pred_flag = (UCHAR)pps.weighted_pred_flag;
  pic.weighted_bipred_idc = (UCHAR)pps.weighted_bipred_idc;
  pic.transform_8x8_mode_flag = (UCHAR)pps.transform_8x8_mode_flag;
  pic.constrained_intra_pred_flag = (UCHAR)pps.constrained_intra_pred_flag;
  pic.num_ref_frames = (UCHAR)sps.num_ref_frames;
  pic.MbsConsecutiveFlag = 1;
  pic.frame_mbs_only_flag = (UCHAR)sps.frame_mbs_only_flag;
  pic.MinLumaBipredSize8x8Flag = sps.level_idc >= 31;
  pic.RefPicFlag = frame.is_reference ? 1 : 0;
  pic.frame_num = (USHORT)slice.frame_num;
  pic.pic_init_qp_minus26 = (CHAR)pps.pic_init_qp_minus26;
  pic.pic_init_qs_minus26 = (CHAR)pps.pic_init_qs_minus26;
  pic.chroma_qp_index_offset = (CHAR)pps.chroma_qp_index_offset;
  pic.second_chroma_qp_index_offset = (CHAR)pps.second_chroma_qp_index_offset;
  pic.log2_max_frame_num_minus4 = (UCHAR)sps.log2_max_frame_num_minus4;
  pic.pic_order_cnt_type = (UCHAR)sps.pic_order_cnt_type;
  pic.log2_max_pic_order_cnt_lsb_minus4 = (UCHAR)sps.log2_max_pic_order_cnt_lsb_minus4;
  pic.delta_pic_order_always_zero_flag = (UCHAR)sps.delta_pic_order_always_zero_flag;
  pic.direct_8x8_inference_flag = (UCHAR)sps.direct_8x8_inference_flag;
  pic.entropy_coding_mode_flag = (UCHAR)pps.entropy_coding_mode_flag;
  pic.pic_order_present_flag = (UCHAR)pps.pic_order_present_flag;
  pic.num_slice_groups_minus1 = (UCHAR)pps.num_slice_groups_minus1;
  pic.slice_group_map_type = (UCHAR)pps.slice_group_map_type;
  pic.deblocking_filter_control_present_flag = (UCHAR)pps.deblocking_filter_control_present_flag;
  pic.redundant_pic_cnt_present_flag = (UCHAR)pps.redundant_pic_cnt_present_flag;
  pic.slice_group_change_rate_minus1 = (USHORT)pps.slice_group_change_rate_minus1;
  pic.Reserved16Bits = 3;
  pic.StatusReportFeedbackNumber = feedback == 0 ? 1 : feedback;
  pic.ContinuationFlag = 1;
  pic.num_ref_idx_l0_active_minus1 = (UCHAR)pps.num_ref_idx_l0_active_minus1;
  pic.num_ref_idx_l1_active_minus1 = (UCHAR)pps.num_ref_idx_l1_active_minus1;
}
#endif

} // namespace openmedia::dx_h264
