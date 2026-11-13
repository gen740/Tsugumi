#include <gtest/gtest.h>

#include "../agent_fixture_server.hh"
#include "host/skills.hh"

namespace tsugumi::test {
TEST(HostSkillRead,
     UnindexedFilesAreBoundedAndSymlinksStayWithinTheGrantedRoot) {
  TemporaryDirectory directory;
  const auto root = directory.path / "project";
  Write(root / "custom/skill.md", "skill outside the project document index");
  EXPECT_EQ(host::ReadSkill(root, root / "custom/skill.md"),
            "skill outside the project document index");
  std::filesystem::create_symlink(root / "custom/skill.md", root / "alias.md");
  EXPECT_EQ(host::ReadSkill(root, root / "alias.md"),
            "skill outside the project document index");
  Write(directory.path / "private.md", "outside the grant");
  std::filesystem::create_symlink(directory.path / "private.md",
                                  root / "escape.md");
  EXPECT_THROW(host::ReadSkill(root, root / "escape.md"), std::runtime_error);
  Write(root / "boundary.md", std::string(512 * 1024 - 1, 'x'));
  EXPECT_EQ(host::ReadSkill(root, root / "boundary.md").size(), 512 * 1024 - 1);
  Write(root / "boundary.md", std::string(512 * 1024, 'x'));
  EXPECT_THROW(host::ReadSkill(root, root / "boundary.md"), std::runtime_error);
}
}  // namespace tsugumi::test
