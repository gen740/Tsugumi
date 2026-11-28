#include "agent_catalog.hh"

#include <flatbuffers/idl.h>
#include <flatbuffers/reflection.h>

#include <regex>
#include <stdexcept>

#include "tsugumi/util/json_object.hh"

extern const unsigned char tsugumi_agent_bfbs[];
extern const unsigned long tsugumi_agent_bfbs_size;

namespace tsugumi::cli {
namespace {
using reflection::BaseType;

// Requests that are not operations of their own: the connection's Hello, the
// call bookkeeping of ToolCallRequest, showing and closing a project
// (tsugumi_open, tsugumi_close) and the person's answer to a decision, which
// only the person gives.
constexpr std::string_view kHidden[] = {
    "HelloRequest", "ToolCallCancelRequest", "CallIdRequest",
    "ShowRequest",  "CloseRequest",          "DecisionResolveRequest"};

auto Schema() -> const reflection::Schema& {
  static const auto* schema = [] {
    flatbuffers::Verifier verifier(tsugumi_agent_bfbs, tsugumi_agent_bfbs_size);
    if (!reflection::VerifySchemaBuffer(verifier))
      throw std::runtime_error("invalid embedded Agent schema");
    return reflection::GetSchema(tsugumi_agent_bfbs);
  }();
  return *schema;
}

auto Documentation(const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>* lines)
    -> std::string {
  std::string out;
  if (!lines) return out;
  for (const auto* line : *lines) {
    std::string text = line->str();
    while (!text.empty() && text.front() == ' ') text.erase(0, 1);
    if (!out.empty()) out += ' ';
    out += text;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

auto EnumNames(const reflection::Enum& e, bool skip_none) -> std::string {
  std::vector<std::string> names;
  for (const auto* value : *e.values())
    if (!skip_none || value->value() != 0) names.push_back(json::Quote(value->name()->str()));
  return json::Encode(names);
}

auto ObjectSchema(int index, int depth) -> std::string;

auto TypeSchema(const reflection::Type& type, int depth) -> json::Members {
  const auto& schema = Schema();
  switch (type.base_type()) {
    case BaseType::Bool:
      return {{"type", json::Quote("boolean")}};
    case BaseType::Byte:
    case BaseType::UByte:
    case BaseType::Short:
    case BaseType::UShort:
    case BaseType::Int:
    case BaseType::UInt:
      if (type.index() >= 0)
        return {{"type", json::Quote("string")},
                {"enum", EnumNames(*schema.enums()->Get(type.index()), false)}};
      return {{"type", json::Quote("integer")}};
    case BaseType::Long:
    case BaseType::ULong:
      return {{"type", json::Quote("string")},
              {"pattern", json::Quote("^-?[0-9]+$")},
              {"description", json::Quote("64 bit integer as a decimal string")}};
    case BaseType::Float:
    case BaseType::Double:
      return {{"type", json::Quote("number")}};
    case BaseType::String:
      return {{"type", json::Quote("string")}};
    case BaseType::Obj:
      return json::Object(ObjectSchema(type.index(), depth + 1));
    case BaseType::Union: {
      std::vector<std::string> variants;
      for (const auto* value : *schema.enums()->Get(type.index())->values())
        if (value->union_type() && value->union_type()->index() >= 0)
          variants.push_back(ObjectSchema(value->union_type()->index(), depth + 1));
      return {{"anyOf", json::Encode(variants)}};
    }
    case BaseType::Vector: {
      auto items = [](std::string_view t) {
        return json::Members{{"type", json::Quote("array")},
                             {"items", json::Encode(json::Members{{"type", json::Quote(t)}})}};
      };
      switch (type.element()) {
        case BaseType::String:
        case BaseType::Long:
        case BaseType::ULong:
          return items("string");
        case BaseType::Obj:
          return {{"type", json::Quote("array")}, {"items", ObjectSchema(type.index(), depth + 1)}};
        case BaseType::Float:
        case BaseType::Double:
          return items("number");
        case BaseType::Bool:
          return items("boolean");
        case BaseType::Byte:
        case BaseType::UByte:
        case BaseType::Short:
        case BaseType::UShort:
        case BaseType::Int:
        case BaseType::UInt:
          return items("integer");
        default:
          return {{"type", json::Quote("array")}};
      }
    }
    default:
      return {};
  }
}

auto ObjectSchema(int index, int depth) -> std::string {
  if (depth >= 8) return json::Encode(json::Members{{"type", json::Quote("object")}});
  const auto& schema = Schema();
  const auto* object = schema.objects()->Get(index);
  json::Members properties;
  std::vector<std::string> required;
  for (const auto* field : *object->fields()) {
    if (field->type()->base_type() == BaseType::UType) continue;
    auto property = TypeSchema(*field->type(), depth);
    if (auto doc = Documentation(field->documentation()); !doc.empty())
      property["description"] = json::Quote(doc);
    if (field->type()->base_type() == BaseType::Union)
      properties[field->name()->str() + "_type"] = json::Encode(json::Members{
          {"type", json::Quote("string")},
          {"enum", EnumNames(*schema.enums()->Get(field->type()->index()), true)},
          {"description", json::Quote("variant of " + field->name()->str())}});
    properties[field->name()->str()] = json::Encode(property);
    if (field->required()) required.push_back(json::Quote(field->name()->str()));
  }
  if (object->name()->string_view() == "Tsugumi.Data.CardPath")
    properties["segments"] =
        R"({"type":"array","items":{"type":"string","pattern":"^[a-z][a-z0-9_]{0,63}$"}})";
  json::Members out{{"type", json::Quote("object")},
                    {"properties", json::Encode(properties)},
                    {"additionalProperties", "false"}};
  if (!required.empty()) out["required"] = json::Encode(required);
  return json::Encode(out);
}

auto ToolName(std::string request) -> std::string {
  request = std::regex_replace(request, std::regex("Request"), "");
  std::string out;
  for (size_t i = 0; i < request.size(); ++i) {
    const char c = request[i];
    if (i && std::isupper(static_cast<unsigned char>(c))) out += '_';
    out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return "agent_" + out;
}

}  // namespace

auto AgentOperations() -> const std::vector<AgentOperation>& {
  static const auto operations = [] {
    std::vector<AgentOperation> out;
    const auto& schema = Schema();
    const auto* payload = schema.enums()->LookupByKey("Tsugumi.Agent.RequestPayload");
    if (!payload) throw std::runtime_error("RequestPayload union not found");
    for (const auto* value : *payload->values()) {
      if (value->value() == 0 || !value->union_type() || value->union_type()->index() < 0) continue;
      const auto* object = schema.objects()->Get(value->union_type()->index());
      const auto full = object->name()->str();
      const auto request = full.substr(full.rfind('.') + 1);
      bool hidden = false;
      for (const auto name : kHidden) hidden = hidden || request == name;
      if (hidden) continue;
      const auto operation = std::regex_replace(request, std::regex("Request$"), "");
      out.push_back({ToolName(request), value->name()->str(),
                     operation + ": " + Documentation(object->documentation()),
                     ObjectSchema(value->union_type()->index(), 0)});
    }
    return out;
  }();
  return operations;
}

auto AgentSchema() -> std::pair<const uint8_t*, size_t> {
  return {tsugumi_agent_bfbs, tsugumi_agent_bfbs_size};
}

}  // namespace tsugumi::cli
