#pragma once

#include <openmedia/macro.h>
#include <cstdint>
#include <string_view>
#include <memory>

OM_ENUM(OMLogLevel, uint8_t) {
  OM_LEVEL_FATAL = 0,
  OM_LEVEL_ERROR = 1,
  OM_LEVEL_WARNING = 2,
  OM_LEVEL_INFO = 3,
  OM_LEVEL_VERBOSE = 4,
  OM_LEVEL_DEBUG = 5,
};

OM_ENUM(OMLogCategory, uint8_t) {
  OM_CATEGORY_IO = 0,
  OM_CATEGORY_MUXER = 1,
  OM_CATEGORY_DEMUXER = 2,
  OM_CATEGORY_ENCODER = 3,
  OM_CATEGORY_DECODER = 4,
  OM_CATEGORY_HARDWARE = 5,
};

namespace openmedia {

class Logger;

using LoggerRef = std::shared_ptr<Logger>;

class OPENMEDIA_ABI Logger {
public:
  virtual ~Logger() = default;

  virtual void log(OMLogCategory category, OMLogLevel level, std::string_view message) = 0;

  static auto refDefault() noexcept -> LoggerRef; // Do not use here `const LoggerRef&`, C++ is fucking shit
};

}
