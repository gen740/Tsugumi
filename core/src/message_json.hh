// A FlatBuffer message as JSON, written from its parsed schema, for the Cards
// that read their inputs as JSON (bash, R, Julia) and what a Logger Card shows.
// This is the output of flatbuffers::GenText with strict_json, natural_utf8 and
// output_default_scalars_in_json, indented by the parser's indent_step (negative:
// one line), except for floats: GenText keeps 12 decimals
// of a double and 6 of a float, with no option, so 1e-20 becomes 0.0. Here they
// are the shortest digits that read back as the same value. tests/cpp compares
// the two on every kind of field. The parser comes from a binary schema, which
// keeps no builtin attributes: no bit_flags, flexbuffer or nested_flatbuffer to
// print here (GenText prints those as numbers and bytes too).
#pragma once

#include <flatbuffers/idl.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tsugumi::message_json {

namespace detail {

namespace fb = flatbuffers;

template <std::floating_point T>
void append_float(std::string& out, T v) {
  if (!std::isfinite(v)) {  // nan, inf: not JSON, and to_chars has more spellings of them
    out += fb::NumToString(v);
    return;
  }
  // Plain decimals where GenText's were readable, also for a whole number
  // ("100000.0", which the plain shortest form would make "1e+05").
  const T size = std::abs(v);
  const bool plain = size == 0 || (size >= T(1e-4) && size < T(1e16));
  char digits[64];
  auto end = std::to_chars(digits, digits + sizeof digits, v, plain ? std::chars_format::fixed : std::chars_format::scientific).ptr;
  out.append(digits, end);
  if (plain && std::string_view(digits, end).find('.') == std::string_view::npos) out += ".0";
}

// Calls f with a value of the C++ type of a scalar BaseType.
template <class F>
void with_scalar_type(fb::BaseType type, F&& f) {
  switch (type) {
#define TSUGUMI_SCALAR(ENUM, IDLTYPE, CTYPE, ...) \
  case fb::BASE_TYPE_##ENUM:                      \
    f(CTYPE{});                                   \
    break;
    FLATBUFFERS_GEN_TYPES_SCALAR(TSUGUMI_SCALAR)
#undef TSUGUMI_SCALAR
    default:
      break;
  }
}

class Writer {
 public:
  Writer(std::string& out, int indent_step) : out_(out), indent_step_(indent_step) {}

  const char* table(const fb::StructDef& def, const fb::Table* t, int indent) {
    out_ += '{';
    const int inner = deeper(indent);
    int written = 0;
    const std::uint8_t* previous = nullptr;  // a union's type field comes just before it
    for (const fb::FieldDef* field : def.fields.vec) {
      const fb::Type& type = field->value.type;
      const fb::voffset_t offset = field->value.offset;
      if (!def.fixed && !t->CheckField(offset) && (!fb::IsScalar(type.base_type) || field->deprecated)) continue;
      if (written++) out_ += ',';
      next_line(inner);
      out_ += '"' + field->name + "\": ";
      if (fb::IsScalar(type.base_type)) {
        scalar_field(*field, t, def.fixed);
      } else if (const char* error = pointer_field(*field, t, def.fixed, inner, previous)) {
        return error;
      }
      previous = def.fixed ? reinterpret_cast<const std::uint8_t*>(t) + offset : t->GetAddressOf(offset);
    }
    next_line(indent);
    out_ += '}';
    return nullptr;
  }

 private:
  int deeper(int indent) const { return indent + std::max(indent_step_, 0); }

  void next_line(int indent) {
    if (indent_step_ < 0) return;
    out_ += '\n';
    out_.append(indent, ' ');
  }

  template <class T>
  void scalar(T v, const fb::Type& type) {
    if constexpr (std::floating_point<T>) {
      append_float(out_, v);
    } else {
      if (fb::IsBool(type.base_type)) {
        out_ += v != 0 ? "true" : "false";
        return;
      }
      if (type.enum_def) {
        if (const fb::EnumVal* named = type.enum_def->ReverseLookup(static_cast<std::int64_t>(v))) {
          out_ += '"' + named->name + '"';
          return;
        }
      }
      out_ += fb::NumToString(v);
    }
  }

  void scalar_field(const fb::FieldDef& field, const fb::Table* t, bool fixed) {
    const fb::Type& type = field.value.type;
    const fb::voffset_t offset = field.value.offset;
    with_scalar_type(type.base_type, [&]<class T>(T) {
      if (fixed) {
        scalar(reinterpret_cast<const fb::Struct*>(t)->GetField<T>(offset), type);
      } else if (field.IsOptional()) {
        if (auto v = t->GetOptional<T, T>(offset)) {
          scalar(*v, type);
        } else {
          out_ += "null";
        }
      } else {
        T fallback{};
        fb::StringToNumber(field.value.constant.c_str(), &fallback);
        scalar(t->GetField<T>(offset, fallback), type);
      }
    });
  }

  const char* pointer_field(const fb::FieldDef& field, const fb::Table* t, bool fixed, int indent, const std::uint8_t* previous) {
    const fb::Type& type = field.value.type;
    const fb::voffset_t offset = field.value.offset;
    if (fixed) return value(reinterpret_cast<const fb::Struct*>(t)->GetStruct<const void*>(offset), type, indent, previous, -1);
    const void* v = fb::IsStruct(type) ? t->GetStruct<const void*>(offset) : t->GetPointer<const void*>(offset);
    return value(v, type, indent, previous, -1);
  }

  const char* value(const void* v, const fb::Type& type, int indent, const std::uint8_t* previous, fb::soffset_t index) {
    switch (type.base_type) {
      case fb::BASE_TYPE_UNION: {
        // The type field holds the member, or a vector of them for a vector of unions.
        std::uint8_t member = *previous;
        if (index >= 0) {
          const auto* types = reinterpret_cast<const fb::Vector<std::uint8_t>*>(previous + fb::ReadScalar<fb::uoffset_t>(previous));
          member = types->Get(static_cast<fb::uoffset_t>(index));
        }
        const fb::EnumVal* named = type.enum_def->ReverseLookup(member, true);
        return named ? value(v, named->union_type, indent, nullptr, -1) : "unknown enum value";
      }
      case fb::BASE_TYPE_STRUCT:
        return table(*type.struct_def, reinterpret_cast<const fb::Table*>(v), indent);
      case fb::BASE_TYPE_STRING: {
        const auto* s = reinterpret_cast<const fb::String*>(v);
        return fb::EscapeString(s->c_str(), s->size(), &out_, false, true) ? nullptr : "string contains non-utf8 bytes";
      }
      case fb::BASE_TYPE_VECTOR: {
        const auto* vec = reinterpret_cast<const fb::Vector<fb::Offset<void>>*>(v);  // only its size and where it starts
        return elements(vec->Data(), vec->size(), type.VectorType(), indent, previous);
      }
      case fb::BASE_TYPE_ARRAY:
        return elements(reinterpret_cast<const std::uint8_t*>(v), type.fixed_length, type.VectorType(), indent, nullptr);
      default:
        return "unknown type";
    }
  }

  // Scalars and structs are stored in place, the rest behind an offset.
  const char* elements(const std::uint8_t* data, std::size_t count, const fb::Type& element, int indent, const std::uint8_t* previous) {
    const int inner = deeper(indent);
    out_ += '[';
    next_line(0);
    for (std::size_t i = 0; i < count; ++i) {
      if (i) {
        out_ += ',';
        next_line(0);
      }
      out_.append(inner, ' ');
      const auto index = static_cast<fb::soffset_t>(i);
      const char* error = nullptr;
      if (fb::IsScalar(element.base_type)) {
        with_scalar_type(element.base_type, [&]<class T>(T) { scalar(fb::ReadScalar<T>(data + i * sizeof(T)), element); });
      } else if (fb::IsStruct(element)) {
        error = value(data + i * element.struct_def->bytesize, element, inner, previous, index);
      } else {
        const std::uint8_t* at = data + i * sizeof(fb::uoffset_t);
        error = value(at + fb::ReadScalar<fb::uoffset_t>(at), element, inner, previous, index);
      }
      if (error) return error;
    }
    next_line(indent);
    out_ += ']';
    return nullptr;
  }

  std::string& out_;
  int indent_step_;
};

}  // namespace detail

// Like flatbuffers::GenText: nullptr, or what went wrong. The parser has its root type set.
inline const char* gen_text(const flatbuffers::Parser& parser, const void* buffer, std::string* out) {
  detail::Writer writer(*out, parser.opts.indent_step);
  const char* error = writer.table(*parser.root_struct_def_, flatbuffers::GetRoot<flatbuffers::Table>(buffer), 0);
  if (!error && parser.opts.indent_step >= 0) *out += '\n';
  return error;
}

}  // namespace tsugumi::message_json
