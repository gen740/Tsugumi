// definitions/<id>/README.md: what a Card is for, in Markdown.
//
// Front matter holds a non-empty title and a description, each on one line.
// The rest of the file is the Markdown body.
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
// The text of a README: the front matter, then the body.
auto print(const Readme& readme) -> std::string;

}  // namespace tsugumi::readme
