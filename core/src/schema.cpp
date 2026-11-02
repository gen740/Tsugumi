#include "schema.hpp"

#include <stdexcept>

namespace tsugumi::schema {

namespace detail {

std::string describe(const hosq::Error& e) {
  std::string msg(e.detail);
  if (msg.empty()) msg = "invalid value";
  std::string path;
  for (const auto& seg : e.path) {
    if (seg.has_name) path += "." + std::string(seg.name);
    else path += "[" + std::to_string(seg.index) + "]";
  }
  if (!path.empty()) msg = path.substr(path[0] == '.' ? 1 : 0) + ": " + msg;
  return msg;
}

std::optional<DecodeError> validate(std::string_view text, hosq::json::GenericDom<>& dom) {
  auto e = dom.assign(std::string(text));
  if (e) return DecodeError{describe(e), e.offset};
  return std::nullopt;
}

}  // namespace detail

std::string text(const hosq::json::GenericDom<>& value) { return std::string(value[""].source()); }

hosq::json::GenericDom<> capture(std::string_view json_text) {
  hosq::json::GenericDom<> dom;
  if (auto e = dom.assign(std::string(json_text))) throw std::runtime_error("invalid JSON value: " + detail::describe(e));
  return dom;
}

hosq::json::GenericDom<> string_value(const std::string& s) {
  v::String str;
  str.value() = s;
  return capture(encode(str));
}

std::string object_text(const std::vector<std::pair<std::string, const hosq::json::GenericDom<>*>>& members) {
  v::Mapping<v::String, Json> m;
  for (const auto& [k, val] : members) m.value().emplace_back(k, *val);
  return encode(m);
}

bool is_null(const hosq::json::GenericDom<>& value) {
  v::Null n;
  return !value[""].decode_into(n);
}

}  // namespace tsugumi::schema
