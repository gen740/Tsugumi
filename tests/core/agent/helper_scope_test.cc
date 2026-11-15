#include "agent/helper_scope.hh"

#include <gtest/gtest.h>

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
      R"({"read_cards":["reader","outer/child"],"write_cards":["writer"],"run_cards":[]})"));
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
  HelperScope one(json::Object(
      R"({"read_cards":["outer/child"],"write_cards":[],"run_cards":[]})"));
  EXPECT_THROW(one.Check("project_file_read", args, read), std::runtime_error);
  HelperScope both(json::Object(
      R"({"read_cards":["outer/child","alias/child"],"write_cards":[],"run_cards":[]})"));
  EXPECT_NO_THROW(both.Check("project_file_read", args, read));
}
TEST(HelperScope,
     ContainerChangesAndFailedOwnershipChecksCannotCrossTheBoundary) {
  HelperScope scope(json::Object(
      R"({"read_cards":["outer"],"write_cards":["outer"],"run_cards":[]})"));
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
  const auto task =
      json::Object(R"({"read_cards":[],"write_cards":[],"run_cards":[]})");
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
TEST(HelperScope, ExecutionPlanRequiresRunForEveryCard) {
  HelperScope scope(json::Object(
      R"({"read_cards":["start","downstream"],"write_cards":["start"],"run_cards":[]})"));
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
  HelperScope scope(json::Object(
      R"({"read_cards":["visible"],"write_cards":[],"run_cards":[]})"));
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
          json::Object(
              R"({"read_cards":["../other"],"write_cards":[],"run_cards":[]})"),
          false),
      std::runtime_error);
  EXPECT_NO_THROW(HelperScope::Validate(
      json::Object(
          R"({"read_cards":["card.v2"],"write_cards":[],"run_cards":[]})"),
      false));
}
TEST(HelperScope, RunOwnershipComesFromReceiptsEvenAfterTranscriptCompaction) {
  const auto task = json::Object(
      R"({"read_cards":["a"],"write_cards":[],"run_cards":["a"]})");
  HelperScope restored(task), sibling(task);
  restored.Restore(
      R"({"reply":"RunAccepted","value":{"run":{"run_id":"owned"}},"helper_run_card":{"segments":["a"]}})",
      kRead);
  EXPECT_NO_THROW(
      restored.Check("run_cancel", {{"run_id", json::Quote("owned")}}, kRead));
  EXPECT_THROW(
      sibling.Check("run_cancel", {{"run_id", json::Quote("owned")}}, kRead),
      std::runtime_error);
  restored.Completed(
      "run", json::Object(R"({"card":{"segments":["a"]}})"),
      R"({"reply":"RunAccepted","value":{"run":{"run_id":"live"}}})", kRead);
  EXPECT_NO_THROW(
      restored.Check("run_status", {{"run_id", json::Quote("live")}}, kRead));
}
TEST(HelperScope, WriteDoesNotGrantRunAndReadRunDoesNotGrantWrite) {
  HelperScope writer(json::Object(
      R"({"read_cards":["a"],"write_cards":["a"],"run_cards":[]})"));
  HelperScope runner(json::Object(
      R"({"read_cards":["a"],"write_cards":[],"run_cards":["a"]})"));
  const auto card = json::Object(R"({"card":{"segments":["a"]}})");
  for (const auto *tool : {"run", "card_s_d_k_prepare"}) {
    EXPECT_THROW(writer.Check(tool, card, kRead), std::runtime_error);
    EXPECT_NO_THROW(runner.Check(tool, card, kRead));
  }
  EXPECT_THROW(
      runner.Check(
          "project_save",
          json::Object(
              R"({"changes":[{"value_type":"RemoveDefinition","value":{"id":"a"}}]})"),
          kRead),
      std::runtime_error);
  EXPECT_THROW(runner.Check("run",
                            json::Object(R"({"card":{"segments":["b"]}})"),
                            kRead),
               std::runtime_error);
  const auto plan =
      R"({"reply":"RunPlanResult","value":{"targets":[{"segments":["a"]}],"starts":[],"build_candidates":[]}})";
  EXPECT_NO_THROW(runner.CheckPlan(plan, true));
  EXPECT_THROW(writer.CheckPlan(plan, true), std::runtime_error);
  const auto downstream =
      R"({"reply":"RunPlanResult","value":{"targets":[{"segments":["a"]},{"segments":["b"]}],"starts":[],"build_candidates":[]}})";
  EXPECT_THROW(runner.CheckPlan(downstream, true), std::runtime_error);
  runner.Ran("owned", "a");
  EXPECT_NO_THROW(
      runner.Check("run_cancel", {{"run_id", json::Quote("owned")}}, kRead));
  EXPECT_THROW(
      runner.Check("run_cancel", {{"run_id", json::Quote("other")}}, kRead),
      std::runtime_error);
  writer.Ran("unassigned", "a");
  EXPECT_THROW(writer.Check("run_cancel",
                            {{"run_id", json::Quote("unassigned")}}, kRead),
               std::runtime_error);
  runner.Created("new", "", kRead);
  EXPECT_NO_THROW(runner.Check(
      "run", json::Object(R"({"card":{"segments":["new"]}})"), kRead));
  EXPECT_THROW(HelperScope::Validate(
                   json::Object(R"({"read_cards":[],"write_cards":[]})"), true),
               std::runtime_error);
}
} // namespace
} // namespace tsugumi::agent

namespace tsugumi::agent {
namespace {
TEST(HelperGraphScope, AddsOnlyOwnedCardsAndKeepsTheHiddenGraph) {
  HelperScope scope(
      json::Object(R"({"read_cards":["up"],"write_cards":[],"run_cards":[]})"));
  const HelperScope::Reader read = [](const std::string& path) -> std::string {
    if (path == "tsugumi.json")
      return R"({"name":"P","secret":"keep","canvas":{"cards":[{"id":"up"},{"id":"hidden"}],"connections":[{"from":{"card":"hidden","port":"out"},"to":{"card":"up","port":"in"}}]}})";
    return "";
  };
  scope.Created("mine", "", read);
  auto project = json::Object(scope.Graph(read("tsugumi.json"), ""));
  project["canvas"] =
      R"({"cards":[{"id":"up"},{"id":"mine"}],"connections":[{"from":{"card":"up","port":"out"},"to":{"card":"mine","port":"in"}}]})";
  auto args = [&](const std::string& doc) {
    return json::Members{
        {"changes",
         json::Encode(std::vector<std::string>{json::Encode(json::Members{
             {"value_type", json::Quote("SetProject")},
             {"value",
              json::Encode(json::Members{{"json", json::Quote(doc)}})}})})}};
  };
  EXPECT_NO_THROW(
      scope.Check("project_save", args(json::Encode(project)), read));
  const auto expanded = scope.ExpandSave(args(json::Encode(project)), read);
  const auto value = json::Object(
      json::Get(json::Object(json::Array(expanded.at("changes"))[0]), "value"));
  const auto full = json::Object(json::Text(value, "json"));
  EXPECT_EQ(json::Text(full, "secret"), "keep");
  const auto graph = json::Object(full.at("canvas"));
  EXPECT_EQ(json::Array(graph.at("cards")).size(), 3u);
  EXPECT_EQ(json::Array(graph.at("connections")).size(), 2u);
  auto canvas = json::Object(project.at("canvas"));
  canvas["cards"] = R"([{"id":"up"},{"id":"not-mine"}])";
  project["canvas"] = json::Encode(canvas);
  EXPECT_THROW(scope.Check("project_save", args(json::Encode(project)), read),
               std::runtime_error);
  canvas["cards"] = R"([{"id":"up"},{"id":"mine"}])";
  canvas["connections"] =
      R"([{"from":{"card":"hidden","port":"out"},"to":{"card":"mine","port":"in"}}])";
  project["canvas"] = json::Encode(canvas);
  EXPECT_THROW(scope.Check("project_save", args(json::Encode(project)), read),
               std::runtime_error);
  project["pkgsPath"] = json::Quote("changed");
  EXPECT_THROW(scope.Check("project_save", args(json::Encode(project)), read),
               std::runtime_error);
}
TEST(HelperGraphScope, ReadOnlyWiresAndSiblingReservationsAreRefused) {
  HelperScope scope(json::Object(
      R"({"read_cards":["a","b"],"write_cards":[],"run_cards":[]})"));
  const HelperScope::Reader read = [](const std::string& path) -> std::string {
    return path == "tsugumi.json"
               ? R"({"canvas":{"cards":[{"id":"a"},{"id":"b"}],"connections":[]}})"
               : "";
  };
  const auto graph =
      R"({"canvas":{"cards":[{"id":"a"},{"id":"b"}],"connections":[{"from":{"card":"a","port":"out"},"to":{"card":"b","port":"in"}}]}})";
  const auto args = json::Members{
      {"changes",
       json::Encode(std::vector<std::string>{json::Encode(json::Members{
           {"value_type", json::Quote("SetProject")},
           {"value",
            json::Encode(json::Members{{"json", json::Quote(graph)}})}})})}};
  EXPECT_THROW(scope.Check("project_save", args, read), std::runtime_error);
}
TEST(HelperGraphScope, NestedGraphAndBoundaryRebindingKeepOtherEndpoints) {
  const HelperScope::Reader read = [](const std::string& path) -> std::string {
    if (path == "tsugumi.json")
      return R"({"canvas":{"cards":[{"id":"outer"}],"connections":[]}})";
    if (path == "definitions/outer/_link.json")
      return R"({"external":"storage"})";
    if (path == "externals/storage/tsugumi.json")
      return R"({"boundary":{"inputs":{"visible":{"card":"draft","port":""},"hidden":{"card":"secret","port":"in"}},"outputs":{}},"canvas":{"cards":[{"id":"draft"},{"id":"secret"}],"connections":[]}})";
    return "";
  };
  HelperScope scope(json::Object(
      R"({"read_cards":["outer/draft"],"write_cards":["outer/draft"],"run_cards":[]})"));
  scope.Created("mine", "externals/storage/", read);
  EXPECT_NO_THROW(scope.Check(
      "project_file_read",
      {{"path", json::Quote("externals/storage/tsugumi.json")}}, read));
  auto graph = json::Object(
      scope.Graph(read("externals/storage/tsugumi.json"), "outer/"));
  EXPECT_EQ(json::Encode(graph).find("secret"), std::string::npos);
  graph["boundary"] =
      R"({"inputs":{"visible":{"card":"draft","port":"real"}},"outputs":{}})";
  auto canvas = json::Object(graph.at("canvas"));
  canvas["cards"] = R"([{"id":"draft"},{"id":"mine"}])";
  graph["canvas"] = json::Encode(canvas);
  const auto args = json::Members{
      {"changes",
       json::Encode(std::vector<std::string>{json::Encode(json::Members{
           {"value_type", json::Quote("PutSource")},
           {"value",
            json::Encode(json::Members{
                {"path", json::Quote("externals/storage/tsugumi.json")},
                {"bytes", json::Quote(json::Encode(graph))}})}})})}};
  EXPECT_NO_THROW(scope.Check("project_save", args, read));
  const auto saved = scope.ExpandSave(args, read);
  const auto value = json::Object(
      json::Get(json::Object(json::Array(saved.at("changes"))[0]), "value"));
  EXPECT_NE(json::Text(value, "bytes").find("secret"), std::string::npos);
  const HelperScope::Reader changed =
      [&](const std::string& path) -> std::string {
    return path == "definitions/outer/_link.json"
               ? R"({"external":"elsewhere"})"
               : read(path);
  };
  EXPECT_THROW(scope.CheckBindings(read, changed), std::runtime_error);
}
}  // namespace
}  // namespace tsugumi::agent

namespace tsugumi::agent {
namespace {
TEST(HelperGraphScope,
     SaveReceiptFiltersOtherHelpersCardsAndCollisionMessages) {
  HelperScope scope(json::Object(
      R"({"read_cards":["mine"],"write_cards":["mine"],"run_cards":[]})"));
  const auto result =
      R"({"reply":"SaveResult","value":{"rev":"2","delta":{"project_json":"{\"canvas\":{\"cards\":[{\"id\":\"mine\"},{\"id\":\"SECRET\"}],\"connections\":[]}}","layout_json":"SECRET","definitions_upsert":[],"sources_upsert":[],"resolution":[{"card":{"segments":["mine"]},"diagnostics":[]},{"card":{"segments":["SECRET"]}}],"diagnostics":[{"message":"SECRET"}]}}})";
  const auto filtered = scope.SaveResult(result, kRead);
  EXPECT_EQ(filtered.find("SECRET"), std::string::npos);
  EXPECT_NE(filtered.find("mine"), std::string::npos);
  EXPECT_TRUE(scope.SafeCollision(
      "Concurrent edit conflicts at "
      "/definitions~1mine~1_card.json/timeoutSeconds; inspect and re-apply",
      kRead));
  EXPECT_FALSE(scope.SafeCollision(
      "Concurrent edit conflicts at "
      "/definitions~1SECRET~1_card.json/timeoutSeconds; inspect and re-apply",
      kRead));
}
TEST(HelperCapability, NarrowedChildrenCannotRegainAnAncestorsSets) {
  const auto ancestor = json::Object(
      R"({"depth":3,"count":4,"read_cards":["a","b"],"write_cards":["a","b"],"run_cards":["a","b"]})");
  auto task = json::Object(
      R"({"depth":2,"count":3,"read_cards":["a"],"write_cards":["a"],"run_cards":["a"]})");
  HelperScope outer(ancestor, "authorization", "outer-scratch");
  EXPECT_NO_THROW(outer.CheckChild(task));
  HelperScope parent(task, "authorization", "parent-scratch");
  EXPECT_EQ(parent.authorization(), "authorization");
  EXPECT_EQ(parent.scratch_identity(), "parent-scratch");
  EXPECT_EQ(parent.depth(), 2u);
  EXPECT_EQ(parent.count(), 3u);
  task["depth"] = "1";
  EXPECT_NO_THROW(parent.CheckChild(task));
  HelperScope child(task, "authorization", "child-scratch");
  task["depth"] = "0";
  EXPECT_NO_THROW(child.CheckChild(task));
  for (const auto* field : {"read_cards", "write_cards", "run_cards"}) {
    auto widened = task;
    widened[field] = R"(["b"])";
    EXPECT_THROW(parent.CheckChild(widened), std::runtime_error);
    EXPECT_THROW(child.CheckChild(widened), std::runtime_error);
  }
  EXPECT_THROW(child.CheckChild({}), std::runtime_error);
}

TEST(HelperCapability, DefaultsCannotSpawnAndChildrenCannotWidenDepthOrSets) {
  auto task = json::Object(
      R"({"read_cards":["a"],"write_cards":["b"],"run_cards":["c"]})");
  HelperScope defaults(task);
  EXPECT_EQ(defaults.depth(), 0u);
  EXPECT_EQ(defaults.count(), 0u);
  EXPECT_THROW(defaults.CheckChild(task), std::runtime_error);
  task["depth"] = "2";
  task["count"] = "3";
  HelperScope parent(task);
  EXPECT_EQ(parent.count(), 3u);
  EXPECT_THROW(parent.CheckChild(task), std::runtime_error);
  task["depth"] = "1";
  EXPECT_NO_THROW(parent.CheckChild(task));
  for (const auto* access : {"read_cards", "write_cards", "run_cards"}) {
    auto child = task;
    child[access] = R"(["outside"])";
    EXPECT_THROW(parent.CheckChild(child), std::runtime_error);
  }
  auto unscoped = json::Object(R"({"depth":0,"count":0})");
  EXPECT_THROW(parent.CheckChild(unscoped), std::runtime_error);
  for (const auto* invalid : {"-1", "1.5", "true", "\"1\""}) {
    task["depth"] = invalid;
    EXPECT_THROW(HelperScope::Validate(task, false), std::runtime_error);
  }
}
}  // namespace
}  // namespace tsugumi::agent
