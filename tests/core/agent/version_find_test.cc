// version_find composes recorded history reads; these cases answer them from
// fixed replies, without sockets.
#include "agent/version_find.hh"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <vector>

#include "agent/catalog.hh"
#include "host/failure.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::agent {
namespace {

// Newest first: c3 changes bg's parameters, c2 only peaks, c1 bg's source.
constexpr auto kGraph = R"({"enabled":true,"commits":[
  {"id":"c3","parents":["c2"],"message":"Lower the background","changed":["bg"],
   "runs":[{"run_id":"r3","state":"succeeded","ended":"2026-01-04T00:00:00Z"}]},
  {"id":"c2","parents":["c1"],"changed":["peaks"],
   "runs":[{"run_id":"r2a","state":"failed","ended":"2026-01-03T01:00:00Z"},
           {"run_id":"r2b","state":"succeeded","ended":"2026-01-03T00:00:00Z"}]},
  {"id":"c1","parents":["c0"],"changed":["bg"],
   "runs":[{"run_id":"r1","state":"succeeded","ended":"2026-01-02T00:00:00Z"}]},
  {"id":"c0","changed":[""],
   "runs":[{"run_id":"r0","state":"succeeded","ended":"2026-01-01T00:00:00Z"}]}]})";

auto Record(const std::string& invocation, const std::string& outputs)
    -> std::string {
  return R"({"exists":true,"record":{"summary":{"invocation":")" +
         invocation + R"(","state":"succeeded"},"outputs":)" + outputs + "}}";
}

struct Fake {
  std::map<std::string, std::string> notes{
      {"c3", R"({"changes":{"changed":[{"card":"bg","kinds":["parameters"]}]}})"},
      {"c1", R"({"changes":{"changed":[{"card":"bg","kinds":["source"]}]}})"}};
  std::map<std::string, std::string> records{
      {"r3", Record("1", R"([{"name":"peaks","root_type":"Peaks","content_hash":"B"},{"name":"plot","root_type":"tsugumi.Artifact","artifacts":[{"id":"bb"}]}])")},
      {"r2b", Record("2", R"([{"name":"peaks","root_type":"Peaks","content_hash":"A"},{"name":"plot","root_type":"tsugumi.Artifact","artifacts":[{"id":"aa"}]}])")},
      {"r2a", R"({"exists":false})"},
      {"r1", Record("1", R"([{"name":"peaks","root_type":"Peaks","content_hash":"A"}])")}};
  std::vector<std::string> calls;

  auto Reader() -> ProjectRead {
    return [this](const std::string& name, const json::Members& args) {
      calls.push_back(name);
      if (name == "version_graph") return json::Object(kGraph);
      if (name == "version_note") {
        const auto found = notes.find(json::Text(args, "commit"));
        if (found == notes.end())
          throw host::Failure("not_found", "no run definition changes");
        return json::Object(found->second);
      }
      EXPECT_EQ(name, "run_record");
      EXPECT_EQ(json::Get(args, "card"), R"({"segments":["peaks"]})");
      const auto found = records.find(json::Text(args, "run_id"));
      if (found == records.end())
        throw host::Failure("not_found", "Run not found");
      return json::Object(found->second);
    };
  }
};

auto Candidates(const std::string& result) -> std::vector<json::Members> {
  std::vector<json::Members> out;
  for (const auto& raw : json::Array(json::Object(result).at("candidates")))
    out.push_back(json::Object(raw));
  return out;
}

TEST(VersionFind, ComparesEachChangeWithTheRunBeforeIt) {
  Fake fake;
  const auto result = FindVersions(
      json::Object(R"({"card":"bg","observe":"peaks"})"), fake.Reader());
  const auto candidates = Candidates(result);
  ASSERT_EQ(candidates.size(), 2u) << result;

  const auto& latest = candidates[0];
  EXPECT_EQ(json::Text(latest, "commit"), "c3");
  EXPECT_EQ(json::Get(latest, "changes"), R"(["parameters"])");
  // The newest run of the parent that recorded peaks; r2a did not run it.
  const auto before = json::Object(latest.at("before"));
  EXPECT_EQ(json::Text(before, "run_id"), "r2b");
  EXPECT_EQ(json::Text(before, "link"), "run:r2b");
  const auto run = json::Object(json::Array(latest.at("runs")).at(0));
  EXPECT_EQ(json::Text(run, "link"), "run:r3");
  const auto observed = json::Object(run.at("observed"));
  EXPECT_EQ(json::Text(observed, "link"), "run:r3/peaks#1");
  const auto compared = json::Array(observed.at("compared"));
  ASSERT_EQ(compared.size(), 2u);
  EXPECT_EQ(json::Text(json::Object(compared[0]), "port"), "peaks");
  EXPECT_EQ(json::Text(json::Object(compared[0]), "result"), "changed");
  // An artifact id alone provides no content hash to compare.
  EXPECT_EQ(json::Text(json::Object(compared[1]), "result"), "unknown");

  // c0 kept no record: the earliest change has nothing to compare with, and
  // says so.
  const auto& first = candidates[1];
  EXPECT_EQ(json::Text(first, "commit"), "c1");
  EXPECT_FALSE(first.contains("before"));
  EXPECT_NE(json::Get(first, "unverified").find("nothing to compare"),
            std::string::npos);
  const auto first_run = json::Object(
      json::Object(json::Array(first.at("runs")).at(0)).at("observed"));
  EXPECT_FALSE(first_run.contains("compared"));
  EXPECT_EQ(json::Get(first_run, "outputs"),
            R"([{"hash":"A","port":"peaks","type":"Peaks"}])");
}

TEST(VersionFind, KindsAndLimitNarrowTheCandidates) {
  Fake fake;
  const auto result = FindVersions(
      json::Object(R"({"card":"bg","observe":"peaks","kinds":["source"]})"),
      fake.Reader());
  const auto candidates = Candidates(result);
  ASSERT_EQ(candidates.size(), 1u) << result;
  EXPECT_EQ(json::Text(candidates[0], "commit"), "c1");
  // Each run is read at most once.
  EXPECT_EQ(std::count(fake.calls.begin(), fake.calls.end(), "run_record"),
            2);

  // limit counts versions; the rest are announced, not dropped silently.
  const auto newest = json::Object(FindVersions(
      json::Object(R"({"card":"bg","observe":"peaks","limit":1})"),
      fake.Reader()));
  EXPECT_EQ(json::Array(newest.at("candidates")).size(), 1u);
  EXPECT_EQ(json::Get(newest, "more_candidates"), "true");
}

TEST(VersionFind, NoHistoryMeansNothingToFind) {
  const auto result = FindVersions(
      json::Object(R"({"card":"bg"})"),
      [](const std::string&, const json::Members&) {
        return json::Object(R"({"enabled":false})");
      });
  EXPECT_EQ(json::Object(result).at("candidates"), "[]");
  EXPECT_TRUE(json::Object(result).contains("unverified"));
}

TEST(VersionFind, IsAReadOnlyVersionsAction) {
  const Catalog catalog;
  const auto* tool = catalog.Find("version_find");
  ASSERT_NE(tool, nullptr);
  EXPECT_FALSE(tool->mutating);
  bool found = false;
  for (const auto& spec : catalog.FamilySpecs("versions", ModelRole::Main))
    found = found || spec.name == "find";
  EXPECT_TRUE(found);
}

}  // namespace
}  // namespace tsugumi::agent
