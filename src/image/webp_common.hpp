#pragma once

#include <memory>
#include <openmedia/codec_api.hpp>
#include <openmedia/format_api.hpp>

namespace openmedia {

auto createWEBPEncoder() -> std::unique_ptr<Encoder>;

auto createWEBPMuxer() -> std::unique_ptr<Muxer>;

} // namespace openmedia
