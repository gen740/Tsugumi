#include <gtest/gtest.h>

#include "model.hh"
#include "wire.hh"

namespace tsugumi {
namespace {
TEST(DisplayModule, RetainedSnapshotRoundTripsNamedBinaryBlobs) {
  DisplayData input;
  input.card = {"cloud"};
  input.run_id = "00000000-0000-4000-8000-000000000001";
  input.invocation = 3;
  input.revision = 7;
  const std::string bytes("\0\xff\0\x80", 4);
  input.value = DisplayData::Module{"/nix/store/00000000000000000000000000000000-point-cloud-view",
                                    "{\"count\":1}", {{"points", bytes}, {"empty", ""}}};
  auto decoded = wire::standalone_display(wire::standalone_display(input));
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->run_id, input.run_id);
  EXPECT_EQ(decoded->invocation, 3u);
  EXPECT_EQ(decoded->revision, 7u);
  const auto& module = std::get<DisplayData::Module>(decoded->value);
  EXPECT_EQ(module.module, std::get<DisplayData::Module>(input.value).module);
  EXPECT_EQ(module.config_json, "{\"count\":1}");
  EXPECT_EQ(module.blobs.at("points"), bytes);
  EXPECT_TRUE(module.blobs.at("empty").empty());
}
TEST(DisplayModule, RejectsInvalidIdentityAndOversizedInput) {
  DisplayData data;
  data.value = DisplayData::Module{"plotly", "{}", {}};
  EXPECT_TRUE(data.validate());
  data.value = DisplayData::Module{"/nix/store/00000000000000000000000000000000-view", "{", {}};
  EXPECT_TRUE(data.validate());
  data.value = DisplayData::Module{"/nix/store/00000000000000000000000000000000-view", "{}",
                                   {{"points", std::string((8u << 20) + 1, '\0')}}};
  EXPECT_TRUE(data.validate());
}
}  // namespace
}  // namespace tsugumi
