#include "tsugumi/document/schema_id.hh"

#include <flatbuffers/reflection.h>

#include <algorithm>
#include <map>
#include <vector>

#include "tsugumi/document/types.hh"
#include "tsugumi/util/crypto.hh"

namespace tsugumi {
namespace {

class CanonicalSchema {
 public:
  explicit CanonicalSchema(const reflection::Schema& schema)
      : schema_(schema) {}

  auto build() -> std::string {
    collect(*schema_.root_table());
    auto index = [](auto& types) {
      int32_t i = 0;
      for (auto& [_, index] : types) index = i++;
    };
    index(objects_);
    index(enums_);
    std::vector<flatbuffers::Offset<reflection::Object>> objects;
    flatbuffers::Offset<reflection::Object> root;
    for (const auto& [name, _] : objects_) {
      const auto* original = schema_.objects()->LookupByKey(name.c_str());
      const auto object = encode(*original);
      objects.push_back(object);
      if (original == schema_.root_table()) root = object;
    }
    std::vector<flatbuffers::Offset<reflection::Enum>> enums;
    for (const auto& [name, _] : enums_)
      enums.push_back(encode(*schema_.enums()->LookupByKey(name.c_str())));
    const auto object_vector = builder_.CreateVector(objects);
    const auto enum_vector = builder_.CreateVector(enums);
    reflection::FinishSchemaBuffer(
        builder_, reflection::CreateSchema(builder_, object_vector, enum_vector,
                                           0, 0, root));
    return crypto::sha256_hex(
        {reinterpret_cast<const char*>(builder_.GetBufferPointer()),
         builder_.GetSize()});
  }

 private:
  static auto is_object(const reflection::Type& type) -> bool {
    return type.base_type() == reflection::Obj ||
           type.element() == reflection::Obj;
  }

  auto collect(const reflection::Type& type) -> void {
    if (type.index() < 0) return;
    if (is_object(type))
      collect(*schema_.objects()->Get(type.index()));
    else
      collect(*schema_.enums()->Get(type.index()));
  }

  auto collect(const reflection::Object& object) -> void {
    if (!objects_.try_emplace(object.name()->str()).second) return;
    for (const auto* field : *object.fields()) {
      collect(*field->type());
      if (const auto* attrs = field->attributes())
        if (const auto* nested = attrs->LookupByKey("nested_flatbuffer"))
          if (const auto* target =
                  schema_.objects()->LookupByKey(nested->value()->c_str()))
            collect(*target);
    }
  }

  auto collect(const reflection::Enum& enumeration) -> void {
    if (!enums_.try_emplace(enumeration.name()->str()).second) return;
    collect(*enumeration.underlying_type());
    for (const auto* value : *enumeration.values())
      if (const auto* member = value->union_type()) collect(*member);
  }

  auto encode(const reflection::Type& type)
      -> flatbuffers::Offset<reflection::Type> {
    int32_t index = -1;
    if (type.index() >= 0) {
      if (is_object(type))
        index =
            objects_.at(schema_.objects()->Get(type.index())->name()->str());
      else
        index = enums_.at(schema_.enums()->Get(type.index())->name()->str());
    }
    return reflection::CreateType(builder_, type.base_type(), type.element(),
                                  index, type.fixed_length(), type.base_size(),
                                  type.element_size());
  }

  using Attributes =
      flatbuffers::Vector<flatbuffers::Offset<reflection::KeyValue>>;
  auto attributes(const Attributes* attrs) -> flatbuffers::Offset<Attributes> {
    if (!attrs || attrs->size() == 0) return {};
    std::vector<flatbuffers::Offset<reflection::KeyValue>> entries;
    for (const auto* kv : *attrs) {
      const auto key = builder_.CreateString(kv->key());
      const auto value = kv->value()
                             ? builder_.CreateString(kv->value())
                             : flatbuffers::Offset<flatbuffers::String>{};
      entries.push_back(reflection::CreateKeyValue(builder_, key, value));
    }
    return builder_.CreateVector(entries);
  }

  auto encode(const reflection::Object& object)
      -> flatbuffers::Offset<reflection::Object> {
    std::vector<flatbuffers::Offset<reflection::Field>> fields;
    for (const auto* field : *object.fields()) {
      const auto name = builder_.CreateString(field->name());
      const auto type = encode(*field->type());
      const auto attrs = attributes(field->attributes());
      fields.push_back(reflection::CreateField(
          builder_, name, type, field->id(), field->offset(),
          field->default_integer(), field->default_real(), field->deprecated(),
          field->required(), field->key(), attrs, 0, field->optional(),
          field->padding(), field->offset64()));
    }
    const auto entries = builder_.CreateVector(fields);
    const auto attrs = attributes(object.attributes());
    const auto name = builder_.CreateString(object.name());
    return reflection::CreateObject(builder_, name, entries, object.is_struct(),
                                    object.minalign(), object.bytesize(),
                                    attrs);
  }

  auto encode(const reflection::Enum& enumeration)
      -> flatbuffers::Offset<reflection::Enum> {
    std::vector<flatbuffers::Offset<reflection::EnumVal>> values;
    for (const auto* value : *enumeration.values()) {
      const auto name = builder_.CreateString(value->name());
      const auto type = value->union_type()
                            ? encode(*value->union_type())
                            : flatbuffers::Offset<reflection::Type>{};
      const auto attrs = attributes(value->attributes());
      values.push_back(reflection::CreateEnumVal(builder_, name, value->value(),
                                                 type, 0, attrs));
    }
    const auto entries = builder_.CreateVector(values);
    const auto name = builder_.CreateString(enumeration.name());
    const auto type = encode(*enumeration.underlying_type());
    const auto attrs = attributes(enumeration.attributes());
    return reflection::CreateEnum(builder_, name, entries,
                                  enumeration.is_union(), type, attrs);
  }

  const reflection::Schema& schema_;
  std::map<std::string, int32_t> objects_;
  std::map<std::string, int32_t> enums_;
  flatbuffers::FlatBufferBuilder builder_;
};

}  // namespace

auto types::Type::canonical_id() const -> const std::string& {
  std::call_once(canonical_id_once_, [this] {
    canonical_id_ = CanonicalSchema(*schema()).build();
  });
  return canonical_id_;
}

auto schema_id(const types::Type& type) -> std::string {
  return type.canonical_id();
}

}  // namespace tsugumi
