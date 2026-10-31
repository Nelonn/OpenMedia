#pragma once

#include <cstddef>
#include <iterator>
#include <utility>
#include <vector>

namespace openmedia {

// For what a demuxer has parsed but not handed out yet. MSVC's std::deque gives anything
// over 16 bytes a heap block of its own, which is one allocation per packet.
template<typename T>
class Fifo {
  // Compacting no more often than this keeps pop_front() amortised O(1).
  static constexpr size_t MIN_COMPACT = 64;

  std::vector<T> items_;
  size_t head_ = 0;

public:
  auto empty() const noexcept -> bool { return head_ == items_.size(); }
  auto size() const noexcept -> size_t { return items_.size() - head_; }

  auto front() noexcept -> T& { return items_[head_]; }
  auto front() const noexcept -> const T& { return items_[head_]; }
  auto back() noexcept -> T& { return items_.back(); }
  auto back() const noexcept -> const T& { return items_.back(); }
  auto operator[](size_t index) noexcept -> T& { return items_[head_ + index]; }

  auto push_back(T item) -> T& { return items_.emplace_back(std::move(item)); }

  template<typename Iterator>
  void append(Iterator first, Iterator last) {
    items_.insert(items_.end(), std::make_move_iterator(first), std::make_move_iterator(last));
  }

  void pop_front() {
    items_[head_++] = T {}; // releases whatever the slot still owns
    if (head_ == items_.size()) {
      clear();
    } else if (head_ >= MIN_COMPACT && head_ * 2 >= items_.size()) {
      items_.erase(items_.begin(), items_.begin() + static_cast<std::ptrdiff_t>(head_));
      head_ = 0;
    }
  }

  void clear() noexcept {
    items_.clear();
    head_ = 0;
  }
};

} // namespace openmedia
