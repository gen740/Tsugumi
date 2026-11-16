#include <gtest/gtest.h>

#include "host/failure.hh"
#include "host/save.hh"

namespace tsugumi::host {
namespace {
namespace json = agent::json;

TEST(HostSaveInspection, WriterBetweenSnapshotAndExternalReadsRestartsCapture) {
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
          // Return one page before another client's edit commits. The next
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
} // namespace tsugumi::host
