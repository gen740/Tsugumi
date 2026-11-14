#include <gtest/gtest.h>

#include "store.hh"
#include "util/crypto.hh"
#include "util/fs.hh"

namespace tsugumi {
namespace {
TEST(LayoutBoundary, CoreAcceptsOpaqueObjectsButRejectsOtherJsonAndOversize) {
  EXPECT_NO_THROW(check_opaque_layout(
      R"({"cards":"a client extension","future":{"x":true}})"));
  for (const auto* value : {"[]", "null", "4", "invalid"})
    EXPECT_THROW(check_opaque_layout(value), DocumentError);
  EXPECT_THROW(check_opaque_layout("{\"x\":\"" +
                                   std::string(8 * 1024 * 1024, 'x') + "\"}"),
               DocumentError);
}

TEST(LayoutBoundary, OnlyExternalProjectLayoutIsExcludedFromExecutionIdentity) {
  Document doc;
  doc.project_json = "{}";
  const auto empty = execution_fingerprint(doc);
  doc.layout_json = R"({"future":true})";
  doc.sources["externals/child/layout.json"] = {"one", 1};
  doc.sources["externals/child/externals/nested/layout.json"] = {"two", 2};
  EXPECT_EQ(execution_fingerprint(doc), empty);
  doc.sources["externals/child/layout.json"].hash = "three";
  EXPECT_EQ(execution_fingerprint(doc), empty);
  for (const auto* path : {"definitions/a/src/layout.json",
                           "externals/child/definitions/a/src/layout.json"}) {
    doc.sources[path] = {"input", 5};
    EXPECT_NE(execution_fingerprint(doc), empty) << path;
    doc.sources.erase(path);
  }
}

TEST(LayoutBoundary, DeletingACardDoesNotInterpretOrRewriteLayout) {
  auto root = std::filesystem::temp_directory_path() /
              ("layout-boundary-" + crypto::uuid4());
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{root};
  fs::write_atomic(
      root / "tsugumi.json",
      R"({"name":"test","pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"a"}],"connections":[]}})");
  fs::write_atomic(root / "definitions/a/_card.json",
                   R"({"template":"draft"})");
  const std::string layout = R"({ "cards": {"a": "opaque"}, "extension": 1 })";
  fs::write_atomic(root / "layout.json", layout);
  Store store({root, ""});
  store.open();
  ChangeIn change{};
  change.kind = ChangeIn::Kind::SetProject;
  change.json =
      R"({"name":"test","pkgsPath":"tsugumi:default","canvas":{"cards":[],"connections":[]}})";
  const auto staged = store.stage({change}, crypto::uuid4(), "test");
  EXPECT_EQ(staged.doc->layout_json, layout);
  EXPECT_FALSE(staged.delta.layout_changed);
}
}  // namespace
}  // namespace tsugumi
