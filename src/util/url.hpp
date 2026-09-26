#pragma once

#include <string>
#include <string_view>

namespace openmedia {

// RFC 3986 reference resolution, as much of it as a manifest or a playlist needs. A
// one-letter scheme is read as a Windows drive letter, so `C:/media/x.mpd` stays a path.
auto resolveUrl(std::string_view base, std::string_view reference) -> std::string;

} // namespace openmedia
