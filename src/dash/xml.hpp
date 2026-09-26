#pragma once

#include <openmedia/error.h>
#include <openmedia/result.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openmedia::dash::xml {

// A tree rather than a pull parser: an MPD declares SegmentTemplate, BaseURL and half
// its attributes at one level for the levels below to inherit, and a tree answers that
// by walking up. Names point into the document; values and text are entity-decoded, so
// they have to be owned.
struct Node {
  std::string_view name; // namespace prefix stripped
  std::vector<std::pair<std::string_view, std::string>> attributes;
  std::string text; // trimmed
  std::vector<Node> children;

  auto attribute(std::string_view key) const -> std::optional<std::string_view>;
  auto child(std::string_view child_name) const -> const Node*;
  auto childrenNamed(std::string_view child_name) const -> std::vector<const Node*>;
};

// The tree points into `document`, which must outlive it.
auto parse(std::string_view document) -> Result<Node, OMError>;

} // namespace openmedia::dash::xml
