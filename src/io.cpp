#include <algorithm>
#include <codecvt>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <openmedia/io.hpp>
#include <thread>

namespace openmedia {

#if defined(_WIN32)
static auto utf8_to_wstring(const std::string& str) -> std::wstring {
  std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
  return converter.from_bytes(str);
}
#endif

class FileInputStream final : public InputStream {
private:
  std::ifstream file_stream_;
  int64_t size_;
  bool is_open_;

public:
  explicit FileInputStream(const std::string& filename)
      : size_(0), is_open_(false) {
#if defined(_WIN32)
    file_stream_.open(utf8_to_wstring(filename), std::ios::in | std::ios::binary);
#else
    file_stream_.open(filename, std::ios::in | std::ios::binary);
#endif
    if (file_stream_.is_open()) {
      file_stream_.seekg(0, std::ios::end);
      size_ = static_cast<int64_t>(file_stream_.tellg());
      file_stream_.seekg(0, std::ios::beg);
      is_open_ = true;
    }
  }

  ~FileInputStream() override {
    if (is_open_) {
      file_stream_.close();
    }
  }

  auto read(std::span<uint8_t> buffer) -> size_t override {
    if (!is_open_) {
      return 0;
    }

    file_stream_.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
    return static_cast<size_t>(file_stream_.gcount());
  }

  auto tell() const -> int64_t override {
    if (!is_open_) return -1;
    return static_cast<int64_t>(const_cast<std::ifstream&>(file_stream_).tellg());
  }

  auto isEOF() const -> bool override {
    return !is_open_ || file_stream_.eof();
  }

  auto isValid() const -> bool override {
    return is_open_;
  }

  auto skip(size_t bytes) -> bool override {
    if (bytes == 0) return true;
    return seek(bytes, Whence::CUR);
  }

  auto canSeek() const -> bool override {
    return true;
  }

  auto seek(int64_t pos, Whence whence) -> bool override {
    if (!is_open_) return false;

    std::ios_base::seekdir dir;
    switch (whence) {
      case Whence::BEG:
        dir = std::ios::beg;
        break;
      case Whence::CUR:
        dir = std::ios::cur;
        break;
      case Whence::END:
        dir = std::ios::end;
        break;
      default: return false;
    }

    file_stream_.clear();
    file_stream_.seekg(pos, dir);
    return !file_stream_.fail();
  }

  auto size() const -> int64_t override {
    return size_;
  }
};

class MemoryInputStream final : public InputStream {
private:
  std::span<const uint8_t> data_;
  size_t pos_;

public:
  MemoryInputStream(std::span<const uint8_t> data)
      : data_(data), pos_(0) {}

  auto read(std::span<uint8_t> buffer) -> size_t override {
    size_t available = data_.size() - pos_;
    size_t to_read = std::min(buffer.size(), available);
    if (to_read > 0) {
      std::memcpy(buffer.data(), data_.data() + pos_, to_read);
      pos_ += to_read;
    }
    return to_read;
  }

  auto tell() const -> int64_t override { return pos_; }

  auto isEOF() const -> bool override { return pos_ >= data_.size(); }

  auto isValid() const -> bool override { return true; }

  auto skip(size_t bytes) -> bool override {
    return seek(bytes, Whence::BEG);
  }

  auto canSeek() const -> bool override {
    return true;
  }

  auto seek(int64_t pos, Whence whence) -> bool override {
    int64_t new_pos = 0;
    switch (whence) {
      case Whence::BEG:
        new_pos = static_cast<size_t>(pos);
        break;
      case Whence::CUR:
        new_pos = static_cast<int64_t>(pos_) + pos;
        break;
      case Whence::END:
        new_pos = static_cast<int64_t>(data_.size()) + pos;
        break;
      default: return false;
    }

    if (new_pos < 0 || new_pos > static_cast<int64_t>(data_.size())) return false;
    pos_ = static_cast<size_t>(new_pos);
    return true;
  }

  auto size() const -> int64_t override {
    return data_.size();
  }
};

auto InputStream::createFileStream(const std::string& path) noexcept -> std::unique_ptr<InputStream> {
  auto stream = std::make_unique<FileInputStream>(path);
  return stream->isValid() ? std::move(stream) : nullptr;
}

auto InputStream::createMemoryStream(std::span<const uint8_t> data) noexcept -> std::unique_ptr<InputStream> {
  if (data.empty()) return nullptr;
  return std::make_unique<MemoryInputStream>(data);
}

class FileOutputStream final : public OutputStream {
private:
  std::ofstream file_stream_;
  bool is_open_;

public:
  explicit FileOutputStream(const std::string& filename)
      : is_open_(false) {
#if defined(_WIN32)
    file_stream_.open(utf8_to_wstring(filename), std::ios::out | std::ios::binary | std::ios::trunc);
#else
    file_stream_.open(filename, std::ios::out | std::ios::binary | std::ios::trunc);
#endif
    if (file_stream_.is_open()) {
      is_open_ = true;
    }
  }

  ~FileOutputStream() override {
    if (is_open_) {
      file_stream_.close();
    }
  }

  auto write(std::span<const uint8_t> data) -> size_t override {
    if (!is_open_ || data.empty()) {
      return 0;
    }

    file_stream_.write(reinterpret_cast<const char*>(data.data()), data.size());
    return file_stream_.good() ? data.size() : 0;
  }

  auto tell() const -> int64_t override {
    if (!is_open_) return -1;
    return static_cast<int64_t>(const_cast<std::ofstream&>(file_stream_).tellp());
  }

  auto isValid() const -> bool override {
    return is_open_ && file_stream_.good();
  }

  auto canSeek() const -> bool override {
    return true;
  }

  auto seek(int64_t pos, Whence whence) -> bool override {
    if (!is_open_) return false;

    std::ios_base::seekdir dir;
    switch (whence) {
      case Whence::BEG:
        dir = std::ios::beg;
        break;
      case Whence::CUR:
        dir = std::ios::cur;
        break;
      case Whence::END:
        dir = std::ios::end;
        break;
      default: return false;
    }

    file_stream_.clear();
    file_stream_.seekp(pos, dir);
    return !file_stream_.fail();
  }

  auto flush() -> bool override {
    if (!is_open_) return false;
    file_stream_.flush();
    return file_stream_.good();
  }
};

auto OutputStream::createFileStream(const std::string& path) noexcept -> std::unique_ptr<OutputStream> {
  auto stream = std::make_unique<FileOutputStream>(path);
  return stream->isValid() ? std::move(stream) : nullptr;
}

} // namespace openmedia
