#pragma once

#include <openmedia/macro.h>
#include <cstdint>
#include <string_view>
#include <memory>
#include <format>

OM_ENUM(OMLogLevel, uint8_t) {
  OM_LEVEL_FATAL = 0,
  OM_LEVEL_ERROR = 1,
  OM_LEVEL_WARNING = 2,
  OM_LEVEL_INFO = 3,
  OM_LEVEL_VERBOSE = 4,
  OM_LEVEL_DEBUG = 5,
};

OM_ENUM(OMLogCategory, uint8_t) {
  OM_CATEGORY_NONE = 0,
  OM_CATEGORY_IO = 1,
  OM_CATEGORY_MUXER = 2,
  OM_CATEGORY_DEMUXER = 3,
  OM_CATEGORY_ENCODER = 4,
  OM_CATEGORY_DECODER = 5,
  OM_CATEGORY_HARDWARE = 6,
};

namespace openmedia {

class OPENMEDIA_ABI Logger {
public:
  virtual ~Logger() = default;

  virtual void log(OMLogCategory category, OMLogLevel level, std::string_view message) = 0;
};

OPENMEDIA_ABI
void setLogger(std::unique_ptr<Logger>&& logger);

OPENMEDIA_ABI
void log(OMLogCategory category, OMLogLevel level, std::string_view message);

template <typename... Args>
static void log(OMLogCategory category, OMLogLevel level, std::format_string<Args...> fmt, Args&&... args) {
  std::string message = std::format(fmt, std::forward<Args>(args)...);
  log(category, level, std::string_view(message));
}

}
