#include "store.hh"
#include "util/crypto.hh"
#include "util/fs.hh"
#include <gtest/gtest.h>

namespace tsugumi {
namespace {
class OpaqueMetadata : public ::testing::Test {
protected:
  std::filesystem::path root = std::filesystem::temp_directory_path() /
                               ("opaque-meta-" + crypto::uuid4());
  auto SetUp() -> void override {
    fs::write_atomic(
        root / "tsugumi.json",
        R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"start"}],"connections":[]}})");
    fs::write_atomic(root / "definitions/start/_card.json",
                     R"({"template":"trigger"})");
    fs::write_atomic(root / "layout.json", "{}");
  }
  auto TearDown() -> void override { std::filesystem::remove_all(root); }
  auto commit(Store &store, std::vector<ChangeIn> changes) -> DocumentPtr {
    auto staged = store.stage(changes, crypto::uuid4(), "agent");
    store.commit(staged, "save", "digest",
                 [](const Delta *) { return "receipt"; });
    return store.current();
  }
};
TEST_F(OpaqueMetadata,
       UnknownAndMalformedPresentationFieldsSurviveLoadSaveAndResolution) {
  const std::string meta =
      R"({"name":[],"description":false,"comments":"not a list","outputs":false,"future":{"x":1}})";
  fs::write_atomic(root / "definitions/start/_meta.json", meta);
  Store store({root, ""});
  store.open();
  auto before = store.current();
  ASSERT_EQ(before->definitions.at("start").meta_json, meta);
  ASSERT_TRUE(before->resolution->nodes.front().diags.empty());
  ChangeIn put{};
  put.kind = ChangeIn::Kind::PutDefinition;
  put.id = "start";
  put.json = R"({"template":"trigger","displayMode":{"future":true}})";
  put.meta_json = R"({"comments":[false],"unknown":12})";
  auto after = commit(store, {put});
  EXPECT_EQ(after->execution_rev, before->execution_rev);
  EXPECT_EQ(after->execution_fingerprint, before->execution_fingerprint);
  EXPECT_EQ(after->definitions.at("start").meta_json, put.meta_json);
  EXPECT_TRUE(after->resolution->nodes.front().diags.empty());
  EXPECT_TRUE(after->resolution->nodes.front().exec_diags.empty());
}
TEST_F(OpaqueMetadata, ProjectNameIsOptionalOpaqueAndDoesNotChangeExecution) {
  Store store({root, ""});
  store.open();
  auto before = store.current();
  ChangeIn project{};
  project.kind = ChangeIn::Kind::SetProject;
  project.json =
      R"({"name":{"future":"anything"},"pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"start"}],"connections":[]}})";
  auto after = commit(store, {project});
  EXPECT_EQ(after->execution_rev, before->execution_rev);
  EXPECT_EQ(after->execution_fingerprint, before->execution_fingerprint);
  EXPECT_EQ(after->project_json, project.json);
  ChangeIn card{};
  card.kind = ChangeIn::Kind::PutDefinition;
  card.id = "start";
  card.json = R"({"template":"trigger","maxRuns":2})";
  after = commit(store, {card});
  EXPECT_NE(after->execution_fingerprint, before->execution_fingerprint);
  EXPECT_GT(after->execution_rev, before->execution_rev);
}
TEST_F(OpaqueMetadata,
       ClientReformattingAlongWithPresentationKeepsTheSameFingerprint) {
  Store store({root, ""});
  store.open();
  auto before = store.current();
  ChangeIn project{};
  project.kind = ChangeIn::Kind::SetProject;
  project.json =
      R"({ "canvas": { "connections": [ ], "cards": [ { "id": "start" } ] }, "name": "Renamed", "pkgsPath": "tsugumi:default" })";
  ChangeIn card{};
  card.kind = ChangeIn::Kind::PutDefinition;
  card.id = "start";
  card.json = R"({"displayMode":false, "template": "trigger"})";
  auto after = commit(store, {project, card});
  EXPECT_EQ(after->execution_fingerprint, before->execution_fingerprint);
  EXPECT_EQ(after->execution_rev, before->execution_rev);
}
TEST_F(OpaqueMetadata, MetadataMustStillBeABoundedJsonObject) {
  for (const auto &bad : {"[]", "null", "{", "{\"x\":NaN}"})
    EXPECT_THROW(check_meta_json(bad), DocumentError);
  EXPECT_THROW(
      check_meta_json("{\"x\":\"" + std::string(1u << 20, 'a') + "\"}"),
      DocumentError);
}
TEST_F(OpaqueMetadata, ExternalPresentationUsesTheSameBoundaryAndPendingBytes) {
  fs::write_atomic(
      root / "tsugumi.json",
      R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"outer"}],"connections":[]}})");
  std::filesystem::remove_all(root / "definitions/start");
  fs::write_atomic(root / "definitions/outer/_link.json",
                   R"({"external":"library"})");
  const std::string project =
      R"({"canvas":{"cards":[{"id":"start"}],"connections":[]},"boundary":{"inputs":{},"outputs":{}}})";
  fs::write_atomic(root / "externals/library/tsugumi.json", project);
  fs::write_atomic(root / "externals/library/definitions/start/_card.json",
                   R"({"template":"trigger"})");
  fs::write_atomic(root / "externals/library/definitions/start/_meta.json",
                   R"({"name":false,"outputs":[false],"unknown":1})");
  Store store({root, ""});
  store.open();
  const auto before = store.current();
  auto put = [&](const std::string &path, const std::string &bytes) {
    ChangeIn c{};
    c.kind = ChangeIn::Kind::PutSource;
    c.path = path;
    c.bytes = bytes;
    c.base_hash = store.current()->sources.at(path).hash;
    return c;
  };
  auto after = commit(
      store,
      {put("externals/library/tsugumi.json",
           R"({"name":false,"canvas":{"cards":[{"id":"start"}],"connections":[]},"boundary":{"inputs":{},"outputs":{}}})"),
       put("externals/library/definitions/start/_card.json",
           R"({"template":"trigger","displayMode":false})"),
       put("externals/library/definitions/start/_meta.json",
           R"({"future":12,"comments":false})")});
  EXPECT_EQ(after->execution_fingerprint, before->execution_fingerprint);
  EXPECT_EQ(after->execution_rev, before->execution_rev);
  ASSERT_NE(after->resolution->find({"outer", "start"}), nullptr);
  EXPECT_TRUE(after->resolution->find({"outer", "start"})->diags.empty());
  after = commit(store, {put("externals/library/definitions/start/_card.json",
                             R"({"template":"trigger","maxRuns":2})")});
  EXPECT_NE(after->execution_fingerprint, before->execution_fingerprint);
}
TEST_F(OpaqueMetadata, DraftPortsIgnoreUnrelatedMalformedFields) {
  fs::write_atomic(root / "definitions/start/_card.json",
                   R"({"template":"draft"})");
  fs::write_atomic(
      root / "definitions/start/_meta.json",
      R"({"name":false,"comments":12,"outputs":[{"name":"planned","description":false,"future":1},false]})");
  Store store({root, ""});
  store.open();
  ASSERT_NE(store.current()->resolution->find({"start"})->output("planned"),
            nullptr);
}
} // namespace
} // namespace tsugumi
