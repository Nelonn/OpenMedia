#include <openmedia/log.hpp>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#endif

#include <atomic>
#include <format>
#include <iostream>
#include <ctime>

namespace openmedia {

namespace {

constexpr auto categoryToString(OMLogCategory category) -> std::string_view {
  switch (category) {
    case OM_CATEGORY_IO:
      return "IO";
    case OM_CATEGORY_MUXER:
      return "MUXER";
    case OM_CATEGORY_DEMUXER:
      return "DEMUXER";
    case OM_CATEGORY_ENCODER:
      return "ENCODER";
    case OM_CATEGORY_DECODER:
      return "DECODER";
    default:
      return "UNKNOWN";
  }
}

#if defined(__ANDROID__)
android_LogPriority levelToAndroidPriority(OMLogLevel level) {
  switch (level) {
    case OM_LEVEL_FATAL:
      return ANDROID_LOG_FATAL;
    case OM_LEVEL_ERROR:
      return ANDROID_LOG_ERROR;
    case OM_LEVEL_WARNING:
      return ANDROID_LOG_WARN;
    case OM_LEVEL_INFO:
      return ANDROID_LOG_INFO;
    case OM_LEVEL_VERBOSE:
      return ANDROID_LOG_VERBOSE;
    case OM_LEVEL_DEBUG:
      return ANDROID_LOG_DEBUG;
    default:
      return ANDROID_LOG_DEFAULT;
  }
}
#endif

constexpr auto levelToString(OMLogLevel level) -> std::string_view {
  switch (level) {
    case OM_LEVEL_FATAL:
      return "FATAL";
    case OM_LEVEL_ERROR:
      return "ERROR";
    case OM_LEVEL_WARNING:
      return "WARN";
    case OM_LEVEL_INFO:
      return "INFO";
    case OM_LEVEL_VERBOSE:
      return "VERB";
    case OM_LEVEL_DEBUG:
      return "DEBUG";
    default:
      return "????";
  }
}

} // namespace

class DefaultLogger final : public Logger {
public:
  DefaultLogger() = default;
  ~DefaultLogger() override = default;

  void log(OMLogCategory category, OMLogLevel level, std::string_view message) override {
#if defined(__ANDROID__)
    auto prefix = std::format("OpenMedia/{}", categoryToString(category));
    std::string msg(message);
    __android_log_write(levelToAndroidPriority(level), prefix.c_str(), msg.c_str());
#else
    auto formatted = std::format("[{}] [{}] {}\n",
                                 categoryToString(category),
                                 levelToString(level),
                                 message);
#if defined(_WIN32)
    OutputDebugStringA(formatted.c_str());
#endif
    std::cerr << formatted;
#endif
  }
};

static std::atomic<std::shared_ptr<Logger>> LOGGER{std::make_shared<DefaultLogger>()};

void setLogger(std::unique_ptr<Logger>&& logger) {
  LOGGER.store(std::move(logger));
}

void log(OMLogCategory category, OMLogLevel level, std::string_view message) {
  if (auto logger = LOGGER.load()) {
    logger->log(category, level, message);
  }
}

} // namespace openmedia
