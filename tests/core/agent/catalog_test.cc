// The allow list is the sole source of project tools, including external MCP.
#include <gtest/gtest.h>
#include <flatbuffers/reflection_generated.h>

#include <set>

#include "agent/approval.hh"
#include "agent/catalog.hh"
#include "agent/json.hh"
#include "tsugumi/protocol/tsugumi_generated.h"

extern const unsigned char tsugumi_agent_settings[];
extern const unsigned long tsugumi_agent_settings_size;
extern const unsigned char tsugumi_rpc_bfbs[];

namespace tsugumi::agent {
namespace {

TEST(AgentCatalog, EveryEntryInBothListsExistsInTheProjectRequestUnion) {
  const auto settings = json::Object(
      std::string_view(reinterpret_cast<const char *>(tsugumi_agent_settings),
                       tsugumi_agent_settings_size));
  const auto *requests = reflection::GetSchema(tsugumi_rpc_bfbs)
                             ->enums()->LookupByKey("Tsugumi.RPC.RequestPayload");
  ASSERT_NE(requests, nullptr);
  for (const auto *list : {"requests", "approvalRequests"}) {
    SCOPED_TRACE(list);
    for (const auto &entry : json::Array(json::Get(settings, list))) {
      const auto name = json::String(entry);
      SCOPED_TRACE(name);
      bool found = false;
      for (const auto *value : *requests->values())
        if (value->name()->str() == name && value->value()) found = true;
      EXPECT_TRUE(found) << "invalid Agent request allow list entry: " << name;
    }
  }
}

TEST(AgentCatalog, InternalVersionNoteRequestsAreNotTools) {
  const Catalog catalog;
  for (const auto *name : {"version_note", "version_note_store", "version_summary"}) {
    EXPECT_EQ(catalog.Find(name), nullptr);
    EXPECT_THROW(catalog.Request(name, "{}", 1), std::runtime_error);
  }
  const auto read = EncodeProjectRequest(
      "version_note", R"({"commit":"abc","language":"en"})", 7);
  EXPECT_EQ(Tsugumi::RPC::GetMessage(read.data())->request_id(), 7u);
  EXPECT_NE(Tsugumi::RPC::GetMessage(read.data())->payload_as_Request()
                ->value_as_VersionNoteRequest(), nullptr);
  const auto write = EncodeProjectRequest(
      "version_note_store",
      R"({"op_id":"operation","origin":"agent","commit":"abc","summary":"Note"})", 8);
  EXPECT_NE(Tsugumi::RPC::GetMessage(write.data())->payload_as_Request()
                ->value_as_VersionNoteStoreRequest(), nullptr);
  EXPECT_THROW(EncodeProjectRequest("version_summary", "{}", 1), std::runtime_error);
}

TEST(AgentCatalog, ExposesExactlyTheSharedRequestList) {
  const auto settings = json::Object(
      std::string_view(reinterpret_cast<const char *>(tsugumi_agent_settings),
                       tsugumi_agent_settings_size));
  std::set<std::string> expected;
  for (const auto &entry : json::Array(json::Get(settings, "requests")))
    ASSERT_TRUE(expected.insert(json::String(entry)).second);
  std::set<std::string> approval;
  for (const auto &entry :
       json::Array(json::Get(settings, "approvalRequests"))) {
    ASSERT_TRUE(approval.insert(json::String(entry)).second);
    ASSERT_TRUE(expected.insert(json::String(entry)).second);
  }
  ASSERT_FALSE(approval.empty());
  const Catalog catalog;
  std::set<std::string> actual;
  for (const auto &tool : catalog.tools()) {
    actual.insert(tool.request_name);
    EXPECT_EQ(tool.approval_required, approval.contains(tool.request_name));
  }
  EXPECT_EQ(actual, expected);
  EXPECT_EQ(catalog.tools().size(), expected.size());
  for (const auto *denied : {"view_save", "view_set_visible", "project_open",
                             "project_close", "session_hello"}) {
    EXPECT_EQ(catalog.Find(denied), nullptr);
    EXPECT_THROW(catalog.Request(denied, "{}", 1), std::runtime_error);
  }
  for (const auto *name : {"project_save", "project_reload", "project_shell"}) {
    const auto *tool = catalog.Find(name);
    ASSERT_NE(tool, nullptr);
    const auto properties = json::Object(
        json::Get(json::Object(tool->spec.input_schema), "properties"));
    for (const auto *owned : {"origin", "op_id", "approved_permissions"})
      EXPECT_FALSE(properties.contains(owned));
    EXPECT_TRUE(tool->mutating);
    EXPECT_TRUE(tool->has_origin);
  }
}

TEST(AgentApprovalDescription, HistoryListsEverySelectedRunAcrossPages) {
  int calls = 0;
  const auto preview = DescribeOperation(
      "run_history_delete", {{"ended_before", "\"50\""}}, "project",
      [&](const auto &name, const auto &args) {
        EXPECT_EQ(name, "run_history");
        // The public history RPC accepts at most 500 rows per page.
        EXPECT_EQ(json::Get(args, "limit"), "500");
        ++calls;
        if (calls == 1) {
          EXPECT_FALSE(args.contains("cursor"));
          return json::Object(
              R"({"runs":[{"status":{"run_id":"recent","state":"succeeded","ended_at":"60"}},{"status":{"run_id":"active","state":"running"}}],"next_cursor":"page-two"})");
        }
        EXPECT_EQ(json::Text(args, "cursor"), "page-two");
        return json::Object(
            R"({"runs":[{"status":{"run_id":"old","state":"failed","ended_at":"10"}}]})");
      });
  EXPECT_EQ(calls, 2);
  EXPECT_NE(preview.find("old"), std::string::npos);
  EXPECT_EQ(preview.find("recent"), std::string::npos);
  EXPECT_EQ(preview.find("active"), std::string::npos);
  EXPECT_NE(preview.find("\"run_count\":1"), std::string::npos);
}

TEST(AgentApprovalDescription,
     CacheAndScratchUseOnlyTheCurrentProjectsMeasurements) {
  auto read = [](const auto &name, const auto &) {
    EXPECT_EQ(name, "project_cache_usage");
    return json::Object(
        R"({"current_project":"our-cache","projects":[{"project":"other-cache","build_bytes":"9999"},{"project":"our-cache","name":"Our project","build_bytes":"1234","run_scratch":[{"path":"recovery","bytes":"75"},{"path":"runs/old","bytes":"80"}]}]})");
  };
  const auto cache =
      DescribeOperation("project_cache_clear", {}, "project", read);
  EXPECT_NE(cache.find("1234"), std::string::npos);
  EXPECT_EQ(cache.find("9999"), std::string::npos);
  const auto scratch = DescribeOperation(
      "run_scratch_delete", {{"paths", R"(["recovery"])"}}, "project", read);
  EXPECT_NE(scratch.find("75"), std::string::npos);
  EXPECT_EQ(scratch.find("runs/old"), std::string::npos);
}

TEST(AgentApprovalDescription,
     VersionsShowCoreIdentityAndRejectMovingReferences) {
  auto read = [](const auto &name, const auto &) {
    if (name == "project_inspect")
      return json::Object(
          R"({"rev":"7","project_json":"{\"name\":\"Actual project\"}"})");
    EXPECT_EQ(name, "version_graph");
    return json::Object(
        R"({"head":"full-id","enabled":true,"dirty":false,"commits":[{"id":"full-id","summary":"Actual version"}]})");
  };
  const auto checkout = DescribeOperation(
      "version_checkout", {{"commit", "\"full-id\""}}, "project", read);
  EXPECT_NE(checkout.find("Actual version"), std::string::npos);
  EXPECT_NE(checkout.find("Actual project"), std::string::npos);
  EXPECT_THROW(DescribeOperation("version_checkout", {{"commit", "\"HEAD\""}},
                                 "project", read),
               std::runtime_error);
}

} // namespace
} // namespace tsugumi::agent
