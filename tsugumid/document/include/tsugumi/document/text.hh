// The project documents' FlatBuffers schemas (protocol/*_document.fbs) and
// their JSON text form. The FlatBuffers parser is the schema check.
#pragma once

#include <string>
#include <string_view>
#include <type_traits>

#include "tsugumi/protocol/card_document_generated.h"
#include "tsugumi/protocol/comments_document_generated.h"
#include "tsugumi/protocol/layout_document_generated.h"
#include "tsugumi/protocol/project_document_generated.h"
#include "tsugumi/protocol/project_lock_generated.h"

namespace tsugumi::document_text {

namespace D = Tsugumi::Document;

enum class Schema { Project, Card, Comments, Layout, Lock };

// Parses JSON text into a FlatBuffer of the schema. Throws
// DocumentError(invalid_document) naming `path`, the line and the column.
auto parse(Schema schema, std::string_view path, std::string_view text)
    -> std::string;
// The text form of a buffer: strict JSON, two-space indent, fields in schema
// order, empty vectors and tables as [] and {}, and a final newline. Equal
// buffers give equal text, so saved files diff by field.
auto print(Schema schema, std::string_view buffer) -> std::string;
// Parses and prints: the canonical text of a document.
auto canonical(Schema schema, std::string_view path, std::string_view text)
    -> std::string;
// Verifies a buffer of the schema.
auto verify(Schema schema, std::string_view buffer) -> bool;

template <class T>
auto root(std::string_view buffer) -> const T& {
  return *flatbuffers::GetRoot<T>(buffer.data());
}

template <class T>
auto pack(const typename T::NativeTableType& object) -> std::string {
  flatbuffers::FlatBufferBuilder b;
  if constexpr (std::is_same_v<T, Tsugumi::Layout::Layout>)
    b.Finish(T::Pack(b, &object), Tsugumi::Layout::LayoutIdentifier());
  else
    b.Finish(T::Pack(b, &object));
  return {reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()};
}

// A string field's value, or "" when absent.
inline auto str(const flatbuffers::String* s) -> std::string {
  return s ? s->str() : std::string();
}

}  // namespace tsugumi::document_text
