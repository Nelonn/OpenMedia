#pragma once

#include <video/parser/h264_types.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// The parts of H.264 decoding that a hardware accelerator which takes
// ready-made reference lists (VA-API) leaves to the caller: picture order
// counts (8.2.1), reference list construction and modification (8.2.4),
// reference picture marking (8.2.5) and frame_num gaps (8.2.5.2).
//
// DXVA builds the lists inside the driver, which is why dx_h264::Dpb gets by
// without most of this. Frame pictures only -- progressive and MBAFF; field
// pictures are turned away before they get here.
namespace openmedia::h264_dpb {

struct PictureOrder {
  int32_t top = 0;
  int32_t bottom = 0;

  auto poc() const -> int32_t { return std::min(top, bottom); }
};

// 8.2.1. The state the derivation carries from one picture to the next.
class PocState {
public:
  void reset() { *this = {}; }

  // `frame_num` and the delta fields come from the slice header; for a
  // non-existing frame (8.2.5.2) the caller passes a header with those set and
  // everything else zero.
  auto compute(const h264::SPS& sps, const h264::SliceHeader& slice, bool is_idr, bool is_reference) const
      -> PictureOrder {
    PictureOrder order;
    const int max_frame_num = 1 << (sps.log2_max_frame_num_minus4 + 4);

    if (sps.pic_order_cnt_type == 0) {
      int prev_msb = 0;
      int prev_lsb = 0;
      if (!is_idr) {
        prev_msb = prev_ref_msb_;
        prev_lsb = prev_ref_lsb_;
      }
      const int max_lsb = 1 << (sps.log2_max_pic_order_cnt_lsb_minus4 + 4);
      const int lsb = slice.pic_order_cnt_lsb;
      int msb = prev_msb;
      if (lsb < prev_lsb && (prev_lsb - lsb) >= max_lsb / 2) {
        msb = prev_msb + max_lsb;
      } else if (lsb > prev_lsb && (lsb - prev_lsb) > max_lsb / 2) {
        msb = prev_msb - max_lsb;
      }
      order.top = msb + lsb;
      order.bottom = order.top + slice.delta_pic_order_cnt_bottom;
      pending_msb_ = msb;
      return order;
    }

    int frame_num_offset = 0;
    if (!is_idr) {
      frame_num_offset = prev_frame_num_offset_;
      if (prev_frame_num_ > slice.frame_num) frame_num_offset += max_frame_num;
    }
    pending_frame_num_offset_ = frame_num_offset;

    if (sps.pic_order_cnt_type == 1) {
      const int cycle = sps.num_ref_frames_in_pic_order_cnt_cycle;
      int abs_frame_num = cycle != 0 ? frame_num_offset + slice.frame_num : 0;
      if (!is_reference && abs_frame_num > 0) --abs_frame_num;

      int expected = 0;
      if (abs_frame_num > 0) {
        int expected_delta_per_cycle = 0;
        for (int i = 0; i < cycle; ++i) expected_delta_per_cycle += sps.offset_for_ref_frame[i];
        const int cycle_count = (abs_frame_num - 1) / cycle;
        const int frame_in_cycle = (abs_frame_num - 1) % cycle;
        expected = cycle_count * expected_delta_per_cycle;
        for (int i = 0; i <= frame_in_cycle; ++i) expected += sps.offset_for_ref_frame[i];
      }
      if (!is_reference) expected += sps.offset_for_non_ref_pic;

      order.top = expected + slice.delta_pic_order_cnt[0];
      order.bottom = order.top + sps.offset_for_top_to_bottom_field + slice.delta_pic_order_cnt[1];
      return order;
    }

    // Type 2: output order is decoding order.
    int temp = 0;
    if (!is_idr) temp = is_reference ? 2 * (frame_num_offset + slice.frame_num)
                                     : 2 * (frame_num_offset + slice.frame_num) - 1;
    order.top = temp;
    order.bottom = temp;
    return order;
  }

  // Called once the picture has been decoded and marked. `order` is what
  // compute() returned; a picture with memory_management_control_operation 5
  // has its counts rebased so that the smaller one is 0 (8.2.1), and it is the
  // rebased values the next picture derives from.
  void commit(const h264::SPS& sps, const h264::SliceHeader& slice, bool is_reference,
              PictureOrder& order) {
    if (slice.mmco5) {
      const int32_t temp = order.poc();
      order.top -= temp;
      order.bottom -= temp;
    }

    if (sps.pic_order_cnt_type == 0) {
      if (!is_reference) return;
      if (slice.mmco5) {
        prev_ref_msb_ = 0;
        prev_ref_lsb_ = order.top;
      } else {
        prev_ref_msb_ = pending_msb_;
        prev_ref_lsb_ = slice.pic_order_cnt_lsb;
      }
      return;
    }

    prev_frame_num_ = slice.mmco5 ? 0 : slice.frame_num;
    prev_frame_num_offset_ = slice.mmco5 ? 0 : pending_frame_num_offset_;
  }

private:
  int prev_ref_msb_ = 0;
  int prev_ref_lsb_ = 0;
  int prev_frame_num_ = 0;
  int prev_frame_num_offset_ = 0;
  // compute() is const so the decoder can ask before it commits to decoding;
  // these carry its intermediate results over to commit().
  mutable int pending_msb_ = 0;
  mutable int pending_frame_num_offset_ = 0;
};

struct RefPicture {
  // Whatever the decoder identifies its surfaces by; -1 for a frame the
  // gap process inferred and that therefore has no surface.
  int surface = -1;
  int32_t top_poc = 0;
  int32_t bottom_poc = 0;
  int frame_num = 0;
  // FrameNumWrap (8.2.4.1), refreshed for every picture: short-term pictures
  // are addressed relative to the picture being decoded.
  int frame_num_wrap = 0;
  bool long_term = false;
  int long_term_frame_idx = 0;

  auto poc() const -> int32_t { return std::min(top_poc, bottom_poc); }
  auto nonExisting() const -> bool { return surface < 0; }
};

// A reference picture list: indices into Dpb::pictures(), -1 where the list
// names no picture ("no reference picture", 8.2.4.2).
using RefList = std::vector<int>;

class Dpb {
public:
  void reset() {
    pictures_.clear();
    max_long_term_frame_idx_ = -1;
    prev_ref_frame_num_ = 0;
    have_prev_ref_frame_num_ = false;
  }

  auto pictures() const -> const std::vector<RefPicture>& { return pictures_; }

  auto holds(int surface) const -> bool {
    return std::any_of(pictures_.begin(), pictures_.end(),
                       [surface](const RefPicture& p) { return p.surface == surface; });
  }

  // 8.2.4.1 for a frame picture: CurrPicNum is frame_num.
  void updateFrameNumWrap(int frame_num, int max_frame_num) {
    for (auto& pic : pictures_) {
      if (pic.long_term) continue;
      pic.frame_num_wrap = pic.frame_num > frame_num ? pic.frame_num - max_frame_num : pic.frame_num;
    }
  }

  // 8.2.5.2. Called before a non-IDR picture is decoded. Inferred frames take
  // part in the sliding window and in list initialisation like any other, but
  // a conforming stream never predicts from one, so they have no surface.
  // `infer_order` computes the counts an inferred frame would have had, which
  // is what places it among the others in a B list.
  template<typename InferOrder>
  void fillFrameNumGap(const h264::SPS& sps, int frame_num, InferOrder&& infer_order) {
    if (!have_prev_ref_frame_num_) return;
    const int max_frame_num = 1 << (sps.log2_max_frame_num_minus4 + 4);
    if (frame_num == prev_ref_frame_num_ || frame_num == (prev_ref_frame_num_ + 1) % max_frame_num) return;

    // Only the last max_num_ref_frames inferred frames can still be in the
    // buffer once the gap has been filled; the ones before them would be slid
    // straight back out. Skipping them also bounds the work a corrupt or
    // seek-damaged frame_num can cause.
    const int max_refs = std::max(sps.num_ref_frames, 1);
    int missing = (frame_num - prev_ref_frame_num_ - 1 + max_frame_num) % max_frame_num;
    int unused = (prev_ref_frame_num_ + 1) % max_frame_num;
    if (missing > max_refs) {
      unused = (unused + (missing - max_refs)) % max_frame_num;
      missing = max_refs;
    }

    for (int i = 0; i < missing; ++i) {
      updateFrameNumWrap(unused, max_frame_num);
      slidingWindow(max_refs);
      RefPicture pic;
      pic.frame_num = unused;
      pic.frame_num_wrap = unused;
      const PictureOrder order = infer_order(unused);
      pic.top_poc = order.top;
      pic.bottom_poc = order.bottom;
      pictures_.push_back(pic);
      prev_ref_frame_num_ = unused;
      unused = (unused + 1) % max_frame_num;
    }
  }

  // 8.2.4.2.1: short-term by descending PicNum, then long-term by ascending
  // LongTermPicNum.
  auto initialListP() const -> RefList {
    RefList list = shortTermIndices();
    std::sort(list.begin(), list.end(), [&](int a, int b) {
      return pictures_[a].frame_num_wrap > pictures_[b].frame_num_wrap;
    });
    appendLongTerm(list);
    return list;
  }

  // 8.2.4.2.3.
  void initialListsB(int32_t curr_poc, RefList& list0, RefList& list1) const {
    RefList before;
    RefList after;
    for (int index : shortTermIndices()) {
      (pictures_[index].poc() < curr_poc ? before : after).push_back(index);
    }
    const auto by_poc_desc = [&](int a, int b) { return pictures_[a].poc() > pictures_[b].poc(); };
    const auto by_poc_asc = [&](int a, int b) { return pictures_[a].poc() < pictures_[b].poc(); };
    std::sort(before.begin(), before.end(), by_poc_desc);
    std::sort(after.begin(), after.end(), by_poc_asc);

    list0 = before;
    list0.insert(list0.end(), after.begin(), after.end());
    appendLongTerm(list0);

    list1 = after;
    list1.insert(list1.end(), before.begin(), before.end());
    appendLongTerm(list1);

    if (list1.size() > 1 && list0 == list1) std::swap(list1[0], list1[1]);
  }

  // 8.2.4.2 (truncation / padding to num_ref_idx_lX_active_minus1 + 1) and
  // 8.2.4.3 (modification). Returns false when a modification names a picture
  // the buffer does not hold -- a broken stream, or one entered mid-GOP.
  auto finishList(RefList& list, const h264::SliceHeader& slice, int which, int max_frame_num) const -> bool {
    const int active = which == 0 ? slice.num_ref_idx_l0_active_minus1 + 1 : slice.num_ref_idx_l1_active_minus1 + 1;
    list.resize(static_cast<size_t>(active), -1);
    if (!slice.ref_pic_list_modification_flag[which]) return true;

    const int max_pic_num = max_frame_num; // frames: MaxPicNum = MaxFrameNum
    const int curr_pic_num = slice.frame_num;
    int pic_num_pred = curr_pic_num;
    int ref_idx = 0;
    bool ok = true;

    // NOTE 2 in 8.2.4.3.2: the list is one entry longer while it is modified.
    list.push_back(-1);
    for (int m = 0; m < slice.num_ref_pic_list_modifications[which]; ++m) {
      const auto& mod = slice.ref_pic_list_modifications[which][m];
      if (ref_idx >= active) break;

      int target = -1;
      if (mod.modification_of_pic_nums_idc == 0 || mod.modification_of_pic_nums_idc == 1) {
        const int abs_diff = mod.abs_diff_pic_num_minus1 + 1;
        int no_wrap = 0;
        if (mod.modification_of_pic_nums_idc == 0) {
          no_wrap = pic_num_pred - abs_diff;
          if (no_wrap < 0) no_wrap += max_pic_num;
        } else {
          no_wrap = pic_num_pred + abs_diff;
          if (no_wrap >= max_pic_num) no_wrap -= max_pic_num;
        }
        pic_num_pred = no_wrap;
        const int pic_num = no_wrap > curr_pic_num ? no_wrap - max_pic_num : no_wrap;
        target = findShortTerm(pic_num);
        if (target < 0) ok = false;
        insertAt(list, ref_idx, active, target, [&](int index) {
          return index >= 0 && !pictures_[index].long_term && pictures_[index].frame_num_wrap == pic_num;
        });
      } else if (mod.modification_of_pic_nums_idc == 2) {
        const int long_term_pic_num = mod.long_term_pic_num;
        target = findLongTerm(long_term_pic_num);
        if (target < 0) ok = false;
        insertAt(list, ref_idx, active, target, [&](int index) {
          return index >= 0 && pictures_[index].long_term &&
                 pictures_[index].long_term_frame_idx == long_term_pic_num;
        });
      } else {
        break;
      }
      ++ref_idx;
    }
    list.resize(static_cast<size_t>(active));
    return ok;
  }

  // 8.2.5.1, for the picture just decoded into `surface`. Pictures that are
  // not references never enter the buffer: output is someone else's job.
  void markCurrent(const h264::SPS& sps, const h264::SliceHeader& slice, bool is_idr, bool is_reference,
                   int surface, const PictureOrder& order) {
    RefPicture current;
    current.surface = surface;
    current.top_poc = order.top;
    current.bottom_poc = order.bottom;
    current.frame_num = slice.mmco5 ? 0 : slice.frame_num;
    current.frame_num_wrap = current.frame_num;

    if (!is_reference) return;
    const int max_refs = std::max(sps.num_ref_frames, 1);

    if (is_idr) {
      pictures_.clear();
      if (slice.long_term_reference_flag) {
        current.long_term = true;
        current.long_term_frame_idx = 0;
        max_long_term_frame_idx_ = 0;
      } else {
        max_long_term_frame_idx_ = -1;
      }
      pictures_.push_back(current);
      prev_ref_frame_num_ = 0;
      have_prev_ref_frame_num_ = true;
      return;
    }

    bool current_long_term = false;
    if (slice.adaptive_ref_pic_marking_mode_flag) {
      current_long_term = applyMarkings(slice, current);
    } else {
      slidingWindow(max_refs);
    }
    if (!current_long_term) pictures_.push_back(current);

    // A stream that keeps more references than it declared would otherwise
    // grow the buffer without bound; drop the oldest short-term ones as the
    // sliding window would.
    while (static_cast<int>(pictures_.size()) > max_refs) {
      if (!removeOldestShortTerm()) break;
    }

    prev_ref_frame_num_ = current.frame_num;
    have_prev_ref_frame_num_ = true;
  }

private:
  auto shortTermIndices() const -> RefList {
    RefList list;
    for (int i = 0; i < static_cast<int>(pictures_.size()); ++i) {
      if (!pictures_[i].long_term) list.push_back(i);
    }
    return list;
  }

  // LongTermPicNum is LongTermFrameIdx for frames.
  void appendLongTerm(RefList& list) const {
    const size_t first = list.size();
    for (int i = 0; i < static_cast<int>(pictures_.size()); ++i) {
      if (pictures_[i].long_term) list.push_back(i);
    }
    std::sort(list.begin() + static_cast<ptrdiff_t>(first), list.end(), [&](int a, int b) {
      return pictures_[a].long_term_frame_idx < pictures_[b].long_term_frame_idx;
    });
  }

  auto findShortTerm(int pic_num) const -> int {
    for (int i = 0; i < static_cast<int>(pictures_.size()); ++i) {
      if (!pictures_[i].long_term && pictures_[i].frame_num_wrap == pic_num) return i;
    }
    return -1;
  }

  auto findLongTerm(int long_term_pic_num) const -> int {
    for (int i = 0; i < static_cast<int>(pictures_.size()); ++i) {
      if (pictures_[i].long_term && pictures_[i].long_term_frame_idx == long_term_pic_num) return i;
    }
    return -1;
  }

  // 8.2.4.3.1 / 8.2.4.3.2: shift the tail right, put `target` at `ref_idx`,
  // then drop the later duplicate of it.
  template<typename Matches>
  static void insertAt(RefList& list, int ref_idx, int active, int target, Matches&& matches) {
    for (int c = active; c > ref_idx; --c) list[c] = list[c - 1];
    list[ref_idx] = target;
    int n = ref_idx + 1;
    for (int c = ref_idx + 1; c <= active; ++c) {
      if (!matches(list[c])) list[n++] = list[c];
    }
    for (; n <= active; ++n) list[n] = -1;
  }

  auto removeOldestShortTerm() -> bool {
    int oldest = -1;
    for (int i = 0; i < static_cast<int>(pictures_.size()); ++i) {
      if (pictures_[i].long_term) continue;
      if (oldest < 0 || pictures_[i].frame_num_wrap < pictures_[oldest].frame_num_wrap) oldest = i;
    }
    if (oldest < 0) return false;
    pictures_.erase(pictures_.begin() + oldest);
    return true;
  }

  // 8.2.5.3.
  void slidingWindow(int max_refs) {
    if (static_cast<int>(pictures_.size()) >= max_refs) removeOldestShortTerm();
  }

  void dropLongTermIdx(int long_term_frame_idx) {
    std::erase_if(pictures_, [&](const RefPicture& p) {
      return p.long_term && p.long_term_frame_idx == long_term_frame_idx;
    });
  }

  // 8.2.5.4. Returns true when operation 6 made the current picture long-term,
  // in which case it has already been added.
  auto applyMarkings(const h264::SliceHeader& slice, RefPicture& current) -> bool {
    const int curr_pic_num = slice.frame_num;
    bool current_long_term = false;

    for (int i = 0; i < slice.num_ref_pic_markings; ++i) {
      const auto& marking = slice.ref_pic_markings[i];
      switch (marking.operation) {
        case 1: {
          const int pic_num = curr_pic_num - (marking.difference_of_pic_nums_minus1 + 1);
          std::erase_if(pictures_, [&](const RefPicture& p) { return !p.long_term && p.frame_num_wrap == pic_num; });
          break;
        }
        case 2:
          std::erase_if(pictures_, [&](const RefPicture& p) {
            return p.long_term && p.long_term_frame_idx == marking.long_term_pic_num;
          });
          break;
        case 3: {
          const int pic_num = curr_pic_num - (marking.difference_of_pic_nums_minus1 + 1);
          const int index = findShortTerm(pic_num);
          if (index < 0) break;
          const int surface = pictures_[index].surface;
          const int frame_num = pictures_[index].frame_num;
          // Freeing the index first may move the picture being converted.
          dropLongTermIdx(marking.long_term_frame_idx);
          for (auto& p : pictures_) {
            if (!p.long_term && p.surface == surface && p.frame_num == frame_num) {
              p.long_term = true;
              p.long_term_frame_idx = marking.long_term_frame_idx;
              break;
            }
          }
          break;
        }
        case 4:
          max_long_term_frame_idx_ = marking.max_long_term_frame_idx_plus1 - 1;
          std::erase_if(pictures_, [&](const RefPicture& p) {
            return p.long_term && p.long_term_frame_idx > max_long_term_frame_idx_;
          });
          break;
        case 5:
          pictures_.clear();
          max_long_term_frame_idx_ = -1;
          break;
        case 6:
          dropLongTermIdx(marking.long_term_frame_idx);
          current.long_term = true;
          current.long_term_frame_idx = marking.long_term_frame_idx;
          pictures_.push_back(current);
          current_long_term = true;
          break;
        default:
          break;
      }
    }
    return current_long_term;
  }

  std::vector<RefPicture> pictures_;
  int max_long_term_frame_idx_ = -1; // -1: "no long-term frame indices"
  int prev_ref_frame_num_ = 0;
  bool have_prev_ref_frame_num_ = false;
};

} // namespace openmedia::h264_dpb
