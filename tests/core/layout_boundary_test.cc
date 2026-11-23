#include <gtest/gtest.h>

#include "store.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"

namespace tsugumi {
namespace {
TEST(LayoutBoundary, OnlyExternalProjectLayoutIsExcludedFromExecutionIdentity) {
  Document doc;
  doc.project_json = initial_project_json("test");
  doc.project_buffer = parse_project_json(doc.project_json).buffer;
  const auto fingerprint = [&] { return execution_fingerprint(doc); };
  const auto empty = fingerprint();
  doc.layout = "presentation";
  doc.sources["externals/child/layout.bin"] = {"one", 1};
  doc.sources["externals/child/externals/nested/layout.bin"] = {"two", 2};
  EXPECT_EQ(fingerprint(), empty);
  doc.sources["externals/child/layout.bin"].hash = "three";
  EXPECT_EQ(fingerprint(), empty);
  for (const auto* path : {"definitions/a/src/layout.bin",
                           "externals/child/definitions/a/src/layout.bin"}) {
    doc.sources[path] = {"input", 5};
    EXPECT_NE(fingerprint(), empty) << path;
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
                   R"({"type":"draft"})");
  const auto layout = document_text::parse(
      document_text::Schema::Layout, "layout.bin",
      R"({"cards":[{"key":"a","x":12,"y":8}]})");
  fs::write_atomic(root / "layout.bin", layout);
  Store store({root, ""});
  store.open();
  ChangeIn change{};
  change.kind = ChangeIn::Kind::SetProject;
  change.json = initial_project_json("test");
  const auto staged = store.stage({change}, crypto::uuid4(), "test");
  EXPECT_EQ(staged.doc->layout, layout);
  EXPECT_FALSE(staged.delta.layout_changed);
}
}  // namespace
}  // namespace tsugumi
