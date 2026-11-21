#pragma once

#include <array>
#include <cstddef>
#include <memory>

namespace openmedia::video_parser {

// Parameter sets keyed by id. The id space is wide (256 H.264 PPS ids) but a
// stream uses one or two, and every set carries kilobytes of scaling lists: a
// dense array of them is most of a megabyte, which overflows a thread stack as
// soon as its owner is a local. Slots are allocated on first store and reused
// afterwards, so a stream that resends its headers on every keyframe does not
// allocate.
template <typename T, size_t N>
class ParameterSetTable {
public:
  static constexpr size_t capacity = N;

  ParameterSetTable() = default;
  ParameterSetTable(ParameterSetTable&&) noexcept = default;
  auto operator=(ParameterSetTable&&) noexcept -> ParameterSetTable& = default;
  ParameterSetTable(const ParameterSetTable& other) { *this = other; }
  auto operator=(const ParameterSetTable& other) -> ParameterSetTable& {
    if (this == &other) return *this;
    for (size_t i = 0; i < N; ++i) {
      if (other.slots_[i]) {
        store(static_cast<int>(i), *other.slots_[i]);
      } else {
        slots_[i].reset();
      }
    }
    count_ = other.count_;
    return *this;
  }

  // nullptr for an id out of range or never stored, so a caller cannot read a
  // set the stream never sent.
  auto find(int id) const noexcept -> const T* {
    return inRange(id) ? slots_[static_cast<size_t>(id)].get() : nullptr;
  }
  auto contains(int id) const noexcept -> bool { return find(id) != nullptr; }

  auto store(int id, const T& value) -> bool {
    if (!inRange(id)) return false;
    auto& slot = slots_[static_cast<size_t>(id)];
    if (slot) {
      *slot = value;
    } else {
      slot = std::make_unique<T>(value);
      ++count_;
    }
    return true;
  }

  void clear() noexcept {
    for (auto& slot : slots_) slot.reset();
    count_ = 0;
  }

  auto empty() const noexcept -> bool { return count_ == 0; }
  auto size() const noexcept -> size_t { return count_; }

  // The lowest-id set: what decoders configure from before a slice names one.
  auto first() const noexcept -> const T* {
    for (const auto& slot : slots_)
      if (slot) return slot.get();
    return nullptr;
  }

  // Calls fn(const T&) for every stored set, in id order.
  template <typename Fn>
  void forEach(Fn&& fn) const {
    for (const auto& slot : slots_)
      if (slot) fn(*slot);
  }

private:
  static constexpr auto inRange(int id) noexcept -> bool { return id >= 0 && static_cast<size_t>(id) < N; }

  std::array<std::unique_ptr<T>, N> slots_ = {};
  size_t count_ = 0;
};

} // namespace openmedia::video_parser
