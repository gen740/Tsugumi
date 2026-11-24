#include <gtest/gtest.h>

#include <algorithm>
#include <set>

#include "agent/catalog.hh"
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

TEST(ModelCatalog, RolesExposeCapabilitiesWithoutWideningPermissions) {
  const Catalog catalog;
  const auto check = [&](ModelRole role, bool can_delegate) {
    const auto names = Names(catalog.ModelSpecs(role, can_delegate));
    const std::set<std::string> unique(names.begin(), names.end());
    for (const auto* name :
         {"project_inspect", "project_file_read", "run_record"})
      EXPECT_TRUE(unique.contains(name));
    if (role == ModelRole::Guide) {
      EXPECT_TRUE(unique.contains("guide_show"));
      for (const auto* name :
           {"project_save", "project_edit", "run", "delegate"})
        EXPECT_FALSE(unique.contains(name));
    } else {
      EXPECT_TRUE(unique.contains("project_edit"));
      EXPECT_TRUE(unique.contains("project_save"));
      EXPECT_TRUE(unique.contains("run"));
      EXPECT_EQ(unique.contains("delegate"), can_delegate);
    }
  };
  check(ModelRole::Main, true);
  check(ModelRole::Helper, false);
  check(ModelRole::ScopedHelper, false);
  check(ModelRole::ScopedHelper, true);
  check(ModelRole::Guide, false);
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
  const auto helper = catalog.ModelSpecs(ModelRole::ScopedHelper);
  EXPECT_TRUE(std::ranges::any_of(helper, [](const ToolSpec& tool) {
    return tool.name == "evaluate_nix";
  }));
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
  EXPECT_THROW(catalog.Request("run_status", R"({"extra":1})", 1),
               host::Failure);
}
}  // namespace
}  // namespace tsugumi::agent
