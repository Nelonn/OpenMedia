#include "start_code.hpp"

#if defined(__aarch64__) || defined(__ARM_ARCH_7A__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace openmedia::video_parser {

#if defined(__aarch64__) || defined(__ARM_ARCH_7A__) || defined(_M_ARM64)
auto nextStartCodeNEON(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t {
  size_t i = 0;
  const size_t size32 = (size >> 5u) << 5u;
  if (size32 > 32) {
    const uint8x16_t v0 = vdupq_n_u8(0);
    const uint8x16_t v1 = vdupq_n_u8(1);
    uint8x16_t vdata = vld1q_u8(data);
    const uint8x16_t vbfr = vreinterpretq_u8_u16(vdupq_n_u16(((bit_buffer << 8u) & 0xff00u) | ((bit_buffer >> 8u) & 0xffu)));
    uint8x16_t vprev1 = vextq_u8(vbfr, vdata, 15);
    uint8x16_t vprev2 = vextq_u8(vbfr, vdata, 14);
    const uint8_t idx0n[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const uint8x16_t v015 = vld1q_u8(idx0n);
    for (; i < size32 - 32; i += 32) {
      for (int c = 0; c < 32; c += 16) {
        const uint8x16_t prev_zero = vceqq_u8(vorrq_u8(vprev2, vprev1), v0);
        const uint8x16_t vmask = vceqq_u8(vandq_u8(prev_zero, vdata), v1);
#if defined(__aarch64__) || defined(_M_ARM64)
        const uint64_t result = vmaxvq_u8(vmask);
#else
        const uint64_t result = vget_lane_u64(vreinterpret_u64_u8(vmax_u8(vget_low_u8(vmask), vget_high_u8(vmask))), 0);
#endif
        if (result) {
          const uint8x16_t vmasked = vbslq_u8(vmask, v015, vdupq_n_u8(UINT8_MAX));
#if defined(__aarch64__) || defined(_M_ARM64)
          const uint8_t offset = vminvq_u8(vmasked);
#else
          uint8x8_t minval = vmin_u8(vget_low_u8(vmasked), vget_high_u8(vmasked));
          minval = vpmin_u8(minval, minval);
          minval = vpmin_u8(minval, minval);
          const uint8_t offset = vget_lane_u8(vpmin_u8(minval, minval), 0);
#endif
          found_start_code = true;
          bit_buffer = 1;
          return static_cast<size_t>(offset) + i + static_cast<size_t>(c) + 1;
        }
        const uint8x16_t next = vld1q_u8(&data[i + static_cast<size_t>(c) + 16]);
        vprev1 = vextq_u8(vdata, next, 15);
        vprev2 = vextq_u8(vdata, next, 14);
        vdata = next;
      }
    }
    bit_buffer = (static_cast<uint32_t>(data[i - 2]) << 8u) | data[i - 1];
  }
  return nextStartCodeC(data + i, size - i, bit_buffer, found_start_code) + i;
}
#else
auto nextStartCodeNEON(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t {
  return nextStartCodeC(data, size, bit_buffer, found_start_code);
}
#endif

} // namespace openmedia::video_parser
