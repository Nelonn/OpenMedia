#include "start_code.hpp"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace openmedia::video_parser {

auto countTrailingZerosForStartCode(uint64_t value) -> int;

#if defined(__x86_64__) || defined(_M_X64)
auto nextStartCodeAVX2(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t {
  size_t i = 0;
  const size_t size64 = (size >> 6u) << 6u;
  if (size64 > 64) {
    const __m256i v1 = _mm256_set1_epi8(1);
    __m256i vdata = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data));
    const __m256i vbfr = _mm256_set1_epi16(((bit_buffer << 8u) & 0xff00u) | ((bit_buffer >> 8u) & 0xffu));
    const __m256i init = _mm256_permute2f128_si256(vbfr, vdata, 1 | (2 << 4));
    __m256i vprev1 = _mm256_alignr_epi8(vdata, init, 15);
    __m256i vprev2 = _mm256_alignr_epi8(vdata, init, 14);
    for (; i < size64 - 64; i += 64) {
      for (int c = 0; c < 64; c += 32) {
        const __m256i prev_zero = _mm256_cmpeq_epi8(_mm256_or_si256(vprev2, vprev1), _mm256_setzero_si256());
        const __m256i vmask = _mm256_cmpeq_epi8(_mm256_and_si256(vdata, prev_zero), v1);
        const int mask = _mm256_movemask_epi8(vmask);
        if (mask) {
          const int offset = countTrailingZerosForStartCode(static_cast<uint64_t>(mask & 0xffffffff));
          found_start_code = true;
          bit_buffer = 1;
          return static_cast<size_t>(offset) + i + static_cast<size_t>(c) + 1;
        }
        const __m256i next = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(&data[i + static_cast<size_t>(c) + 32]));
        const __m256i align_next = _mm256_permute2f128_si256(vdata, next, 1 | (2 << 4));
        vprev1 = _mm256_alignr_epi8(next, align_next, 15);
        vprev2 = _mm256_alignr_epi8(next, align_next, 14);
        vdata = next;
      }
    }
    bit_buffer = (static_cast<uint32_t>(data[i - 2]) << 8u) | data[i - 1];
  }
  return nextStartCodeC(data + i, size - i, bit_buffer, found_start_code) + i;
}
#else
auto nextStartCodeAVX2(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t {
  return nextStartCodeC(data, size, bit_buffer, found_start_code);
}
#endif

} // namespace openmedia::video_parser
