#pragma once

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace openmedia {

struct BitStreamFilterInput {
  std::span<const uint8_t> bytes;
  std::vector<uint8_t> storage; // optional

  BitStreamFilterInput() = default;

  BitStreamFilterInput(std::span<const uint8_t> input) noexcept
      : bytes(input) {}

  BitStreamFilterInput(const std::vector<uint8_t>& input) noexcept
      : bytes(input.data(), input.size()) {}

  BitStreamFilterInput(std::vector<uint8_t>&& input) noexcept
      : storage(std::move(input)) {
    bytes = {storage.data(), storage.size()};
  }

  auto mutableBytes() noexcept -> std::span<uint8_t> {
    if (storage.empty()) return {};
    return {storage.data(), storage.size()};
  }

  auto ownsBytes() const noexcept -> bool {
    return bytes.data() == storage.data() && bytes.size() <= storage.size();
  }
};

struct FilteredBitstream {
  std::span<const uint8_t> bytes;
  std::vector<uint8_t> storage;
  bool in_place = false;
  bool copied_storage = false;

  auto converted() const noexcept -> bool { return in_place || copied_storage; }
  auto copied() const noexcept -> bool { return copied_storage; }
};

class BitStreamFilter {
public:
  virtual ~BitStreamFilter() = default;

  virtual auto filter(BitStreamFilterInput input) const -> FilteredBitstream = 0;

  virtual auto convert(std::span<const uint8_t> input, bool /*is_keyframe*/) const -> std::vector<uint8_t> {
    auto filtered = filter(input);
    return {filtered.bytes.begin(), filtered.bytes.end()};
  }
};

inline auto makeFilteredBitstream(std::span<const uint8_t> bytes) -> FilteredBitstream {
  return {bytes, {}, false};
}

inline auto makeFilteredBitstream(std::vector<uint8_t>&& storage,
                                  size_t size,
                                  bool in_place,
                                  bool copied_storage = false) -> FilteredBitstream {
  FilteredBitstream out;
  out.storage = std::move(storage);
  const size_t bytes_size = size < out.storage.size() ? size : out.storage.size();
  out.bytes = {out.storage.data(), bytes_size};
  out.in_place = in_place;
  out.copied_storage = copied_storage;
  return out;
}

} // namespace openmedia
