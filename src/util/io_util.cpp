#include "io_util.hpp"

namespace openmedia {

auto RandomRead::read(size_t pos, void* dst, size_t n) -> bool {
  if (!input_ || pos + n > stream_size_) return false;
  if (n == 0) return true;

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
