#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace openmedia {

class BitStreamFilter {
public:
  virtual ~BitStreamFilter() = default;

  virtual auto convert(std::span<const uint8_t> avcc_sample, bool is_keyframe) const
      -> std::vector<uint8_t> = 0;
};

class AnnexBFilter : public BitStreamFilter {
public:
  static constexpr uint8_t START_CODE_LONG[4] = {0x00, 0x00, 0x00, 0x01};
  static constexpr uint8_t START_CODE_SHORT[3] = {0x00, 0x00, 0x01};

  AnnexBFilter(uint8_t nalu_len_sz, std::vector<uint8_t> annexb_extra)
      : nalu_len_sz_(nalu_len_sz), annexb_extra_(std::move(annexb_extra)) {}

  ~AnnexBFilter() override = default;

  auto convert(std::span<const uint8_t> avcc_sample, bool is_keyframe) const
      -> std::vector<uint8_t> override {
    std::vector<uint8_t> out;
    out.reserve(reserveSize(avcc_sample.size(), is_keyframe));

    if (is_keyframe && !annexb_extra_.empty()) {
      appendBytes(out, annexb_extra_);
    }

    convertNalus(avcc_sample, out);
    return out;
  }

private:
  uint8_t nalu_len_sz_;
  std::vector<uint8_t> annexb_extra_;

  auto reserveSize(size_t sample_size, bool is_keyframe) const -> size_t {
    size_t sz = sample_size + (sample_size / 4 + 1) * 4;
    if (is_keyframe) {
      sz += annexb_extra_.size();
    }
    return sz;
  }

  void convertNalus(std::span<const uint8_t> src,
                    std::vector<uint8_t>& out) const {
    size_t pos = 0;
    while (pos + nalu_len_sz_ <= src.size()) {
      const uint32_t nalu_len = readLen(src, pos);
      pos += nalu_len_sz_;

      if (nalu_len == 0) continue;
      if (pos + nalu_len > src.size()) break;

      out.insert(out.end(), std::begin(START_CODE_LONG), std::end(START_CODE_LONG));
      out.insert(out.end(), src.begin() + pos, src.begin() + pos + nalu_len);
      pos += nalu_len;
    }
  }

  auto readLen(std::span<const uint8_t> src, size_t pos) const -> uint32_t {
    uint32_t len = 0;
    for (uint8_t i = 0; i < nalu_len_sz_; ++i) {
      len = (len << 8u) | src[pos + i];
    }
    return len;
  }

  template<size_t N>
  static void appendBytes(std::vector<uint8_t>& out,
                          std::span<const uint8_t, N> arr) {
    out.insert(out.end(), arr, arr + N);
  }

  static void appendBytes(std::vector<uint8_t>& out,
                          std::span<const uint8_t> data) {
    out.insert(out.end(), data.begin(), data.end());
  }
};

} // namespace openmedia
