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

auto nextStartCodeC(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t;
auto nextStartCodeSSSE3(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t;
auto nextStartCodeAVX2(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t;
auto nextStartCodeAVX512(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t;
auto nextStartCodeNEON(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t;
auto nextStartCodeSVE(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t;

} // namespace openmedia::video_parser
