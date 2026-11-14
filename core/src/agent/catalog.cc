#include "agent/catalog.hh"

#include <flatbuffers/idl.h>
#include <flatbuffers/reflection_generated.h>

#include <algorithm>
#include <cctype>
#include <set>

#include "agent/json.hh"
#include "tsugumi/protocol/tsugumi_generated.h"
#include "util/crypto.hh"
#include "util/text.hh"

extern const unsigned char tsugumi_rpc_bfbs[];
extern const unsigned long tsugumi_rpc_bfbs_size;

extern const unsigned char tsugumi_agent_settings[];
extern const unsigned long tsugumi_agent_settings_size;

namespace tsugumi::agent {
namespace {

auto Schema() -> const reflection::Schema & {
  return *reflection::GetSchema(tsugumi_rpc_bfbs);
}

auto Name(std::string name) -> std::string {
  if (name.ends_with("Request"))
    name.resize(name.size() - 7);
  std::string out;
  for (char c : name) {
    if (std::isupper(static_cast<unsigned char>(c)) && !out.empty())
      out += '_';
    out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

auto Docs(
    const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>> *docs)
    -> std::string {
  std::string out;
  if (docs)
    for (const auto *line : *docs)
      out += (out.empty() ? "" : " ") + line->str();
  return out;
}

const std::set<std::string> kOwnedFields{"origin", "op_id",
                                         "approved_permissions", "run_ids",
                                         "restrict_to_run_ids"};

auto OwnOperationId(const reflection::Object &object) -> bool {
  const auto name = object.name()->str();
  return object.fields()->LookupByKey("op_id") &&
         !name.ends_with(".OperationGetRequest") &&
         !name.ends_with(".CardSDKStatusRequest");
}

auto ObjectSchema(const reflection::Object &object, int depth, bool top)
    -> std::string;

auto TypeSchema(reflection::BaseType type, reflection::BaseType element,
                int index, int depth) -> std::string {
  using namespace reflection;
  if (type == String)
    return R"({"type":"string"})";
  if (type == Bool)
    return R"({"type":"boolean"})";
  if (type == Obj)
    return ObjectSchema(*Schema().objects()->Get(index), depth + 1, false);
  if (type == Vector) {
    if (element == UByte || element == Byte)
      return R"({"type":"string","description":"UTF-8 text; binary data may be supplied as an object with a base64 field"})";
    return json::Encode(
        json::Members{{"type", json::Quote("array")},
                      {"items", TypeSchema(element, None, index, depth)}});
  }
  if (type == Union) {
    std::vector<std::string> alternatives;
    for (const auto *value : *Schema().enums()->Get(index)->values()) {
      if (value->union_type() && value->union_type()->base_type() == Obj)
        alternatives.push_back(
            ObjectSchema(*Schema().objects()->Get(value->union_type()->index()),
                         depth + 1, false));
    }
    return json::Encode(json::Members{{"anyOf", json::Encode(alternatives)}});
  }
  if (index >= 0) {
    std::vector<std::string> names;
    for (const auto *value : *Schema().enums()->Get(index)->values())
      names.push_back(json::Quote(value->name()->str()));
    return json::Encode(json::Members{{"type", json::Quote("string")},
                                      {"enum", json::Encode(names)}});
  }
  if (type == Float || type == Double)
    return R"({"type":"number"})";
  // FlatBuffers parses 64-bit decimal strings without rounding through JS.
  if (type == Long || type == ULong)
    return R"({"type":"string","pattern":"^-?[0-9]+$"})";
  return R"({"type":"integer"})";
}

auto ObjectSchema(const reflection::Object &object, int depth, bool top)
    -> std::string {
  if (depth >= 12)
    return R"({"type":"object"})";
  json::Members properties;
  std::vector<std::string> required;
  for (const auto *field : *object.fields()) {
    const auto name = field->name()->str();
    if (field->deprecated() || (top && kOwnedFields.contains(name) &&
                                (name != "op_id" || OwnOperationId(object))))
      continue;
    const auto &type = *field->type();
    if (type.base_type() == reflection::UType) {
      std::vector<std::string> values;
      for (const auto *value : *Schema().enums()->Get(type.index())->values())
        if (value->value())
          values.push_back(json::Quote(value->name()->str()));
      properties[name] = json::Encode(json::Members{
          {"type", json::Quote("string")}, {"enum", json::Encode(values)}});
    } else {
      auto schema = json::Object(
          TypeSchema(type.base_type(), type.element(), type.index(), depth));
      const auto docs = Docs(field->documentation());
      if (!docs.empty())
        schema["description"] = json::Quote(docs);
      properties[name] = json::Encode(schema);
    }
    if (field->required())
      required.push_back(json::Quote(name));
  }
  // Agent save conveniences belong to agentd, not the project protocol.
  if (object.name()->str() == "Tsugumi.RPC.PutDefinition") {
    properties["fill_description"] = R"({"type":"string","description":"Fill an empty description, preserving existing text."})";
    properties["fill_context"] = R"({"type":"string","description":"Fill an empty context, preserving existing text."})";
  }
  if (object.name()->str() == "Tsugumi.RPC.CardIdRequest") {
    properties.erase("stem");
    properties["name"] = R"({"type":"string","description":"Human name; the host derives the identifier stem."})";
    required = {json::Quote("name")};
  }
  json::Members schema{{"type", json::Quote("object")},
                       {"properties", json::Encode(properties)},
                       {"additionalProperties", "false"}};
  if (!required.empty())
    schema["required"] = json::Encode(required);
  return json::Encode(schema);
}

auto Normalize(const reflection::Object &object, std::string_view raw)
    -> std::string {
  auto values = json::Object(raw);
  for (const auto *field : *object.fields()) {
    auto it = values.find(field->name()->str());
    if (it == values.end() || it->second == "null")
      continue;
    const auto &type = *field->type();
    if (type.base_type() == reflection::Obj) {
      it->second =
          Normalize(*Schema().objects()->Get(type.index()), it->second);
    } else if (type.base_type() == reflection::Union) {
      const auto tag = json::Text(values, field->name()->str() + "_type");
      for (const auto *value : *Schema().enums()->Get(type.index())->values()) {
        if (value->name()->str() == tag &&
            value->union_type()->base_type() == reflection::Obj)
          it->second =
              Normalize(*Schema().objects()->Get(value->union_type()->index()),
                        it->second);
      }
    } else if (type.base_type() == reflection::Vector &&
               (type.element() == reflection::UByte ||
                type.element() == reflection::Byte) &&
               !it->second.empty() && it->second[0] != '[') {
      std::string bytes;
      if (it->second[0] == '"')
        bytes = json::String(it->second);
      else {
        const auto encoded = json::Text(json::Object(it->second), "base64");
        if (!crypto::base64_decode(encoded, bytes))
          throw std::runtime_error("invalid base64");
      }
      std::vector<std::string> numbers;
      for (unsigned char c : bytes)
        numbers.push_back(std::to_string(c));
      it->second = json::Encode(numbers);
    } else if (type.base_type() == reflection::Vector &&
               type.element() == reflection::Obj) {
      auto elements = json::Array(it->second);
      for (auto &element : elements)
        element = Normalize(*Schema().objects()->Get(type.index()), element);
      it->second = json::Encode(elements);
    }
  }
  return json::Encode(values);
}

auto Output(const reflection::Object &object, std::string_view raw)
    -> std::string {
  auto values = json::Object(raw);
  for (const auto *field : *object.fields()) {
    auto it = values.find(field->name()->str());
    if (it == values.end() || it->second == "null")
      continue;
    const auto &type = *field->type();
    if (type.base_type() == reflection::Obj)
      it->second = Output(*Schema().objects()->Get(type.index()), it->second);
    else if (type.base_type() == reflection::Union) {
      const auto tag = json::Text(values, field->name()->str() + "_type");
      for (const auto *value : *Schema().enums()->Get(type.index())->values())
        if (value->name()->str() == tag &&
            value->union_type()->base_type() == reflection::Obj)
          it->second =
              Output(*Schema().objects()->Get(value->union_type()->index()),
                     it->second);
    } else if (type.base_type() == reflection::Vector) {
      auto elements = json::Array(it->second);
      if (type.element() == reflection::UByte ||
          type.element() == reflection::Byte) {
        std::string bytes;
        for (const auto &element : elements)
          bytes += static_cast<char>(std::stoi(element));
        it->second =
            text::valid_utf8(bytes) && bytes.find('\0') == std::string::npos
                ? json::Quote(bytes)
                : json::Encode(json::Members{
                      {"base64", json::Quote(crypto::base64(bytes))}});
      } else {
        for (auto &element : elements) {
          if (type.element() == reflection::Obj)
            element = Output(*Schema().objects()->Get(type.index()), element);
          else if (type.element() == reflection::Long ||
                   type.element() == reflection::ULong)
            element = json::Quote(element);
        }
        it->second = json::Encode(elements);
      }
    } else if ((type.base_type() == reflection::Long ||
                type.base_type() == reflection::ULong) &&
               type.index() < 0)
      it->second = json::Quote(it->second);
  }
  return json::Encode(values);
}

auto Parser() -> std::unique_ptr<flatbuffers::Parser> {
  auto parser = std::make_unique<flatbuffers::Parser>();
  parser->opts.strict_json = true;
  parser->opts.output_default_scalars_in_json = true;
  parser->opts.indent_step = -1;
  if (!parser->Deserialize(tsugumi_rpc_bfbs, tsugumi_rpc_bfbs_size))
    throw std::runtime_error("cannot load project RPC schema");
  return parser;
}

} // namespace

Catalog::Catalog() {
  const auto *requests =
      Schema().enums()->LookupByKey("Tsugumi.RPC.RequestPayload");
  const auto settings = json::Object(
      std::string_view(reinterpret_cast<const char *>(tsugumi_agent_settings),
                       tsugumi_agent_settings_size));
  std::set<std::string> seen;
  for (const auto *list : {"requests", "approvalRequests"}) {
    for (const auto &entry : json::Array(json::Get(settings, list))) {
      const auto name = json::String(entry);
      const auto found = std::find_if(
          requests->values()->begin(), requests->values()->end(),
          [&](const auto *value) { return value->name()->str() == name; });
      if (found == requests->values()->end() || !(*found)->value() ||
          !seen.insert(name).second)
        throw std::runtime_error("invalid Agent request allow list entry: " +
                                 name);
      const auto &object =
          *Schema().objects()->Get((*found)->union_type()->index());
      CatalogTool tool{{Name(name), Docs(object.documentation()),
                        ObjectSchema(object, 0, true)},
                       name,
                       false};
      if (tool.spec.name == "card_id")
        tool.spec.description = "Reserve a permanent Card id from its human name. The host derives a readable stem; core reserves a unique id. Use the id in paths and wires and keep the name in _meta.json.name. project_path selects a nested project.";
      tool.approval_required = std::string_view(list) == "approvalRequests";
      if (tool.approval_required)
        tool.spec.description +=
            " Requires the person's approval for this call and its exact "
            "targets; a declined call changes nothing.";
      tool.mutating = OwnOperationId(object);
      tool.has_origin = object.fields()->LookupByKey("origin") != nullptr;
      tools_.push_back(std::move(tool));
    }
  }
}

auto Catalog::Find(std::string_view name) const -> const CatalogTool * {
  auto found =
      std::find_if(tools_.begin(), tools_.end(),
                   [&](const auto &tool) { return tool.spec.name == name; });
  return found == tools_.end() ? nullptr : &*found;
}

auto Catalog::Json() const -> std::string {
  std::vector<std::string> tools;
  for (const auto &tool : tools_)
    tools.push_back(json::Encode(
        json::Members{{"name", json::Quote(tool.spec.name)},
                      {"description", json::Quote(tool.spec.description)},
                      {"inputSchema", tool.spec.input_schema},
                      {"mutating", tool.mutating ? "true" : "false"}}));
  return json::Encode(tools);
}

auto Catalog::Request(std::string_view name, std::string_view arguments,
                      uint64_t id) const -> std::string {
  const auto *tool = Find(name);
  if (!tool)
    throw std::runtime_error("unknown project operation");
  return EncodeProjectRequest(name, arguments, id);
}

auto EncodeProjectRequest(std::string_view name, std::string_view arguments,
                          uint64_t id) -> std::string {
  const auto *requests =
      Schema().enums()->LookupByKey("Tsugumi.RPC.RequestPayload");
  const auto found = std::find_if(
      requests->values()->begin(), requests->values()->end(),
      [&](const auto *value) {
        return value->value() && Name(value->name()->str()) == name;
      });
  if (found == requests->values()->end())
    throw std::runtime_error("unknown project operation");
  const auto request_name = (*found)->name()->str();
  const auto *object = Schema().objects()->LookupByKey(
      ("Tsugumi.RPC." + request_name).c_str());
  auto body = json::Encode(json::Members{
      {"request_id", std::to_string(id)},
      {"payload_type", json::Quote("Request")},
      {"payload", json::Encode(json::Members{
                      {"value_type", json::Quote(request_name)},
                      {"value", Normalize(*object, arguments)}})}});
  auto parser = Parser();
  if (!parser->ParseJson(body.c_str()))
    throw std::runtime_error(parser->error_);
  return {reinterpret_cast<const char *>(parser->builder_.GetBufferPointer()),
          parser->builder_.GetSize()};
}

auto Catalog::Reply(std::string_view bytes) const -> std::string {
  flatbuffers::Verifier verifier(
      reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size());
  if (!Tsugumi::RPC::VerifyMessageBuffer(verifier))
    throw std::runtime_error("invalid project reply");
  auto parser = Parser();
  std::string raw;
  if (auto error = flatbuffers::GenerateText(*parser, bytes.data(), &raw))
    throw std::runtime_error(error);
  auto envelope = json::Object(Output(*Schema().root_table(), raw));
  auto payload = json::Object(json::Get(envelope, "payload"));
  return json::Encode(
      json::Members{{"reply", json::Get(payload, "value_type")},
                    {"value", json::Get(payload, "value", "{}")}});
}

} // namespace tsugumi::agent
