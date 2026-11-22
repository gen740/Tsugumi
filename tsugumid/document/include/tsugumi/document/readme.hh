// definitions/<id>/README.md: what a Card is for, in Markdown.
//
// An optional front matter holds one-line `key: value` strings between two
// `---` lines; for now the only key is `description`. The rest of the file is
// the body. The Card id is the Card's only name: a README has no title field.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace tsugumi::readme {

struct Readme {
  std::map<std::string, std::string> fields;  // front matter, by key
  std::string body;
};

// The keys a front matter may hold.
auto known_key(std::string_view key) -> bool;

// Parses a README.md; nullopt with `error` set when the front matter is not
// one-line `key: value` strings of known keys.
auto parse(std::string_view text, std::string* error = nullptr)
    -> std::optional<Readme>;
// The text of a README: the front matter when it has fields, then the body.
auto print(const Readme& readme) -> std::string;

}  // namespace tsugumi::readme
