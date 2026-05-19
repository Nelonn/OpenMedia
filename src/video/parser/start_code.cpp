#include "start_code.hpp"

#include <cassert>

#if defined(__aarch64__) && defined(__linux__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#elif defined(_M_X64)
#include <array>
#include <bitset>
#include <intrin.h>
#endif

namespace openmedia::video_parser {

static auto countTrailingZeros(uint64_t value) -> int {
  assert(value != 0);
#ifdef _WIN64
  unsigned long offset = 0;
  (void)_BitScanForward64(&offset, value);
  return static_cast<int>(offset);
#elif defined(_WIN32)
  unsigned long offset = 0;
  unsigned long low = static_cast<unsigned long>(value & 0xffffffffULL);
  if (low != 0) {
    (void)_BitScanForward(&offset, low);
  } else {
    (void)_BitScanForward(&offset, static_cast<unsigned long>(value >> 32U));
    offset += 32U;
  }
  return static_cast<int>(offset);
#else
  return __builtin_ctzll(value);
#endif
}

#if defined(_M_X64)
class InstructionSet {
public:
  static auto ssse3() -> bool { return cpu_.ecx1[9]; }
  static auto avx2() -> bool { return cpu_.ebx7[5]; }
  static auto avx512() -> bool { return cpu_.ebx7[16] && cpu_.ebx7[30]; }

private:
  struct CpuInfo {
    CpuInfo() {
      std::array<int, 4> cpui{};
      __cpuid(cpui.data(), 0);
      const int nids = cpui[0];
      if (nids >= 1) {
        __cpuidex(cpui.data(), 1, 0);
        ecx1 = static_cast<unsigned long>(cpui[2]);
      }
      if (nids >= 7) {
        __cpuidex(cpui.data(), 7, 0);
        ebx7 = static_cast<unsigned long>(cpui[1]);
      }
    }

    std::bitset<32> ecx1{};
    std::bitset<32> ebx7{};
  };

  static const CpuInfo cpu_;
};

const InstructionSet::CpuInfo InstructionSet::cpu_;
#endif

auto detectSimdIsa() -> SimdIsa {
#if defined(_M_X64)
#if defined(OPENMEDIA_ENABLE_AVX512_START_CODE)
  if (InstructionSet::avx512()) return SimdIsa::avx512;
#endif
  if (InstructionSet::avx2()) return SimdIsa::avx2;
  if (InstructionSet::ssse3()) return SimdIsa::ssse3;
#elif defined(__x86_64__)
#if defined(OPENMEDIA_ENABLE_AVX512_START_CODE)
  if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw")) return SimdIsa::avx512;
#endif
  if (__builtin_cpu_supports("avx2")) return SimdIsa::avx2;
  if (__builtin_cpu_supports("ssse3")) return SimdIsa::ssse3;
#elif defined(__aarch64__) && defined(__linux__)
  const long hwcaps = getauxval(AT_HWCAP);
#if defined(OPENMEDIA_ENABLE_SVE_START_CODE)
  if (hwcaps & HWCAP_SVE) return SimdIsa::sve;
#endif
  if (hwcaps & HWCAP_ASIMD) return SimdIsa::neon;
#elif defined(__aarch64__) && defined(__APPLE__)
  return SimdIsa::neon;
#elif defined(__ARM_ARCH_7A__) || defined(_M_ARM64)
  return SimdIsa::neon;
#endif
  return SimdIsa::none;
}

StartCodeScanner::StartCodeScanner() {
  switch (detectSimdIsa()) {
#if defined(OPENMEDIA_ENABLE_AVX512_START_CODE)
    case SimdIsa::avx512: find_ = nextStartCodeAVX512; break;
#endif
    case SimdIsa::avx2: find_ = nextStartCodeAVX2; break;
    case SimdIsa::ssse3: find_ = nextStartCodeSSSE3; break;
#if defined(OPENMEDIA_ENABLE_SVE_START_CODE)
    case SimdIsa::sve: find_ = nextStartCodeSVE; break;
#endif
    case SimdIsa::neon: find_ = nextStartCodeNEON; break;
    default: find_ = nextStartCodeC; break;
  }
}

auto StartCodeScanner::next(const uint8_t* data, size_t size, bool& found_start_code) -> size_t {
  if (size == 0) {
    found_start_code = false;
    return 0;
  }
  return find_(data, size, bit_buffer_, found_start_code);
}

auto nextStartCodeC(const uint8_t* data, size_t size, uint32_t& bit_buffer, bool& found_start_code) -> size_t {
  uint32_t bfr = bit_buffer;
  size_t i = 0;
  do {
    bfr = (bfr << 8u) | data[i++];
    if ((bfr & 0x00ffffffu) == 1u) break;
  } while (i < size);
  bit_buffer = bfr;
  found_start_code = ((bfr & 0x00ffffffu) == 1u);
  return i;
}

auto countTrailingZerosForStartCode(uint64_t value) -> int {
  return countTrailingZeros(value);
}

} // namespace openmedia::video_parser
