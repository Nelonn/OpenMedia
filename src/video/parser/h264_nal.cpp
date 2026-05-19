#include "h264_types.hpp"

namespace h264 {

static auto isStartCode(const uint8_t* p, const uint8_t* end, size_t& size) -> bool {
  if (p + 3 <= end && p[0] == 0 && p[1] == 0 && p[2] == 1) {
    size = 3;
    return true;
  }
  if (p + 4 <= end && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
    size = 4;
    return true;
  }
  return false;
}

auto find_next_nal(Bitstream& bs) -> bool {
  if (!bs.valid() || !bs.current() || bs.byte_offset() >= bs.size) return false;
  const uint8_t* begin = bs.data;
  const uint8_t* end = bs.data + bs.size;
  const uint8_t* p = bs.current();
  while (p < end) {
    size_t start_code_size = 0;
    if (isStartCode(p, end, start_code_size)) {
      bs.set_offset(static_cast<size_t>((p + start_code_size) - begin));
      return bs.byte_offset() < bs.size;
    }
    ++p;
  }
  bs.finish();
  return false;
}

auto read_nal_header(NALHeader& nal, Bitstream& bs) -> bool {
  if (!bs.valid() || bs.remaining() < 1) return false;
  const uint8_t header = *bs.current();
  bs.consume(1);
  nal.forbidden_zero_bit = (header >> 7u) & 1u;
  nal.idc = static_cast<NAL_REF_IDC>((header >> 5u) & 0x03u);
  nal.type = static_cast<NAL_UNIT_TYPE>(header & 0x1fu);
  return nal.forbidden_zero_bit == 0;
}

} // namespace h264
