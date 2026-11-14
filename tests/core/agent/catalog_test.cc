// The allow list is the sole source of project tools, including external MCP.
#include <gtest/gtest.h>

#include <set>

#include "agent/catalog.hh"
#include "agent/json.hh"

extern const unsigned char tsugumi_agent_settings[];
extern const unsigned long tsugumi_agent_settings_size;

namespace tsugumi::agent {
namespace {

TEST(AgentCatalog, ExposesExactlyTheSharedRequestList) {
  const auto settings = json::Object(std::string_view(
      reinterpret_cast<const char*>(tsugumi_agent_settings),
      tsugumi_agent_settings_size));
  std::set<std::string> expected;
  for (const auto& entry : json::Array(json::Get(settings, "requests")))
    ASSERT_TRUE(expected.insert(json::String(entry)).second);
  const Catalog catalog;
  std::set<std::string> actual;
  for (const auto& tool : catalog.tools())
    actual.insert(tool.request_name);
  EXPECT_EQ(actual, expected);
  EXPECT_EQ(catalog.tools().size(), expected.size());
  for (const auto* denied : {"run_history_delete", "project_cache_clear",
                             "version_checkout", "version_summary"}) {
    EXPECT_EQ(catalog.Find(denied), nullptr);
    EXPECT_THROW(catalog.Request(denied, "{}", 1), std::runtime_error);
  }
  for (const auto* name : {"project_save", "project_reload", "project_shell"}) {
    const auto* tool = catalog.Find(name);
    ASSERT_NE(tool, nullptr);
    const auto properties = json::Object(json::Get(
        json::Object(tool->spec.input_schema), "properties"));
    for (const auto* owned : {"origin", "op_id", "approved_permissions"})
      EXPECT_FALSE(properties.contains(owned));
    EXPECT_TRUE(tool->mutating);
    EXPECT_TRUE(tool->has_origin);
  }
}

}  // namespace
}  // namespace tsugumi::agent
