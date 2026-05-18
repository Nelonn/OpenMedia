#pragma once

#include <bitstream_filter.hpp>

#include <optional>

namespace openmedia {

class AnnexBBitStreamFilter : public BitStreamFilter {
public:
  static constexpr uint8_t START_CODE_LONG[4] = {0x00, 0x00, 0x00, 0x01};
  static constexpr uint8_t START_CODE_SHORT[3] = {0x00, 0x00, 0x01};

  explicit AnnexBBitStreamFilter(uint8_t nal_length_size = 0,
                                 uint8_t fallback_nal_length_size = 0,
                                 uint8_t start_code_length = 4)
      : nal_length_size_(validLengthSize(nal_length_size)), fallback_nal_length_size_(validLengthSize(fallback_nal_length_size)), start_code_length_(start_code_length == 3 ? 3 : 4) {}

  void setNalLengthSize(uint8_t nal_length_size) noexcept {
    nal_length_size_ = validLengthSize(nal_length_size);
  }

  auto nalLengthSize() const noexcept -> uint8_t { return nal_length_size_; }

  void setFallbackNalLengthSize(uint8_t nal_length_size) noexcept {
    fallback_nal_length_size_ = validLengthSize(nal_length_size);
  }

  void parseAvcCExtradata(std::span<const uint8_t> extradata) noexcept {
    if (extradata.size() >= 7 && extradata[0] == 1) {
      nal_length_size_ = static_cast<uint8_t>((extradata[4] & 0x03u) + 1);
    }
  }

  auto filter(BitStreamFilterInput input) const -> FilteredBitstream override {
    if (input.bytes.empty() || isAnnexB(input.bytes)) {
      if (input.ownsBytes()) return makeFilteredBitstream(std::move(input.storage), input.bytes.size(), false);
      return makeFilteredBitstream(input.bytes);
    }

    auto mutable_bytes = input.mutableBytes();
    if (!mutable_bytes.empty()) {
      if (auto size = rewriteLengthPrefixedInPlace(mutable_bytes, nal_length_size_, start_code_length_)) {
        return makeFilteredBitstream(std::move(input.storage), *size, true);
      }
      if (fallback_nal_length_size_ != nal_length_size_) {
        if (auto size = rewriteLengthPrefixedInPlace(mutable_bytes, fallback_nal_length_size_, start_code_length_)) {
          return makeFilteredBitstream(std::move(input.storage), *size, true);
        }
      }
    }

    auto converted = convertLengthPrefixed(input.bytes, nal_length_size_, start_code_length_);
    if (converted.empty() && fallback_nal_length_size_ != nal_length_size_) {
      converted = convertLengthPrefixed(input.bytes, fallback_nal_length_size_, start_code_length_);
    }

    if (converted.empty()) {
      if (input.ownsBytes()) return makeFilteredBitstream(std::move(input.storage), input.bytes.size(), false);
      return makeFilteredBitstream(input.bytes);
    }
    const size_t converted_size = converted.size();
    return makeFilteredBitstream(std::move(converted), converted_size, false, true);
  }

  static auto isAnnexB(std::span<const uint8_t> data) noexcept -> bool {
    return data.size() >= 3 && data[0] == 0x00 && data[1] == 0x00 &&
           (data[2] == 0x01 || (data.size() >= 4 && data[2] == 0x00 && data[3] == 0x01));
  }

private:
  uint8_t nal_length_size_ = 0;
  uint8_t fallback_nal_length_size_ = 0;
  uint8_t start_code_length_ = 4;

  static auto validLengthSize(uint8_t nal_length_size) noexcept -> uint8_t {
    return nal_length_size <= 4 ? nal_length_size : 0;
  }

  static auto readNalSize(std::span<const uint8_t> input, size_t offset, uint8_t nal_length_size) noexcept -> uint32_t {
    uint32_t nal_size = 0;
    for (uint8_t i = 0; i < nal_length_size; ++i) {
      nal_size = (nal_size << 8u) | input[offset + i];
    }
    return nal_size;
  }

  static auto rewriteLengthPrefixedInPlace(std::span<uint8_t> input,
                                           uint8_t nal_length_size,
                                           uint8_t start_code_length) -> std::optional<size_t> {
    if (nal_length_size == 0 || nal_length_size != start_code_length) return std::nullopt;

    size_t offset = 0;
    size_t output_size = 0;
    while (offset + nal_length_size <= input.size()) {
      const size_t prefix_offset = offset;
      const uint32_t nal_size = readNalSize(input, offset, nal_length_size);
      offset += nal_length_size;
      if (nal_size == 0 || offset + nal_size > input.size()) break;

      if (start_code_length == 4) input[prefix_offset] = 0x00;
      input[prefix_offset + start_code_length - 3] = 0x00;
      input[prefix_offset + start_code_length - 2] = 0x00;
      input[prefix_offset + start_code_length - 1] = 0x01;
      offset += nal_size;
      output_size = offset;
    }

    if (output_size == 0) return std::nullopt;
    return output_size;
  }

  static auto convertLengthPrefixed(std::span<const uint8_t> input,
                                    uint8_t nal_length_size,
                                    uint8_t start_code_length) -> std::vector<uint8_t> {
    std::vector<uint8_t> out;
    if (nal_length_size == 0) return out;

    out.reserve(input.size() + 16);
    size_t offset = 0;
    while (offset + nal_length_size <= input.size()) {
      const uint32_t nal_size = readNalSize(input, offset, nal_length_size);
      offset += nal_length_size;
      if (nal_size == 0 || offset + nal_size > input.size()) break;

      if (start_code_length == 4) out.push_back(0x00);
      out.push_back(0x00);
      out.push_back(0x00);
      out.push_back(0x01);
      out.insert(out.end(), input.begin() + offset, input.begin() + offset + nal_size);
      offset += nal_size;
    }
    return out;
  }
};

class AnnexBFilter final : public AnnexBBitStreamFilter {
private:
  std::vector<uint8_t> annexb_extra_;

public:
  AnnexBFilter(uint8_t nalu_len_sz, std::vector<uint8_t> annexb_extra)
      : AnnexBBitStreamFilter(nalu_len_sz, 0, 4), annexb_extra_(std::move(annexb_extra)) {}

  auto convert(std::span<const uint8_t> input, bool is_keyframe) const -> std::vector<uint8_t> override {
    auto filtered = filter(input);
    auto bytes = filtered.bytes;
    if (!is_keyframe || annexb_extra_.empty()) return {bytes.begin(), bytes.end()};

    std::vector<uint8_t> out;
    out.reserve(annexb_extra_.size() + bytes.size());
    out.insert(out.end(), annexb_extra_.begin(), annexb_extra_.end());
    out.insert(out.end(), bytes.begin(), bytes.end());
    return out;
  }
};

inline auto isAnnexBBitstream(std::span<const uint8_t> data) noexcept -> bool {
  return AnnexBBitStreamFilter::isAnnexB(data);
}

} // namespace openmedia
