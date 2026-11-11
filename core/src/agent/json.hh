#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "schema.hh"

namespace tsugumi::agent::json {

using Members = std::map<std::string, std::string>;

inline auto Quote(std::string_view value) -> std::string {
  schema::v::String v;
  v.value() = value;
  return schema::encode(v);
}

inline auto String(std::string_view raw, std::string fallback = {})
    -> std::string {
  schema::v::String v;
  if (schema::decode(raw, v))
    return fallback;
  return v.value();
}

inline auto Object(std::string_view raw) -> Members {
  schema::v::Mapping<schema::v::String, schema::Json> v;
  if (auto error = schema::decode(raw, v))
    throw std::runtime_error(error->message);
  Members out;
  for (const auto &[key, value] : v.value())
    out.emplace(key, schema::text(value));
  return out;
}

inline auto Array(std::string_view raw) -> std::vector<std::string> {
  schema::v::Array<schema::Json> v;
  if (auto error = schema::decode(raw, v))
    throw std::runtime_error(error->message);
  std::vector<std::string> out;
  for (const auto &value : v.value())
    out.push_back(schema::text(value));
  return out;
}

inline auto Encode(const Members &values) -> std::string {
  std::string out = "{";
  for (const auto &[key, value] : values) {
    if (out.size() > 1)
      out += ',';
    out += Quote(key) + ':' + value;
  }
  return out + '}';
}

inline auto Encode(const std::vector<std::string> &values) -> std::string {
  std::string out = "[";
  for (const auto &value : values) {
    if (out.size() > 1)
      out += ',';
    out += value;
  }
  return out + ']';
}

inline auto Get(const Members &values, std::string_view key,
                std::string fallback = "null") -> std::string {
  auto found = values.find(std::string(key));
  return found == values.end() ? fallback : found->second;
}

inline auto Text(const Members &values, std::string_view key,
                 std::string fallback = {}) -> std::string {
  return String(Get(values, key), std::move(fallback));
}

inline auto Bool(std::string_view raw, bool fallback = false) -> bool {
  schema::v::Bool value;
  return schema::decode(raw, value) ? fallback : value.value();
}

inline auto UInt(std::string_view raw, uint64_t fallback = 0) -> uint64_t {
  schema::v::Uint64 value;
  return schema::decode(raw, value) ? fallback : value.value();
}

} // namespace tsugumi::agent::json
