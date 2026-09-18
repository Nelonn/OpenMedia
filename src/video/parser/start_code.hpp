#pragma once

#include <cstddef>
#include <cstdint>

namespace openmedia::video_parser {

enum class SimdIsa {
  none = 0,
  ssse3,
  avx2,
  avx512,
  neon,
  sve,
};

auto detectSimdIsa() -> SimdIsa;

class StartCodeScanner {
public:
  StartCodeScanner();

  void reset() noexcept { bit_buffer_ = 0; }

  auto next(const uint8_t* data, size_t size, bool& found_start_code) -> size_t;

private:
  using FindFn = size_t (*)(const uint8_t*, size_t, uint32_t&, bool&);

  uint32_t bit_buffer_ = 0;
  FindFn find_ = nullptr;
};

// Returns the bytes consumed: the index just past the 0x01 on a hit, `size`
// otherwise. `bit_buffer` carries the two bytes either side of the call, so a
// start code split across calls is still found.
//
// nextStartCodeC is the definition and the rest must agree with it byte for
// byte. The trap: each byte is compared against the two preceding it *in the
// stream*, and the byte-wise align instructions of AVX2 and AVX-512 work inside
// 128 bit lanes only, so reaching past a lane needs a lane crossing step first.
// Without it four positions per vector compare against an older block, which
// loses and invents start codes -- rarely, and only in buffers long enough to
// reach the vector loop. NEON and SVE have no lanes to cross.
auto nextStartCodeC(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t;
auto nextStartCodeSSSE3(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t;
auto nextStartCodeAVX2(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t;
auto nextStartCodeAVX512(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t;
auto nextStartCodeNEON(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t;
auto nextStartCodeSVE(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t;

} // namespace openmedia::video_parser
