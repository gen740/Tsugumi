#include <gtest/gtest.h>

#include "store.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"

TEST(GraphDefinition, CodeCardSelectsItsImplementationInNix) {
  namespace ts = tsugumi;
  const auto root = std::filesystem::temp_directory_path() /
                    ("tsugumi-language-" + ts::crypto::uuid4());
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::filesystem::remove_all(path); }
  } cleanup{root};
  ts::fs::write_atomic(
      root / "tsugumi.json",
      R"({"name":"test","pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"card"}],"connections":[]}})");
  ts::fs::write_atomic(root / "definitions/card/default.nix",
                       "{pkgs,inputs,params}:{run=pkgs.emptyDirectory;}");
  ts::fs::write_atomic(root / "definitions/card/_outputs.fbs",
                       "table Outputs {} root_type Outputs;");
  ts::fs::write_atomic(root / "definitions/card/_card.json", R"({"type":"code"})");
  ts::Store store({root, ""});
  store.open();
  const auto& node = store.current()->resolution->nodes.at(0);
  EXPECT_TRUE(node.exec_diags.empty());
}
