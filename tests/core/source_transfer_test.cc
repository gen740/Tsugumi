#include <gtest/gtest.h>

#include <fstream>

#include "store.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"

namespace tsugumi {
namespace {
TEST(SourceTransfer, StreamsFixedChunksAndDiscardsAnInterruptedImport) {
  const auto root = std::filesystem::temp_directory_path() /
                    ("source-transfer-" + crypto::uuid4());
  struct Cleanup {
    std::filesystem::path root;
    ~Cleanup() { std::filesystem::remove_all(root); }
  } cleanup{root};
  fs::write_atomic(
      root / "tsugumi.json",
      R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[],"connections":[]}})");
  fs::write_atomic(root / "definitions/data/_card.json",
                   R"({"template":"draft"})");
  constexpr uint64_t size = 80u << 20;
  const std::string chunk(65536, 'x');
  crypto::Sha256 digest;
  {
    std::ofstream output(root / "input.csv", std::ios::binary);
    for (uint64_t n = 0; n < size; n += chunk.size()) {
      output.write(chunk.data(), chunk.size());
      digest.update(chunk);
    }
  }
  Store store({root, ""});
  store.open();
  size_t chunks = 0;
  EXPECT_THROW(store.stage_file(root, "input.csv", std::nullopt,
                                [&] {
                                  if (++chunks == 65)
                                    throw std::runtime_error(
                                        "interrupted during transfer");
                                }),
               std::runtime_error);
  EXPECT_EQ(chunks, 65u);
  EXPECT_TRUE(std::filesystem::is_empty(root / ".tsugumi/document-files"));
  EXPECT_FALSE(std::filesystem::exists(root / "definitions/data/input.csv"));
  EXPECT_EQ(store.current()->rev, 0u);
  ChangeIn change{};
  change.kind = ChangeIn::Kind::PutSource;
  change.path = "definitions/data/input.csv";
  change.file = store.stage_file(root, "input.csv", std::nullopt, [] {});
  EXPECT_EQ(change.file->size, size);
  EXPECT_EQ(change.file->hash, digest.finish_hex());
  EXPECT_TRUE(change.bytes.empty());
  auto staged = store.stage({change}, crypto::uuid4(), "agent");
  ASSERT_EQ(staged.ops.size(), 1u);
  EXPECT_FALSE(staged.ops.front().content);
  store.commit(staged, "save", "digest",
               [](const Delta*) { return "receipt"; });
  EXPECT_EQ(fs::sha256_file(root / change.path), change.file->hash);
  EXPECT_LT(std::filesystem::file_size(root / ".tsugumi/document.journal"),
            4096u);
  change.path = "definitions/data/cancelled.csv";
  auto cancelled = store.stage({change}, crypto::uuid4(), "agent");
  const auto journal_size = std::filesystem::file_size(root / ".tsugumi/document.journal");
  size_t checks = 0;
  EXPECT_THROW(store.commit(cancelled, "save", "cancelled",
      [](const Delta*) { return "cancelled receipt"; }, [&] {
        if (++checks == 2) throw std::runtime_error("cancelled before durable intent");
      }), std::runtime_error);
  EXPECT_EQ(checks, 2u);
  EXPECT_EQ(std::filesystem::file_size(root / ".tsugumi/document.journal"), journal_size);
  EXPECT_EQ(store.current()->rev, 1u);
  EXPECT_FALSE(std::filesystem::exists(root / change.path));
  EXPECT_FALSE(change.file->journaled);
}
}  // namespace
}  // namespace tsugumi
