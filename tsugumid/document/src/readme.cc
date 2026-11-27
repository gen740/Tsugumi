#include "tsugumi/document/readme.hh"

namespace tsugumi::readme {

auto known_key(std::string_view key) -> bool {
  return key == "title" || key == "description";
}

auto parse(std::string_view text, std::string* error) -> std::optional<Readme> {
  auto fail = [&](std::string message) -> std::optional<Readme> {
    if (error) *error = std::move(message);
    return std::nullopt;
  };
  Readme out;
  constexpr std::string_view fence = "---\n";
  if (!text.starts_with(fence)) return fail("front matter is required");
  size_t at = fence.size();
  size_t line_number = 1;
  while (true) {
    ++line_number;
    const auto end = text.find('\n', at);
    if (end == std::string_view::npos)
      return fail("the front matter has no closing --- line");
    const auto line = text.substr(at, end - at);
    at = end + 1;
    if (line == "---") break;
    const auto where = "front matter line " + std::to_string(line_number);
    const auto colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0)
      return fail(where + ": expected key: value");
    const auto key = line.substr(0, colon);
    if (!known_key(key)) {
      // Quote the key as written so stray whitespace is visible.
      const auto quoted = "\"" + std::string(key) + "\"";
      const auto first = key.find_first_not_of(" \t");
      const auto last = key.find_last_not_of(" \t");
      const auto trimmed = first == std::string_view::npos
                               ? std::string_view{}
                               : key.substr(first, last - first + 1);
      if (known_key(trimmed)) {
        const bool leading = first > 0;
        const bool trailing = last + 1 < key.size();
        return fail(where + ": key " + quoted + " has " +
                    (leading && trailing ? "leading and trailing whitespace"
                     : leading           ? "leading whitespace"
                                         : "trailing whitespace") +
                    "; write \"" + std::string(trimmed) +
                    ":\" at the start of the line");
      }
      return fail(where + ": unknown key " + quoted +
                  " (expected \"title\" or \"description\")");
    }
    auto value = line.substr(colon + 1);
    if (!value.empty()) {
      if (!value.starts_with(' '))
        return fail(where + ": expected a space after the colon");
      value.remove_prefix(1);
    }
    if (value.find('\r') != std::string_view::npos)
      return fail(where + ": a value is one line");
    if (!out.fields.emplace(std::string(key), std::string(value)).second)
      return fail(where + ": duplicate key \"" + std::string(key) + "\"");
  }
  for (const auto* key : {"title", "description"})
    if (!out.fields.contains(key))
      return fail(std::string("front matter needs ") + key);
  if (out.fields.at("title").find_first_not_of(" \t") == std::string::npos)
    return fail("front matter title must be non-empty");
  if (text.substr(at).starts_with('\n')) ++at;
  out.body = std::string(text.substr(at));
  return out;
}

auto print(const Readme& readme) -> std::string {
  std::string out = "---\n";
  for (const auto* key : {"title", "description"}) {
    const auto field = readme.fields.find(key);
    out += std::string(key) + ": ";
    if (field != readme.fields.end()) out += field->second;
    out += '\n';
  }
  out += "---\n";
  if (!readme.body.empty()) out += '\n';
  return out + readme.body;
}

}  // namespace tsugumi::readme
