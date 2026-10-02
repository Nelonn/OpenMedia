#pragma once

#include <memory>
#include <openmedia/buffer.hpp>
#include <openmedia/encryption.hpp>

namespace openmedia {

struct OPENMEDIA_ABI Packet {
  std::shared_ptr<Buffer> buffer;
  std::span<uint8_t> bytes;

  // Set when `bytes` is ciphertext that has to go through a decryption module
  // before it can be decoded, and null otherwise -- which covers every packet of
  // an unprotected track and the clear lead of a protected one. Shared rather
  // than owned so that a Packet stays copyable.
  std::shared_ptr<const SampleEncryption> encryption;

  int64_t pts = -1;
  int64_t dts = -1;
  int32_t stream_index = -1;
  uint32_t flags = 0;
  int64_t duration = 0;
  int64_t pos = -1;
  bool is_keyframe = false;

  void allocate(size_t size_in) {
    buffer = BufferPool::getInstance().get(size_in);
    bytes = buffer->bytes();
  }

  void unref() {
    buffer.reset();
    bytes = {};
    encryption.reset();
    pts = -1;
    dts = -1;
    stream_index = -1;
    flags = 0;
    duration = 0;
    pos = -1;
  }
};

} // namespace openmedia
