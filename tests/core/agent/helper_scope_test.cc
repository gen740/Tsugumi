#include "agent/helper_scope.hh"

#include <gtest/gtest.h>

namespace tsugumi::agent {
namespace {
const HelperScope::Reader kRead = [](const std::string& path) -> std::string {
  if (path == "tsugumi.json")
    return R"({"canvas":{"cards":[{"id":"outer"}],"connections":[]}})";
  if (path == "definitions/outer/_card.json")
    return R"({"type":"subgraph","external":"storage"})";
  return "";
};
TEST(HelperScope, FailureKeepsInScopeDiagnosticAndTypedFields) {
  HelperScope scope(json::Object(
      R"({"read_cards":["knn","compare"],"write_cards":["knn"],"run_cards":[]})"));
  const auto read = [](const std::string& path) -> std::string {
    if (path == "tsugumi.json")
      return R"({"canvas":{"cards":[{"id":"knn"},{"id":"compare"},{"id":"private"}]}})";
    return "";
  };
  for (const std::string diagnostic :
       {"wire knn -> compare: compare is still a Draft, so its to end keeps "
        "port \"\"",
        "Concurrent edit conflicts at /tsugumi.json/canvas/cards/knn/name; "
        "inspect and re-apply",
        "Concurrent edit conflicts at /definitions~1knn~1_card.json/inputs; "
        "inspect and re-apply"}) {
    const auto failure = json::Encode(json::Members{
        {"reply", json::Quote("Failure")},
        {"value", json::Encode(json::Members{
                      {"kind", json::Quote("validation_failed")},
                      {"diagnostic", json::Quote(diagnostic)},
                      {"message", json::Quote(diagnostic)},
                      {"args", R"([{"key":"card","value":"knn"}])"},
                      {"current_rev", "7"}})}});
    const auto value = json::Object(
        json::Get(json::Object(scope.FailureResult(failure, read)), "value"));
    EXPECT_EQ(json::Text(value, "kind"), "validation_failed");
    EXPECT_EQ(json::Text(value, "diagnostic"), diagnostic);
    EXPECT_EQ(json::Text(value, "message"), diagnostic);
    EXPECT_EQ(value.at("args"), R"([{"key":"card","value":"knn"}])");
    EXPECT_EQ(value.at("current_rev"), "7");
  }
}
TEST(HelperScope, FailureRedactsUnreadableCardsAndFilesOnly) {
  HelperScope scope(json::Object(
      R"({"read_cards":["mine"],"write_cards":["mine","SECRET"],"run_cards":[]})"));
  const auto read = [](const std::string& path) -> std::string {
    if (path == "tsugumi.json")
      return R"({"canvas":{"cards":[{"id":"mine"},{"id":"SECRET"}]}})";
    return "";
  };
  for (const auto& diagnostic :
       {"wire mine -> SECRET: SECRET is still a Draft; write to.port as \"\"",
        "definitions/mine/main.py: cannot read definitions/SECRET/hidden.csv",
        "Concurrent edit conflicts at /definitions~1SECRET~1_card.json/name; "
        "inspect and re-apply",
        "cannot read hidden.csv"}) {
    const auto result = scope.FailureResult(
        json::Encode(json::Members{
            {"reply", json::Quote("Failure")},
            {"value", json::Encode(json::Members{
                          {"kind", json::Quote("validation_failed")},
                          {"diagnostic", json::Quote(diagnostic)},
                          {"message", json::Quote(diagnostic)},
                          {"args", R"([{"key":"card","value":"SECRET"}])"}})}}),
        read);
    EXPECT_EQ(result.find("SECRET"), std::string::npos);
    EXPECT_EQ(result.find("hidden.csv"), std::string::npos);
    EXPECT_NE(result.find("[redacted]"), std::string::npos);
    if (std::string(diagnostic).starts_with("definitions/mine"))
      EXPECT_NE(result.find("definitions/mine/main.py"), std::string::npos);
    const auto value = json::Object(json::Get(json::Object(result), "value"));
    EXPECT_EQ(json::Text(value, "kind"), "validation_failed");
  }
}
TEST(HelperScope, FailureKeepsNestedCardAndFileNamesWithinReadScope) {
  HelperScope scope(json::Object(
      R"({"read_cards":["outer/child"],"write_cards":["outer/child"],"run_cards":[]})"));
  const auto read = [](const std::string& path) -> std::string {
    if (path == "tsugumi.json")
      return R"({"canvas":{"cards":[{"id":"outer"}]}})";
    if (path == "definitions/outer/_card.json")
      return R"({"type":"subgraph","external":"storage"})";
    if (path == "externals/storage/tsugumi.json")
      return R"({"canvas":{"cards":[{"id":"child"},{"id":"SECRET"}]}})";
    return "";
  };
  const std::string diagnostic =
      "outer/child: externals/storage/definitions/child/_card.json inputs need "
      "a name";
  const auto failure = json::Encode(json::Members{
      {"reply", json::Quote("Failure")},
      {"value",
       json::Encode(json::Members{{"kind", json::Quote("validation_failed")},
                                  {"diagnostic", json::Quote(diagnostic)}})}});
  const auto value = json::Object(
      json::Get(json::Object(scope.FailureResult(failure, read)), "value"));
  EXPECT_EQ(json::Text(value, "kind"), "validation_failed");
  EXPECT_EQ(json::Text(value, "diagnostic"), diagnostic);
}
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
          R"({"changes":[{"value_type":"PutDefinition","value":{"id":"writer","card_json":"{\"type\":\"code\"}"}}]})"),
      kRead));
  EXPECT_THROW(
      scope.Check(
          "project_save",
          json::Object(
              R"({"changes":[{"value_type":"RemoveDefinition","value":{"id":"reader"}}]})"),
          kRead),
      std::runtime_error);
  EXPECT_THROW(
      scope.Check("log_range",
                  json::Object(R"({"card":{"segments":["outer"]}})"), kRead),
      std::runtime_error);
}
TEST(HelperScope,
     ExecutorChecksTheCompleteGraphWithoutRewritingUntouchedBytes) {
  const std::string before =
      R"({"name": "workflow", "nixPath": [], "canvas":{"cards":[{"id":"writer"},{"id":"other"}],"connections":[]}})";
  const auto reader = [&](const std::string& path) {
    return path == "tsugumi.json" ? before : "";
  };
  HelperScope scope(json::Object(
      R"({"read_cards":["writer"],"write_cards":["writer"],"run_cards":[]})"));
  auto plan = json::Members{
      {"changes",
       json::Encode(std::vector<std::string>{json::Encode(json::Members{
           {"value_type", json::Quote("SetProject")},
           {"value",
            json::Encode(json::Members{{"json", json::Quote(before)}})}})})}};
  const auto bytes = json::Encode(plan);
  EXPECT_NO_THROW(scope.CheckCurrentSave(plan, reader));
  EXPECT_EQ(json::Encode(plan), bytes);
  auto changed = before;
  changed.replace(changed.find("workflow"), 8, "changed");
  plan["changes"] =
      json::Encode(std::vector<std::string>{json::Encode(json::Members{
          {"value_type", json::Quote("SetProject")},
          {"value",
           json::Encode(json::Members{{"json", json::Quote(changed)}})}})});
  EXPECT_THROW(scope.CheckCurrentSave(plan, reader), std::runtime_error);
}

TEST(HelperScope, WireEndEditsDoNotDependOnCardOrConnectionOrder) {
  for (const auto nested : {false, true}) {
    for (const auto source : {false, true}) {
      for (size_t writable = 0; writable < 3; ++writable) {
        const std::string prefix = nested ? "outer/" : "";
        const std::string document =
            nested ? "externals/storage/tsugumi.json" : "tsugumi.json";
        const std::vector<std::string> ids{"alpha", "middle", "zulu"};
        std::vector<std::string> cards{R"({"id":"share"})"}, wires;
        for (const auto& id : ids) {
          cards.push_back(json::Encode(json::Members{{"id", json::Quote(id)}}));
          const auto own = json::Encode(json::Members{
              {"card", json::Quote(id)}, {"port", json::Quote("")}});
          const std::string share = R"({"card":"share","port":"X"})";
          wires.push_back(json::Encode(json::Members{
              {"from", source ? own : share}, {"to", source ? share : own}}));
        }
        auto graph = json::Members{{"cards", json::Encode(cards)},
                                   {"connections", json::Encode(wires)}};
        const auto before =
            json::Encode(json::Members{{"canvas", json::Encode(graph)}});
        auto wire = json::Object(wires[writable]);
        auto endpoint = json::Object(wire.at(source ? "from" : "to"));
        endpoint["port"] = json::Quote("settings");
        wire[source ? "from" : "to"] = json::Encode(endpoint);
        wires[writable] = json::Encode(wire);
        graph["connections"] = json::Encode(wires);
        const auto after =
            json::Encode(json::Members{{"canvas", json::Encode(graph)}});
        const auto read = [&](const std::string& path) {
          if (path == document) return before;
          return kRead(path);
        };
        // Write ownership need not also grant a separate read capability.
        for (const auto read_own : {false, true}) {
          std::vector<std::string> reads{json::Quote(prefix + "share")};
          if (read_own) reads.push_back(json::Quote(prefix + ids[writable]));
          HelperScope scope(
              {{"read_cards", json::Encode(reads)},
               {"write_cards", json::Encode(std::vector<std::string>{
                                   json::Quote(prefix + ids[writable])})},
               {"run_cards", "[]"}});
          const auto value =
              nested ? json::Members{{"path", json::Quote(document)},
                                     {"bytes", json::Quote(after)}}
                     : json::Members{{"json", json::Quote(after)}};
          const auto plan = json::Members{
              {"changes",
               json::Encode(std::vector<std::string>{json::Encode(json::Members{
                   {"value_type",
                    json::Quote(nested ? "PutSource" : "SetProject")},
                   {"value", json::Encode(value)}})})}};
          SCOPED_TRACE(prefix + ids[writable] + (source ? ":from" : ":to"));
          EXPECT_NO_THROW(scope.CheckCurrentSave(plan, read));
        }
      }
    }
  }
}

TEST(HelperScope, FullGraphDenialNamesTheUnassignedField) {
  const std::string before =
      R"({"name":"original","canvas":{"cards":[{"id":"writer"},{"id":"secret"}],"connections":[]}})";
  const auto read = [&](const std::string&) { return before; };
  HelperScope scope(json::Object(
      R"({"read_cards":["writer"],"write_cards":["writer"],"run_cards":[]})"));
  for (
      const auto& after :
      {std::string(
           R"({"name":"changed","canvas":{"cards":[{"id":"writer"},{"id":"secret"}],"connections":[]}})"),
       std::string(
           R"({"name":"original","canvas":{"cards":[{"id":"writer"},{"id":"secret","extra":true}],"connections":[]}})")}) {
    const auto plan = json::Object(
        "{\"changes\":[{\"value_type\":\"SetProject\",\"value\":{\"json\":" +
        json::Quote(after) + "}}]}");
    try {
      scope.CheckCurrentSave(plan, read);
      FAIL() << "Unassigned edit was accepted";
    } catch (const std::runtime_error& error) {
      const std::string message = error.what();
      EXPECT_NE(message.find(after.find("changed") != std::string::npos
                                 ? "tsugumi.json/name"
                                 : "tsugumi.json/canvas/cards/1/extra"),
                std::string::npos)
          << message;
    }
  }
}

TEST(HelperScope, ChangedWiresRequireAssignedEndsAndOneWritableEnd) {
  const std::string before =
      R"({"canvas":{"cards":[{"id":"writer"},{"id":"share"},{"id":"reader"},{"id":"secret"}],"connections":[{"from":{"card":"reader","port":"out"},"to":{"card":"share","port":"in"}}]}})";
  const auto read = [&](const std::string&) { return before; };
  HelperScope scope(json::Object(
      R"({"read_cards":["writer","share","reader"],"write_cards":["writer"],"run_cards":[]})"));
  const auto check = [&](const std::string& after) {
    scope.CheckCurrentSave(json::Object("{\"changes\":[{\"value_type\":"
                                        "\"SetProject\",\"value\":{\"json\":" +
                                        json::Quote(after) + "}}]}"),
                           read);
  };
  // JSON formatting of an untouched read-only wire is not a wire edit.
  auto formatted = before;
  formatted.insert(formatted.find("\"from\":"), " ");
  EXPECT_NO_THROW(check(formatted));
  for (const auto& replacement :
       {std::string("reader"), std::string("secret")}) {
    auto after = before;
    if (replacement == "reader")
      after.replace(after.find("out"), 3, "new");
    else
      after.replace(after.find("reader", after.find("connections")), 6,
                    "secret");
    try {
      check(after);
      FAIL() << "Out-of-scope wire edit was accepted";
    } catch (const std::runtime_error& error) {
      EXPECT_NE(std::string(error.what()).find("canvas/connections"),
                std::string::npos);
    }
  }
}

TEST(HelperScope, SharedNestedDefinitionsRequireEveryAliasInTheScope) {
  const HelperScope::Reader read = [](const std::string& path) -> std::string {
    if (path == "tsugumi.json")
      return R"({"canvas":{"cards":[{"id":"outer"},{"id":"alias"}]}})";
    if (path == "definitions/outer/_card.json" ||
        path == "definitions/alias/_card.json")
      return R"({"type":"subgraph","external":"storage"})";
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
      [](const std::string&) -> std::string {
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
           {"layout", json::Quote("SECRET")},
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
  for (const auto* tool : {"run", "card_api"}) {
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
  EXPECT_THROW(
      runner.Check("run", json::Object(R"({"card":{"segments":["b"]}})"),
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
}  // namespace
}  // namespace tsugumi::agent

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
  project["nixPath"] = R"([{"name":"science","url":"github:org/science"}])";
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
    if (path == "definitions/outer/_card.json")
      return R"({"type":"subgraph","external":"storage"})";
    if (path == "externals/storage/tsugumi.json")
      return R"({"boundary":{"inputs":[{"name":"visible","card":"draft","port":""},{"name":"hidden","card":"secret","port":"in"}],"outputs":[]},"canvas":{"cards":[{"id":"draft"},{"id":"secret"}],"connections":[]}})";
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
      R"({"inputs":[{"name":"visible","card":"draft","port":"real"}],"outputs":[]})";
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
    return path == "definitions/outer/_card.json"
               ? R"({"type":"subgraph","external":"elsewhere"})"
               : read(path);
  };
  EXPECT_THROW(scope.CheckBindings(read, changed), std::runtime_error);
}
}  // namespace
}  // namespace tsugumi::agent

namespace tsugumi::agent {
namespace {
TEST(HelperGraphScope, SaveReceiptFiltersOtherHelpersCards) {
  HelperScope scope(json::Object(
      R"({"read_cards":["mine"],"write_cards":["mine"],"run_cards":[]})"));
  const auto result =
      R"({"reply":"SaveResult","value":{"rev":"2","delta":{"project_json":"{\"canvas\":{\"cards\":[{\"id\":\"mine\"},{\"id\":\"SECRET\"}],\"connections\":[]}}","layout":"SECRET","definitions_upsert":[],"sources_upsert":[],"resolution":[{"card":{"segments":["mine"]},"diagnostics":[]},{"card":{"segments":["SECRET"]}}],"diagnostics":[{"message":"SECRET"}]}}})";
  const auto filtered = scope.SaveResult(result, kRead);
  EXPECT_EQ(filtered.find("SECRET"), std::string::npos);
  EXPECT_NE(filtered.find("mine"), std::string::npos);
}
TEST(HelperCapability, NarrowedChildrenCannotRegainAnAncestorsSets) {
  const auto ancestor = json::Object(
      R"({"depth":3,"count":4,"read_cards":["a","b"],"write_cards":["a","b"],"run_cards":["a","b"]})");
  auto task = json::Object(
      R"({"depth":2,"count":3,"read_cards":["a"],"write_cards":["a"],"run_cards":["a"]})");
  HelperScope outer(ancestor, std::nullopt, "outer-scratch");
  EXPECT_NO_THROW(outer.CheckChild(task));
  HelperScope parent(task, std::nullopt, "parent-scratch");
  EXPECT_EQ(parent.scratch_identity(), "parent-scratch");
  EXPECT_EQ(parent.depth(), 2u);
  EXPECT_EQ(parent.count(), 3u);
  task["depth"] = "1";
  EXPECT_NO_THROW(parent.CheckChild(task));
  HelperScope child(task, std::nullopt, "child-scratch");
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
