#include <openmedia/buffer.hpp>

namespace openmedia {

auto BufferPool::getInstance() -> BufferPool& {
  static BufferPool instance;
  return instance;
}

}
