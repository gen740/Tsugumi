// Strict, schema-independent JSON values and encoding.
#pragma once

#include <hosq/hosq.hh>
#include <hosq/json/compact_writer.hh>
#include <hosq/json/dom.hh>
#include <hosq/json/fast_reader.hh>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace tsugumi::schema {

namespace t = hosq::types;
namespace v = hosq::types::value;

// Captured arbitrary JSON value (validated, owned, original spelling kept).
using Json = hosq::Elem<hosq::decode::json_dom<> | hosq::store::assign,
                        hosq::encode::json_dom>;
// A number accepted from integer or decimal spellings (layout coordinates).
using Number = hosq::Elem<hosq::scan::scalar | hosq::decode::number<double> |
                              hosq::store::assign,
                          hosq::encode::value>;
template <class S>
using Obj =
    hosq::Elem<hosq::scan::object | hosq::decode::to<S> | hosq::store::assign,
               hosq::encode::value>;
template <hosq::StringLiteral K, class E>
using Opt = t::Optional<K, E>;

// ---- helpers
// ---------------------------------------------------------------------

struct DecodeError {
  std::string message;
  uint64_t offset = 0;
};

namespace detail {
std::optional<DecodeError> validate(std::string_view text,
                                    hosq::json::GenericDom<>& dom);
std::string describe(const hosq::Error& e);
auto encoding_failure(std::string_view type, std::string_view partial,
                      const hosq::Error& error) -> std::string;

template <class S>
constexpr auto type_name() -> std::string_view {
  constexpr std::string_view signature = __PRETTY_FUNCTION__;
  constexpr auto begin = signature.find("S = ") + 4;
  return signature.substr(begin, signature.find_first_of(";]", begin) - begin);
}
}  // namespace detail

// Strict decode of a complete JSON document into schema S.
template <class S>
std::optional<DecodeError> decode(std::string_view text, S& out) {
  hosq::json::GenericDom<> dom;
  if (auto err = detail::validate(text, dom)) return err;
  auto e = dom[""].decode_into(out);
  if (e) return DecodeError{detail::describe(e), e.offset};
  return std::nullopt;
}

// Decodes a captured JSON value into schema/Elem S.
template <class S>
std::optional<DecodeError> decode(const hosq::json::GenericDom<>& value,
                                  S& out) {
  auto e = value[""].decode_into(out);
  if (e) return DecodeError{detail::describe(e), e.offset};
  return std::nullopt;
}

template <class S>
std::string encode(const S& value) {
  std::string out;
  hosq::json::CompactWriter<> w{out};
  if (!hosq::encode_into(value, w))
    throw std::runtime_error(
        detail::encoding_failure(detail::type_name<S>(), out, w.error()));
  return out;
}

// Source text of a captured value (validated JSON, original spelling).
std::string text(const hosq::json::GenericDom<>& value);
// Captures a JSON text as a value. Throws std::runtime_error when invalid.
hosq::json::GenericDom<> capture(std::string_view json_text);
// Captures a string value.
hosq::json::GenericDom<> string_value(const std::string& s);
// JSON object text from (key, captured value) pairs, in the given order.
std::string object_text(
    const std::vector<std::pair<std::string, const hosq::json::GenericDom<>*>>&
        members);
// The form the clients write document files in: two spaces, a final newline.
// `compact` is validated JSON without whitespace between its tokens.
std::string pretty_text(std::string_view compact);
bool is_null(const hosq::json::GenericDom<>& value);

// Value of an optional field.
template <class F>
auto get(const F& field) {
  using T = std::remove_cvref_t<decltype(field->value())>;
  return field->has_value() ? &field->value() : static_cast<const T*>(nullptr);
}

}  // namespace tsugumi::schema
