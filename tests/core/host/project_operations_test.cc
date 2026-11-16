#include "host/project_operations.hh"

#include <gtest/gtest.h>

#include "host/failure.hh"
#include "host/layout_saving.hh"

namespace tsugumi::host {
namespace {
namespace H = Tsugumi::Host;
namespace json = agent::json;
TEST(ProjectOperations,
     MissingHostIdentityIsAnInternalFailureBeforeAnyReadOrWrite) {
  for (const auto &invalid : {"project", "operation"}) {
    H::ProjectOperateRequestT request;
    request.project_id = std::string(invalid) == "project" ? "other" : "p";
    request.op_id = std::string(invalid) == "operation"
                        ? ""
                        : "d7f52370-d5f6-4c4f-8a1f-546eb93090f1";
    bool called = false;
    try {
      ProjectOperations().Execute(
          "p", request, {}, [&](const auto &, const auto &) -> OperationReply {
            called = true;
            return {};
          });
      FAIL() << "invalid host identity must fail";
    } catch (const Failure &failure) {
      EXPECT_EQ(failure.code, "internal_error");
    }
    EXPECT_FALSE(called);
  }
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
        intent = args.at("save_intent");
        ++revision;
        reply = "Failure";
        value = {{"kind", "\"revision_conflict\""}};
      } else {
        EXPECT_EQ(args.at("save_intent"), intent);
        EXPECT_EQ(json::Text(args, "base_rev"), "2");
        EXPECT_NE(args.at("changes").find("bmV3ICsgdW5yZWxhdGVk"),
                  std::string::npos);
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
  const auto result =
      operations.Execute("p", request, {"ui", {}, {}, {}}, call);
  EXPECT_EQ(json::Text(json::Object(result.json), "reply"), "SaveResult");
  EXPECT_EQ(saves, 2);
}
}  // namespace
}  // namespace tsugumi::host

namespace tsugumi::host {
namespace {
TEST(ProjectOperationUndo,
     UnrelatedWriterChangesSurviveAndConflictsNameTheField) {
  SaveBase before;
  before.snapshot["rev"] = "1";
  before.files["tsugumi.json"] =
      R"({"name":"old","canvas":{"cards":[],"connections":[]}})";
  const json::Members forward{
      {"changes",
       R"([{"value_type":"SetProject","value":{"json":"{\"name\":\"new\",\"canvas\":{\"cards\":[],\"connections\":[]}}"}}])"}};
  const auto read = [](const auto &) -> std::optional<std::string> {
    return std::nullopt;
  };
  auto inverse = ReverseProjectChanges(forward, before, read);
  ASSERT_EQ(inverse.edits.size(), 1);
  SaveBase current = before;
  current.files["tsugumi.json"] =
      R"({"name":"new","canvas":{"cards":[],"connections":[]},"saveRunHistory":false})";
  const auto plan = PrepareProjectEdits(inverse, current, read);
  const auto change = json::Object(json::Array(plan.at("changes")).front());
  const auto restored =
      json::Object(json::Text(json::Object(change.at("value")), "json"));
  EXPECT_EQ(json::Text(restored, "name"), "old");
  EXPECT_EQ(restored.at("saveRunHistory"), "false");
  current.files["tsugumi.json"] =
      R"({"name":"changed-by-someone-else","canvas":{"cards":[],"connections":[]}})";
  try {
    PrepareProjectEdits(inverse, current, read);
    FAIL() << "Undo must refuse a conflicting writer";
  } catch (const Failure &failure) {
    EXPECT_EQ(failure.code, "value_conflict");
    EXPECT_NE(std::string(failure.what()).find("tsugumi.json.name"),
              std::string::npos);
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
  const auto read = [](const auto &) -> std::optional<std::string> {
    return std::nullopt;
  };
  const auto inverse = ReverseProjectChanges(forward, before, read);
  SaveBase current = before;
  current.files["layout.json"] = R"({"groups":[{"id":"g","name":"new"}]})";
  const auto plan = PrepareProjectEdits(inverse, current, read);
  EXPECT_EQ(json::Array(plan.at("changes")).size(), 1);
  EXPECT_EQ(plan.at("changes").find("other"), std::string::npos);
}

TEST(ProjectOperationUndo, MoveUndoPreservesThePreviouslyAcceptedPortOrder) {
  SaveBase before;
  before.snapshot["rev"] = "1";
  before.files["layout.json"] =
      R"({"cards":{"a":{"x":10,"y":20,"inputOrder":["second","first"]}}})";
  const json::Members move{
      {"changes",
       R"([{"value_type":"SetLayout","value":{"json":"{\"cards\":{\"a\":{\"x\":30,\"y\":20,\"inputOrder\":[\"first\",\"second\"]}}}"}}])"}};
  const auto read = [](const auto &) -> std::optional<std::string> {
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
} // namespace
} // namespace tsugumi::host

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
  auto call = [&](const std::string &method,
                  const json::Members &args) -> OperationReply {
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
  const auto first = executor.Execute("p", request, {"ui", {}, {}, {}}, call);
  const auto repeat = executor.Execute("p", request, {"ui", {}, {}, {}}, call);
  EXPECT_EQ(first.json, repeat.json);
  EXPECT_EQ(reads, 1);
  EXPECT_EQ(applications, 1);
}
} // namespace
} // namespace tsugumi::host

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
} // namespace
} // namespace tsugumi::host
