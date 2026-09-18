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

// Each of these scans `data` for the next three byte start code and returns how
// many bytes it consumed: the index just past the 0x01 when it found one, and
// `size` when it did not. `bit_buffer` carries the two bytes before `data` in and
// the last two bytes out, so a start code split across calls is still found.
//
// nextStartCodeC is the definition; the rest have to agree with it byte for byte.
// The trap is that a vector kernel has to compare each byte against the two that
// precede it *in the stream*, and the byte-wise align instructions of both AVX2
// and AVX-512 work inside 128 bit lanes only, so reaching the byte before a lane
// needs a lane crossing step first. Skipping it leaves four positions in every
// vector comparing against bytes from an older block, which loses and invents
// start codes -- rarely, and only in buffers long enough to reach the vector
// loop. NEON and SVE have no lanes to cross and do not need it.
auto nextStartCodeC(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t;
auto nextStartCodeSSSE3(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t;
auto nextStartCodeAVX2(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t;
auto nextStartCodeAVX512(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t;
auto nextStartCodeNEON(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t;
auto nextStartCodeSVE(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t;

} // namespace openmedia::video_parser
