#include <gtest/gtest.h>

#include "tsugumi/workerd/protocol.hh"

TEST(RemoteResult, GeneratedManifestRoundtrip) {
  namespace R = Tsugumi::RemoteWorker;
  R::ResultManifestT manifest;
  manifest.invocation_result_sha256 = "worker";
  auto file = std::make_unique<R::ResultFileT>();
  file->path = "out.txt";
  file->sha256 = "file";
  file->size = 42;
  manifest.outputs.push_back(std::move(file));
  flatbuffers::FlatBufferBuilder builder;
  builder.Finish(R::ResultManifest::Pack(builder, &manifest));
  flatbuffers::Verifier v(builder.GetBufferPointer(), builder.GetSize());
  ASSERT_TRUE(v.VerifyBuffer<R::ResultManifest>(nullptr));
  const auto* result =
      flatbuffers::GetRoot<R::ResultManifest>(builder.GetBufferPointer());
  EXPECT_EQ(result->invocation_result_sha256()->str(), "worker");
  ASSERT_EQ(result->outputs()->size(), 1u);
  EXPECT_EQ(result->outputs()->Get(0)->path()->str(), "out.txt");
  EXPECT_EQ(result->outputs()->Get(0)->size(), 42u);
}
