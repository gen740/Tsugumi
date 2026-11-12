#include <gtest/gtest.h>

#include "remote_invocation.hh"

namespace tsugumi {

TEST(RemoteResult, ManifestObjectKeysCanArriveInAnyOrder) {
  const std::string cases[] = {
      R"({"invocation_result_sha256":"worker","outputs":[{"path":"out.txt","sha256":"file","size":42}]})",
      R"({"outputs":[{"size":42,"sha256":"file","path":"out.txt"}],"invocation_result_sha256":"worker"})",
  };
  for (const auto& text : cases) {
    remote::ResultJson manifest;
    ASSERT_FALSE(schema::decode(text, manifest));
    const auto* worker = schema::get(manifest.invocation_result_sha256);
    ASSERT_NE(worker, nullptr);
    EXPECT_EQ(*worker, "worker");
    const auto* outputs = schema::get(manifest.outputs);
    ASSERT_NE(outputs, nullptr);
    ASSERT_EQ(outputs->size(), 1u);
    const auto& file = outputs->front();
    ASSERT_NE(schema::get(file.path), nullptr);
    ASSERT_NE(schema::get(file.sha256), nullptr);
    ASSERT_NE(schema::get(file.size), nullptr);
    EXPECT_EQ(*schema::get(file.path), "out.txt");
    EXPECT_EQ(*schema::get(file.sha256), "file");
    EXPECT_EQ(*schema::get(file.size), 42u);
  }
}

}  // namespace tsugumi
