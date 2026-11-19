#include "agent/catalog.hh"

#include <flatbuffers/idl.h>
#include <flatbuffers/reflection_generated.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <set>

#include "host/agent_policy.hh"
#include "host/failure.hh"
#include "tsugumi/protocol/host_generated.h"
#include "tsugumi/protocol/tsugumi_generated.h"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/json_object.hh"
#include "tsugumi/util/text.hh"

extern const unsigned char tsugumi_rpc_bfbs[];
extern const unsigned long tsugumi_rpc_bfbs_size;

extern const unsigned char tsugumi_host_bfbs[];
extern const unsigned long tsugumi_host_bfbs_size;

extern const unsigned char tsugumi_agent_settings[];
extern const unsigned long tsugumi_agent_settings_size;

namespace tsugumi::agent {
namespace {

thread_local bool host_schema = false;
struct SchemaScope {
  bool previous = host_schema;
  explicit SchemaScope(bool host) { host_schema = host; }
  ~SchemaScope() { host_schema = previous; }
};
auto Namespace() -> std::string {
  return host_schema ? "Tsugumi.Host." : "Tsugumi.RPC.";
}
auto Schema() -> const reflection::Schema& {
  return *reflection::GetSchema(host_schema ? tsugumi_host_bfbs
                                            : tsugumi_rpc_bfbs);
}

auto Name(std::string name) -> std::string {
  if (name.ends_with("Request")) name.resize(name.size() - 7);
  std::string out;
  for (char c : name) {
    if (std::isupper(static_cast<unsigned char>(c)) && !out.empty()) out += '_';
    out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

// Shared JSON keeps readable indentation on disk. Provider request builders
// embed schemas verbatim, so remove formatting outside JSON strings once here.
auto CompactSchema(std::string_view raw) -> std::string {
  std::string out;
  bool quoted = false, escaped = false;
  for (const char c : raw) {
    if (quoted || !std::isspace(static_cast<unsigned char>(c))) out += c;
    if (escaped)
      escaped = false;
    else if (quoted && c == '\\')
      escaped = true;
    else if (c == '"')
      quoted = !quoted;
  }
  return out;
}

auto Docs(
    const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>* docs)
    -> std::string {
  std::string out;
  if (docs)
    for (const auto* line : *docs)
      out += (out.empty() ? "" : " ") + line->str();
  return out;
}

auto OwnOperationId(const reflection::Object& object) -> bool {
  const auto name = object.name()->str();
  return object.fields()->LookupByKey("op_id") &&
         !name.ends_with(".OperationGetRequest") &&
         !name.ends_with(".CardSDKStatusRequest");
}

auto ObjectSchema(const reflection::Object& object, int depth, bool top)
    -> std::string;

auto TypeSchema(reflection::BaseType type, reflection::BaseType element,
                int index, int depth) -> std::string {
  using namespace reflection;
  if (type == String) return R"({"type":"string"})";
  if (type == Bool) return R"({"type":"boolean"})";
  if (type == Obj)
    return ObjectSchema(*Schema().objects()->Get(index), depth + 1, false);
  if (type == Vector) {
    if (element == UByte || element == Byte)
      return R"({"oneOf":[{"type":"string"},{"type":"object","properties":{"base64":{"type":"string"}},"required":["base64"],"additionalProperties":false}]})";
    return json::Encode(
        json::Members{{"type", json::Quote("array")},
                      {"items", TypeSchema(element, None, index, depth)}});
  }
  if (type == Union) {
    std::vector<std::string> alternatives;
    for (const auto* value : *Schema().enums()->Get(index)->values()) {
      if (value->union_type() && value->union_type()->base_type() == Obj)
        alternatives.push_back(
            ObjectSchema(*Schema().objects()->Get(value->union_type()->index()),
                         depth + 1, false));
    }
    return json::Encode(json::Members{{"anyOf", json::Encode(alternatives)}});
  }
  if (index >= 0) {
    std::vector<std::string> names;
    for (const auto* value : *Schema().enums()->Get(index)->values())
      names.push_back(json::Quote(value->name()->str()));
    return json::Encode(json::Members{{"type", json::Quote("string")},
                                      {"enum", json::Encode(names)}});
  }
  if (type == Float || type == Double) return R"({"type":"number"})";
  // FlatBuffers parses 64-bit decimal strings without rounding through JS.
  if (type == Long || type == ULong)
    return R"({"type":"string","pattern":"^-?[0-9]+$"})";
  return R"({"type":"integer"})";
}

auto ObjectSchema(const reflection::Object& object, int depth, bool top)
    -> std::string {
  if (depth >= 12) return R"({"type":"object"})";
  json::Members properties;
  std::vector<std::string> required;
  for (const auto* field : *object.fields()) {
    const auto name = field->name()->str();
    if (top && host::OwnedFields.contains(name) &&
        (name != "op_id" || OwnOperationId(object)))
      continue;
    const auto& type = *field->type();
    if (type.base_type() == reflection::UType) {
      std::vector<std::string> values;
      for (const auto* value : *Schema().enums()->Get(type.index())->values())
        if (value->value()) values.push_back(json::Quote(value->name()->str()));
      properties[name] = json::Encode(json::Members{
          {"type", json::Quote("string")}, {"enum", json::Encode(values)}});
    } else {
      auto schema = json::Object(
          TypeSchema(type.base_type(), type.element(), type.index(), depth));
      const auto docs = Docs(field->documentation());
      if (!docs.empty()) schema["description"] = json::Quote(docs);
      properties[name] = json::Encode(schema);
    }
    if (field->required()) required.push_back(json::Quote(name));
  }
  // Agent save conveniences belong to hostd, not the project protocol.
  if (object.name()->str() == "Tsugumi.RPC.PutDefinition") {
    properties["fill_description"] =
        R"({"type":"string","description":"Fill an empty description, preserving existing text."})";
    properties["fill_context"] =
        R"({"type":"string","description":"Fill an empty context, preserving existing text."})";
  }
  if (object.name()->str() == "Tsugumi.RPC.CardIdRequest") {
    properties.erase("stem");
    properties["name"] =
        R"({"type":"string","description":"Human name; the host derives the identifier stem."})";
    required = {json::Quote("name")};
  }
  json::Members schema{{"type", json::Quote("object")},
                       {"properties", json::Encode(properties)},
                       {"additionalProperties", "false"}};
  if (!required.empty()) schema["required"] = json::Encode(required);
  return json::Encode(schema);
}

auto Normalize(const reflection::Object& object, std::string_view raw)
    -> std::string {
  auto values = json::Object(raw);
  for (const auto* field : *object.fields()) {
    auto it = values.find(field->name()->str());
    if (it == values.end() || it->second == "null") continue;
    const auto& type = *field->type();
    if (type.base_type() == reflection::Obj) {
      it->second =
          Normalize(*Schema().objects()->Get(type.index()), it->second);
    } else if (type.base_type() == reflection::Union) {
      const auto tag = json::Text(values, field->name()->str() + "_type");
      for (const auto* value : *Schema().enums()->Get(type.index())->values()) {
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
      for (unsigned char c : bytes) numbers.push_back(std::to_string(c));
      it->second = json::Encode(numbers);
    } else if (type.base_type() == reflection::Vector &&
               type.element() == reflection::Obj) {
      auto elements = json::Array(it->second);
      for (auto& element : elements)
        element = Normalize(*Schema().objects()->Get(type.index()), element);
      it->second = json::Encode(elements);
    }
  }
  return json::Encode(values);
}

auto Output(const reflection::Object& object, std::string_view raw)
    -> std::string {
  auto values = json::Object(raw);
  for (const auto* field : *object.fields()) {
    auto it = values.find(field->name()->str());
    if (it == values.end() || it->second == "null") continue;
    const auto& type = *field->type();
    if (type.base_type() == reflection::Obj)
      it->second = Output(*Schema().objects()->Get(type.index()), it->second);
    else if (type.base_type() == reflection::Union) {
      const auto tag = json::Text(values, field->name()->str() + "_type");
      for (const auto* value : *Schema().enums()->Get(type.index())->values())
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
        for (const auto& element : elements)
          bytes += static_cast<char>(std::stoi(element));
        it->second =
            text::valid_utf8(bytes) && bytes.find('\0') == std::string::npos
                ? json::Quote(bytes)
                : json::Encode(json::Members{
                      {"base64", json::Quote(crypto::base64(bytes))}});
      } else {
        for (auto& element : elements) {
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
  if (!parser->Deserialize(
          host_schema ? tsugumi_host_bfbs : tsugumi_rpc_bfbs,
          host_schema ? tsugumi_host_bfbs_size : tsugumi_rpc_bfbs_size))
    throw std::runtime_error("cannot load project RPC schema");
  return parser;
}

}  // namespace

auto Catalog::shared() const -> json::Members {
  return json::Object(
      std::string_view(reinterpret_cast<const char*>(tsugumi_agent_settings),
                       tsugumi_agent_settings_size));
}

Catalog::Catalog() {
  tools_.push_back(
      {{"project_file_format",
        "Return formatted text for path and language using the host's "
        "formatter.",
        R"({"type":"object","properties":{"path":{"type":"string"},"text":{"type":"string"},"language":{"type":"string","enum":["json","cpp","python","nix","bash","fbs"]}},"required":["path","text"],"additionalProperties":false})"},
       "FileFormatRequest",
       false,
       false,
       false});
  const auto settings = json::Object(
      std::string_view(reinterpret_cast<const char*>(tsugumi_agent_settings),
                       tsugumi_agent_settings_size));
  std::set<std::string> seen;
  for (const auto* list : {"requests", "approvalRequests", "hostRequests"}) {
    SchemaScope scope(std::string_view(list) == "hostRequests");
    const auto* requests =
        Schema().enums()->LookupByKey((Namespace() + "RequestPayload").c_str());
    for (const auto& entry : json::Array(json::Get(settings, list, "[]"))) {
      const auto name = json::String(entry);
      const auto found = std::find_if(
          requests->values()->begin(), requests->values()->end(),
          [&](const auto* value) { return value->name()->str() == name; });
      if (found == requests->values()->end() || !(*found)->value() ||
          !seen.insert(name).second)
        throw std::runtime_error("invalid Agent request allow list entry: " +
                                 name);
      const auto& object =
          *Schema().objects()->Get((*found)->union_type()->index());
      CatalogTool tool{{Name(name), Docs(object.documentation()),
                        ObjectSchema(object, 0, true)},
                       name,
                       false};
      tool.host = host_schema;
      // Client-only RPC fields are not model arguments.
      const auto hidden =
          json::Object(json::Get(settings, "notForModels", "{}"));
      if (hidden.contains(name)) {
        auto schema = json::Object(tool.spec.input_schema);
        auto properties = json::Object(json::Get(schema, "properties"));
        for (const auto& field : json::Array(hidden.at(name)))
          properties.erase(json::String(field));
        schema["properties"] = json::Encode(properties);
        tool.spec.input_schema = json::Encode(schema);
      }
      if (tool.spec.name == "card_id")
        tool.spec.description =
            "Reserve a permanent Card id from name; returns id. project_path "
            "selects an external project; omitted means root.";
      if (tool.spec.name == "run_value_read" ||
          tool.spec.name == "artifact_read" || tool.spec.name == "log_range" ||
          tool.spec.name == "module_read")
        tool.spec.description +=
            " Agent pages default to 2048 bytes; continue with next_offset.";
      if (tool.spec.name == "run_history" ||
          tool.spec.name == "run_invocations" ||
          tool.spec.name == "artifact_list" ||
          tool.spec.name == "run_environments" ||
          tool.spec.name == "run_environment_paths")
        tool.spec.description +=
            " Agent pages default to 8; continue with the "
            "returned cursor or next_offset.";
      if (tool.spec.name == "project_save")
        tool.spec.description += " " + json::Text(settings, "saveNote");
      if (tool.spec.name == "run" || tool.spec.name == "run_status") {
        const auto wait = json::Object(json::Get(settings, "runWait"));
        auto schema = json::Object(tool.spec.input_schema);
        auto properties = json::Object(json::Get(schema, "properties"));
        properties["wait_seconds"] = json::Get(wait, "schema");
        schema["properties"] = json::Encode(properties);
        tool.spec.input_schema = json::Encode(schema);
        tool.spec.description += " " + json::Text(wait, "description");
      }
      tool.approval_required = std::string_view(list) == "approvalRequests";
      tool.mutating = OwnOperationId(object);
      tool.has_origin = object.fields()->LookupByKey("origin") != nullptr;
      tools_.push_back(std::move(tool));
    }
  }
  for (const auto& raw : json::Array(json::Get(settings, "hostTools", "[]"))) {
    const auto tool = json::Object(raw);
    const auto name = json::Text(tool, "name");
    if (Find(name)) throw std::runtime_error("duplicate host tool: " + name);
    tools_.push_back({{name, json::Text(tool, "description"),
                       CompactSchema(json::Get(tool, "inputSchema"))},
                      name,
                      false,
                      json::Bool(json::Get(tool, "mutating", "false")),
                      false});
  }
  for (auto& tool : tools_)
    tool.spec.input_schema = CompactSchema(tool.spec.input_schema);
  const std::vector<std::string> editing{
      "project_edit",   "project_connect", "project_make_subgraph",
      "project_layout", "project_copy",    "project_save"};
  auto rank = [&](const CatalogTool& tool) {
    return std::find(editing.begin(), editing.end(), tool.spec.name) -
           editing.begin();
  };
  std::stable_sort(tools_.begin(), tools_.end(),
                   [&](const auto& a, const auto& b) {
                     const auto ar = rank(a), br = rank(b);
                     if (ar == static_cast<decltype(ar)>(editing.size()))
                       return br != static_cast<decltype(br)>(editing.size());
                     if (br == static_cast<decltype(br)>(editing.size()))
                       return false;
                     return ar < br;
                   });
}

auto Catalog::AllSpecs() const -> std::vector<ToolSpec> {
  std::vector<ToolSpec> specs;
  for (const auto& tool : tools()) specs.push_back(tool.spec);
  specs.push_back(
      {"document_template",
       "Read a Card template by name and reserved id. Returns definition_json "
       "and source files for one project_edit under definitions/<id>/.",
       R"({"type":"object","properties":{"name":{"type":"string"},"id":{"type":"string"}},"required":["name","id"],"additionalProperties":false})"});
  specs.push_back(
      {"demo_list",
       "List bundled demos with paths, names and one-line descriptions.",
       R"({"type":"object","properties":{},"additionalProperties":false})"});
  specs.push_back(
      {"demo_read",
       "Read a bundled demo or reference file; path is relative to demos/.",
       R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":false})"});
  {
    const auto definition = json::Object(
        json::Get(json::Object(json::Get(shared(), "guide")), "showTool"));
    specs.push_back({json::Text(definition, "name"),
                     json::Text(definition, "description"),
                     json::Get(definition, "inputSchema")});
  }
  {
    const auto definition =
        json::Object(json::Get(shared(), "delegateTool", "{}"));
    specs.push_back({"delegate", json::Text(definition, "description"),
                     json::Get(definition, "inputSchema", "{}")});
  }
  for (auto& spec : specs) spec.input_schema = CompactSchema(spec.input_schema);
  return specs;
}

namespace {
auto ContainsName(const std::string& raw, std::string_view name) -> bool {
  for (const auto& value : json::Array(raw))
    if (json::String(value) == name) return true;
  return false;
}
}  // namespace

auto Catalog::Allows(std::string_view name, ModelRole role) const -> bool {
  if (role == ModelRole::Guide) {
    const auto guide = json::Object(json::Get(shared(), "guide"));
    return name == "guide_show" || name == "demo_list" || name == "demo_read" ||
           name == "tool_result_read" ||
           ContainsName(json::Get(guide, "tools"), name);
  }
  if (name == "guide_show") return false;
  if (role == ModelRole::ScopedHelper)
    return ContainsName(
        json::Get(json::Object(json::Get(shared(), "modelTools")), "scoped"),
        name);
  return true;
}

auto Catalog::FamilySpecs(std::string_view family, ModelRole role) const
    -> std::vector<ToolSpec> {
  const auto settings = json::Object(json::Get(shared(), "modelTools"));
  const auto families = json::Object(json::Get(settings, "families"));
  const auto found = families.find(std::string(family));
  if (found == families.end())
    throw host::Failure("invalid_request", "Unknown tool family");
  const auto all = AllSpecs();
  std::vector<ToolSpec> result;
  for (const auto& [action, raw] : json::Object(found->second)) {
    const auto name = json::String(raw);
    if (!Allows(name, role)) continue;
    const auto spec =
        std::find_if(all.begin(), all.end(),
                     [&](const auto& tool) { return tool.name == name; });
    if (spec == all.end())
      throw std::runtime_error("Unknown family tool: " + name);
    result.push_back({action, spec->description, spec->input_schema});
  }
  return result;
}

auto Catalog::FamilyTool(std::string_view family, std::string_view action,
                         ModelRole role) const -> std::string {
  const auto settings = json::Object(json::Get(shared(), "modelTools"));
  const auto families = json::Object(json::Get(settings, "families"));
  const auto actions = json::Object(json::Get(families, family, "{}"));
  const auto name = json::Text(actions, action);
  if (name.empty() || !Allows(name, role))
    throw host::Failure("permission_denied",
                        "Tool action unavailable for this role");
  return name;
}

auto Catalog::ModelSpecs(ModelRole role, bool can_delegate) const
    -> std::vector<ToolSpec> {
  const auto settings = json::Object(json::Get(shared(), "modelTools"));
  const auto all = AllSpecs();
  std::vector<ToolSpec> result;
  for (const auto& raw : json::Array(json::Get(settings, "core"))) {
    const auto name = json::String(raw);
    if (!Allows(name, role) || (name == "delegate" && !can_delegate)) continue;
    const auto found =
        std::find_if(all.begin(), all.end(),
                     [&](const auto& tool) { return tool.name == name; });
    if (found == all.end())
      throw std::runtime_error("Unknown core tool: " + name);
    result.push_back(*found);
  }
  std::vector<std::string> families;
  for (const auto& [family, actions] :
       json::Object(json::Get(settings, "families"))) {
    (void)actions;
    if (!FamilySpecs(family, role).empty())
      families.push_back(json::Quote(family));
  }
  const auto schema = json::Encode(json::Members{
      {"type", json::Quote("object")},
      {"additionalProperties", "false"},
      {"required", R"(["family"])"},
      {"properties",
       json::Encode(json::Members{
           {"family",
            json::Encode(json::Members{{"type", json::Quote("string")},
                                       {"enum", json::Encode(families)}})},
           {"action", R"({"type":"string"})"},
           {"arguments",
            R"({"type":"object","additionalProperties":true})"}})}});
  const ToolSpec index{
      "tools",
      "Fetch a family's actions and argument schemas (omit action), "
      "or call action with arguments; returns that action's result.",
      schema};
  // Keep narrow edit tools first and whole-document save last.
  const auto save = std::find_if(
      result.begin(), result.end(),
      [](const auto& tool) { return tool.name == "project_save"; });
  result.insert(save, index);
  if (role == ModelRole::Guide)
    for (const auto& tool : all)
      if (tool.name == "guide_show") result.push_back(tool);
  return result;
}

auto Catalog::Find(std::string_view name) const -> const CatalogTool* {
  auto found =
      std::find_if(tools_.begin(), tools_.end(),
                   [&](const auto& tool) { return tool.spec.name == name; });
  return found == tools_.end() ? nullptr : &*found;
}

auto Catalog::Json() const -> std::string {
  std::vector<std::string> tools;
  for (const auto& tool : tools_)
    tools.push_back(json::Encode(
        json::Members{{"name", json::Quote(tool.spec.name)},
                      {"description", json::Quote(tool.spec.description)},
                      {"inputSchema", tool.spec.input_schema},
                      {"mutating", tool.mutating ? "true" : "false"}}));
  return json::Encode(tools);
}

auto Catalog::Request(std::string_view name, std::string_view arguments,
                      uint64_t id) const -> std::string {
  const auto* tool = Find(name);
  if (!tool) throw std::runtime_error("unknown project operation");
  return tool->host ? HostRequest(name, arguments, id)
                    : EncodeProjectRequest(name, arguments, id);
}

auto EncodeProjectRequest(std::string_view name, std::string_view arguments,
                          uint64_t id) -> std::string {
  const auto* requests =
      Schema().enums()->LookupByKey((Namespace() + "RequestPayload").c_str());
  const auto found = std::find_if(
      requests->values()->begin(), requests->values()->end(),
      [&](const auto* value) {
        return value->value() && Name(value->name()->str()) == name;
      });
  if (found == requests->values()->end())
    throw std::runtime_error("unknown project operation");
  const auto request_name = (*found)->name()->str();
  const auto* object =
      Schema().objects()->LookupByKey((Namespace() + request_name).c_str());
  auto body = json::Encode(json::Members{
      {"request_id", std::to_string(id)},
      {"payload_type", json::Quote("Request")},
      {"payload",
       json::Encode(json::Members{{"value_type", json::Quote(request_name)},
                                  {"value", Normalize(*object, arguments)}})}});
  auto parser = Parser();
  if (!parser->ParseJson(body.c_str())) {
    auto diagnostic = parser->error_;
    std::replace(diagnostic.begin(), diagnostic.end(), '\n', ' ');
    throw host::Failure(
        "invalid_request",
        std::string(name) +
            ": arguments do not match its schema: " + diagnostic);
  }
  return {reinterpret_cast<const char*>(parser->builder_.GetBufferPointer()),
          parser->builder_.GetSize()};
}

auto Catalog::HostRequest(std::string_view name, std::string_view arguments,
                          uint64_t id) const -> std::string {
  SchemaScope scope(true);
  return EncodeProjectRequest(name, arguments, id);
}
auto Catalog::HostReply(std::string_view bytes) const -> std::string {
  SchemaScope scope(true);
  return Reply(bytes);
}
auto Catalog::Reply(std::string_view bytes) const -> std::string {
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

}  // namespace tsugumi::agent
