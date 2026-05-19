#include "start_code.hpp"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace openmedia::video_parser {

auto countTrailingZerosForStartCode(uint64_t value) -> int;

#if defined(__x86_64__) || defined(_M_X64)
auto nextStartCodeSSSE3(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t {
  size_t i = 0;
  const size_t size32 = (size >> 5u) << 5u;
  if (size32 > 32) {
    const __m128i v1 = _mm_set1_epi8(1);
    __m128i vdata = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data));
    const __m128i vbfr = _mm_set1_epi16(((bit_buffer << 8u) & 0xff00u) | ((bit_buffer >> 8u) & 0xffu));
    __m128i vprev1 = _mm_alignr_epi8(vdata, vbfr, 15);
    __m128i vprev2 = _mm_alignr_epi8(vdata, vbfr, 14);
    for (; i < size32 - 32; i += 32) {
      for (int c = 0; c < 32; c += 16) {
        const __m128i prev_zero = _mm_cmpeq_epi8(_mm_or_si128(vprev2, vprev1), _mm_setzero_si128());
        const __m128i vmask = _mm_cmpeq_epi8(_mm_and_si128(vdata, prev_zero), v1);
        const int mask = _mm_movemask_epi8(vmask);
        if (mask) {
          const int offset = countTrailingZerosForStartCode(static_cast<uint64_t>(mask & 0xffffffff));
          found_start_code = true;
          bit_buffer = 1;
          return static_cast<size_t>(offset) + i + static_cast<size_t>(c) + 1;
        }
        const __m128i next = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&data[i + static_cast<size_t>(c) + 16]));
        vprev1 = _mm_alignr_epi8(next, vdata, 15);
        vprev2 = _mm_alignr_epi8(next, vdata, 14);
        vdata = next;
      }
    }
    bit_buffer = (static_cast<uint32_t>(data[i - 2]) << 8u) | data[i - 1];
  }
  return nextStartCodeC(data + i, size - i, bit_buffer, found_start_code) + i;
}
#else
auto nextStartCodeSSSE3(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t {
  return nextStartCodeC(data, size, bit_buffer, found_start_code);
}
#endif

} // namespace openmedia::video_parser
