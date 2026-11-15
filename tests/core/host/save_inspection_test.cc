#include <gtest/gtest.h>

#include "host/failure.hh"
#include "agent/catalog.hh"
#include "host/save.hh"

namespace tsugumi::host {
namespace {
namespace json = agent::json;

TEST(HostSaveInspection,
     PlacementBetweenSnapshotAndExternalReadsRestartsCapture) {
  const std::string path = "externals/storage/layout.json";
  const std::string before = R"({"cards":{}})",
                    placed = R"({"cards":{"child":{"x":40,"y":80}}})";
  auto snapshot = json::Members{
      {"rev", json::Quote("7")},
      {"execution_rev", json::Quote("3")},
      {"project_json", json::Quote("{}")},
      {"layout_json", json::Quote("{}")},
      {"sources",
       R"([{"path":"externals/storage/layout.json","hash":"before"}])"}};
  int inspections = 0, reads = 0;
  const auto inspection =
      InspectForSave([&](const std::string &method,
                         const json::Members &args) -> json::Members {
        if (method == "project_inspect") {
          ++inspections;
          return snapshot;
        }
        EXPECT_EQ(method, "project_file_read");
        EXPECT_EQ(json::Text(args, "path"), path);
        ++reads;
        if (reads == 1) {
          EXPECT_EQ(json::Text(args, "at_rev"), "7");
          EXPECT_EQ(json::Text(args, "if_hash"), "before");
          // Return one page before the observer's placement commits. The next
          // page must be rejected, and even these bytes must be discarded.
          snapshot["rev"] = json::Quote("8");
          snapshot["sources"] =
              R"([{"path":"externals/storage/layout.json","hash":"placed"}])";
          return {{"bytes", json::Quote(before.substr(0, 5))},
                  {"eof", "false"}};
        }
        if (reads == 2) {
          EXPECT_EQ(json::Text(args, "at_rev"), "7");
          EXPECT_EQ(json::Get(args, "offset"), "5");
          throw Failure("revision_conflict",
                        "the document is at another revision");
        }
        EXPECT_EQ(json::Text(args, "at_rev"), "8");
        EXPECT_EQ(json::Text(args, "if_hash"), "placed");
        EXPECT_EQ(json::Get(args, "offset"), "0");
        return {{"bytes", json::Quote(placed)}, {"eof", "true"}};
      });
  EXPECT_EQ(inspections, 2);
  EXPECT_EQ(reads, 3);
  EXPECT_EQ(json::Text(inspection.snapshot, "rev"), "8");
  EXPECT_EQ(json::Text(inspection.snapshot, "execution_rev"), "3");
  EXPECT_EQ(inspection.base.files.at(path), placed);
  EXPECT_EQ(inspection.base.snapshot.at("sources"), snapshot.at("sources"));
  EXPECT_EQ(inspection.base.snapshot.at("rev"), snapshot.at("rev"));
}

TEST(HostSaveInspection, NonRevisionFailureIsNotRetriedOrErased) {
  int reads = 0;
  try {
    (void)InspectForSave([&](const auto &, const auto &) -> json::Members {
      ++reads;
      throw Failure(
          "external_change",
          "externals/storage/layout.json differs from the confirmed document");
    });
    FAIL() << "Expected the read failure";
  } catch (const Failure &error) {
    EXPECT_EQ(error.code, "external_change");
    EXPECT_NE(std::string(error.what()).find("externals/storage/layout.json"),
              std::string::npos);
  }
  EXPECT_EQ(reads, 1);
}

TEST(HostSaveInspection, ContinuousWritersProduceAnExplainedBoundedFailure) {
  int reads = 0;
  try {
    (void)InspectForSave([&](const auto &, const auto &) -> json::Members {
      ++reads;
      throw Failure("revision_conflict", "the document is at another revision");
    });
    FAIL() << "Expected the inspection to stop retrying";
  } catch (const Failure &error) {
    EXPECT_EQ(error.code, "revision_conflict");
    EXPECT_NE(std::string(error.what()).find("inspection"), std::string::npos);
  }
  EXPECT_EQ(reads, 8);
}
} // namespace

TEST(HostSaveInspection, UIEditKeepsPlacementAndRetriesTheOriginalBase) {
  const std::string project =
      R"({"canvas":{"cards":[{"id":"a"},{"id":"b"}],"connections":[]}})";
  auto snapshot =
      json::Members{{"rev", json::Quote("7")},
                    {"project_json", json::Quote(project)},
                    {"layout_json", R"("{}")"},
                    {"definitions",
                     R"([{"id":"a","card_json":"{\"template\":\"draft\"}"}])"}};
  const auto read = [&](const auto &, const auto &) { return snapshot; };
  const auto base = CaptureSaveBase(snapshot, read);
  const auto original = json::Members{
      {"base_rev", json::Quote("7")},
      {"changes",
       R"([{"value_type":"PutDefinition","value":{"id":"a","card_json":"{\"template\":\"draft\"}","meta_json":"{\"description\":\"mine\"}"}}])"}};
  // The placement writer saves after the UI reads, before host inspection.
  snapshot["rev"] = json::Quote("8");
  snapshot["layout_json"] = json::Quote(R"({"cards":{"a":{"x":40,"y":80}}})");
  int writes = 0;
  std::string first_operation;
  const auto saved = SaveWithMerge(original, base, read, [&](const auto &args) {
    ++writes;
    if (writes == 1) {
      EXPECT_EQ(json::Text(args, "base_rev"), "8");
      first_operation = json::Text(args, "op_id");
      // Another placement wins the CAS after inspection, too.
      snapshot["rev"] = json::Quote("9");
      snapshot["layout_json"] = json::Quote(
          R"({"cards":{"a":{"x":40,"y":80},"b":{"x":120,"y":80}}})");
      return json::Members{{"reply", json::Quote("Failure")},
                           {"value", R"({"kind":"revision_conflict"})"}};
    }
    EXPECT_EQ(json::Text(args, "base_rev"), "9");
    EXPECT_NE(json::Text(args, "op_id"), first_operation);
    const auto value = json::Object(json::Get(
        json::Object(json::Array(args.at("changes")).at(0)), "value"));
    EXPECT_EQ(json::Text(value, "meta_json"), R"({"description":"mine"})");
    return json::Members{{"reply", json::Quote("SaveResult")}, {"value", "{}"}};
  });
  EXPECT_EQ(writes, 2);
  EXPECT_EQ(json::Text(saved, "reply"), "SaveResult");
  EXPECT_NE(json::Text(snapshot, "layout_json").find("120"), std::string::npos);
  // A real collision never reaches the core, and identifies the field.
  snapshot["definitions"] =
      R"([{"id":"a","card_json":"{\"template\":\"draft\"}","meta_json":"{\"description\":\"theirs\"}"}])";
  try {
    (void)SaveWithMerge(original, base, read,
                        [&](const auto &) -> json::Members {
                          ADD_FAILURE() << "A collision must not be submitted";
                          return {};
                        });
    FAIL() << "Expected a collision";
  } catch (const Failure &error) {
    EXPECT_EQ(error.code, "revision_conflict");
    EXPECT_NE(std::string(error.what()).find("_meta.json"), std::string::npos);
  }
}

TEST(HostSaveInspection, ExternalReadBaseCanPrecedeTheRootSnapshot) {
  const auto path = "externals/inner/layout.json";
  const SaveBase base{
      {{"rev", json::Quote("8")},
       {"sources", R"([{"path":"externals/inner/layout.json","hash":"before"}])"}},
      {{"tsugumi.json", "{}"}, {"layout.json", "{}"},
       {path, R"({"cards":{"a":{"x":0,"y":0}}})"}}};
  const json::Members snapshot{
      {"rev", json::Quote("8")}, {"project_json", json::Quote("{}")},
      {"layout_json", json::Quote("{}")},
      {"sources", R"([{"path":"externals/inner/layout.json","hash":"placed"}])"}};
  const auto original = json::Members{
      {"base_rev", json::Quote("8")},
      {"changes", R"([{"value_type":"PutSource","value":{"path":"externals/inner/layout.json","bytes":"{\"cards\":{\"a\":{\"x\":200,\"y\":0}}}","base_hash":"before"}}])"}};
  int writes = 0;
  (void)SaveWithMerge(original, base,
      [&](const auto& method, const auto&) {
        if (method == "project_inspect") return snapshot;
        return json::Members{{"bytes", json::Quote(R"({"cards":{"a":{"x":0,"y":0},"b":{"x":400,"y":0}}})")}, {"eof", "true"}};
      }, [&](const auto& args) {
        ++writes;
        const auto value = json::Object(json::Get(json::Object(json::Array(args.at("changes")).at(0)), "value"));
        EXPECT_EQ(json::Text(value, "base_hash"), "placed");
        const auto cards = json::Object(json::Object(json::Text(value, "bytes")).at("cards"));
        EXPECT_EQ(json::Get(json::Object(cards.at("a")), "x"), "200");
        EXPECT_EQ(json::Get(json::Object(cards.at("b")), "x"), "400");
        return json::Members{{"reply", json::Quote("SaveResult")}, {"value", "{}"}};
      });
  EXPECT_EQ(writes, 1);
}

TEST(HostSaveInspection, MergedLinkStillSuppliesTheRequiredCardDocument) {
  const json::Members snapshot{{"rev", json::Quote("8")},
      {"project_json", json::Quote("{}")}, {"layout_json", json::Quote("{}")}};
  const SaveBase base{{{"rev", json::Quote("7")}},
      {{"tsugumi.json", "{}"}, {"layout.json", "{}"}}};
  const json::Members original{{"base_rev", json::Quote("7")},
      {"origin", json::Quote("ui")},
      {"changes", R"([{"value_type":"PutDefinition","value":{"id":"inner","card_json":"","link_json":"{\"external\":\"inner\"}"}}])"}};
  (void)SaveWithMerge(original, base,
      [&](const auto&, const auto&) { return snapshot; },
      [&](const auto& args) {
        const auto value = json::Object(json::Get(json::Object(json::Array(args.at("changes")).at(0)), "value"));
        EXPECT_TRUE(value.contains("card_json"));
        EXPECT_EQ(json::Text(value, "card_json"), "");
        EXPECT_EQ(json::Text(value, "link_json"), R"({"external":"inner"})");
        EXPECT_NO_THROW((void)agent::EncodeProjectRequest("project_save", json::Encode(args), 1));
        return json::Members{{"reply", json::Quote("SaveResult")}, {"value", "{}"}};
      });
}
} // namespace tsugumi::host
