#include "io_util.hpp"

namespace openmedia {

auto RandomRead::read(size_t pos, void* dst, size_t n) -> bool {
  const size_t total = size();
  if (n > total || pos > total - n) return false;
  if (n == 0) return true;

  if (!memory_.empty()) {
    memcpy(dst, memory_.data() + pos, n);
    return true;
  }
  if (!input_) return false;

  uint8_t* out = static_cast<uint8_t*>(dst);

  if (isInCache(pos, n)) {
    memcpy(out, cache_.data() + (pos - cache_pos_), n);
    return true;
  }

  if (n <= DEFAULT_CACHE_SIZE) {
    if (!loadCache(pos)) return false;
    const size_t offset = pos - cache_pos_;
    if (cache_size_ < n) return false;
    memcpy(out, cache_.data() + offset, n);
    return true;
  }

  invalidateCache();
  if (!input_->seek(pos, Whence::BEG)) return false;
  const size_t got = input_->read(std::span<uint8_t>(out, n));
  return got == n;
}

}
