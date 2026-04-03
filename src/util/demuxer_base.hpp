#pragma once

#include <memory>
#include <openmedia/format_api.hpp>
#include <openmedia/io.hpp>
#include <formats.hpp>
#include <vector>

namespace openmedia {

class BaseDemuxer : public Demuxer {
protected:
  std::unique_ptr<InputStream> input_;
  std::vector<Track> tracks_;

public:
  BaseDemuxer() = default;
  ~BaseDemuxer() override = default;

  void close() override {
    input_.reset();
    tracks_.clear();
  }

  auto tracks() const -> const std::vector<Track>& override {
    return tracks_;
  }
};

class BaseMuxer : public Muxer {
protected:
  std::unique_ptr<OutputStream> output_;
  std::vector<Track> tracks_;
  bool opened_ = false;
  bool finalized_ = false;

public:
  BaseMuxer() = default;
  ~BaseMuxer() override = default;

  void close() override {
    if (opened_ && !finalized_) {
      finalize();
    }
    output_.reset();
    tracks_.clear();
    opened_ = false;
    finalized_ = false;
  }

  auto tracks() const -> const std::vector<Track>& {
    return tracks_;
  }
};

} // namespace openmedia
