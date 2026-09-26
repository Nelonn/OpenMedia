#pragma once

#include <cstdint>

namespace openmedia {

/** Rescales a timestamp between two timescales without going through a wider
 * type: a nanosecond figure multiplied by a timescale overflows int64 after a few
 * hours, and media timestamps get that big routinely. */
inline auto rescaleTime(int64_t value, int64_t from, int64_t to) -> int64_t {
  if (from <= 0 || to <= 0) return 0;
  const int64_t whole = value / from;
  const int64_t remainder = value % from;
  return whole * to + (remainder * to) / from;
}

inline auto nsToScale(int64_t ns, uint32_t timescale) -> int64_t {
  return rescaleTime(ns, 1'000'000'000, timescale);
}

inline auto scaleToNs(int64_t value, uint32_t timescale) -> int64_t {
  return rescaleTime(value, timescale, 1'000'000'000);
}

} // namespace openmedia
