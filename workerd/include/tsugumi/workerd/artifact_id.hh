#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "tsugumi/util/crypto.hh"

namespace tsugumi {

inline auto parse_artifact_id(std::string_view text)
    -> std::optional<std::string> {
  if (!crypto::is_uuid(text)) return std::nullopt;
  const auto digit = [](char c) {
    if (c <= '9') return c - '0';
    return (c | 32) - 'a' + 10;
  };
  std::string id;
  for (size_t i = 0; i < text.size();) {
    if (text[i] == '-') ++i;
    id.push_back(static_cast<char>((digit(text[i]) << 4) | digit(text[i + 1])));
    i += 2;
  }
  return id;
}

inline auto artifact_id_text(std::string_view id) -> std::string {
  std::string text;
  constexpr std::string_view digits = "0123456789abcdef";
  for (size_t i = 0; i < id.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) text += '-';
    const auto byte = static_cast<unsigned char>(id[i]);
    text += digits[byte >> 4];
    text += digits[byte & 15];
  }
  return text;
}

}  // namespace tsugumi
