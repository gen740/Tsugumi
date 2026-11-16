#include "host/project_edit.hh"

#include <gtest/gtest.h>

#include "host/failure.hh"
#include "util/crypto.hh"

namespace tsugumi::host {
namespace {
namespace H = Tsugumi::Host;
namespace json = agent::json;
auto Field(std::vector<std::string> fields, std::string old_value,
           std::string new_value) -> H::ProjectFieldEditT {
  H::ProjectFieldEditT edit;
  edit.file_path = "tsugumi.json";
  for (const auto& key : fields) {
    auto step = std::make_unique<H::ProjectSelectorT>();
    step->field = key;
    edit.selector.push_back(std::move(step));
  }
  edit.old_value = std::make_unique<H::ProjectValueT>();
  edit.old_value->present = true;
  edit.old_value->json = std::move(old_value);
  edit.new_value = std::make_unique<H::ProjectValueT>();
  edit.new_value->present = true;
  edit.new_value->json = std::move(new_value);
  return edit;
}
TEST(ProjectEdit,
     PreservesEveryUntouchedByteIncludingPinsHashesAndUint64Spelling) {
  const std::string before =
      "{\n  \"name\": \"before\",\n  \"pkgsPath\": \"nixpkgs-stable\",\n"
      "  \"nixSources\": { \"pin\": {\"hash\": \"sha256-ExactBytes=\", "
      "\"rev\": 18446744073709551615} },\n"
      "  \"canvas\": {\"cards\":[],\"connections\":[]}\n}\n";
  auto edit = Field({"name"}, "\"before\"", "\"after\"");
  auto expected = before;
  expected.replace(expected.find("before"), 6, "after");
  EXPECT_EQ(EditProjectField(before, edit), expected);
}
TEST(ProjectEdit, KeyedSelectorsIgnoreArrayPositionsAndRefuseAmbiguity) {
  auto edit = Field({"cards"}, "1", "2");
  auto key = std::make_unique<H::ProjectSelectorT>();
  key->match_json = R"({"id":"b"})";
  edit.selector.push_back(std::move(key));
  auto field = std::make_unique<H::ProjectSelectorT>();
  field->field = "value";
  edit.selector.push_back(std::move(field));
  EXPECT_EQ(
      EditProjectField(
          R"({"cards":[{"id":"a","value":1},{"id":"b","value":1}]})", edit),
      R"({"cards":[{"id":"a","value":1},{"id":"b","value":2}]})");
  EXPECT_THROW(
      EditProjectField(
          R"({"cards":[{"id":"b","value":1},{"id":"b","value":1}]})", edit),
      Failure);
}
TEST(ProjectEdit, MissingNullEmptyAndGuardedRemovalAreDistinct) {
  auto edit = Field({"value"}, "null", "\"\"");
  EXPECT_THROW(EditProjectField("{}", edit), Failure);
  EXPECT_EQ(EditProjectField("{\"value\": null, \"other\":1}", edit),
            "{\"value\": \"\", \"other\":1}");
  edit.new_value->present = false;
  EXPECT_EQ(EditProjectField("{\"value\": null, \"other\":1}", edit),
            "{\"other\":1}");
  EXPECT_EQ(EditProjectField("{\"other\":1, \"value\": null}", edit),
            "{\"other\":1}");
  edit.old_value->present = false;
  edit.new_value->present = true;
  EXPECT_EQ(EditProjectField("{\"other\":1}", edit),
            "{\"other\":1,\"value\":\"\"}");
}
TEST(ProjectEdit, SourceReplacementRequiresTheExactMatchCountAndIsOnePlan) {
  H::ProjectEditsT edits;
  H::ProjectTextEditT text;
  text.file_path = "definitions/a/main.py";
  text.old_string = "old";
  text.new_string = "new";
  auto entry = std::make_unique<H::ProjectEditT>();
  entry->value.Set(text);
  edits.edits.push_back(std::move(entry));
  SaveBase base;
  base.snapshot["rev"] = "\"7\"";
  auto read = [](const auto&) -> std::optional<std::string> {
    return "old old";
  };
  EXPECT_THROW(PrepareProjectEdits(edits, base, read), Failure);
  edits.edits.front()->value.AsProjectTextEdit()->expected_matches = 2;
  auto plan = PrepareProjectEdits(edits, base, read);
  auto changes = json::Array(plan.at("changes"));
  ASSERT_EQ(changes.size(), 1u);
  auto value = json::Object(json::Get(json::Object(changes.front()), "value"));
  EXPECT_EQ(json::Text(value, "base_hash"), crypto::sha256_hex("old old"));
  EXPECT_EQ(json::Text(value, "bytes"), "");  // binary-safe base64 transport
  EXPECT_TRUE(json::Object(json::Get(value, "bytes")).contains("base64"));
}
TEST(ProjectEdit, FileMoveGuardsBothEndsAndPreservesBinaryBytes) {
  H::ProjectEditsT edits;
  H::ProjectFileMoveT move;
  move.file_path = "definitions/a/from.bin";
  move.to = "definitions/a/to.bin";
  const std::string bytes("\0\xff\n", 3);
  move.expected_hash = crypto::sha256_hex(bytes);
  move.destination_missing = true;
  auto entry = std::make_unique<H::ProjectEditT>();
  entry->value.Set(move);
  edits.edits.push_back(std::move(entry));
  SaveBase base;
  base.snapshot["rev"] = "\"0\"";
  auto read = [&](const std::string& path) -> std::optional<std::string> {
    return path == move.file_path ? std::optional(bytes) : std::nullopt;
  };
  EXPECT_EQ(
      json::Array(PrepareProjectEdits(edits, base, read).at("changes")).size(),
      2u);
  EXPECT_THROW(
      PrepareProjectEdits(edits, base,
                          [](const auto&) -> std::optional<std::string> {
                            return "concurrently replaced";
                          }),
      Failure);
}
TEST(ProjectEdit, AuthorizesEveryReadBeforeMaterializingIt) {
  H::ProjectEditsT edits;
  H::ProjectFileRemoveT remove;
  remove.file_path = "definitions/private/data";
  remove.expected_hash = "hash";
  auto entry = std::make_unique<H::ProjectEditT>();
  entry->value.Set(remove);
  edits.edits.push_back(std::move(entry));
  bool read = false;
  EXPECT_THROW(PrepareProjectEdits(
                   edits, {},
                   [&](const auto&) -> std::optional<std::string> {
                     read = true;
                     return "secret";
                   },
                   [](const auto&) {
                     throw Failure("permission_denied", "outside read set");
                   }),
               Failure);
  EXPECT_FALSE(read);
}
}  // namespace
}  // namespace tsugumi::host
