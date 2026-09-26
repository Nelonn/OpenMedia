#include <dash/xml.hpp>

#include <cstdlib>

namespace openmedia::dash::xml {
namespace {

// An MPD nests Period > AdaptationSet > Representation > SegmentTemplate and
// little else, so a handful of levels is all a well-formed one needs. The limit
// is here to keep a hostile document from recursing the parser off the stack.
constexpr size_t MAX_DEPTH = 64;

auto isSpace(char c) -> bool {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

auto isNameStart(char c) -> bool {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' ||
         static_cast<unsigned char>(c) >= 0x80;
}

auto isNameChar(char c) -> bool {
  return isNameStart(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0x10FFFF) {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// Resolves the five predefined entities and numeric character references. An
// unrecognised entity is left as written: a manifest is not a document to be
// rendered, and a literal ampersand-name inside a URL is likelier than a DTD
// nobody shipped. Entity declarations are not honoured at all, which also takes
// external entity expansion off the table.
auto decodeEntities(std::string_view raw) -> std::string {
  std::string out;
  out.reserve(raw.size());
  for (size_t i = 0; i < raw.size();) {
    if (raw[i] != '&') {
      out.push_back(raw[i++]);
      continue;
    }
    const size_t semi = raw.find(';', i + 1);
    if (semi == std::string_view::npos || semi - i > 12) {
      out.push_back(raw[i++]);
      continue;
    }
    const std::string_view name = raw.substr(i + 1, semi - i - 1);
    if (name == "amp") {
      out.push_back('&');
    } else if (name == "lt") {
      out.push_back('<');
    } else if (name == "gt") {
      out.push_back('>');
    } else if (name == "quot") {
      out.push_back('"');
    } else if (name == "apos") {
      out.push_back('\'');
    } else if (name.size() >= 2 && name[0] == '#') {
      const bool hex = name[1] == 'x' || name[1] == 'X';
      const std::string digits(name.substr(hex ? 2 : 1));
      char* end = nullptr;
      const unsigned long cp = std::strtoul(digits.c_str(), &end, hex ? 16 : 10);
      if (end != nullptr && *end == '\0' && cp != 0) {
        appendUtf8(out, static_cast<uint32_t>(cp));
      } else {
        out.append(raw.substr(i, semi - i + 1));
      }
    } else {
      out.append(raw.substr(i, semi - i + 1));
      i = semi + 1;
      continue;
    }
    i = semi + 1;
  }
  return out;
}

auto trim(std::string_view text) -> std::string_view {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && isSpace(text[begin])) ++begin;
  while (end > begin && isSpace(text[end - 1])) --end;
  return text.substr(begin, end - begin);
}

// `xsi:type` and `dash:BaseURL` name the same things as `type` and `BaseURL`. An
// MPD declares its own default namespace, and the schema gives no element or
// attribute a meaning that depends on a prefix, so the prefix carries no
// information worth the lookups it would cost to honour.
auto stripPrefix(std::string_view name) -> std::string_view {
  const size_t colon = name.rfind(':');
  return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

class Parser {
  std::string_view doc_;
  size_t pos_ = 0;
  bool failed_ = false;

public:
  explicit Parser(std::string_view doc) : doc_(doc) {}

  auto failed() const -> bool { return failed_; }

  auto done() const -> bool { return pos_ >= doc_.size(); }

  auto peek() const -> char { return pos_ < doc_.size() ? doc_[pos_] : '\0'; }

  void skipSpace() {
    while (pos_ < doc_.size() && isSpace(doc_[pos_])) ++pos_;
  }

  auto startsWith(std::string_view prefix) const -> bool {
    return doc_.compare(pos_, prefix.size(), prefix) == 0;
  }

  // Everything that is neither an element nor character data: the declaration,
  // comments, processing instructions, the doctype, and CDATA sections, whose
  // contents do belong to the enclosing element and so are appended to `text`.
  // Returns false when there was nothing of the sort at the cursor.
  auto skipMarkupNoise(std::string& text) -> bool {
    if (startsWith("<!--")) {
      const size_t end = doc_.find("-->", pos_ + 4);
      pos_ = (end == std::string_view::npos) ? doc_.size() : end + 3;
      return true;
    }
    if (startsWith("<?")) {
      const size_t end = doc_.find("?>", pos_ + 2);
      pos_ = (end == std::string_view::npos) ? doc_.size() : end + 2;
      return true;
    }
    if (startsWith("<![CDATA[")) {
      const size_t body = pos_ + 9;
      const size_t end = doc_.find("]]>", body);
      const size_t stop = (end == std::string_view::npos) ? doc_.size() : end;
      text.append(doc_.substr(body, stop - body));
      pos_ = (end == std::string_view::npos) ? doc_.size() : end + 3;
      return true;
    }
    if (startsWith("<!")) {
      // A doctype may carry a bracketed internal subset; step over it as a unit
      // so a `>` inside cannot be mistaken for the end of the declaration.
      size_t i = pos_ + 2;
      int depth = 0;
      while (i < doc_.size()) {
        if (doc_[i] == '[') {
          ++depth;
        } else if (doc_[i] == ']') {
          --depth;
        } else if (doc_[i] == '>' && depth <= 0) {
          break;
        }
        ++i;
      }
      pos_ = (i < doc_.size()) ? i + 1 : doc_.size();
      return true;
    }
    return false;
  }

  auto readName() -> std::string_view {
    const size_t start = pos_;
    if (pos_ < doc_.size() && isNameStart(doc_[pos_])) {
      ++pos_;
      while (pos_ < doc_.size() && isNameChar(doc_[pos_])) ++pos_;
    }
    return doc_.substr(start, pos_ - start);
  }

  void readAttributes(Node& node) {
    while (!failed_) {
      skipSpace();
      const char c = peek();
      if (c == '>' || c == '/' || c == '\0') return;

      const std::string_view name = readName();
      if (name.empty()) { // neither a name nor a terminator: the tag is broken
        failed_ = true;
        return;
      }
      skipSpace();
      if (peek() != '=') {
        // A valueless attribute is not XML. Treat it as empty rather than
        // abandon a manifest that is otherwise perfectly readable.
        node.attributes.emplace_back(stripPrefix(name), std::string());
        continue;
      }
      ++pos_; // '='
      skipSpace();
      const char quote = peek();
      if (quote != '"' && quote != '\'') {
        failed_ = true;
        return;
      }
      ++pos_;
      const size_t start = pos_;
      const size_t end = doc_.find(quote, start);
      if (end == std::string_view::npos) {
        failed_ = true;
        return;
      }
      pos_ = end + 1;
      node.attributes.emplace_back(stripPrefix(name),
                                   decodeEntities(doc_.substr(start, end - start)));
    }
  }

  // Parses one element with the cursor on its `<`, and leaves the cursor just
  // past its end tag.
  auto readElement(size_t depth) -> Node {
    Node node;
    if (depth > MAX_DEPTH) {
      failed_ = true;
      return node;
    }
    ++pos_; // '<'
    node.name = stripPrefix(readName());
    if (node.name.empty()) {
      failed_ = true;
      return node;
    }
    readAttributes(node);
    if (failed_) return node;

    if (peek() == '/') { // empty element
      ++pos_;
      if (peek() != '>') {
        failed_ = true;
        return node;
      }
      ++pos_;
      return node;
    }
    if (peek() != '>') {
      failed_ = true;
      return node;
    }
    ++pos_;

    std::string text;
    while (!failed_) {
      if (done()) { // unterminated element
        failed_ = true;
        return node;
      }
      if (doc_[pos_] != '<') {
        const size_t next = doc_.find('<', pos_);
        const size_t stop = (next == std::string_view::npos) ? doc_.size() : next;
        text.append(doc_.substr(pos_, stop - pos_));
        pos_ = stop;
        continue;
      }
      if (skipMarkupNoise(text)) continue;
      if (startsWith("</")) {
        pos_ += 2;
        const std::string_view name = stripPrefix(readName());
        skipSpace();
        if (peek() != '>' || name != node.name) {
          failed_ = true;
          return node;
        }
        ++pos_;
        break;
      }
      node.children.push_back(readElement(depth + 1));
    }

    node.text = decodeEntities(trim(text));
    return node;
  }

  auto readDocument() -> Node {
    std::string discarded;
    while (!failed_) {
      skipSpace();
      if (done()) break;
      if (doc_[pos_] != '<') { // stray character data outside the root
        ++pos_;
        continue;
      }
      if (skipMarkupNoise(discarded)) continue;
      if (startsWith("</")) break;
      return readElement(0);
    }
    failed_ = true;
    return {};
  }
};

} // namespace

auto Node::attribute(std::string_view key) const -> std::optional<std::string_view> {
  for (const auto& [name, value] : attributes) {
    if (name == key) return std::string_view(value);
  }
  return std::nullopt;
}

auto Node::child(std::string_view child_name) const -> const Node* {
  for (const Node& node : children) {
    if (node.name == child_name) return &node;
  }
  return nullptr;
}

auto Node::childrenNamed(std::string_view child_name) const -> std::vector<const Node*> {
  std::vector<const Node*> found;
  for (const Node& node : children) {
    if (node.name == child_name) found.push_back(&node);
  }
  return found;
}

auto parse(std::string_view document) -> Result<Node, OMError> {
  // A UTF-8 BOM is legal in front of the declaration and is not markup.
  if (document.starts_with("\xEF\xBB\xBF")) document.remove_prefix(3);

  Parser parser(document);
  Node root = parser.readDocument();
  if (parser.failed()) return Err(OM_FORMAT_INVALID_HEADER);
  return Ok(std::move(root));
}

} // namespace openmedia::dash::xml
