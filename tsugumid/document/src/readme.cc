#include "tsugumi/document/readme.hh"

namespace tsugumi::readme {

auto known_key(std::string_view key) -> bool { return key == "description"; }

auto parse(std::string_view text, std::string* error) -> std::optional<Readme> {
  auto fail = [&](std::string message) -> std::optional<Readme> {
    if (error) *error = std::move(message);
    return std::nullopt;
  };
  Readme out;
  constexpr std::string_view fence = "---\n";
  if (!text.starts_with(fence)) {
    out.body = std::string(text);
    return out;
  }
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
    if (!known_key(key))
      return fail(where + ": unknown key " + std::string(key) +
                  " (the front matter holds only description)");
    auto value = line.substr(colon + 1);
    if (!value.starts_with(' '))
      return fail(where + ": expected a space after the colon");
    value.remove_prefix(1);
    if (value.find('\r') != std::string_view::npos)
      return fail(where + ": a value is one line");
    if (!out.fields.emplace(std::string(key), std::string(value)).second)
      return fail(where + ": duplicate key " + std::string(key));
  }
  if (text.substr(at).starts_with('\n')) ++at;
  out.body = std::string(text.substr(at));
  return out;
}

auto print(const Readme& readme) -> std::string {
  std::string out;
  bool any = false;
  for (const auto& [key, value] : readme.fields) {
    if (value.empty()) continue;
    if (!any) out += "---\n";
    any = true;
    out += key + ": " + value + '\n';
  }
  if (!any && readme.body.starts_with("---\n")) {
    out = "---\n";
    any = true;
  }
  if (any) out += "---\n";
  if (any && !readme.body.empty()) out += '\n';
  out += readme.body;
  return out;
}

}  // namespace tsugumi::readme
