#include <gtest/gtest.h>

#include "agent/helper_scope.hh"

namespace tsugumi::agent {
namespace {
const HelperScope::Reader kRead = [](const std::string &path) -> std::string {
  if (path == "tsugumi.json")
    return R"({"canvas":{"cards":[{"id":"outer"}],"connections":[]}})";
  if (path == "definitions/outer/_link.json")
    return R"({"external":"storage"})";
  return "";
};
TEST(HelperScope, ReadsAndWritesAreSeparateExactSetsWithNestedIdentifiers) {
  HelperScope scope(json::Object(
      R"({"read_cards":["reader","outer/child"],"write_cards":["writer"]})"));
  EXPECT_NO_THROW(scope.Check(
      "project_file_read",
      {{"path", json::Quote("definitions/reader/src/a.py")}}, kRead));
  EXPECT_THROW(
      scope.Check("project_file_read",
                  {{"path", json::Quote("definitions/writer/_card.json")}},
                  kRead),
      std::runtime_error);
  EXPECT_NO_THROW(scope.Check(
      "project_file_read",
      {{"path", json::Quote("externals/storage/definitions/child/_card.json")}},
      kRead));
  EXPECT_THROW(scope.Check("project_file_list",
                           {{"path", json::Quote("externals/storage")}}, kRead),
               std::runtime_error);
  EXPECT_THROW(
      scope.Check(
          "project_file_read",
          {{"path",
            json::Quote("externals/storage/definitions/sibling/_card.json")}},
          kRead),
      std::runtime_error);
  EXPECT_THROW(
      scope.Check(
          "project_file_read",
          {{"path", json::Quote("definitions/reader/../writer/_card.json")}},
          kRead),
      std::runtime_error);
  EXPECT_NO_THROW(scope.Check(
      "project_save",
      json::Object(
          R"({"changes":[{"value_type":"PutDefinition","value":{"id":"writer","card_json":"{}"}}]})"),
      kRead));
  EXPECT_THROW(
      scope.Check(
          "project_save",
          json::Object(
              R"({"changes":[{"value_type":"RemoveDefinition","value":{"id":"reader"}}]})"),
          kRead),
      std::runtime_error);
  EXPECT_THROW(scope.Check("log_range",
                           json::Object(R"({"card":{"segments":["outer"]}})"),
                           kRead),
               std::runtime_error);
}
TEST(HelperScope, SharedNestedDefinitionsRequireEveryAliasInTheScope) {
  const HelperScope::Reader read = [](const std::string &path) -> std::string {
    if (path == "tsugumi.json")
      return R"({"canvas":{"cards":[{"id":"outer"},{"id":"alias"}]}})";
    if (path == "definitions/outer/_link.json" ||
        path == "definitions/alias/_link.json")
      return R"({"external":"storage"})";
    return "";
  };
  const auto args = json::Object(
      R"({"path":"externals/storage/definitions/child/_card.json"})");
  HelperScope one(
      json::Object(R"({"read_cards":["outer/child"],"write_cards":[]})"));
  EXPECT_THROW(one.Check("project_file_read", args, read), std::runtime_error);
  HelperScope both(json::Object(
      R"({"read_cards":["outer/child","alias/child"],"write_cards":[]})"));
  EXPECT_NO_THROW(both.Check("project_file_read", args, read));
}
TEST(HelperScope,
     ContainerChangesAndFailedOwnershipChecksCannotCrossTheBoundary) {
  HelperScope scope(
      json::Object(R"({"read_cards":["outer"],"write_cards":["outer"]})"));
  const auto definition = json::Object(
      R"({"changes":[{"value_type":"RemoveDefinition","value":{"id":"outer"}}]})");
  EXPECT_THROW(scope.Check("project_save", definition, kRead),
               std::runtime_error);
  const auto source = json::Object(
      R"({"changes":[{"value_type":"PutSource","value":{"path":"definitions/outer/_card.json","bytes":[]}}]})");
  EXPECT_THROW(scope.Check("project_save", source, kRead), std::runtime_error);
  const HelperScope::Reader unavailable =
      [](const std::string &) -> std::string {
    throw std::runtime_error("unavailable");
  };
  EXPECT_THROW(scope.Check("project_save", definition, unavailable),
               std::runtime_error);
}
TEST(HelperScope, NewCardsAreOwnedByTheirCreatorOnly) {
  const auto task = json::Object(R"({"read_cards":[],"write_cards":[]})");
  HelperScope owner(task), sibling(task);
  owner.Created("new-card", "", kRead);
  const auto args = json::Object(R"({"path":"definitions/new-card/src/a.py"})");
  EXPECT_NO_THROW(owner.Check("project_file_read", args, kRead));
  EXPECT_THROW(sibling.Check("project_file_read", args, kRead),
               std::runtime_error);
  EXPECT_NO_THROW(owner.Check(
      "project_save",
      json::Object(
          R"({"changes":[{"value_type":"PutSource","value":{"path":"definitions/new-card/src/a.py","bytes":[]}}]})"),
      kRead));
}
TEST(HelperScope, AllRunPlanCardsMustBeInBothSetsBeforeExecution) {
  HelperScope scope(json::Object(
      R"({"read_cards":["start","downstream"],"write_cards":["start"]})"));
  const auto plan =
      R"({"reply":"RunPlanResult","value":{"targets":[{"segments":["start"]},{"segments":["downstream"]}],"starts":[{"segments":["start"]}],"build_candidates":[]}})";
  EXPECT_NO_THROW(scope.CheckPlan(plan, false));
  EXPECT_THROW(scope.CheckPlan(plan, true), std::runtime_error);
  EXPECT_THROW(
      scope.CheckResult(
          R"({"value":{"upstream":[{"card":{"segments":["secret"]},"bytes":[1,2]}]}})",
          kRead),
      std::runtime_error);
}
TEST(HelperScope, InspectionDoesNotLeakOtherCardsOrProjectWideState) {
  HelperScope scope(
      json::Object(R"({"read_cards":["visible"],"write_cards":[]})"));
  const auto project = json::Encode(json::Members{
      {"name", json::Quote("Project")},
      {"canvas",
       R"({"cards":[{"id":"visible"},{"id":"SECRET"}],"connections":[]})"}});
  const auto result = json::Encode(json::Members{
      {"reply", json::Quote("ProjectSnapshot")},
      {"value",
       json::Encode(json::Members{
           {"rev", "7"},
           {"project_json", json::Quote(project)},
           {"definitions",
            R"([{"id":"visible","card_json":"VISIBLE"},{"id":"SECRET","card_json":"SECRET"}])"},
           {"sources",
            R"([{"path":"definitions/visible/a"},{"path":"definitions/SECRET/b"}])"},
           {"layout_json", json::Quote("SECRET")},
           {"run", R"({"failure":"SECRET"})"},
           {"diagnostics", R"([{"message":"SECRET"}])"}})}});
  const auto filtered = scope.Inspect(result, kRead);
  EXPECT_EQ(filtered.find("SECRET"), std::string::npos);
  EXPECT_NE(filtered.find("VISIBLE"), std::string::npos);
}
TEST(HelperScope,
     UnscopedWorkRequiresTheUsersSettingAndCompleteSetsAreValidated) {
  EXPECT_THROW(HelperScope::Validate({}, false), std::runtime_error);
  EXPECT_NO_THROW(HelperScope::Validate({}, true));
  EXPECT_THROW(
      HelperScope::Validate(json::Object(R"({"read_cards":[]})"), true),
      std::runtime_error);
  EXPECT_THROW(
      HelperScope::Validate(
          json::Object(R"({"read_cards":["../other"],"write_cards":[]})"),
          false),
      std::runtime_error);
  EXPECT_NO_THROW(HelperScope::Validate(
      json::Object(R"({"read_cards":["card.v2"],"write_cards":[]})"), false));
}
} // namespace
} // namespace tsugumi::agent
