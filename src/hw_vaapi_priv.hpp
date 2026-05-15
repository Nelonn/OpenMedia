#pragma once

#include <openmedia/hw_vaapi.h>
#include <video/vaapi_loader.hpp>
#include <fcntl.h>
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
        return false;
      }
    }

    if (init.display) {
      display = init.display;
    } else {
      // Try to open a DRM device
      drm_fd = open("/dev/dri/renderD128", O_RDWR);
      if (drm_fd < 0) {
        drm_fd = open("/dev/dri/card0", O_RDWR);
      }
      
      if (drm_fd < 0) return false;

      display = libva.vaGetDisplayDRM(drm_fd);
      if (!display) return false;

      int major, minor;
      VAStatus status = libva.vaInitialize(display, &major, &minor);
      if (status != VA_STATUS_SUCCESS) {
        close(drm_fd);
        drm_fd = -1;
        return false;
      }
      owns_display = true;
    }

    return true;
  }
};
