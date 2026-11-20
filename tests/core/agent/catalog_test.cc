// The allow list is the sole source of project tools, including external MCP.
#include "agent/catalog.hh"

#include <gtest/gtest.h>

#include "agent/approval.hh"
#include "host/agent_policy.hh"
#include "tsugumi/protocol/tsugumi_generated.h"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::agent {
namespace {

TEST(AgentCatalog, DraftSavePreservesEmptyEnds) {
  const Catalog catalog;
  const auto* tool = catalog.Find("project_save");
  ASSERT_NE(tool, nullptr);
  const auto request = catalog.Request(
      "project_save",
      R"({"base_rev":0,"op_id":"save","origin":"agent","changes":[{"value_type":"SetProject","value":{"json":"{\"canvas\":{\"connections\":[{\"from\":{\"card\":\"plan\",\"port\":\"\"},\"to\":{\"card\":\"next\",\"port\":\"in\"}}]}}"}}]})",
      1);
  const auto* save = Tsugumi::RPC::GetMessage(request.data())
                         ->payload_as_Request()
                         ->value_as_ProjectSaveRequest();
  ASSERT_NE(save, nullptr);
  const auto* project = save->changes()->Get(0)->value_as_SetProject();
  ASSERT_NE(project, nullptr);
  const auto canvas =
      json::Object(json::Get(json::Object(project->json()->str()), "canvas"));
  const auto wire =
      json::Object(json::Array(json::Get(canvas, "connections")).front());
  const auto from = json::Object(json::Get(wire, "from"));
  EXPECT_TRUE(from.contains("port"));
  EXPECT_EQ(json::Text(from, "port"), "");
}

TEST(AgentCatalog, InternalVersionNoteRequestsAreNotTools) {
  const Catalog catalog;
  for (const auto* name :
       {"version_note", "version_diff", "version_comments", "version_summary"}) {
    EXPECT_EQ(catalog.Find(name), nullptr);
    EXPECT_THROW(catalog.Request(name, "{}", 1), std::runtime_error);
  }
  const auto read =
      EncodeProjectRequest("version_note", R"({"commit":"abc"})", 7);
  EXPECT_EQ(Tsugumi::RPC::GetMessage(read.data())->request_id(), 7u);
  EXPECT_NE(Tsugumi::RPC::GetMessage(read.data())
                ->payload_as_Request()
                ->value_as_VersionNoteRequest(),
            nullptr);
  EXPECT_THROW(EncodeProjectRequest("project_file_format", "{}", 8),
               std::runtime_error);
  const auto summary = EncodeProjectRequest(
      "version_summary", R"({"commit":"abc","english":true})", 8);
  EXPECT_NE(Tsugumi::RPC::GetMessage(summary.data())
                ->payload_as_Request()
                ->value_as_VersionSummaryRequest(),
            nullptr);
}

TEST(AgentCatalog, ExposesPublicToolsWithTheExpectedApprovalPolicy) {
  const Catalog catalog;
  for (const auto* name : {"project_inspect", "project_file_read"}) {
    const auto* tool = catalog.Find(name);
    ASSERT_NE(tool, nullptr) << name;
    EXPECT_FALSE(tool->approval_required) << name;
  }
  const auto* guarded = catalog.Find("run_history_delete");
  ASSERT_NE(guarded, nullptr);
  EXPECT_TRUE(guarded->approval_required);
  for (const auto* denied :
       {"view_save", "view_set_visible", "project_open", "session_hello"}) {
    EXPECT_EQ(catalog.Find(denied), nullptr);
    EXPECT_THROW(catalog.Request(denied, "{}", 1), std::runtime_error);
  }
  for (const auto* name : {"project_save", "project_reload", "project_shell"}) {
    const auto* tool = catalog.Find(name);
    ASSERT_NE(tool, nullptr);
    const auto properties = json::Object(
        json::Get(json::Object(tool->spec.input_schema), "properties"));
    for (const auto* owned :
         {"origin", "op_id", "approved_permissions", "check_permissions",
          "read_only", "hide_credentials", "no_project_socket", "transfers",
          "working_directory", "writable_paths", "mounts", "output_prefix",
          "workspace_only", "protected_paths"})
      EXPECT_FALSE(properties.contains(owned));
  }
  const auto history = json::Object(json::Get(
      json::Object(catalog.Find("run_history_delete")->spec.input_schema),
      "properties"));
  EXPECT_FALSE(history.contains("run_ids"));
  EXPECT_FALSE(history.contains("restrict_to_run_ids"));
}

TEST(AgentPolicy, SaveAndReloadAlwaysRequestPermissionComparison) {
  for (const auto* name : {"project_save", "project_reload"}) {
    auto arguments =
        json::Object(R"({"origin":"ui","check_permissions":false})");
    host::RestrictAgentRequest(name, arguments);
    EXPECT_EQ(json::Get(arguments, "check_permissions"), "true");
    arguments["op_id"] = json::Quote("operation");
    if (std::string_view(name) == "project_save") arguments["changes"] = "[]";
    const auto bytes = EncodeProjectRequest(name, json::Encode(arguments), 1);
    const auto* request =
        Tsugumi::RPC::GetMessage(bytes.data())->payload_as_Request();
    if (std::string_view(name) == "project_save")
      EXPECT_TRUE(request->value_as_ProjectSaveRequest()->check_permissions());
    else
      EXPECT_TRUE(
          request->value_as_ProjectReloadRequest()->check_permissions());
  }
}

TEST(AgentPolicy, ShellAlwaysRequestsAllRestrictions) {
  auto arguments = json::Object(
      R"({"op_id":"operation","command":"true","origin":"ui","read_only":false,"hide_credentials":false,"no_project_socket":false})");
  host::RestrictAgentRequest("project_shell", arguments);
  EXPECT_FALSE(arguments.contains("origin"));
  const auto bytes =
      EncodeProjectRequest("project_shell", json::Encode(arguments), 1);
  const auto* request = Tsugumi::RPC::GetMessage(bytes.data())
                            ->payload_as_Request()
                            ->value_as_ProjectShellRequest();
  ASSERT_NE(request, nullptr);
  EXPECT_TRUE(request->read_only());
  EXPECT_TRUE(request->hide_credentials());
  EXPECT_TRUE(request->no_project_socket());
}

TEST(AgentCatalog, AttachedFoldersHaveNoProjectRPC) {
  const Catalog catalog;
  EXPECT_NE(catalog.Find("attached_folder_read"), nullptr);
  EXPECT_THROW(EncodeProjectRequest("attached_folder_read",
                                    R"({"folder":"/ungranted","path":""})", 1),
               std::runtime_error);
}

TEST(AgentApprovalDescription, HistoryListsEverySelectedRunAcrossPages) {
  int calls = 0;
  std::vector<std::string> run_ids;
  const auto preview = DescribeOperation(
      "run_history_delete", {{"ended_before", "\"50\""}}, "project",
      [&](const auto& name, const auto& args) {
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
      },
      &run_ids);
  EXPECT_EQ(calls, 2);
  EXPECT_NE(preview.find("old"), std::string::npos);
  EXPECT_EQ(preview.find("recent"), std::string::npos);
  EXPECT_EQ(preview.find("active"), std::string::npos);
  EXPECT_EQ(json::UInt(json::Get(
                json::Object(preview.substr(
                    preview.find('{'),
                    preview.find('\n', preview.find('{')) - preview.find('{'))),
                "run_count")),
            1u);
  EXPECT_EQ(run_ids, std::vector<std::string>{json::Quote("old")});
  // A run completing after the final description must not enter the RPC's
  // deletion set. Only IDs collected from that description go over the wire.
  const auto bytes = EncodeProjectRequest(
      "run_history_delete",
      json::Encode(json::Members{{"op_id", json::Quote("operation")},
                                 {"run_ids", json::Encode(run_ids)},
                                 {"restrict_to_run_ids", "true"}}),
      9);
  const auto* request = Tsugumi::RPC::GetMessage(bytes.data())
                            ->payload_as_Request()
                            ->value_as_RunHistoryDeleteRequest();
  EXPECT_TRUE(request->restrict_to_run_ids());
  ASSERT_NE(request->run_ids(), nullptr);
  ASSERT_EQ(request->run_ids()->size(), 1u);
  EXPECT_EQ(request->run_ids()->Get(0)->str(), "old");
}

TEST(AgentApprovalDescription, EmptyHistoryApprovalSendsAnExplicitEmptySet) {
  std::vector<std::string> run_ids{json::Quote("stale")};
  DescribeOperation(
      "run_history_delete", {}, "project",
      [](const auto&, const auto&) {
        return json::Object(
            R"({"runs":[{"status":{"run_id":"active","state":"running"}}]})");
      },
      &run_ids);
  EXPECT_TRUE(run_ids.empty());
  const auto bytes = EncodeProjectRequest(
      "run_history_delete",
      json::Encode(json::Members{{"op_id", json::Quote("operation")},
                                 {"run_ids", json::Encode(run_ids)},
                                 {"restrict_to_run_ids", "true"}}),
      10);
  const auto* request = Tsugumi::RPC::GetMessage(bytes.data())
                            ->payload_as_Request()
                            ->value_as_RunHistoryDeleteRequest();
  EXPECT_TRUE(request->restrict_to_run_ids());
  EXPECT_TRUE(!request->run_ids() || request->run_ids()->size() == 0);
}

TEST(AgentApprovalDescription,
     CacheAndScratchUseOnlyTheCurrentProjectsMeasurements) {
  auto read = [](const auto& name, const auto&) {
    EXPECT_EQ(name, "project_cache_usage");
    return json::Object(
        R"({"current_project":"our-cache","projects":[{"project":"other-cache","build_bytes":"9999"},{"project":"our-cache","name":"Our project","build_bytes":"1234","run_scratch":[{"path":"runs/00000000-0000-4000-8000-000000000001","bytes":"75"},{"path":"runs/old","bytes":"80"}]}]})");
  };
  const auto cache =
      DescribeOperation("project_cache_clear", {}, "project", read);
  EXPECT_NE(cache.find("1234"), std::string::npos);
  EXPECT_EQ(cache.find("9999"), std::string::npos);
  const auto scratch = DescribeOperation(
      "run_scratch_delete",
      {{"paths", R"(["runs/00000000-0000-4000-8000-000000000001"])"}},
      "project", read);
  EXPECT_NE(scratch.find("75"), std::string::npos);
  EXPECT_EQ(scratch.find("runs/old"), std::string::npos);
}

TEST(AgentApprovalDescription,
     VersionsShowCoreIdentityAndRejectMovingReferences) {
  auto read = [](const auto& name, const auto&) {
    if (name == "project_inspect")
      return json::Object(
          R"({"rev":"7","project_json":"{\"name\":\"Actual project\"}"})");
    EXPECT_EQ(name, "version_graph");
    return json::Object(
        R"({"head":"0123456789abcdef0123456789abcdef01234567","enabled":true,"dirty":false,"commits":[{"id":"0123456789abcdef0123456789abcdef01234567","summary":"Actual version"}]})");
  };
  const auto checkout = DescribeOperation(
      "version_checkout",
      {{"commit", "\"0123456789abcdef0123456789abcdef01234567\""}}, "project",
      read);
  EXPECT_NE(checkout.find("Actual version"), std::string::npos);
  EXPECT_NE(checkout.find("Actual project"), std::string::npos);
  EXPECT_THROW(DescribeOperation("version_checkout", {{"commit", "\"HEAD\""}},
                                 "project", read),
               std::runtime_error);
}

}  // namespace
}  // namespace tsugumi::agent
