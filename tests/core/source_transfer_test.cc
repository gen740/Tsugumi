#include <gtest/gtest.h>

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
  std::string bytes(128 * 1024 + 1, 'x');
  bytes.back() = 'z';
  fs::write_atomic(root / "input.csv", bytes);
  Store store({root, ""});
  store.open();
  size_t chunks = 0;
  EXPECT_THROW(store.stage_file(root, "input.csv", std::nullopt,
                                [&] {
                                  if (++chunks == 2)
                                    throw std::runtime_error(
                                        "interrupted during transfer");
                                }),
               std::runtime_error);
  EXPECT_TRUE(std::filesystem::is_empty(root / ".tsugumi/document-files"));
  EXPECT_FALSE(std::filesystem::exists(root / "definitions/data/input.csv"));
  EXPECT_EQ(store.current()->rev, 0u);
  ChangeIn change{};
  change.kind = ChangeIn::Kind::PutSource;
  change.path = "definitions/data/input.csv";
  change.file = store.stage_file(root, "input.csv", std::nullopt, [] {});
  EXPECT_EQ(change.file->size, bytes.size());
  EXPECT_EQ(change.file->hash, crypto::sha256_hex(bytes));
  auto staged = store.stage({change}, crypto::uuid4(), "agent");
  store.commit(staged, "save", "digest",
               [](const Delta*) { return "receipt"; });
  EXPECT_EQ(fs::read_file_or_throw(root / change.path), bytes);
  const auto receipt = store.receipt(staged.delta.op_id);
  ASSERT_TRUE(receipt);
  EXPECT_EQ(receipt->bytes, "receipt");
  change.path = "definitions/data/cancelled.csv";
  auto cancelled = store.stage({change}, crypto::uuid4(), "agent");
  size_t checks = 0;
  EXPECT_THROW(
      store.commit(
          cancelled, "save", "cancelled",
          [](const Delta*) { return "cancelled receipt"; },
          [&] {
            if (++checks == 2)
              throw std::runtime_error("cancelled before durable intent");
          }),
      std::runtime_error);
  EXPECT_FALSE(store.receipt(cancelled.delta.op_id));
  EXPECT_EQ(store.current()->rev, 1u);
  EXPECT_FALSE(std::filesystem::exists(root / change.path));
}
}  // namespace
}  // namespace tsugumi
