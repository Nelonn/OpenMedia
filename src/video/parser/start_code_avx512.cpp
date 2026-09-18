#include "start_code.hpp"

#if defined(OPENMEDIA_ENABLE_AVX512_START_CODE) && (defined(__x86_64__) || defined(_M_X64))
#include <immintrin.h>
#endif

namespace openmedia::video_parser {

auto countTrailingZerosForStartCode(uint64_t value) -> int;

#if defined(OPENMEDIA_ENABLE_AVX512_START_CODE) && (defined(__x86_64__) || defined(_M_X64))

static auto laneShift(__m512i next, __m512i prev) -> __m512i {
  return _mm512_alignr_epi32(next, prev, 12);
}

auto nextStartCodeAVX512(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t {
  size_t i = 0;
  const size_t size64 = (size >> 6u) << 6u;
  if (size64 > 64) {
    const __m512i v1 = _mm512_set1_epi8(1);
    __m512i vdata = _mm512_loadu_si512(reinterpret_cast<const void*>(data));
    const __m512i vbfr = _mm512_set1_epi16(static_cast<short>(((bit_buffer << 8u) & 0xff00u) | ((bit_buffer >> 8u) & 0xffu)));
    const __m512i init = laneShift(vdata, vbfr);
    __m512i vprev1 = _mm512_alignr_epi8(vdata, init, 15);
    __m512i vprev2 = _mm512_alignr_epi8(vdata, init, 14);
    for (; i < size64 - 64; i += 64) {
      const __mmask64 prev_zero = _mm512_cmpeq_epi8_mask(_mm512_or_si512(vprev2, vprev1), _mm512_setzero_si512());
      const __mmask64 mask = _mm512_cmpeq_epi8_mask(_mm512_maskz_mov_epi8(prev_zero, vdata), v1);
      if (mask) {
        const int offset = countTrailingZerosForStartCode(static_cast<uint64_t>(mask));
        found_start_code = true;
        bit_buffer = 1;
        return static_cast<size_t>(offset) + i + 1;
      }
      const __m512i next = _mm512_loadu_si512(reinterpret_cast<const void*>(&data[i + 64]));
      const __m512i aligned = laneShift(next, vdata);
      vprev1 = _mm512_alignr_epi8(next, aligned, 15);
      vprev2 = _mm512_alignr_epi8(next, aligned, 14);
      vdata = next;
    }
    bit_buffer = (static_cast<uint32_t>(data[i - 2]) << 8u) | data[i - 1];
  }
  return nextStartCodeC(data + i, size - i, bit_buffer, found_start_code) + i;
}
#else
auto nextStartCodeAVX512(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t {
  return nextStartCodeC(data, size, bit_buffer, found_start_code);
}
#endif

} // namespace openmedia::video_parser
