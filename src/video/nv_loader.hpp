#pragma once

#include <dynlink_loader.h>
#include <memory>
#include <mutex>
#include <openmedia/log.hpp>

namespace openmedia {

class NVLoader {
public:
  static auto getInstance() -> NVLoader& {
    static NVLoader instance;
    return instance;
  }

  auto cuda() -> CudaFunctions* { return cuda_funcs_; }
  auto cuvid() -> CuvidFunctions* { return cuvid_funcs_; }
  auto nvenc() -> NvencFunctions* { return nvenc_funcs_; }

  auto load() -> bool {
    std::lock_guard<std::mutex> lock(mutex_);
    if (loaded_) return true;

    if (cuda_load_functions(&cuda_funcs_, nullptr) < 0) {
      openmedia::log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "Failed to load CUDA functions");
      return false;
    }

    if (cuvid_load_functions(&cuvid_funcs_, nullptr) < 0) {
      openmedia::log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "Failed to load CUVID functions");
    }

    if (nvenc_load_functions(&nvenc_funcs_, nullptr) < 0) {
      openmedia::log(OM_CATEGORY_HARDWARE, OM_LEVEL_ERROR, "Failed to load NVENC functions");
    }

    loaded_ = true;
    return true;
  }

private:
  NVLoader() = default;
  ~NVLoader() {
    if (nvenc_funcs_) nvenc_free_functions(&nvenc_funcs_);
    if (cuvid_funcs_) cuvid_free_functions(&cuvid_funcs_);
    if (cuda_funcs_) cuda_free_functions(&cuda_funcs_);
  }

  CudaFunctions* cuda_funcs_ = nullptr;
  CuvidFunctions* cuvid_funcs_ = nullptr;
  NvencFunctions* nvenc_funcs_ = nullptr;
  bool loaded_ = false;
  std::mutex mutex_;
};

} // namespace openmedia
