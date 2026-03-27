#pragma once

#if defined(__unix__) || defined(__APPLE__) || defined(__QNX__) || defined(__Fuchsia__)
#include <dlfcn.h>
#elif defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <string>

namespace openmedia {

class DynamicLoader {
#if defined(__unix__) || defined(__APPLE__) || defined(__QNX__) || defined(__Fuchsia__)
  void* library_;
#elif defined(_WIN32)
  ::HINSTANCE library_;
#endif

public:
  DynamicLoader() = default;

  ~DynamicLoader() noexcept {
    if (!library_) return;
#if defined(__unix__) || defined(__APPLE__) || defined(__QNX__) || defined(__Fuchsia__)
    dlclose(library_);
#elif defined(_WIN32)
    ::FreeLibrary(library_);
#endif
  }

  DynamicLoader(const DynamicLoader&) = delete;

  DynamicLoader(DynamicLoader&& other) noexcept
      : library_(other.library_) {
    other.library_ = nullptr;
  }

  auto operator=(const DynamicLoader&) -> DynamicLoader& = delete;

  auto operator=(DynamicLoader&& other) noexcept -> DynamicLoader& {
    std::swap(library_, other.library_);
    return *this;
  }

  void open(const std::string& library_name) {
    if (library_name.empty()) return;
#if defined(_WIN32)
    library_ = ::LoadLibraryA(library_name.c_str());
#elif defined(__unix__) || defined(__APPLE__) || defined(__QNX__) || defined(__Fuchsia__)
    library_ = dlopen(library_name.c_str(), RTLD_NOW | RTLD_LOCAL);
#else
#error unsupported platform
#endif
  }

  template<typename T>
  auto getProcAddress(const char* function) const noexcept -> T {
    if (!library_) return static_cast<T>(nullptr);
#if defined(__unix__) || defined(__APPLE__) || defined(__QNX__) || defined(__Fuchsia__)
    return reinterpret_cast<T>(dlsym(library_, function));
#elif defined(_WIN32)
    return reinterpret_cast<T>(::GetProcAddress(library_, function));
#else
#error unsupported platform
#endif
  }

  auto success() const noexcept -> bool {
    return library_ != nullptr;
  }
};

template<typename T>
using PFN = T*;

} // namespace openmedia
