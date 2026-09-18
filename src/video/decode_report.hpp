#pragma once

#include <openmedia/error.h>
#include <openmedia/log.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <string_view>
#include <utility>

namespace openmedia {

// Every rejection used to be a bare error return. A stream the parser or the
// driver will not take then yields no pictures at all, and the only symptom a
// player can show for that is a frame rate that never comes up -- which looks
// exactly like slow decoding. Throttled, because a stream that fails tends to
// fail on every picture.
//
// The decoders inherit it privately, so the calls read as their own.
class DecodeReport {
public:
  explicit DecodeReport(std::string_view backend) noexcept : backend_(backend) {}

protected:
  template<typename... Args>
  auto rejectFrame(OMError error, std::format_string<Args...> fmt, Args&&... args) -> OMError {
    if ((rejected_frames_++ % kThrottle) == 0) {
      openmedia::log(OM_CATEGORY_DECODER, OM_LEVEL_ERROR, "{}: {} ({} frames rejected so far)",
                     backend_, std::format(fmt, std::forward<Args>(args)...), rejected_frames_);
    }
    return error;
  }

  // Expected for a moment after a seek. Any other time a reference has been lost
  // and the blocks that wanted it carry whatever was left in the surface, which
  // is the one thing that tells that apart from a clip which simply looks so.
  void reportMissingReferences(size_t missing, int32_t poc) {
    if (missing == 0) return;
    if ((missing_references_++ % kThrottle) == 0) {
      openmedia::log(OM_CATEGORY_DECODER, OM_LEVEL_WARNING,
                     "{}: picture {} predicts from {} picture(s) the decoder no longer holds"
                     " ({} pictures affected so far)",
                     backend_, poc, missing, missing_references_);
    }
  }

private:
  static constexpr uint64_t kThrottle = 120;

  std::string_view backend_;
  uint64_t rejected_frames_ = 0;
  uint64_t missing_references_ = 0;
};

} // namespace openmedia
