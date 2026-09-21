#pragma once

#include <memory>
#include <openmedia/codec_api.hpp>
#include <openmedia/format_api.hpp>

namespace openmedia {

auto createPNGEncoder() -> std::unique_ptr<Encoder>;

auto createPNGMuxer() -> std::unique_ptr<Muxer>;

} // namespace openmedia
