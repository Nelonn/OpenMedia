#pragma once

#include <openmedia/codec_api.hpp>

#include <memory>

namespace openmedia {

// One decoder and one encoder class serve every VA-API codec; the codec is
// picked from the options at configure time.
auto createVAAPIDecoder() -> std::unique_ptr<Decoder>;
auto createVAAPIEncoder() -> std::unique_ptr<Encoder>;

} // namespace openmedia
