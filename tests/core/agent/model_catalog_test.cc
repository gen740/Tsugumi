#include "agent/catalog.hh"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>

#include "host/failure.hh"

namespace tsugumi::agent {
namespace {
auto Names(const std::vector<ToolSpec>& specs) -> std::vector<std::string> {
  std::vector<std::string> names;
  for (const auto& spec : specs) names.push_back(spec.name);
  return names;
}

auto CheckSchema(const std::string& raw) -> void {
  const auto schema = json::Object(raw);
  const auto properties = json::Object(json::Get(schema, "properties", "{}"));
  for (const auto& name : json::Array(json::Get(schema, "required", "[]")))
    EXPECT_TRUE(properties.contains(json::String(name)));
  for (const auto& [name, child] : properties) {
    SCOPED_TRACE(name);
    CheckSchema(child);
  }
  if (schema.contains("type")) {
    const std::set<std::string> types{"object", "array",   "string", "integer",
                                      "number", "boolean", "null"};
    EXPECT_TRUE(types.contains(json::Text(schema, "type")));
  }
  if (schema.contains("items")) CheckSchema(schema.at("items"));
  for (const auto* key : {"oneOf", "anyOf"}) {
    if (!schema.contains(key)) continue;
    const auto children = json::Array(schema.at(key));
    EXPECT_FALSE(children.empty());
    for (const auto& child : children) CheckSchema(child);
  }
}

auto CatalogBytes(const std::vector<ToolSpec>& specs) -> size_t {
  std::vector<std::string> rows;
  for (const auto& spec : specs)
    rows.push_back(json::Encode(
        json::Members{{"name", json::Quote(spec.name)},
                      {"description", json::Quote(spec.description)},
                      {"input_schema", spec.input_schema}}));
  return json::Encode(rows).size();
}

TEST(ModelCatalog, ExactRolesOrderAndBudgets) {
  const Catalog catalog;
  const std::vector<std::string> main{"project_edit",
                                      "project_copy",
                                      "project_connect",
                                      "project_make_subgraph",
                                      "project_layout",
                                      "project_inspect",
                                      "card_id",
                                      "project_file_list",
                                      "project_file_read",
                                      "input_types",
                                      "project_validate",
                                      "run_plan",
                                      "run",
                                      "run_record",
                                      "tool_result_read",
                                      "demo_list",
                                      "demo_read",
                                      "delegate",
                                      "tools",
                                      "project_save"};
  auto helper = main;
  std::erase(helper, "delegate");
  EXPECT_EQ(Names(catalog.ModelSpecs()), main);
  EXPECT_EQ(Names(catalog.ModelSpecs(ModelRole::Helper, false)), helper);
  EXPECT_EQ(Names(catalog.ModelSpecs(ModelRole::ScopedHelper, false)), helper);
  EXPECT_EQ(Names(catalog.ModelSpecs(ModelRole::ScopedHelper, true)), main);
  EXPECT_EQ(Names(catalog.ModelSpecs(ModelRole::Guide)),
            (std::vector<std::string>{"project_inspect", "project_file_list",
                                      "project_file_read", "run_record",
                                      "tool_result_read", "demo_list",
                                      "demo_read", "tools", "guide_show"}));
  // Budgets include names, descriptions, schemas and JSON framing, with room
  // for small additions. Card-copy sources increased the helper catalog to
  // 13614 bytes; retain a small margin without pinning policy wording.
  EXPECT_LT(CatalogBytes(catalog.ModelSpecs()), 15000u);
  EXPECT_LT(CatalogBytes(catalog.ModelSpecs(ModelRole::Helper, false)), 13700u);
  EXPECT_LT(CatalogBytes(catalog.ModelSpecs(ModelRole::ScopedHelper, false)),
            13700u);
  EXPECT_LT(CatalogBytes(catalog.ModelSpecs(ModelRole::Guide)), 4600u);
}

TEST(ModelCatalog, EverySchemaIsStructuredAndEveryOperationIsReachableOnce) {
  const Catalog catalog;
  for (const auto& spec : catalog.AllSpecs()) CheckSchema(spec.input_schema);
  const auto settings = json::Object(json::Get(catalog.shared(), "modelTools"));
  const auto families = json::Object(json::Get(settings, "families"));
  for (const auto role : {ModelRole::Main, ModelRole::Helper,
                          ModelRole::ScopedHelper, ModelRole::Guide}) {
    std::set<std::string> reachable;
    const auto direct = catalog.ModelSpecs(role);
    for (const auto& spec : direct) {
      CheckSchema(spec.input_schema);
      ASSERT_TRUE(reachable.insert(spec.name).second);
    }
    for (const auto& [family, ignored] : families) {
      (void)ignored;
      for (const auto& spec : catalog.FamilySpecs(family, role)) {
        CheckSchema(spec.input_schema);
        const auto name = catalog.FamilyTool(family, spec.name, role);
        EXPECT_TRUE(catalog.Allows(name, role));
        EXPECT_TRUE(reachable.insert(name).second) << name;
      }
    }
    for (const auto& spec : catalog.AllSpecs())
      EXPECT_EQ(reachable.contains(spec.name), catalog.Allows(spec.name, role))
          << spec.name;
  }
  const auto validation = json::Object(json::Get(
      json::Object(catalog.Find("project_validate")->spec.input_schema),
      "properties"));
  EXPECT_FALSE(validation.contains("changes"));
}

TEST(ModelCatalog, FamilyModesRespectRolesAndRejectUnknownArguments) {
  const Catalog catalog;
  EXPECT_EQ(catalog.FamilyTool("runs", "status", ModelRole::Guide),
            "run_status");
  EXPECT_EQ(catalog.FamilyTool("sdk", "prepare", ModelRole::ScopedHelper),
            "card_s_d_k_prepare");
  EXPECT_THROW(catalog.FamilyTool("runs", "cancel", ModelRole::Guide),
               host::Failure);
  EXPECT_THROW(
      catalog.FamilyTool("versions", "checkout", ModelRole::ScopedHelper),
      host::Failure);
  EXPECT_TRUE(catalog.FamilySpecs("maintenance", ModelRole::Guide).empty());
  EXPECT_TRUE(catalog.FamilySpecs("versions", ModelRole::ScopedHelper).empty());
  EXPECT_THROW(catalog.FamilySpecs("unknown", ModelRole::Main), host::Failure);
  EXPECT_THROW(catalog.FamilyTool("files", "unknown", ModelRole::Main),
               host::Failure);
  EXPECT_THROW(
      catalog.ValidateArguments("tools", R"({"family":"runs","extra":1})"),
      host::Failure);
  EXPECT_NO_THROW(catalog.ValidateArguments(
      "tools",
      R"({"family":"runs","action":"status","arguments":{"run_id":"1"}})"));
  EXPECT_THROW(catalog.ValidateArguments("run_status", R"({"extra":1})"),
               host::Failure);
}
}  // namespace
}  // namespace tsugumi::agent
