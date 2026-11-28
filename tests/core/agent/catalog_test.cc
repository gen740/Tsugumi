// The allow list is the sole source of project tools, including external MCP.
#include "agent/catalog.hh"

#include <gtest/gtest.h>

#include "host/agent_policy.hh"
#include "host/failure.hh"
#include "tsugumi/protocol/tsugumi_generated.h"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::agent {
namespace {

TEST(AgentCatalog, DraftSaveKeepsPortLessEnds) {
  const Catalog catalog;
  const auto* tool = catalog.Find("project_save");
  ASSERT_NE(tool, nullptr);
  const auto request = catalog.Request(
      "project_save",
      R"({"base_rev":0,"op_id":"save","origin":"agent","changes":[{"value_type":"SetProject","value":{"json":"{\"canvas\":{\"connections\":[{\"from\":{\"card\":\"plan\"},\"to\":{\"card\":\"next\",\"port\":\"in\"}}]}}"}}]})",
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
  EXPECT_FALSE(from.contains("port"));
}

TEST(AgentCatalog, EnumArgumentsNameTheAcceptedValues) {
  try {
    EncodeProjectRequest(
        "log_range",
        R"({"run_id":"r","card":{"segments":["a"]},"stream":"stdout"})", 1);
    ADD_FAILURE() << "accepted an unknown stream";
  } catch (const host::Failure& error) {
    EXPECT_EQ(error.code, "invalid_request");
    EXPECT_NE(std::string(error.what()).find(
                  "stream must be one of output, journal, build"),
              std::string::npos)
        << error.what();
  }
  EXPECT_NO_THROW(EncodeProjectRequest(
      "log_range",
      R"({"run_id":"r","card":{"segments":["a"]},"stream":"output"})",
      1));
  // The tool description names the streams and how to find an invocation.
  const Catalog catalog;
  EXPECT_NE(catalog.Find("log_range")->spec.input_schema.find("output (a Card's "
                                                         "stdout and stderr"),
            std::string::npos);
  EXPECT_NE(catalog.Find("run_value_read")->spec.input_schema.find("invocations"),
            std::string::npos);
}

TEST(AgentCatalog, InternalVersionNoteRequestsAreNotTools) {
  const Catalog catalog;
  for (const auto* name : {"version_note", "version_comments"}) {
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
}

TEST(AgentCatalog, ExposesThePublicTools) {
  const Catalog catalog;
  for (const auto* name :
       {"project_inspect", "project_file_read", "run_history_delete",
        "project_cache_clear", "version_checkout", "version_init",
        "version_mark", "nix_path_lock_update"})
    EXPECT_NE(catalog.Find(name), nullptr) << name;
  // Run scratch is erased when each invocation ends; there is nothing to
  // delete by hand.
  EXPECT_EQ(catalog.Find("run_scratch_delete"), nullptr);
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
}

TEST(AgentCatalog, TemplateToolsUseTheExpectedApprovalAndRolePolicy) {
  const Catalog catalog;
  EXPECT_EQ(catalog.Find("document_template"), nullptr);
  EXPECT_TRUE(catalog.Allows("template_list", ModelRole::Guide));
  EXPECT_FALSE(catalog.Allows("template_copy", ModelRole::Guide));
  EXPECT_FALSE(catalog.Allows("template_copy", ModelRole::ScopedHelper));
  EXPECT_EQ(catalog.FamilyTool("templates", "copy", ModelRole::Main),
            "template_copy");
  EXPECT_TRUE(catalog.FamilySpecs("templates", ModelRole::Guide).empty());
  for (const auto* name :
       {"template_list", "template_copy", "template_add", "template_import"}) {
    const auto* tool = catalog.Find(name);
    ASSERT_NE(tool, nullptr);
    EXPECT_EQ(tool->mutating, std::string_view(name) != "template_list");
  }
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

}  // namespace
}  // namespace tsugumi::agent
