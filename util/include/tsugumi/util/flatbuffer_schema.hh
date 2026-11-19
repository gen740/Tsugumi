#pragma once

#include <flatbuffers/idl.h>
#include <flatbuffers/reflection_generated.h>

#include <cstdint>
#include <string>

namespace tsugumi::flatbuffer_schema {

// BFBS stores integer defaults in signed slots. Restore the unsigned text.
// The document parser also needs the original field deprecation flags.
inline auto restore_reflection_fields(flatbuffers::Parser& parser,
                                       const reflection::Schema& schema,
                                       bool restore_deprecated) -> void {
  for (auto* object : parser.structs_.vec) {
    const auto name =
        object->defined_namespace->GetFullyQualifiedName(object->name);
    const auto* reflected = schema.objects()->LookupByKey(name.c_str());
    if (!reflected) continue;
    for (auto* field : object->fields.vec) {
      const auto* source =
          reflected->fields()->LookupByKey(field->name.c_str());
      if (!source) continue;
      if (restore_deprecated) field->deprecated = source->deprecated();
      if (!source->optional() &&
          source->type()->base_type() == reflection::BaseType::ULong)
        field->value.constant =
            std::to_string(static_cast<uint64_t>(source->default_integer()));
    }
  }
}

}  // namespace tsugumi::flatbuffer_schema
