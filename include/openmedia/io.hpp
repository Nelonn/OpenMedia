#pragma once

#include <cstdint>
#include <memory>
#include <openmedia/macro.h>
#include <span>
#include <string>

namespace openmedia {

enum class Whence : uint8_t {
  BEG,
  CUR,
  END,
};

class OPENMEDIA_ABI InputStream {
public:
  virtual ~InputStream() = default;

  /**
   * Performs a blocking read from the stream into the provided buffer.
   * Returns the number of bytes actually read.
   * If the returned value is less than buffer.size(),
   * the end of the stream has been reached.
   */
  virtual auto read(std::span<uint8_t> buffer) -> size_t = 0;

  /**
   * Returns the current position in the stream.
   */
  virtual auto tell() const -> int64_t = 0;

  /**
   * Returns true if the end of the stream has been reached.
   */
  virtual auto isEOF() const -> bool = 0;

  /**
   * Returns true if the stream is open and valid for operations.
   */
  virtual auto isValid() const -> bool = 0;

  /**
   * Skips forward by a certain number of bytes.
   */
  virtual auto skip(size_t bytes) -> bool = 0;

  /**
   * Returns true if the stream supports seeking to arbitrary positions.
   */
  virtual auto canSeek() const -> bool = 0;

  /**
   * Seeks to a specific position.
   */
  virtual auto seek(int64_t pos, Whence whence) -> bool = 0;

  /**
   * Returns the total size of the stream, if
   * not available returns -1
   */
  virtual auto size() const -> int64_t = 0;

  static auto createFileStream(const std::string& path) noexcept -> std::unique_ptr<InputStream>;

  static auto createMemoryStream(std::span<const uint8_t> data) noexcept -> std::unique_ptr<InputStream>;
};

class OPENMEDIA_ABI OutputStream {
public:
  virtual ~OutputStream() = default;

  /**
   * Writes data to the stream.
   * Returns the number of bytes actually written.
   */
  virtual auto write(std::span<const uint8_t> data) -> size_t = 0;

  /**
   * Returns the current position in the stream.
   */
  virtual auto tell() const -> int64_t = 0;

  /**
   * Returns true if the stream is open and valid for operations.
   */
  virtual auto isValid() const -> bool = 0;

  /**
   * Returns true if the stream supports seeking to arbitrary positions.
   */
  virtual auto canSeek() const -> bool = 0;

  /**
   * Seeks to a specific position.
   */
  virtual auto seek(int64_t pos, Whence whence) -> bool = 0;

  /**
   * Flushes any buffered data to the underlying stream.
   */
  virtual auto flush() -> bool = 0;

  static auto createFileStream(const std::string& path) noexcept -> std::unique_ptr<OutputStream>;
};

} // namespace openmedia
