#include <gtest/gtest.h>

#include "model.hh"
#include "tsugumi/workerd/display.hh"
#include "wire.hh"

namespace tsugumi {
namespace {
auto module_display(const std::string& identity, const std::string& config,
                    const std::map<std::string, std::string>& blobs)
    -> std::string {
  namespace D = Tsugumi::Data;
  flatbuffers::FlatBufferBuilder b;
  std::vector<flatbuffers::Offset<D::DisplayBlob>> values;
  for (const auto& [name, bytes] : blobs)
    values.push_back(D::CreateDisplayBlob(
        b, b.CreateString(name),
        b.CreateVector(reinterpret_cast<const uint8_t*>(bytes.data()),
                       bytes.size())));
  const auto module =
      D::CreateDisplayModule(b, b.CreateString(identity),
                             b.CreateString(config), b.CreateVector(values));
  const auto card = D::CreateCardPath(
      b, b.CreateVectorOfStrings(std::vector<std::string>{"cloud"}));
  const auto run = b.CreateString("00000000-0000-4000-8000-000000000001");
  b.Finish(D::CreateDisplay(b, card, run, 3, 7, D::DisplayValue::DisplayModule,
                            module.Union()));
  return {reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize()};
}
TEST(DisplayModule, RetainedSnapshotRoundTripsNamedBinaryBlobs) {
  const std::string identity =
      "/nix/store/00000000000000000000000000000000-point-cloud-view";
  const std::string bytes("\0\xff\0\x80", 4);
  const auto input = module_display(identity, "{\"count\":1}",
                                    {{"points", bytes}, {"empty", ""}});
  const auto decoded = wire::standalone_display(input);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->run_id, "00000000-0000-4000-8000-000000000001");
  EXPECT_EQ(decoded->invocation, 3u);
  EXPECT_EQ(decoded->revision, 7u);
  const auto encoded = wire::standalone_display(*decoded);
  const auto* module =
      flatbuffers::GetRoot<Tsugumi::Data::Display>(encoded.data())
          ->value_as_DisplayModule();
  ASSERT_NE(module, nullptr);
  EXPECT_EQ(module->module_()->str(), identity);
  EXPECT_EQ(module->config_json()->str(), "{\"count\":1}");
  ASSERT_EQ(module->blobs()->size(), 2u);
  EXPECT_EQ(module->blobs()->Get(1)->name()->str(), "points");
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(
                            module->blobs()->Get(1)->data()->data()),
                        module->blobs()->Get(1)->data()->size()),
            bytes);
  EXPECT_EQ(module->blobs()->Get(0)->data()->size(), 0u);
}
TEST(DisplayModule, WorkerChecksModuleContentsWhileCoreCarriesThem) {
  for (const auto& encoded :
       {module_display("plotly", "{}", {}),
        module_display("/nix/store/00000000000000000000000000000000-view", "{",
                       {})}) {
    EXPECT_TRUE(workerd::validate_display(
        *flatbuffers::GetRoot<Tsugumi::Data::Display>(encoded.data())));
    EXPECT_TRUE(wire::standalone_display(encoded));
  }
  EXPECT_TRUE(wire::standalone_display(module_display(
      "/nix/store/00000000000000000000000000000000-view", "{}",
      {{"points", std::string((8u << 20) + 1, '\0')}})));
}
TEST(DisplayModule, WorkerOwnsUtf8ValidationBeforeClientUnpacking) {
  for (const auto kind : {wire::D::DisplayValue::DisplayText,
                          wire::D::DisplayValue::DisplayScalar}) {
    flatbuffers::FlatBufferBuilder builder;
    const auto invalid =
        builder.CreateString(std::string(1, static_cast<char>(255)));
    const auto value =
        kind == wire::D::DisplayValue::DisplayText
            ? wire::D::CreateDisplayText(builder, invalid).Union()
            : wire::D::CreateDisplayScalar(builder, 1.0, invalid).Union();
    builder.Finish(wire::D::CreateDisplay(
        builder,
        wire::D::CreateCardPath(builder, builder.CreateVectorOfStrings(
                                             std::vector<std::string>{"card"})),
        builder.CreateString("run"), 1, 0, kind, value));
    EXPECT_TRUE(workerd::validate_display(
        *flatbuffers::GetRoot<wire::D::Display>(builder.GetBufferPointer())));
  }
}
}  // namespace
}  // namespace tsugumi

TEST(DisplayTransport, MissingScalarAndMalformedTableSurviveTheCore) {
  namespace D = Tsugumi::Data;
  flatbuffers::FlatBufferBuilder b;
  const auto label = b.CreateString("missing");
  const auto scalar = D::CreateDisplayScalar(b, flatbuffers::nullopt, label);
  const auto card = D::CreateCardPath(
      b, b.CreateVectorOfStrings(std::vector<std::string>{"card"}));
  b.Finish(D::CreateDisplay(b, card, b.CreateString("run"), 1, 0,
                            D::DisplayValue::DisplayScalar, scalar.Union()));
  const std::string encoded(reinterpret_cast<const char*>(b.GetBufferPointer()),
                            b.GetSize());
  const auto decoded = tsugumi::wire::standalone_display(encoded);
  ASSERT_TRUE(decoded);
  const auto forwarded = tsugumi::wire::standalone_display(*decoded);
  EXPECT_FALSE(flatbuffers::GetRoot<D::Display>(forwarded.data())
                   ->value_as_DisplayScalar()
                   ->value());
}

TEST(DisplayTransport, SharedRowsAndStringsStayOpaqueAndBounded) {
  namespace D = Tsugumi::Data;
  flatbuffers::FlatBufferBuilder b;
  const auto cell = b.CreateString(std::string(4096, 'x'));
  const auto row = D::CreateTableRow(b, b.CreateVector(std::vector{cell}));
  const auto rows = b.CreateVector(std::vector(10000, row));
  const auto columns =
      b.CreateVectorOfStrings(std::vector<std::string>{"value"});
  const auto table = D::CreateDisplayTable(b, columns, rows);
  const auto card = D::CreateCardPath(
      b, b.CreateVectorOfStrings(std::vector<std::string>{"card"}));
  const auto run = b.CreateString("run");
  b.Finish(D::CreateDisplay(b, card, run, 1, 0, D::DisplayValue::DisplayTable,
                            table.Union()));
  const std::string original(
      reinterpret_cast<const char*>(b.GetBufferPointer()), b.GetSize());
  ASSERT_LT(original.size(), 65536u);
  auto decoded = tsugumi::wire::standalone_display(original);
  ASSERT_TRUE(decoded);
  decoded->revision = 3;
  const auto forwarded = tsugumi::wire::standalone_display(*decoded);
  EXPECT_EQ(forwarded.size(), original.size());
  const auto* display = flatbuffers::GetRoot<D::Display>(forwarded.data());
  EXPECT_EQ(display->revision(), 3u);
  EXPECT_EQ(display->value_as_DisplayTable()->rows()->size(), 10000u);
  // Changing only the envelope also leaves the presentation shared.
  decoded->card = {"moved"};
  const auto moved = tsugumi::wire::standalone_display(*decoded);
  EXPECT_LT(moved.size(), original.size() + 256);
  EXPECT_EQ(flatbuffers::GetRoot<D::Display>(moved.data())
                ->card()
                ->segments()
                ->Get(0)
                ->str(),
            "moved");
}
