#include "host/agent_exclude.hh"

#include <gtest/gtest.h>

#include <algorithm>

#include "../agent_fixture_server.hh"
#include "host/attached_folder.hh"
#include "host/demo_references.hh"
#include "host/document_edits.hh"
#include "host/failure.hh"
#include "host/project_operations.hh"
#include "host/projects.hh"
#include "tsugumi/document/document.hh"
#include "tsugumi/util/crypto.hh"

namespace tsugumi::host {
namespace {
namespace j = ::tsugumi::json;
namespace H = Tsugumi::Host;
using test::Read;
using test::Write;
class AgentExcludeTest : public testing::Test {
 protected:
  test::TemporaryDirectory temporary;
  const std::filesystem::path root = temporary.path / "project";
  auto SetUp() -> void override { std::filesystem::create_directories(root); }
  auto Refused(const std::function<void()>& call,
               const std::string& code = "not_found") -> void {
    try {
      call();
      FAIL() << "expected refusal";
    } catch (const Failure& error) {
      EXPECT_EQ(error.code, code) << error.what();
    }
  }
};
TEST_F(AgentExcludeTest, DefaultsExplicitEmptyAndResolvedPaths) {
  AgentExclude defaults(root, "{}");
  for (const auto* path : {".git", ".git/config", ".DS_Store"})
    EXPECT_TRUE(defaults.Contains(path));
  EXPECT_FALSE(defaults.Contains(".github/workflows/ci.yml"));
  EXPECT_FALSE(AgentExclude(root, R"({"agentExclude":[]})").Contains(".git"));
  Write(root / "private/secret", "SECRET");
  std::filesystem::create_directory_symlink(root / "private", root / "alias");
  AgentExclude policy(root, R"({"agentExclude":["private"]})");
  EXPECT_TRUE(policy.Contains("alias/secret"));
  EXPECT_TRUE(policy.Contains(root / "alias/new-file"));
  EXPECT_FALSE(policy.Contains("private-other/secret"));
  const auto outside = temporary.path / "outside";
  AgentExclude absolute(
      root,
      j::Encode(j::Members{
          {"agentExclude",
           j::Encode(std::vector<std::string>{j::Quote(outside.string())})}}));
  EXPECT_TRUE(absolute.Contains(outside / "new/file"));
}
TEST_F(AgentExcludeTest, NestedCardSelectorsFollowTheActualExternalFolder) {
  Write(root / "definitions/group/_card.json",
        R"({"type":"subgraph","external":"unaccepted-folder"})");
  AgentExclude policy(
      root,
      R"({"agentExclude":["externals/different-folder/definitions/leaf"]})",
      {{"definitions/group/_card.json", R"({"type":"subgraph","external":"different-folder"})"}});
  Refused([&] {
    policy.CheckContent(R"({"targets":[{"segments":["group","leaf"]}]})");
  });
  Refused([&] {
    policy.CheckRequest("run_record",
                        {{"card", R"({"segments":["group","leaf"]})"}});
  });
}
TEST_F(AgentExcludeTest, FileFamiliesAndSnapshotOmitHiddenContent) {
  AgentExclude policy(
      root,
      R"({"agentExclude":["definitions/hidden","definitions/public/_meta.json"]})");
  for (const auto* tool : {"project_file_list", "project_file_read",
                           "project_file_stat", "project_file_format"})
    Refused([&] {
      policy.CheckRequest(tool,
                          {{"path", R"("definitions/hidden/src/main.py")"}});
    });
  const auto filtered = policy.Filter(
      R"({"entries":[{"path":"definitions/hidden/src/main.py"},{"path":"definitions/public/main.py"}],"definitions":[{"id":"hidden","card_json":"SECRET"},{"id":"public","card_json":"{\"type\":\"code\"}","meta_json":"SECRET"}],"project_json":"{\"canvas\":{\"cards\":[{\"id\":\"hidden\"},{\"id\":\"public\"}],\"connections\":[]}}"})");
  EXPECT_EQ(filtered.find("SECRET"), std::string::npos);
  EXPECT_EQ(filtered.find("hidden"), std::string::npos);
  EXPECT_NE(filtered.find("public"), std::string::npos);
  // RPC Card selectors and helper calls use the same host policy.
  Refused([&] {
    policy.CheckRequest("run_record", {{"card", R"({"segments":["hidden"]})"}});
  });
}
TEST_F(AgentExcludeTest, NestedDocumentTextRequiresTheScopedFileRead) {
  AgentExclude policy(root, "{}");
  const auto filtered = j::Object(policy.Filter(
      R"({"sources":[{"path":"externals/sub/tsugumi.json","hash":"h","document_json":"{\"name\":\"Private workflow\"}"}]})"));
  const auto sources = j::Array(filtered.at("sources"));
  ASSERT_EQ(sources.size(), 1u);
  const auto source = j::Object(sources.front());
  EXPECT_EQ(j::Text(source, "path"), "externals/sub/tsugumi.json");
  EXPECT_EQ(j::Text(source, "hash"), "h");
  EXPECT_FALSE(source.contains("document_json"));
}

TEST_F(AgentExcludeTest, AttachmentsAndDemoToolsShareTheRealPathBoundary) {
  Write(root / "visible.txt", "VISIBLE");
  Write(root / "hidden/secret.txt", "SECRET");
  std::filesystem::create_directory_symlink(root / "hidden", root / "alias");
  AgentExclude policy(root, R"({"agentExclude":["hidden"]})");
  const auto listing = ReadAttachedFolder(root, {{"path", "\"\""}}, &policy);
  EXPECT_EQ(listing.find("hidden"), std::string::npos);
  EXPECT_EQ(listing.find("alias"), std::string::npos);
  Refused([&] {
    ReadAttachedFolder(root, {{"path", R"("alias/secret.txt")"}}, &policy);
  });
  const auto demos = temporary.path / "demos";
  Write(
      demos / "en/index.json",
      R"({"demos":[{"id":"finished","path":"finished","title":"Finished"},{"id":"draft","path":"draft","title":"Draft"}]})");
  Write(demos / "en/finished/tsugumi.json", "{}");
  Write(demos / "en/draft/tsugumi.json", "{}");
  AgentExclude demo_policy(
      root,
      j::Encode(j::Members{
          {"agentExclude", j::Encode(std::vector<std::string>{j::Quote(
                               std::filesystem::canonical(demos / "en/finished")
                                   .string())})}}));
  DemoReferences references(demos);
  EXPECT_EQ(references.List(&demo_policy).find("Finished"), std::string::npos);
  EXPECT_EQ(references.Read("en/index.json", &demo_policy).find("finished"),
            std::string::npos);
  Refused([&] { references.Read("en/finished/tsugumi.json", &demo_policy); });
  Refused([&] { references.Project("en/finished", &demo_policy); });
  EXPECT_NO_THROW(references.Read("en/draft/tsugumi.json", &demo_policy));
  EXPECT_NE(references.List().find("Finished"),
            std::string::npos);  // Human view.
}
TEST_F(AgentExcludeTest, SavesAndExpandedEditsCannotChangePolicyOrHiddenFiles) {
  const std::string project =
      R"({"agentExclude":["definitions/hidden"],"canvas":{"cards":[{"id":"hidden"},{"id":"public"}],"connections":[]}})";
  AgentExclude policy(root, project);
  SaveBase base;
  base.files["tsugumi.json"] = project;
  auto plan = [](const std::string& kind, const j::Members& value) {
    return j::Members{
        {"changes",
         j::Encode(std::vector<std::string>{j::Encode(j::Members{
             {"value_type", j::Quote(kind)}, {"value", j::Encode(value)}})})}};
  };
  Refused(
      [&] {
        policy.CheckSave(
            plan("SetProject", {{"json", j::Quote(R"({"agentExclude":[]})")}}),
            base);
      },
      "permission_denied");
  Refused([&] {
    policy.CheckSave(
        plan("PutSource", {{"path", R"("definitions/hidden/new.txt")"}}), base);
  });
  Refused([&] {
    policy.CheckSave(plan("RemoveDefinition", {{"id", R"("hidden")"}}), base);
  });
  EXPECT_NO_THROW(policy.CheckSave(
      plan("PutSource", {{"path", R"("definitions/public/new.txt")"}}), base));
  auto named = j::Object(project);
  named["name"] = "\"New name\"";
  EXPECT_NO_THROW(policy.CheckSave(
      plan("SetProject", {{"json", j::Quote(j::Encode(named))}}), base));
  // A projected whole-project save cannot delete the hidden Card.
  Refused([&] {
    policy.CheckSave(
        plan("SetProject", {{"json", j::Quote(policy.ProjectText(project))}}),
        base);
  });
  // Its host-expanded project_edit form is checked after expansion too.
  H::ProjectEditsT edits;
  auto edit = std::make_unique<H::ProjectEditT>();
  H::ProjectTextEditT text;
  text.file_path = "tsugumi.json";
  text.old_string = "definitions/hidden";
  text.new_string = "other";
  edit->value.Set(std::move(text));
  edits.edits.push_back(std::move(edit));
  const auto expanded = PrepareProjectEdits(
      edits, base, [](const auto&) -> std::optional<std::string> { return {}; },
      {});
  Refused([&] { policy.CheckSave(expanded, base); }, "permission_denied");
}
TEST_F(AgentExcludeTest, ExclusionDoesNotIdentifyARun) {
  Write(root / "tsugumi.json", initial_project_json("Project"));
  const auto before = load_from_disk(root);
  auto project = j::Object(before->project_json);
  project["agentExclude"] = R"(["private","/some/installation/demo"])";
  Write(root / "tsugumi.json", j::Encode(project));
  const auto after = load_from_disk(root);
  EXPECT_EQ(before->execution_fingerprint, after->execution_fingerprint);
}
TEST_F(AgentExcludeTest, DefinitionRemovalChecksExcludedDescendants) {
  AgentExclude policy(root,
                      R"({"agentExclude":["definitions/a/private.txt"]})");
  SaveBase base;
  Refused([&] {
    policy.CheckSave(
        j::Object(
            R"({"changes":[{"value_type":"RemoveDefinition","value":{"id":"a"}}]})"),
        base);
  });
  EXPECT_NO_THROW(policy.CheckSave(
      j::Object(
          R"({"changes":[{"value_type":"RemoveDefinition","value":{"id":"a-other"}}]})"),
      base));
  EXPECT_NO_THROW(policy.CheckSave(
      j::Object(
          R"({"changes":[{"value_type":"PutDefinition","value":{"id":"a","card_json":"{\"type\":\"code\"}"}}]})"),
      base));
}
TEST_F(AgentExcludeTest, WholeDocumentsCannotDropOrRenameTreesWithHiddenFiles) {
  for (const auto& prefix : {std::string{}, std::string{"externals/nested/"}}) {
    const std::string project = j::Encode(j::Members{
        {"agentExclude", j::Encode(std::vector<std::string>{
                             j::Quote(prefix + "definitions/a/private.txt")})},
        {"canvas",
         R"({"cards":[{"id":"a"},{"id":"public"}],"connections":[]})"}});
    AgentExclude policy(root, project);
    SaveBase base;
    base.files[prefix + "tsugumi.json"] = project;
    auto save = [&](const std::string& graph) {
      auto changed = j::Object(project);
      changed["canvas"] = graph;
      const auto bytes = j::Encode(changed);
      const auto value =
          prefix.empty()
              ? j::Members{{"json", j::Quote(bytes)}}
              : j::Members{{"path", j::Quote(prefix + "tsugumi.json")},
                           {"bytes", j::Quote(bytes)}};
      return j::Members{
          {"changes",
           j::Encode(std::vector<std::string>{j::Encode(j::Members{
               {"value_type",
                j::Quote(prefix.empty() ? "SetProject" : "PutSource")},
               {"value", j::Encode(value)}})})}};
    };
    Refused([&] {
      policy.CheckSave(save(R"({"cards":[{"id":"public"}],"connections":[]})"),
                       base);
    });
    Refused([&] {
      policy.CheckSave(
          save(
              R"({"cards":[{"id":"renamed"},{"id":"public"}],"connections":[]})"),
          base);
    });
    EXPECT_NO_THROW(policy.CheckSave(
        save(R"({"cards":[{"id":"a"}],"connections":[]})"), base));
    // Subgraph conversion removes the selected Card from this graph too.
    const auto nested = MakeSubgraph(project, {"a"}, "group");
    Refused([&] {
      policy.CheckSave(save(j::Get(j::Object(nested.project_json), "canvas")),
                       base);
    });
  }
}
TEST_F(AgentExcludeTest, LinksCannotDetachAnExternalWithHiddenDescendants) {
  AgentExclude policy(
      root,
      R"({"agentExclude":["externals/original/definitions/a/private.txt"]})");
  SaveBase base;
  base.files["definitions/group/_card.json"] = R"({"type":"subgraph","external":"original"})";
  Refused([&] {
    policy.CheckSave(
        j::Object(
            R"({"changes":[{"value_type":"RemoveDefinition","value":{"id":"group"}}]})"),
        base);
  });
  Refused([&] {
    policy.CheckSave(
        j::Object(
            R"({"changes":[{"value_type":"PutDefinition","value":{"id":"group","card_json":"{\"type\":\"subgraph\",\"external\":\"elsewhere\"}"}}]})"),
        base);
  });
  Refused([&] {
    policy.CheckSave(
        j::Object(
            R"({"changes":[{"value_type":"RemoveSource","value":{"path":"definitions/group/_card.json"}}]})"),
        base);
  });
  EXPECT_NO_THROW(policy.CheckSave(
      j::Object(
          R"({"changes":[{"value_type":"PutDefinition","value":{"id":"group","card_json":"{\"type\":\"subgraph\",\"external\":\"original\"}"}}]})"),
      base));
}
TEST_F(AgentExcludeTest,
       DraftCreationResolvesBothCounterpartsAndKeepsDefaults) {
  const auto demos = temporary.path / "demos";
  for (const auto* language : {"ja", "en"})
    Write(demos / language / "finished/tsugumi.json",
          initial_project_json("Finished"));
  for (const auto* language : {"ja", "en"}) {
    Write(demos / language / "finished-draft/tsugumi.json",
          initial_project_json("Draft"));
    Write(
        demos / language / "index.json",
        R"({"demos":[{"id":"finished-draft","path":"finished-draft","title":"Draft","agentExclude":["ja/finished","en/finished"]}]})");
    Projects projects(temporary.path / "state", demos);
    H::HostDemoCopyRequestT copy;
    copy.id = "finished-draft";
    copy.language = language;
    copy.parent = (temporary.path / language).string();
    H::RequestT request;
    request.value.Set(std::move(copy));
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(H::Request::Pack(builder, &request));
    projects.Handle(
        *flatbuffers::GetRoot<H::Request>(builder.GetBufferPointer()), 1);
    const auto entries = AgentExclude::Entries(
        Read(temporary.path / language / "finished-draft/tsugumi.json"));
    EXPECT_EQ(entries.size(), AgentExclude::Defaults().size() + 2);
    EXPECT_NE(
        std::find(entries.begin(), entries.end(),
                  std::filesystem::canonical(demos / "ja/finished").string()),
        entries.end());
    EXPECT_NE(
        std::find(entries.begin(), entries.end(),
                  std::filesystem::canonical(demos / "en/finished").string()),
        entries.end());
  }
}
TEST_F(AgentExcludeTest,
       DemoCardCopyIncludesSourcesAndNestedDefinitionClosure) {
  const auto demo = temporary.path / "demo";
  Write(demo / "tsugumi.json", initial_project_json("Demo"));
  Write(demo / "definitions/group/_card.json", R"({"type":"subgraph","external":"nested"})");
  Write(demo / "definitions/group/_meta.json", R"({"name":"Group"})");
  Write(demo / "externals/nested/tsugumi.json", initial_project_json("Nested"));
  Write(demo / "externals/nested/definitions/leaf/_card.json",
        R"({"type":"draft"})");
  Write(demo / "externals/nested/definitions/leaf/src/main.py",
        "COMPLETE_SOURCE");
  H::ProjectOperateRequestT operation;
  operation.project_id = "p";
  operation.op_id = "d7f52370-d5f6-4c4f-8a1f-546eb93090f1";
  H::ProjectCopyT copy;
  auto item = std::make_unique<H::ProjectCopyItemT>();
  item->demo = "en/demo";
  item->from = "group";
  item->id = "copied";
  copy.items.push_back(std::move(item));
  operation.body.Set(std::move(copy));
  OperationContext context;
  context.origin = "ui";
  context.demo = [&](const auto&) { return demo; };
  std::string saved;
  auto call = [&](const std::string& method,
                  const j::Members& args) -> OperationReply {
    std::string type;
    j::Members value;
    if (method == "operation_get") {
      type = "OperationResult";
      value["state"] = "\"unknown\"";
    } else if (method == "project_inspect") {
      type = "ProjectSnapshot";
      value = {{"rev", "1"},
               {"project_json", j::Quote(initial_project_json("Open"))},
               {"layout_json", "\"{}\""},
               {"definitions", "[]"},
               {"sources", "[]"}};
    } else if (method == "project_save") {
      type = "SaveResult";
      value["rev"] = "2";
      saved = j::Encode(args);
    } else {
      ADD_FAILURE() << method;
      type = "Failure";
      value["kind"] = "\"not_found\"";
    }
    return {j::Encode(j::Members{{"reply", j::Quote(type)},
                                 {"value", j::Encode(value)}}),
            {}};
  };
  EXPECT_NO_THROW(ProjectOperations().Execute(operation, context, call));
  EXPECT_NE(saved.find("externals/copied/definitions/leaf/src/main.py"),
            std::string::npos);
  EXPECT_NE(saved.find(crypto::base64("COMPLETE_SOURCE")), std::string::npos);
  EXPECT_NE(saved.find("copied"), std::string::npos);
  operation.body.AsProjectCopy()->items[0]->from = "";
  EXPECT_THROW(ProjectOperations().Execute(operation, context, call), Failure);
}
TEST_F(AgentExcludeTest, PatchSourceOnRootDocumentsChecksExcludedRules) {
  const std::string project =
      R"({"agentExclude":["definitions/a/private.txt"],"canvas":{"cards":[{"id":"a"},{"id":"public"}],"connections":[]}})";
  AgentExclude policy(root, project);
  SaveBase base;
  base.files["tsugumi.json"] = project;
  const std::string dropped =
      R"({"agentExclude":["definitions/a/private.txt"],"canvas":{"cards":[{"id":"public"}],"connections":[]}})";
  const auto plan = j::Members{
      {"changes",
       j::Encode(std::vector<std::string>{j::Encode(j::Members{
           {"value_type", j::Quote("PatchSource")},
           {"value",
            j::Encode(j::Members{
                {"path", j::Quote("tsugumi.json")},
                {"patches",
                 j::Encode(std::vector<std::string>{j::Encode(j::Members{
                     {"offset", j::Quote("0")},
                     {"remove", j::Quote(std::to_string(project.size()))},
                     {"text", j::Quote(dropped)}})})}})}})})}};
  Refused([&] { policy.CheckSave(plan, base); });
}
}  // namespace
}  // namespace tsugumi::host
