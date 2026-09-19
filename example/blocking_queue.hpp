#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>

// Bounded thread-safe queue. Blocking calls give up as soon as their stop
// token is triggered, so shutting a pipeline down is just request_stop().
//
// Besides the item count, items can be weighed against a cost budget (bytes,
// for packets). An empty queue accepts anything, so one oversized item can
// never wedge its producer.
template <typename T>
class BlockingQueue {
public:
  using CostFn = size_t (*)(const T&);

  explicit BlockingQueue(size_t max_items, size_t max_cost = SIZE_MAX,
                         CostFn cost = [](const T&) -> size_t { return 0; })
      : max_items_(max_items), max_cost_(max_cost), cost_fn_(cost) {}

  auto push(T item, std::stop_token stop) -> bool {
    const size_t cost = cost_fn_(item);
    std::unique_lock lock(mutex_);
    const bool has_room = not_full_.wait(lock, stop, [&] {
      return items_.empty() || (items_.size() < max_items_ && cost_ + cost <= max_cost_);
    });
    if (!has_room) return false;
    cost_ += cost;
    items_.push_back(std::move(item));
    not_empty_.notify_one();
    return true;
  }

  auto pop(std::stop_token stop) -> std::optional<T> {
    std::unique_lock lock(mutex_);
    if (!not_empty_.wait(lock, stop, [&] { return !items_.empty(); })) return std::nullopt;
    return takeFront();
  }

  // Non-blocking: pops the front item only if `accept` agrees.
  template <typename Pred>
  auto popIf(Pred&& accept) -> std::optional<T> {
    std::lock_guard lock(mutex_);
    if (items_.empty() || !accept(items_.front())) return std::nullopt;
    return takeFront();
  }

  void clear() {
    std::lock_guard lock(mutex_);
    items_.clear();
    cost_ = 0;
    not_full_.notify_all();
  }

  auto empty() const -> bool {
    std::lock_guard lock(mutex_);
    return items_.empty();
  }

private:
  auto takeFront() -> T {
    T item = std::move(items_.front());
    items_.pop_front();
    cost_ -= cost_fn_(item);
    not_full_.notify_one();
    return item;
  }

  std::deque<T> items_;
  mutable std::mutex mutex_;
  std::condition_variable_any not_full_;
  std::condition_variable_any not_empty_;
  size_t max_items_;
  size_t max_cost_;
  size_t cost_ = 0;
  CostFn cost_fn_;
};
