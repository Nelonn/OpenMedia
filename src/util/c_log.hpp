#pragma once

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <openmedia/log.hpp>
#include <string_view>

namespace openmedia {

// For the printf-style log callbacks of C libraries, which can fire per frame, so the
// message is formatted on the stack. 1 KiB is what FFmpeg's own log formatter allows.
inline void logPrintf(OMLogCategory category, OMLogLevel level, const char* format, va_list args) {
  char buffer[1024];
  const int written = std::vsnprintf(buffer, sizeof(buffer), format, args);
  if (written <= 0) return;
  std::string_view message(buffer, std::min(static_cast<size_t>(written), sizeof(buffer) - 1));
  if (message.ends_with('\n')) message.remove_suffix(1);
  if (!message.empty()) log(category, level, message);
}

} // namespace openmedia
