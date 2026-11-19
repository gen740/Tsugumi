#include "host/attached_folder.hh"

#include <gtest/gtest.h>
#include <sys/stat.h>

#include <fstream>

#include "tsugumi/util/fs.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::host {
namespace {
namespace json = ::tsugumi::json;
class AttachedFolderTest : public testing::Test {
 protected:
  std::filesystem::path root;
  auto SetUp() -> void override {
    auto pattern =
        (std::filesystem::temp_directory_path() / "tsugumi-folder-XXXXXX")
            .string();
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    root = std::filesystem::canonical(pattern);
    fs::write_atomic(root / "note.txt", "日本語のメモ");
    std::filesystem::create_directory(root / "sub");
    fs::write_atomic(root / "sub/nested.txt", "nested");
  }
  auto TearDown() -> void override { std::filesystem::remove_all(root); }
  auto Read(const json::Members& args) -> json::Members {
    return json::Object(
        json::Get(json::Object(ReadAttachedFolder(root, args)), "value"));
  }
};
TEST_F(AttachedFolderTest, ListsAndPagesHostFilesWithoutAProjectEnvironment) {
  EXPECT_EQ(json::Text(Read({{"path", "\"\""}}), "bytes"), "note.txt\nsub/\n");
  const auto page = Read({{"path", "\"sub/nested.txt\""},
                          {"offset", "\"1\""},
                          {"limit_bytes", "3"}});
  EXPECT_EQ(json::Text(page, "bytes"), "est");
  EXPECT_EQ(json::Text(page, "size_bytes"), "6");
  EXPECT_EQ(json::Text(page, "next_offset"), "4");
  EXPECT_EQ(json::Get(page, "eof"), "false");
  EXPECT_EQ(json::Text(Read({{"path", "\"note.txt\""}}), "bytes"),
            "日本語のメモ");
  EXPECT_EQ(
      json::Get(Read({{"path", "\"note.txt\""}, {"offset", "\"999\""}}), "eof"),
      "true");
}
TEST_F(AttachedFolderTest, RejectsTraversalAndSymlinksAtEveryComponent) {
  std::filesystem::create_symlink(root / "note.txt", root / "file-link");
  std::filesystem::create_directory_symlink(root / "sub",
                                            root / "directory-link");
  for (const auto* path :
       {"../note.txt", "/etc/passwd", "file-link", "directory-link/nested.txt"})
    EXPECT_THROW(Read({{"path", json::Quote(path)}}), FolderReadFailure)
        << path;
}
TEST_F(AttachedFolderTest, BoundsFilesAndPageSizesAndRejectsSpecialFiles) {
  std::filesystem::resize_file(root / "note.txt", 200u << 20);
  const auto page = Read({{"path", "\"note.txt\""}});
  EXPECT_EQ(json::Text(page, "size_bytes"), std::to_string(200u << 20));
  EXPECT_NE(json::Get(page, "bytes").find("base64"), std::string::npos);
  EXPECT_EQ(json::Text(page, "next_offset"), "262144");
  ASSERT_EQ(::mkfifo((root / "fifo").c_str(), 0600), 0);
  EXPECT_THROW(Read({{"path", "\"fifo\""}}), FolderReadFailure);
  EXPECT_THROW(Read({{"path", "\"\""}, {"limit_bytes", "1048577"}}),
               FolderReadFailure);
  EXPECT_THROW(Read({{"path", "\"\""}, {"limit_bytes", "0"}}),
               FolderReadFailure);
  EXPECT_THROW(Read({{"path", "\"\""}, {"offset", "\"-1\""}}),
               FolderReadFailure);
  EXPECT_THROW(Read({{"path", "\"\""}, {"offset", "\"18446744073709551616\""}}),
               FolderReadFailure);
}
}  // namespace
}  // namespace tsugumi::host
