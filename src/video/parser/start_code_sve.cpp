#include "start_code.hpp"

#if defined(OPENMEDIA_ENABLE_SVE_START_CODE) && defined(__aarch64__)
#include <arm_sve.h>
#endif

namespace openmedia::video_parser {

#if defined(OPENMEDIA_ENABLE_SVE_START_CODE) && defined(__aarch64__)
auto nextStartCodeSVE(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t {
  size_t i = 0;
  const int lanes = static_cast<int>(svcntb());

  svbool_t pred = svwhilelt_b8_u64(i, size);
  svuint8_t vdata = svld1_u8(pred, data);
  const svuint8_t vbfr = svreinterpret_u8_u16(svdup_n_u16(((bit_buffer << 8u) & 0xff00u) | ((bit_buffer >> 8u) & 0xffu)));

  // Was a function-local static table that every thread raced to fill, and that
  // silently stopped covering the vector once it grew wider than the table.
  const svuint8_t v0n = svindex_u8(0, 1);
  const svbool_t ext15_mask = svcmpge_n_u8(svptrue_b8(), v0n, lanes - 1);
  const svbool_t ext14_mask = svcmpge_n_u8(svptrue_b8(), v0n, lanes - 2);
  svuint8_t vprev1 = svsplice_u8(ext15_mask, vbfr, vdata);
  svuint8_t vprev2 = svsplice_u8(ext14_mask, vbfr, vdata);

  for (; i < size; i += static_cast<size_t>(lanes)) {
    const svuint8_t prev_or = svorr_u8_z(pred, vprev2, vprev1);
    const svbool_t mask = svcmpeq_n_u8(svcmpeq_n_u8(pred, prev_or, 0), vdata, 1);
    if (svmaxv_u8(mask, vdata)) {
      const uint8_t offset = svminv_u8(mask, v0n);
      found_start_code = true;
      bit_buffer = 1;
      return static_cast<size_t>(offset) + i + 1;
    }
    const svbool_t pred_next = svwhilelt_b8_u64(i + static_cast<size_t>(lanes), size);
    const svuint8_t next = svld1_u8(pred_next, &data[i + static_cast<size_t>(lanes)]);
    vprev1 = svsplice_u8(ext15_mask, vdata, next);
    vprev2 = svsplice_u8(ext14_mask, vdata, next);
    pred = pred_next;
    vdata = next;
  }

  if (size >= 2) bit_buffer = data[size - 2];
  bit_buffer = (bit_buffer << 8u) | data[size >= 1 ? size - 1 : 0];
  found_start_code = false;
  return size;
}
#else
auto nextStartCodeSVE(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t {
  return nextStartCodeC(data, size, bit_buffer, found_start_code);
}
#endif

} // namespace openmedia::video_parser
