#pragma once

#include <openmedia/hw_vaapi.h>
#include <openmedia/log.hpp>
#include <video/vaapi_loader.hpp>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string_view>
#include <unistd.h>

struct OMVAAPIContext {
  VADisplay display = nullptr;
  int drm_fd = -1;
  bool owns_display = false;

  ~OMVAAPIContext() {
    auto& libva = openmedia::LibVA::getInstance();
    if (owns_display && display && libva.isLoaded()) {
      libva.vaTerminate(display);
    }
    if (drm_fd >= 0) {
      close(drm_fd);
    }
  }

  auto initialize(const OMVAAPIInit& init) -> bool {
    auto& libva = openmedia::LibVA::getInstance();
    if (!libva.isLoaded()) {
      if (!libva.load()) {
        openmedia::log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "[VAAPI] Failed to load libva/libva-drm");
        return false;
      }
    }

    if (init.display) {
      display = init.display;
      openmedia::log(OM_CATEGORY_HARDWARE, OM_LEVEL_INFO, "[VAAPI] Using caller-provided VADisplay");
    } else {
      if (!libva.vaGetDisplayDRM) {
        openmedia::log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "[VAAPI] libva-drm does not export vaGetDisplayDRM");
        return false;
      }

      auto tryOpenDevice = [&](std::string_view path) -> bool {
        const int fd = open(std::string(path).c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
          if (errno != ENOENT) {
            openmedia::log(OM_CATEGORY_HARDWARE, OM_LEVEL_WARNING, "[VAAPI] Failed to open {}: {}", path, std::strerror(errno));
          }
          return false;
        }

        VADisplay candidate_display = libva.vaGetDisplayDRM(fd);
        if (!candidate_display) {
          close(fd);
          return false;
        }

        int major = 0;
        int minor = 0;
        const VAStatus status = libva.vaInitialize(candidate_display, &major, &minor);
        if (status != VA_STATUS_SUCCESS) {
          const char* error = libva.vaErrorStr ? libva.vaErrorStr(status) : "unknown error";
          openmedia::log(OM_CATEGORY_HARDWARE, OM_LEVEL_WARNING, "[VAAPI] vaInitialize failed for {}: {}", path, error);
          close(fd);
          return false;
        }

        drm_fd = fd;
        display = candidate_display;
        owns_display = true;
        openmedia::log(OM_CATEGORY_HARDWARE, OM_LEVEL_INFO, "[VAAPI] Initialized {} with VA-API {}.{}", path, major, minor);
        return true;
      };

      if (init.drm_fd > 0) {
        const int fd = dup(init.drm_fd);
        if (fd < 0) {
          openmedia::log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "[VAAPI] Failed to duplicate caller-provided DRM fd");
          return false;
        }

        VADisplay candidate_display = libva.vaGetDisplayDRM(fd);
        if (!candidate_display) {
          close(fd);
          openmedia::log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "[VAAPI] vaGetDisplayDRM failed for caller-provided DRM fd");
          return false;
        }

        int major = 0;
        int minor = 0;
        const VAStatus status = libva.vaInitialize(candidate_display, &major, &minor);
        if (status != VA_STATUS_SUCCESS) {
          const char* error = libva.vaErrorStr ? libva.vaErrorStr(status) : "unknown error";
          close(fd);
          openmedia::log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "[VAAPI] vaInitialize failed for caller-provided DRM fd: {}", error);
          return false;
        }

        drm_fd = fd;
        display = candidate_display;
        owns_display = true;
        openmedia::log(OM_CATEGORY_HARDWARE, OM_LEVEL_INFO, "[VAAPI] Initialized caller-provided DRM fd with VA-API {}.{}", major, minor);
        return true;
      }

      std::array<char, 32> path = {};
      for (int i = 128; i < 144; ++i) {
        const int len = snprintf(path.data(), path.size(), "/dev/dri/renderD%d", i);
        if (len > 0 && tryOpenDevice({path.data(), static_cast<size_t>(len)})) return true;
      }

      for (int i = 0; i < 16; ++i) {
        const int len = snprintf(path.data(), path.size(), "/dev/dri/card%d", i);
        if (len > 0 && tryOpenDevice({path.data(), static_cast<size_t>(len)})) return true;
      }

      openmedia::log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "[VAAPI] No usable DRM render/card device found under /dev/dri");
      return false;
    }

    if (!display) {
      openmedia::log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "[VAAPI] VADisplay is null");
      return false;
    }

    return true;
  }
};
