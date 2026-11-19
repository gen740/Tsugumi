#include "host/project_operations.hh"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "../agent_fixture_server.hh"
#include "host/agent_exclude.hh"
#include "host/document_edits.hh"
#include "host/failure.hh"
#include "host/layout_saving.hh"
#include "tsugumi/util/crypto.hh"

namespace tsugumi::host {
namespace {
namespace H = Tsugumi::Host;
namespace json = ::tsugumi::json;
TEST(ProjectOperations, CoreRejectsInvalidOperationIdentity) {
  H::ProjectOperateRequestT request;
  request.project_id = "p";
  int calls = 0;
  EXPECT_THROW(
      ProjectOperations().Execute(
          request, {},
          [&](const auto& method, const auto& args) -> OperationReply {
            ++calls;
            EXPECT_EQ(method, "operation_get");
            EXPECT_EQ(json::Text(args, "op_id"), request.op_id);
            return {
                R"({"reply":"Failure","value":{"kind":"invalid_request","diagnostic":"invalid op_id"}})",
                {}};
          }),
      Failure);
  EXPECT_EQ(calls, 1);
}
TEST(ProjectOperations, RevisionConflictReReadsAndReAppliesTheSameOperation) {
  H::ProjectTextEditT edit;
  edit.file_path = "definitions/a/main.py";
  edit.old_string = "old";
  edit.new_string = "new";
  H::ProjectEditsT edits;
  auto entry = std::make_unique<H::ProjectEditT>();
  entry->value.Set(edit);
  edits.edits.push_back(std::move(entry));
  H::ProjectOperateRequestT request;
  request.project_id = "p";
  request.op_id = "d7f52370-d5f6-4c4f-8a1f-546eb93090f1";
  request.body.Set(std::move(edits));
  int revision = 1, saves = 0;
  std::string intent;
  auto call = [&](const std::string& method, const json::Members& args) {
    std::string reply;
    json::Members value;
    if (method == "operation_get") {
      reply = "OperationResult";
      value = {{"state", "\"unknown\""}};
    } else if (method == "project_inspect") {
      reply = "ProjectSnapshot";
      value = {
          {"rev", json::Quote(std::to_string(revision))},
          {"project_json",
           json::Quote(R"({"canvas":{"cards":[],"connections":[]}})")},
          {"layout_json", "\"{}\""},
          {"sources", R"([{"path":"definitions/a/main.py","hash":"hash"}])"}};
    } else if (method == "project_file_read") {
      reply = "FileContent";
      value = {
          {"bytes", json::Quote(revision == 1 ? "old" : "old + unrelated")},
          {"eof", "true"}};
    } else {
      EXPECT_EQ(method, "project_save");
      EXPECT_EQ(json::Text(args, "op_id"), request.op_id);
      if (saves++ == 0) {
        intent = json::Text(args, "save_intent");
        EXPECT_FALSE(intent.empty());
        ++revision;
        reply = "Failure";
        value = {{"kind", "\"revision_conflict\""}};
      } else {
        EXPECT_EQ(json::Text(args, "save_intent"), intent);
        EXPECT_EQ(json::Text(args, "base_rev"), "2");
        std::optional<std::string> found;
        for (const auto& raw : json::Array(args.at("changes"))) {
          const auto change = json::Object(raw);
          const auto file = json::Object(change.at("value"));
          if (json::Text(file, "path") != edit.file_path) continue;
          std::string bytes;
          EXPECT_TRUE(crypto::base64_decode(
              json::Text(json::Object(file.at("bytes")), "base64"), bytes));
          found = std::move(bytes);
        }
        EXPECT_EQ(found, "new + unrelated");
        reply = "SaveResult";
        value = {{"rev", "\"3\""}};
      }
    }
    return OperationReply{
        json::Encode(json::Members{{"reply", json::Quote(reply)},
                                   {"value", json::Encode(value)}}),
        {}};
  };
  ProjectOperations operations;
  const auto result = operations.Execute(request, {"ui", {}, {}, {}}, call);
  EXPECT_EQ(json::Text(json::Object(result.json), "reply"), "SaveResult");
  EXPECT_EQ(saves, 2);
}
}  // namespace
}  // namespace tsugumi::host

namespace tsugumi::host {
namespace {
TEST(ProjectOperationUndo, UnrelatedWriterChangesSurviveAndConflictsBlockUndo) {
  SaveBase before;
  before.snapshot["rev"] = "1";
  before.files["tsugumi.json"] =
      R"({"name":"old","canvas":{"cards":[],"connections":[]}})";
  const json::Members forward{
      {"changes",
       R"([{"value_type":"SetProject","value":{"json":"{\"name\":\"new\",\"canvas\":{\"cards\":[],\"connections\":[]}}"}}])"}};
  const auto read = [](const auto&) -> std::optional<std::string> {
    return std::nullopt;
  };
  auto inverse = ReverseProjectChanges(forward, before, read);
  ASSERT_EQ(inverse.edits.size(), 1);
  SaveBase current = before;
  current.files["tsugumi.json"] =
      R"({"name":"new","canvas":{"cards":[],"connections":[]},"agentExcludedPaths":["private"]})";
  const auto plan = PrepareProjectEdits(inverse, current, read);
  const auto change = json::Object(json::Array(plan.at("changes")).front());
  const auto restored =
      json::Object(json::Text(json::Object(change.at("value")), "json"));
  EXPECT_EQ(json::Text(restored, "name"), "old");
  EXPECT_EQ(json::Array(restored.at("agentExcludedPaths")).size(), 1u);
  current.files["tsugumi.json"] =
      R"({"name":"changed-by-someone-else","canvas":{"cards":[],"connections":[]}})";
  try {
    PrepareProjectEdits(inverse, current, read);
    FAIL() << "Undo must refuse a conflicting writer";
  } catch (const Failure& failure) {
    EXPECT_EQ(failure.code, "value_conflict");
  }
}

TEST(ProjectOperationUndo, RenameDoesNotResurrectAnotherWritersDeletedGroup) {
  SaveBase before;
  before.snapshot["rev"] = "1";
  before.files["layout.json"] = R"({"groups":[{"id":"g","name":"old"}]})";
  const json::Members forward{
      {"changes",
       R"([{"value_type":"SetLayout","value":{"json":"{\"groups\":[{\"id\":\"g\",\"name\":\"new\"},{\"id\":\"other\"}]}"}}])"}};
  // The actual operation only renamed g; the other group was already there
  // when host preparation observed the current document.
  before.files["layout.json"] =
      R"({"groups":[{"id":"g","name":"old"},{"id":"other"}]})";
  const auto read = [](const auto&) -> std::optional<std::string> {
    return std::nullopt;
  };
  const auto inverse = ReverseProjectChanges(forward, before, read);
  SaveBase current = before;
  current.files["layout.json"] = R"({"groups":[{"id":"g","name":"new"}]})";
  const auto plan = PrepareProjectEdits(inverse, current, read);
  EXPECT_EQ(json::Array(plan.at("changes")).size(), 1);
  const auto change = json::Object(json::Array(plan.at("changes")).front());
  const auto layout =
      json::Object(json::Text(json::Object(change.at("value")), "json"));
  const auto groups = json::Array(layout.at("groups"));
  ASSERT_EQ(groups.size(), 1u);
  EXPECT_EQ(json::Text(json::Object(groups.front()), "id"), "g");
  EXPECT_EQ(json::Text(json::Object(groups.front()), "name"), "old");
}

TEST(ProjectOperationUndo, MoveUndoPreservesThePreviouslyAcceptedPortOrder) {
  SaveBase before;
  before.snapshot["rev"] = "1";
  before.files["layout.json"] =
      R"({"cards":{"a":{"x":10,"y":20,"inputOrder":["second","first"]}}})";
  const json::Members move{
      {"changes",
       R"([{"value_type":"SetLayout","value":{"json":"{\"cards\":{\"a\":{\"x\":30,\"y\":20,\"inputOrder\":[\"first\",\"second\"]}}}"}}])"}};
  const auto read = [](const auto&) -> std::optional<std::string> {
    return std::nullopt;
  };
  const auto inverse = ReverseProjectChanges(move, before, read);
  SaveBase current = before;
  current.files["layout.json"] =
      R"({"cards":{"a":{"x":30,"y":20,"inputOrder":["first","second"]}},"notes":[{"id":"other"}]})";
  const auto plan = PrepareProjectEdits(inverse, current, read);
  const auto raw = json::Text(
      json::Object(
          json::Object(json::Array(plan.at("changes")).front()).at("value")),
      "json");
  EXPECT_NE(raw.find(R"("inputOrder":["second","first"])"), std::string::npos);
  EXPECT_NE(raw.find(R"("x":10)"), std::string::npos);
  EXPECT_NE(raw.find(R"("id":"other")"), std::string::npos);
}
}  // namespace
}  // namespace tsugumi::host

namespace tsugumi::host {
namespace {
TEST(ProjectOperations,
     CompletedOperationReplaysWithoutReadingOrApplyingOldValues) {
  H::ProjectOperateRequestT request;
  request.project_id = "p";
  request.op_id = "d7f52370-d5f6-4c4f-8a1f-546eb93090f1";
  H::ProjectEditsT edits;
  auto edit = std::make_unique<H::ProjectEditT>();
  H::ProjectTextEditT text;
  text.file_path = "definitions/a/main.py";
  text.old_string = "old";
  text.new_string = "new";
  edit->value.Set(std::move(text));
  edits.edits.push_back(std::move(edit));
  request.body.Set(std::move(edits));
  int reads = 0, applications = 0;
  bool completed = false;
  std::string metadata;
  auto call = [&](const std::string& method,
                  const json::Members& args) -> OperationReply {
    std::string type;
    json::Members value;
    if (method == "operation_get") {
      type = "OperationResult";
      value["state"] = json::Quote(completed ? "completed" : "unknown");
    } else if (method == "project_inspect") {
      ++reads;
      type = "ProjectSnapshot";
      value = {
          {"rev", "1"},
          {"project_json",
           json::Quote(R"({"canvas":{"cards":[],"connections":[]}})")},
          {"layout_json", json::Quote("{}")},
          {"sources", R"([{"path":"definitions/a/main.py","hash":"hash"}])"}};
    } else if (method == "project_file_read") {
      type = "FileContent";
      value = {{"bytes", json::Quote("old")}, {"eof", "true"}};
    } else {
      EXPECT_EQ(method, "project_save");
      if (!completed) {
        ++applications;
        metadata = args.at("operation_data");
        completed = true;
      } else {
        EXPECT_EQ(json::Get(args, "changes"), "[]");
      }
      type = "SaveResult";
      value = {{"rev", "2"}, {"operation_data", metadata}};
    }
    return {json::Encode(json::Members{{"reply", json::Quote(type)},
                                       {"value", json::Encode(value)}}),
            {}};
  };
  ProjectOperations executor;
  const auto first = executor.Execute(request, {"ui", {}, {}, {}}, call);
  const auto repeat = executor.Execute(request, {"ui", {}, {}, {}}, call);
  EXPECT_EQ(first.json, repeat.json);
  EXPECT_EQ(reads, 1);
  EXPECT_EQ(applications, 1);
}
}  // namespace
}  // namespace tsugumi::host

namespace tsugumi::host {
namespace {
TEST(ProjectOperationLayout,
     UsesMeasurementsButKeepsCurrentPositionsAndFrameSizes) {
  const std::string project =
      R"({"canvas":{"cards":[{"id":"a"}],"connections":[]}})";
  H::LayoutSceneT scene;
  scene.flow = "topToBottom";
  auto card = std::make_unique<H::LayoutCardT>();
  card->id = "a";
  card->x = 999;
  card->y = 888;
  card->width = 120;
  card->height = 60;
  scene.cards.push_back(std::move(card));
  auto frame = std::make_unique<H::LayoutBoxT>();
  frame->id = "a";
  frame->x = 1020;
  frame->y = 1000;
  frame->width = 320;
  frame->height = 170;
  scene.frames.push_back(std::move(frame));
  const auto untouched = ArrangeProjectLayout(
      project, R"({"cards":{"a":{"x":10,"y":20}}})", {}, true, &scene);
  const auto position =
      json::Object(json::Object(json::Object(untouched).at("cards")).at("a"));
  EXPECT_EQ(json::UInt(position.at("x")), 10);
  EXPECT_EQ(json::UInt(position.at("y")), 20);
  const auto arranged = ArrangeProjectLayout(
      project,
      R"({"cards":{"a":{"x":10,"y":20}},"boundary":{"inputs":{"x":33,"y":44}}})",
      {}, false, &scene);
  const auto layout = json::Object(arranged);
  const auto rectangle =
      json::Object(json::Object(layout.at("displays")).at("a"));
  EXPECT_DOUBLE_EQ(std::stod(rectangle.at("width")), 320);
  EXPECT_DOUBLE_EQ(std::stod(rectangle.at("height")), 170);
  EXPECT_FALSE(layout.contains("boundary"));
}
}  // namespace
}  // namespace tsugumi::host

namespace tsugumi::host {
namespace {
// A controllable project transport: every save is one atomic application, and
// inspection/file reads all refer to the current confirmed revision.
class TransformationProject {
 public:
  std::map<std::string, std::string> files{
      {"tsugumi.json",
       R"({"name":"test","canvas":{"cards":[{"id":"a"},{"id":"sub"}],"connections":[]}})"},
      {"layout.json",
       R"({"cards":{"a":{"x":10,"y":20,"inputOrder":["q","p"]},"sub":{"x":200,"y":20}}})"},
      {"definitions/a/_card.json", R"({"template":"draft"})"},
      {"definitions/a/_meta.json", R"({"name":"source"})"},
      {"definitions/a/data.bin", std::string("\0\xff\x80\n", 4)},
      {"definitions/sub/_card.json", R"({"external":"inner"})"},
      {"externals/inner/tsugumi.json",
       R"({"name":"inner","canvas":{"cards":[{"id":"b"}],"connections":[]}})"},
      {"externals/inner/layout.json", R"({"cards":{"b":{"x":1,"y":2}}})"},
      {"externals/inner/definitions/b/_card.json", R"({"external":"deep"})"},
      {"externals/inner/externals/deep/tsugumi.json",
       R"({"canvas":{"cards":[{"id":"c"}],"connections":[]}})"},
      {"externals/inner/externals/deep/layout.json", "{}"},
      {"externals/inner/externals/deep/definitions/c/_card.json",
       R"({"template":"draft"})"},
      {"externals/inner/externals/deep/definitions/c/raw.bin",
       std::string("\xff\0\x81", 3)}};
  int saves = 0;
  OperationContext context{"ui", {}, {}, {}};
  auto Execute(H::ProjectOperationBodyUnion body) -> OperationReply {
    H::ProjectOperateRequestT request;
    request.project_id = "p";
    request.op_id = crypto::uuid4();
    request.body = std::move(body);
    return ProjectOperations().Execute(
        request, context, [&](const auto& method, const auto& args) {
          return Call(method, args);
        });
  }
  auto Apply(H::ProjectOperationBodyUnion body) -> H::ProjectEditsT {
    const auto reply = Execute(std::move(body));
    EXPECT_EQ(json::Text(json::Object(reply.json), "reply"), "SaveResult");
    const auto value =
        json::Object(json::Get(json::Object(reply.json), "value"));
    const auto encoded = json::Object(value.at("operation_data"));
    std::string bytes;
    EXPECT_TRUE(crypto::base64_decode(json::Text(encoded, "base64"), bytes));
    return *std::unique_ptr<H::ProjectEditsT>(
        flatbuffers::GetRoot<H::ProjectEdits>(bytes.data())->UnPack());
  }
  template <class T>
  auto Apply(T body) -> H::ProjectEditsT {
    H::ProjectOperationBodyUnion operation;
    operation.Set(std::move(body));
    return Apply(std::move(operation));
  }
  template <class T>
  auto ExpectConflict(T body, const std::string& kind) -> void {
    const auto before = files;
    const auto saves_before = saves;
    H::ProjectOperationBodyUnion operation;
    operation.Set(std::move(body));
    const auto reply = json::Object(Execute(std::move(operation)).json);
    ASSERT_EQ(json::Text(reply, "reply"), "Failure");
    EXPECT_EQ(json::Text(json::Object(reply.at("value")), "kind"), kind);
    EXPECT_EQ(saves, saves_before);
    EXPECT_EQ(files, before);
  }

 private:
  auto Call(const std::string& method, const json::Members& args)
      -> OperationReply {
    std::string type;
    json::Members value;
    if (method == "operation_get") {
      type = "OperationResult";
      value["state"] = "\"unknown\"";
    } else if (method == "project_inspect") {
      type = "ProjectSnapshot";
      value = {{"rev", std::to_string(saves)},
               {"project_json", json::Quote(files.at("tsugumi.json"))},
               {"layout_json", json::Quote(files.at("layout.json"))}};
      json::Members definitions;
      std::vector<std::string> sources;
      for (const auto& [path, bytes] : files) {
        if (path == "tsugumi.json" || path == "layout.json") continue;
        if (path.starts_with("definitions/") &&
            std::count(path.begin(), path.end(), '/') == 2) {
          const auto slash = path.find('/', 12);
          const auto name = path.substr(slash + 1);
          const auto key = name == "_card.json"   ? "card_json"
                           : name == "_meta.json" ? "meta_json"
                                                  : "";
          if (key != std::string("")) {
            const auto id = path.substr(12, slash - 12);
            auto d = json::Object(json::Get(definitions, id, "{}"));
            d["id"] = json::Quote(id);
            d[key] = json::Quote(bytes);
            definitions[id] = json::Encode(d);
            continue;
          }
        }
        sources.push_back(json::Encode(
            json::Members{{"path", json::Quote(path)},
                          {"hash", json::Quote(crypto::sha256_hex(bytes))}}));
      }
      std::vector<std::string> list;
      for (const auto& [id, raw] : definitions) {
        (void)id;
        list.push_back(raw);
      }
      value["definitions"] = json::Encode(list);
      value["sources"] = json::Encode(sources);
    } else if (method == "project_file_read") {
      type = "FileContent";
      value = {{"bytes", json::Encode(json::Members{
                             {"base64", json::Quote(crypto::base64(files.at(
                                            json::Text(args, "path"))))}})},
               {"eof", "true"}};
    } else {
      EXPECT_EQ(method, "project_save");
      EXPECT_EQ(json::UInt(args.at("base_rev")), saves);
      for (const auto& raw : json::Array(args.at("changes"))) {
        const auto change = json::Object(raw),
                   v = json::Object(change.at("value"));
        const auto kind = json::Text(change, "value_type");
        if (kind == "SetProject" || kind == "SetLayout")
          files[kind == "SetProject" ? "tsugumi.json" : "layout.json"] =
              json::Text(v, "json");
        else if (kind == "PutDefinition") {
          const auto dir = "definitions/" + json::Text(v, "id") + '/';
          for (const auto& [key, name] : {std::pair{"card_json", "_card.json"},
                                          {"meta_json", "_meta.json"}}) {
            const auto text = json::Text(v, key);
            if (text.empty())
              files.erase(dir + name);
            else
              files[dir + name] = text;
          }
        } else if (kind == "RemoveDefinition") {
          const auto dir = "definitions/" + json::Text(v, "id") + '/';
          std::erase_if(files, [&](const auto& entry) {
            return entry.first.starts_with(dir);
          });
        } else if (kind == "RemoveSource")
          files.erase(json::Text(v, "path"));
        else {
          EXPECT_EQ(kind, "PutSource");
          const auto data = json::Get(v, "bytes");
          std::string bytes;
          if (data.starts_with('"'))
            bytes = json::String(data);
          else
            EXPECT_TRUE(crypto::base64_decode(
                json::Text(json::Object(data), "base64"), bytes));
          files[json::Text(v, "path")] = bytes;
        }
      }
      ++saves;
      type = "SaveResult";
      value = {{"rev", std::to_string(saves)},
               {"operation_data", args.at("operation_data")}};
    }
    return {json::Encode(json::Members{{"reply", json::Quote(type)},
                                       {"value", json::Encode(value)}}),
            {}};
  }
};
TEST(ProjectOperationTransform, FirstLayoutActionsWorkInsideACardInCard) {
  const std::string prefix = "externals/inner/";
  for (const auto* action : {"move", "arrange", "duplicate", "subgraph"}) {
    SCOPED_TRACE(action);
    TransformationProject project;
    project.files.erase(prefix + "layout.json");
    auto positions = [&](const std::string& file) {
      return json::Object(json::Object(project.files.at(file)).at("cards"));
    };
    const auto displayed = json::Object(
        json::Object(
            CompleteLayout(project.files.at(prefix + "tsugumi.json"), "{}"))
            .at("cards"));
    const auto before = json::Object(displayed.at("b"));
    if (std::string_view(action) == "move") {
      H::ProjectMoveT move;
      move.project_path = "externals/inner";
      move.cards = {"b"};
      move.dx = 40;
      project.Apply(std::move(move));
      const auto moved =
          json::Object(positions(prefix + "layout.json").at("b"));
      EXPECT_DOUBLE_EQ(std::stod(moved.at("x")),
                       std::stod(before.at("x")) + 40);
      EXPECT_DOUBLE_EQ(std::stod(moved.at("y")), std::stod(before.at("y")));
    } else if (std::string_view(action) == "arrange") {
      H::ProjectLayoutT arrange;
      arrange.project_path = "externals/inner";
      arrange.action = H::ProjectLayoutAction::Arrange;
      project.Apply(std::move(arrange));
      EXPECT_TRUE(json::Object(positions(prefix + "layout.json").at("b"))
                      .contains("x"));
    } else if (std::string_view(action) == "duplicate") {
      H::ProjectCopyT duplicate;
      duplicate.project_path = "externals/inner";
      duplicate.copy_connections = true;
      auto item = std::make_unique<H::ProjectCopyItemT>();
      item->from = prefix + "definitions/b/";
      item->id = "duplicate";
      duplicate.items.push_back(std::move(item));
      const auto undo = project.Apply(std::move(duplicate));
      const auto after = json::Object(json::Object(CompleteLayout(
          project.files.at(prefix + "tsugumi.json"),
          project.files.at(prefix + "layout.json"))).at("cards"));
      EXPECT_EQ(json::Object(after.at("b")), before);
      const auto copy = json::Object(after.at("duplicate"));
      EXPECT_DOUBLE_EQ(std::stod(copy.at("x")), std::stod(before.at("x")) + 30);
      EXPECT_DOUBLE_EQ(std::stod(copy.at("y")), std::stod(before.at("y")) + 30);
      project.Apply(undo);
      const auto layout = project.files.find(prefix + "layout.json");
      const auto restored = CompleteLayout(
          project.files.at(prefix + "tsugumi.json"),
          layout == project.files.end() ? "{}" : layout->second);
      EXPECT_EQ(json::Object(json::Object(restored).at("cards")), displayed);
      if (layout != project.files.end())
        EXPECT_FALSE(json::Object(json::Get(json::Object(layout->second),
                                            "cards", "{}")).contains("b"));
    } else {
      H::ProjectMakeSubgraphT subgraph;
      subgraph.project_path = "externals/inner";
      subgraph.cards = {"b"};
      subgraph.id = "subgraph";
      project.Apply(std::move(subgraph));
      const auto child = positions(prefix + "externals/subgraph/layout.json");
      EXPECT_TRUE(child.contains("b"));
      const auto outer =
          json::Object(positions(prefix + "layout.json").at("subgraph"));
      EXPECT_DOUBLE_EQ(std::stod(outer.at("x")), std::stod(before.at("x")));
      EXPECT_DOUBLE_EQ(std::stod(outer.at("y")), std::stod(before.at("y")));
    }
  }
}
TEST(ProjectOperationTransform, DeletingTheLastExternalReferenceAndUndo) {
  TransformationProject project;
  project.files["definitions/spare/_card.json"] = R"({"external":"inner"})";
  const auto inner = project.files.at("externals/inner/tsugumi.json");
  H::ProjectSelectionT remove_card;
  remove_card.action = H::ProjectSelectionAction::Delete;
  remove_card.cards = {"sub"};
  const auto restore_card = project.Apply(remove_card);
  EXPECT_FALSE(project.files.contains("definitions/sub/_card.json"));
  EXPECT_EQ(project.files.at("externals/inner/tsugumi.json"), inner);
  project.Apply(restore_card);
  EXPECT_EQ(project.files.at("definitions/sub/_card.json"),
            R"({"external":"inner"})");

  H::ProjectSelectionT remove_definition;
  remove_definition.action = H::ProjectSelectionAction::DeleteDefinitions;
  remove_definition.cards = {"spare"};
  project.Apply(remove_definition);
  EXPECT_FALSE(project.files.contains("definitions/spare/_card.json"));
  EXPECT_EQ(project.files.at("externals/inner/tsugumi.json"), inner);
  const auto restore_tree = project.Apply(remove_card);
  EXPECT_FALSE(project.files.contains("externals/inner/tsugumi.json"));
  project.Apply(restore_tree);
  EXPECT_EQ(project.files.at("externals/inner/tsugumi.json"), inner);
}
TEST(ProjectOperationTransform, AgentCannotDropOrRelocateAnExcludedTree) {
  test::TemporaryDirectory temporary;
  for (const auto* excluded :
       {"definitions/a/data.bin",
        "externals/inner/externals/deep/definitions/c/raw.bin"}) {
    for (const auto* action : {"delete", "subgraph", "rename", "document"}) {
      SCOPED_TRACE(std::string(excluded) + ':' + action);
      TransformationProject project;
      auto document = json::Object(project.files.at("tsugumi.json"));
      document["agentExclude"] =
          json::Encode(std::vector<std::string>{json::Quote(excluded)});
      project.files["tsugumi.json"] = json::Encode(document);
      const auto before = project.files;
      AgentExclude policy(temporary.path, project.files.at("tsugumi.json"));
      project.context.origin = "agent";
      project.context.authorize = [&](const auto& plan, const auto& base) {
        policy.CheckSave(plan, base);
      };
      H::ProjectOperationBodyUnion body;
      const std::string id =
          std::string(excluded).starts_with("definitions/") ? "a" : "sub";
      if (std::string(action) == "delete") {
        H::ProjectSelectionT selection;
        selection.action = H::ProjectSelectionAction::Delete;
        selection.cards = {id};
        body.Set(std::move(selection));
      } else if (std::string(action) == "subgraph") {
        H::ProjectMakeSubgraphT convert;
        convert.cards = {id};
        convert.id = "converted";
        body.Set(std::move(convert));
      } else if (std::string(action) == "rename") {
        H::ProjectEditsT edits;
        H::ProjectTreeEditT tree;
        tree.prefix = "definitions/" + id + '/';
        for (const auto& [path, bytes] : project.files) {
          if (!path.starts_with(tree.prefix)) continue;
          auto hash = std::make_unique<H::ProjectFileHashT>();
          hash->path = path;
          hash->hash = crypto::sha256_hex(bytes);
          tree.expected.push_back(std::move(hash));
          H::ProjectFilePutT put;
          put.file_path =
              "definitions/renamed/" + path.substr(tree.prefix.size());
          put.expect_missing = true;
          put.bytes.assign(bytes.begin(), bytes.end());
          auto entry = std::make_unique<H::ProjectEditT>();
          entry->value.Set(std::move(put));
          edits.edits.push_back(std::move(entry));
        }
        auto entry = std::make_unique<H::ProjectEditT>();
        entry->value.Set(std::move(tree));
        edits.edits.push_back(std::move(entry));
        body.Set(std::move(edits));
      } else {
        H::ProjectReplaceDocumentsT replacement;
        auto file = std::make_unique<H::ProjectDocumentT>();
        file->path = "tsugumi.json";
        document["canvas"] = R"({"cards":[],"connections":[]})";
        file->json = json::Encode(document);
        replacement.documents.push_back(std::move(file));
        body.Set(std::move(replacement));
      }
      try {
        project.Apply(std::move(body));
        FAIL() << "expected refusal";
      } catch (const Failure& failure) {
        EXPECT_EQ(failure.code, "not_found");
      }
      EXPECT_EQ(project.saves, 0);
      EXPECT_EQ(project.files, before);
    }
  }
}
TEST(ProjectOperationTransform,
     ConvertCopyAndUndoKeepBinaryAndNestedDocuments) {
  TransformationProject project;
  const auto initial = project.files;
  H::ProjectMakeSubgraphT convert;
  convert.cards = {"a", "sub"};
  convert.id = "converted";
  convert.name = "converted";
  auto inverse = project.Apply(convert);
  EXPECT_EQ(project.saves, 1);
  EXPECT_FALSE(project.files.contains("definitions/a/_card.json"));
  EXPECT_EQ(project.files.at("externals/converted/definitions/a/data.bin"),
            initial.at("definitions/a/data.bin"));
  const auto deep = "externals/inner/externals/deep/definitions/c/raw.bin";
  EXPECT_EQ(project.files.at("externals/converted/" + std::string(deep)),
            initial.at(deep));
  H::ProjectCopyT copy;
  auto item = std::make_unique<H::ProjectCopyItemT>();
  item->from = "definitions/converted/";
  item->id = "copy";
  copy.items.push_back(std::move(item));
  auto undo_copy = project.Apply(copy);
  EXPECT_EQ(project.saves, 2);
  EXPECT_EQ(project.files.at("externals/copy/definitions/a/data.bin"),
            initial.at("definitions/a/data.bin"));
  EXPECT_EQ(project.files.at("externals/copy/" + std::string(deep)),
            initial.at(deep));
  // A later source addition makes removal of the copied folder unsafe.
  project.files["externals/copy/later.bin"] = "later";
  project.ExpectConflict(undo_copy, "source_changed");
  EXPECT_EQ(project.saves, 2);
  project.files.erase("externals/copy/later.bin");
  auto redo_copy = project.Apply(undo_copy);
  EXPECT_FALSE(project.files.contains("definitions/copy/_card.json"));
  project.Apply(redo_copy);
  EXPECT_EQ(project.files.at("externals/copy/definitions/a/data.bin"),
            initial.at("definitions/a/data.bin"));
  // An unrelated edit survives conversion Undo.
  auto root = json::Object(project.files.at("tsugumi.json"));
  root["name"] = "\"other writer\"";
  project.files["tsugumi.json"] = json::Encode(root);
  project.Apply(inverse);
  EXPECT_EQ(project.files.at("definitions/a/data.bin"),
            initial.at("definitions/a/data.bin"));
  EXPECT_EQ(project.files.at(deep), initial.at(deep));
  EXPECT_EQ(json::Text(json::Object(project.files.at("tsugumi.json")), "name"),
            "other writer");
  EXPECT_TRUE(project.files.contains("definitions/copy/_card.json"));
}
}  // namespace
}  // namespace tsugumi::host

namespace tsugumi::host {
namespace {
TEST(ProjectOperationTransform, ImportIncludesNestedBinaryPlacementAndUndo) {
  TransformationProject project;
  const auto directory = std::filesystem::temp_directory_path() /
                         ("tsugumi-operation-" + crypto::uuid4());
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove_all(path, error);
    }
  } cleanup{directory};
  for (const auto& [path, bytes] : project.files) {
    std::filesystem::create_directories((directory / path).parent_path());
    std::ofstream(directory / path, std::ios::binary)
        .write(bytes.data(), bytes.size());
  }
  H::ProjectImportT imported;
  imported.path = directory.string();
  imported.id = "imported";
  imported.x = 400;
  imported.y = 600;
  const auto inverse = project.Apply(imported);
  EXPECT_EQ(project.saves, 1);
  EXPECT_EQ(project.files.at("externals/imported/definitions/a/data.bin"),
            project.files.at("definitions/a/data.bin"));
  EXPECT_EQ(project.files.at("externals/imported/externals/inner/externals/"
                             "deep/definitions/c/raw.bin"),
            std::string("\xff\0\x81", 3));
  const auto at = json::Object(
      json::Object(json::Object(project.files.at("layout.json")).at("cards"))
          .at("imported"));
  EXPECT_EQ(std::stod(at.at("x")), 400);
  EXPECT_EQ(std::stod(at.at("y")), 600);
  project.Apply(inverse);
  EXPECT_EQ(project.saves, 2);
  for (const auto& [path, ignored] : project.files) {
    (void)ignored;
    EXPECT_FALSE(path.starts_with("externals/imported/"));
  }
  EXPECT_FALSE(project.files.contains("definitions/imported/_card.json"));
}

TEST(ProjectOperationTransform, UntangleThenAtomicMoveAndUndoRetainPortOrder) {
  TransformationProject project;
  project.files = {
      {"tsugumi.json",
       R"({"canvas":{"cards":[{"id":"a"},{"id":"b"},{"id":"d"}],"connections":[{"from":{"card":"a","port":"out"},"to":{"card":"d","port":"q"}},{"from":{"card":"b","port":"out"},"to":{"card":"d","port":"p"}}]}})"},
      {"layout.json",
       R"({"flow":"topToBottom","cards":{"a":{"x":0,"y":0},"b":{"x":200,"y":0},"d":{"x":100,"y":300}}})"},
      {"definitions/a/_card.json", R"({"language":"python"})"},
      {"definitions/b/_card.json", R"({"language":"python"})"},
      {"definitions/d/_card.json",
       R"({"language":"python","inputs":[{"name":"p"},{"name":"q"}]})"}};
  H::LayoutSceneT scene;
  scene.flow = "topToBottom";
  for (const auto& id : {"a", "b", "d"}) {
    auto card = std::make_unique<H::LayoutCardT>();
    card->id = id;
    card->width = 120;
    card->height = 60;
    card->reorderable = true;
    if (card->id == "d") {
      for (const auto& name : {"p", "q"}) {
        auto port = std::make_unique<H::LayoutPortT>();
        port->name = name;
        port->x = port->name == "p" ? 20 : 100;
        card->inputs.push_back(std::move(port));
      }
    } else {
      auto port = std::make_unique<H::LayoutPortT>();
      port->name = "out";
      port->x = 60;
      port->y = 60;
      card->outputs.push_back(std::move(port));
    }
    scene.cards.push_back(std::move(card));
  }
  auto order = [&]() {
    const auto positions =
        json::Object(json::Object(project.files.at("layout.json")).at("cards"));
    return json::Get(json::Object(positions.at("d")), "inputOrder", "[]");
  };
  H::ProjectLayoutT untangle;
  untangle.action = H::ProjectLayoutAction::Untangle;
  untangle.scene = std::make_unique<H::LayoutSceneT>(scene);
  project.Apply(untangle);
  EXPECT_EQ(order(), R"(["q","p"])");
  const auto accepted = project.files.at("layout.json");
  H::ProjectMoveT move;
  move.cards = {"a"};
  move.dx = 400;
  move.scene = std::make_unique<H::LayoutSceneT>(scene);
  const auto undo_move = project.Apply(move);
  EXPECT_EQ(project.saves, 2);  // No later proposal/save can join this move.
  EXPECT_EQ(order(), R"(["p","q"])");
  project.Apply(undo_move);
  EXPECT_TRUE(DocumentsEqual(project.files.at("layout.json"), accepted));

  const std::string unplaced =
      R"({"flow":"topToBottom","cards":{"a":{"x":0,"y":0},"b":{"x":200,"y":0}}})";
  project.files["layout.json"] = unplaced;
  H::ProjectReplaceDocumentsT update;
  update.expected_rev = project.saves;
  auto layout = std::make_unique<H::ProjectDocumentT>();
  layout->path = "layout.json";
  layout->json =
      R"({"flow":"topToBottom","cards":{"a":{"x":0,"y":0},"b":{"x":200,"y":0},"d":{"inputOrder":["q","p"]}}})";
  update.documents.push_back(std::move(layout));
  project.Apply(std::move(update));
  const auto saved = project.files.at("layout.json");
  EXPECT_EQ(order(), R"(["q","p"])");
  const auto entry = [&](const std::string& layout) {
    return json::Object(json::Object(json::Object(layout).at("cards")).at("d"));
  };
  EXPECT_FALSE(entry(saved).contains("x"));
  EXPECT_FALSE(entry(saved).contains("y"));
  const auto& document = project.files.at("tsugumi.json");
  const auto before = entry(CompleteLayout(document, unplaced));
  const auto after = entry(CompleteLayout(document, saved));
  EXPECT_EQ(before.at("x"), after.at("x"));
  EXPECT_EQ(before.at("y"), after.at("y"));
}
}  // namespace
}  // namespace tsugumi::host

namespace tsugumi::host {
namespace {
TEST(ProjectOperationTransform, FirstNotesAndGroupsUseIndependentEntityEdits) {
  TransformationProject project;
  project.files["layout.json"] = "{}";
  auto entity = [&](const std::string& field, const std::string& id) {
    H::ProjectFieldEditT edit;
    edit.file_path = "layout.json";
    auto step = std::make_unique<H::ProjectSelectorT>();
    step->field = field;
    edit.selector.push_back(std::move(step));
    auto match = std::make_unique<H::ProjectSelectorT>();
    match->match_json = json::Encode(json::Members{{"id", json::Quote(id)}});
    edit.selector.push_back(std::move(match));
    edit.old_value = std::make_unique<H::ProjectValueT>();
    edit.new_value = std::make_unique<H::ProjectValueT>();
    edit.new_value->present = true;
    json::Members value{{"id", json::Quote(id)},
                        {field == "notes" ? "text" : "name", json::Quote(id)}};
    if (field == "notes") {
      value["x"] = "10";
      value["y"] = "20";
    }
    edit.new_value->json = json::Encode(value);
    H::ProjectEditsT body;
    auto item = std::make_unique<H::ProjectEditT>();
    item->value.Set(std::move(edit));
    body.edits.push_back(std::move(item));
    return project.Apply(body);
  };
  const auto undo_note = entity("notes", "mine");
  entity("notes", "other");
  const auto undo_group = entity("groups", "group");
  entity("groups", "another");
  project.Apply(undo_note);
  project.Apply(undo_group);
  const auto saved = json::Object(project.files.at("layout.json"));
  EXPECT_EQ(json::Array(saved.at("notes")).size(), 1);
  EXPECT_EQ(
      json::Text(json::Object(json::Array(saved.at("notes")).front()), "id"),
      "other");
  EXPECT_EQ(json::Array(saved.at("groups")).size(), 1);
  EXPECT_EQ(
      json::Text(json::Object(json::Array(saved.at("groups")).front()), "id"),
      "another");
}
TEST(ProjectOperationCutover,
     FormAndSourceEditAreAtomicAndUndoKeepsOtherFields) {
  TransformationProject project;
  project.files["definitions/a/_card.json"] =
      R"({"template":"draft","symbol":"base","concurrent":true})";
  H::ProjectEditsT body;
  H::ProjectJSONEditT form;
  form.file_path = "definitions/a/_card.json";
  form.old_value = std::make_unique<H::ProjectValueT>();
  form.old_value->present = true;
  form.old_value->json = R"({"template":"draft","symbol":"base"})";
  form.new_value = std::make_unique<H::ProjectValueT>(*form.old_value);
  form.new_value->json = R"({"template":"draft","symbol":"mine"})";
  auto entry = std::make_unique<H::ProjectEditT>();
  entry->value.Set(form);
  body.edits.push_back(std::move(entry));
  H::ProjectFilePutT put;
  put.file_path = "definitions/a/new.bin";
  put.expect_missing = true;
  put.bytes = {0, 255, 128};
  entry = std::make_unique<H::ProjectEditT>();
  entry->value.Set(put);
  body.edits.push_back(std::move(entry));
  const auto undo = project.Apply(body);
  EXPECT_EQ(project.saves, 1);
  EXPECT_EQ(
      json::Text(json::Object(project.files.at(form.file_path)), "symbol"),
      "mine");
  EXPECT_EQ(
      json::Get(json::Object(project.files.at(form.file_path)), "concurrent"),
      "true");
  project.files[form.file_path] =
      R"({"template":"draft","symbol":"mine","concurrent":false})";
  project.Apply(undo);
  EXPECT_EQ(
      json::Text(json::Object(project.files.at(form.file_path)), "symbol"),
      "base");
  EXPECT_EQ(
      json::Get(json::Object(project.files.at(form.file_path)), "concurrent"),
      "false");
  EXPECT_FALSE(project.files.contains(put.file_path));
  project.files[form.file_path] = R"({"template":"draft","symbol":"other"})";
  project.ExpectConflict(body, "value_conflict");
  EXPECT_FALSE(project.files.contains(put.file_path));
}
TEST(ProjectOperationCutover, RawDocumentsRefuseAnyStaleRevision) {
  TransformationProject project;
  H::ProjectReplaceDocumentsT body;
  body.expected_rev = 0;
  auto document = std::make_unique<H::ProjectDocumentT>();
  document->path = "definitions/a/_meta.json";
  document->json = R"({"name":"raw"})";
  body.documents.push_back(std::move(document));
  project.Apply(body);
  EXPECT_EQ(project.saves, 1);
  // Even an unrelated later document change does not permit a raw retry.
  const auto before = project.files;
  EXPECT_THROW(project.Apply(body), Failure);
  EXPECT_EQ(project.saves, 1);
  EXPECT_EQ(project.files, before);
}
TEST(ProjectOperationCutover, MixedDeletionAndPlacementUseTheCurrentClosure) {
  TransformationProject project;
  project.files["layout.json"] =
      R"({"notes":[{"id":"n","text":"note","x":0,"y":0}],"groups":[{"id":"g","cards":["a","sub"]}],"displays":{"a":{"x":5,"y":6,"width":100,"height":100}}})";
  const auto before = project.files;
  H::ProjectSelectionT body;
  body.action = H::ProjectSelectionAction::Delete;
  body.cards = {"sub"};
  body.notes = {"n"};
  body.frames = {"a"};
  const auto undo = project.Apply(body);
  EXPECT_EQ(project.saves, 1);
  EXPECT_FALSE(project.files.contains(
      "externals/inner/externals/deep/definitions/c/raw.bin"));
  const auto layout = json::Object(project.files.at("layout.json"));
  EXPECT_TRUE(json::Array(layout.at("notes")).empty());
  EXPECT_EQ(json::Get(json::Object(json::Object(layout.at("displays")).at("a")),
                      "hidden"),
            "true");
  EXPECT_EQ(
      json::Array(
          json::Object(json::Array(layout.at("groups")).front()).at("cards"))
          .size(),
      1);
  project.Apply(undo);
  EXPECT_EQ(
      project.files.at("externals/inner/externals/deep/definitions/c/raw.bin"),
      before.at("externals/inner/externals/deep/definitions/c/raw.bin"));
  H::ProjectSelectionT unplaced;
  unplaced.action = H::ProjectSelectionAction::DeleteDefinitions;
  unplaced.cards = {"a"};
  project.ExpectConflict(unplaced, "value_conflict");
  project.files["definitions/unplaced/_card.json"] = R"({"template":"draft"})";
  H::ProjectSelectionT place;
  place.action = H::ProjectSelectionAction::Place;
  place.cards = {"unplaced"};
  place.x = 12;
  place.y = 34;
  project.Apply(place);
  EXPECT_DOUBLE_EQ(
      std::stod(json::Get(
          json::Object(
              json::Object(
                  json::Object(project.files.at("layout.json")).at("cards"))
                  .at("unplaced")),
          "x")),
      12);
}

TEST(ProjectOperationCutover, ConcurrentFirstCommentsSelectTheirOwnEntities) {
  TransformationProject project;
  auto comment = [&](const std::string& id) {
    H::ProjectJSONEditT edit;
    edit.file_path = "definitions/a/_meta.json";
    edit.old_value = std::make_unique<H::ProjectValueT>();
    edit.old_value->present = true;
    edit.old_value->json = R"({"name":"source"})";
    edit.new_value = std::make_unique<H::ProjectValueT>(*edit.old_value);
    edit.new_value->json =
        "{\"name\":\"source\",\"comments\":[{\"id\":" + json::Quote(id) +
        ",\"text\":\"note\"}]}";
    H::ProjectEditsT body;
    auto entry = std::make_unique<H::ProjectEditT>();
    entry->value.Set(std::move(edit));
    body.edits.push_back(std::move(entry));
    return project.Apply(body);
  };
  const auto undo = comment("first");
  comment("second");
  project.Apply(undo);
  const auto comments =
      json::Array(json::Object(project.files.at("definitions/a/_meta.json"))
                      .at("comments"));
  ASSERT_EQ(comments.size(), 1);
  EXPECT_EQ(json::Text(json::Object(comments.front()), "id"), "second");
}
}  // namespace
}  // namespace tsugumi::host
