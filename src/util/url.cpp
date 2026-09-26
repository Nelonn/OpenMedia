#include <util/url.hpp>

#include <cctype>
#include <string_view>
#include <vector>

namespace openmedia {
namespace {

auto hasScheme(std::string_view url) -> size_t {
  size_t i = 0;
  if (i >= url.size() || !std::isalpha(static_cast<unsigned char>(url[i]))) return 0;
  ++i;
  while (i < url.size() &&
         (std::isalnum(static_cast<unsigned char>(url[i])) || url[i] == '+' || url[i] == '-' ||
          url[i] == '.')) {
    ++i;
  }
  // A one-letter scheme is a Windows drive letter far more often than it is a
  // URL scheme, and reading `C:/media/x.mpd` as one would break every local path.
  if (i < 2 || i >= url.size() || url[i] != ':') return 0;
  return i + 1;
}

auto removeDotSegments(std::string_view path) -> std::string {
  std::vector<std::string_view> out;
  const bool absolute = path.starts_with('/');
  const bool trailing_slash = path.size() > 1 && path.ends_with('/');

  size_t i = 0;
  while (i < path.size()) {
    const size_t slash = path.find('/', i);
    const std::string_view segment =
        path.substr(i, slash == std::string_view::npos ? std::string_view::npos : slash - i);
    if (segment == "..") {
      if (!out.empty()) out.pop_back();
    } else if (segment != "." && !segment.empty()) {
      out.push_back(segment);
    }
    if (slash == std::string_view::npos) break;
    i = slash + 1;
  }

  std::string result;
  if (absolute) result.push_back('/');
  for (size_t n = 0; n < out.size(); ++n) {
    if (n != 0) result.push_back('/');
    result.append(out[n]);
  }
  if (trailing_slash && !result.empty() && result.back() != '/') result.push_back('/');
  return result;
}

/** Splits `url` into the part that any path is relative to -- scheme and
 * authority -- and the path itself. */
void splitUrl(std::string_view url, std::string_view& prefix, std::string_view& path) {
  const size_t scheme = hasScheme(url);
  if (scheme != 0 && url.compare(scheme, 2, "//") == 0) {
    const size_t authority_end = url.find('/', scheme + 2);
    if (authority_end == std::string_view::npos) {
      prefix = url;
      path = {};
    } else {
      prefix = url.substr(0, authority_end);
      path = url.substr(authority_end);
    }
    return;
  }
  prefix = url.substr(0, scheme);
  path = url.substr(scheme);
}

/** The directory part of a path, query and fragment first discarded: a segment
 * named beside `.../dash/stream.mpd?token=x` lives in `.../dash/`. */
auto directoryOf(std::string_view path) -> std::string_view {
  const size_t cut = path.find_first_of("?#");
  if (cut != std::string_view::npos) path = path.substr(0, cut);
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string_view::npos ? std::string_view {} : path.substr(0, slash + 1);
}

} // namespace

auto resolveUrl(std::string_view base, std::string_view reference) -> std::string {
  if (reference.empty()) return std::string(base);

  std::string_view base_prefix;
  std::string_view base_path;
  splitUrl(base, base_prefix, base_path);

  if (hasScheme(reference) != 0) { // already absolute
    std::string_view prefix;
    std::string_view path;
    splitUrl(reference, prefix, path);
    return std::string(prefix) + removeDotSegments(path);
  }
  if (reference.starts_with("//")) { // inherits only the scheme
    const size_t scheme = hasScheme(base);
    std::string_view prefix;
    std::string_view path;
    splitUrl(std::string_view(reference), prefix, path);
    (void)prefix;
    const size_t authority_end = reference.find('/', 2);
    if (authority_end == std::string_view::npos) {
      return std::string(base.substr(0, scheme)) + std::string(reference);
    }
    return std::string(base.substr(0, scheme)) + std::string(reference.substr(0, authority_end)) +
           removeDotSegments(reference.substr(authority_end));
  }
  if (reference.starts_with('/')) { // keeps the authority, replaces the path
    return std::string(base_prefix) + removeDotSegments(reference);
  }

  std::string merged(directoryOf(base_path));
  merged.append(reference);
  // Dot removal would also collapse the query, and `..` inside one is not a path
  // segment; a query only ever appears at the end, so split it off first.
  const size_t query = merged.find_first_of("?#");
  if (query == std::string::npos) {
    return std::string(base_prefix) + removeDotSegments(merged);
  }
  return std::string(base_prefix) + removeDotSegments(std::string_view(merged).substr(0, query)) +
         merged.substr(query);
}


} // namespace openmedia
