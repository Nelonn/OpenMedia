#pragma once

#include <algorithm>
#include <cstdint>
#include <openmedia/codec_extra.hpp>
#include <openmedia/dictionary.hpp>
#include <optional>

// CODEC_ENC_PRESET says how much work an encoder may spend per frame on a
// 1-7 scale. Every encoder states that trade-off in its own terms -- NVENC has
// seven presets, AMF four, VA-API a driver-reported range, Media Foundation a
// 0-100 hint -- so the scale is translated per encoder rather than passed on.
namespace openmedia::enc_preset {

// The ends of the scale and the step an encoder is on when nobody asked.
constexpr int32_t kFastest = 1;
constexpr int32_t kBalanced = 4;
constexpr int32_t kBest = 7;

// Nothing, when the caller did not ask: an encoder then keeps whatever it
// would have configured anyway. Int32 and int64 are both accepted, since a
// Dictionary value only reads back as the type it was written with.
inline auto requested(const Dictionary& extra) -> std::optional<int32_t> {
  const auto* value = extra.get(CODEC_ENC_PRESET);
  if (!value) return std::nullopt;
  if (const auto preset = value->getInt32()) return std::clamp(*preset, kFastest, kBest);
  if (const auto preset = value->getInt64()) {
    return static_cast<int32_t>(std::clamp<int64_t>(*preset, kFastest, kBest));
  }
  return std::nullopt;
}

inline auto requestedOr(const Dictionary& extra, int32_t fallback) -> int32_t {
  return requested(extra).value_or(fallback);
}

// Spreads the scale evenly over the range an encoder works in, where `fastest`
// and `slowest` are that encoder's own values for the two ends. They may run
// either way round: Media Foundation counts up towards quality, VA-API counts
// down towards it. The step is rounded rather than truncated, because a range
// narrower than the scale -- a driver reporting two quality levels, say --
// would otherwise spend six of the seven steps on the fast end.
inline auto scaleTo(int32_t preset, int32_t fastest, int32_t slowest) -> int32_t {
  const int32_t offset = std::clamp(preset, kFastest, kBest) - kFastest;
  const int32_t span = kBest - kFastest;
  const int32_t numerator = (slowest - fastest) * offset;
  const int32_t step = numerator >= 0 ? (numerator + span / 2) / span
                                      : -((-numerator + span / 2) / span);
  return fastest + step;
}

} // namespace openmedia::enc_preset
