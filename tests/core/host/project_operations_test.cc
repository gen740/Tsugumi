#include "host/project_operations.hh"

#include <gtest/gtest.h>

#include "host/failure.hh"

namespace tsugumi::host {
namespace {
namespace H = Tsugumi::Host;
namespace json = agent::json;
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
