// Exercise the common ABI boundary without opening sockets or loading Nix.
#define main tsugumi_worker_main
#include "native_main.cc"
#undef main

#include <gtest/gtest.h>
#include <bit>
#include <filesystem>

namespace {
class NativeContract : public testing::Test {
 protected:
  flatbuffers::FlatBufferBuilder message;
  flatbuffers::Parser schema;
  Call call{};
  std::string valid;
  std::string directory;

  void SetUp() override {
    char pattern[] = "/tmp/tsugumi-native-contract-XXXXXX";
    ASSERT_NE(::mkdtemp(pattern), nullptr);
    directory = pattern;
    ASSERT_TRUE(schema.Parse("namespace sample; table Output { value:string (required); } root_type Output;"));
    ASSERT_TRUE(schema.ParseJson("{\"value\":\"ok\"}"));
    valid.assign(reinterpret_cast<const char*>(schema.builder_.GetBufferPointer()), schema.builder_.GetSize());
    schema.Serialize();
    W::WorkerCallT request;
    request.run_id = "run";
    request.card = std::make_unique<D::CardPathT>();
    request.card->segments = {"card"};
    request.invocation = 1;
    request.params = std::make_unique<D::FlatBufferDataT>();
    request.working_directory = directory;
    request.temporary_directory = directory;
    for (const auto* name : {"first", "second"}) {
      auto port = std::make_unique<D::PortT>();
      port->name = name;
      port->optional = name == std::string("second");
      port->root_type = "sample.Output";
      port->schema_bfbs.assign(schema.builder_.GetBufferPointer(), schema.builder_.GetBufferPointer() + schema.builder_.GetSize());
      request.outputs.push_back(std::move(port));
    }
    message.Finish(W::WorkerCall::Pack(message, &request));
    call.msg = flatbuffers::GetRoot<W::WorkerCall>(message.GetBufferPointer());
    call.card = "card";
    call.run_id = "run";
    call.call_id = 1;
    g_call = &call;
  }
  void TearDown() override { g_call = nullptr; std::filesystem::remove_all(directory); }
  auto emit(const char* port, const std::string& bytes) -> int {
    return emit_output_impl(nullptr, port, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
  }
};

TEST_F(NativeContract, InvalidBytesAreRejectedAtEmit) {
  EXPECT_NE(emit("first", "not a FlatBuffer"), 0);
  EXPECT_TRUE(call.outputs.empty());
}
TEST_F(NativeContract, OversizedOutputIsRejectedAtEmit) {
  EXPECT_NE(emit("first", std::string(kMaxFrame, 'x')), 0);
  EXPECT_TRUE(call.outputs.empty());
}
TEST_F(NativeContract, CombinedOutputsMustFitOneResultFrame) {
  const std::string value(9u << 20, 'a');
  // Valid table messages, individually under the limit.
  ASSERT_TRUE(schema.ParseJson(("{\"value\":\"" + value + "\"}").c_str()));
  const std::string bytes(reinterpret_cast<const char*>(schema.builder_.GetBufferPointer()), schema.builder_.GetSize());
  ASSERT_EQ(emit("first", bytes), 0);
  EXPECT_NE(emit("second", bytes), 0);
  EXPECT_EQ(call.outputs.size(), 1u);
}
TEST_F(NativeContract, ValidOutputAndOmittedOptionalCompletes) {
  ASSERT_EQ(emit("first", valid), 0);
  EXPECT_EQ(finish_outputs_impl(nullptr), 0);
  EXPECT_FALSE(call.failure);
}
TEST_F(NativeContract, RequiredOutputFailsAtTheSdkCompletionCallback) {
  EXPECT_NE(finish_outputs_impl(nullptr), 0);
  ASSERT_TRUE(call.failure);
  EXPECT_NE(call.failure->message.find("required output first"), std::string::npos);
}
TEST_F(NativeContract, ReportRejectionCanBeHandledByTheCard) {
  ASSERT_NE(display_scalar_impl(nullptr, NAN, "value"), 0);
  ASSERT_FALSE(call.failure);
  ASSERT_EQ(emit("first", valid), 0);
  EXPECT_EQ(finish_outputs_impl(nullptr), 0);
}
TEST_F(NativeContract, RequiredOutputFailurePrecedesPostRun) {
  char directory[] = "/tmp/tsugumi-contract-XXXXXX";
  ASSERT_NE(::mkdtemp(directory), nullptr);
  const std::string base = directory;
  write_text(base + "/body", ":\n");
  write_text(base + "/post", "printf '%s' \"$TSUGUMI_FAILED\" > " + base + "/failed\n");
  Options options;
  options.bash = "/bin/bash";
  options.run = base + "/body";
  options.post = base + "/post";
  execute(options, call);
  EXPECT_TRUE(call.failure);
  EXPECT_EQ(read_text(base + "/failed"), "1");
  ::unlink((base + "/body").c_str());
  ::unlink((base + "/post").c_str());
  ::unlink((base + "/failed").c_str());
  ::rmdir(base.c_str());
}
TEST_F(NativeContract, MalformedDisplaysAreRejectedSynchronously) {
  EXPECT_NE(display_scalar_impl(nullptr, INFINITY, "value"), 0);
  EXPECT_NE(display_text_impl(nullptr, "\xff"), 0);
  EXPECT_NE(display_image_impl(nullptr, "image/png", reinterpret_cast<const uint8_t*>("invalid"), 7, ""), 0);
  EXPECT_NE(display_table_impl(nullptr, nullptr, 0, nullptr, 0), 0);
}
}  // namespace

TEST(NumberConversion, CanonicalNonFiniteValuesRemainNumbersAndNullRemainsAbsent) {
  flatbuffers::Parser parser;
  ASSERT_TRUE(parser.Parse("table Numbers { values:[double]; text:string; missing:double = null; } root_type Numbers;"));
  ASSERT_TRUE(tsugumi::message_json::parse_json(parser,
      R"({"values":[NaN,Infinity,-Infinity,-0.0,1e-20,5e-324],"text":"nan inf NaN Infinity","missing":null})"));
  std::string json;
  ASSERT_EQ(tsugumi::message_json::gen_language_text(parser, parser.builder_.GetBufferPointer(), &json), nullptr);
  EXPECT_NE(json.find("NaN"), std::string::npos);
  EXPECT_NE(json.find("Infinity"), std::string::npos);
  EXPECT_NE(json.find("-Infinity"), std::string::npos);
  EXPECT_NE(json.find("-0.0"), std::string::npos);
  EXPECT_NE(json.find("nan inf NaN Infinity"), std::string::npos);
  ASSERT_TRUE(tsugumi::message_json::parse_json(parser, json));
  const auto* table = flatbuffers::GetRoot<flatbuffers::Table>(parser.builder_.GetBufferPointer());
  const auto* values = table->GetPointer<const flatbuffers::Vector<double>*>(4);
  ASSERT_NE(values, nullptr);
  ASSERT_EQ(values->size(), 6u);
  EXPECT_TRUE(std::isnan(values->Get(0)));
  const double expected[] = {NAN, INFINITY, -INFINITY, -0.0, 1e-20, 5e-324};
  for (flatbuffers::uoffset_t i = 1; i < values->size(); ++i)
    EXPECT_EQ(std::bit_cast<uint64_t>(values->Get(i)), std::bit_cast<uint64_t>(expected[i])) << i;
  EXPECT_FALSE((table->GetOptional<double, double>(8)).has_value());
  EXPECT_TRUE(tsugumi::message_json::parse_json(parser, R"({"values":["NaN","Inf","-Inf"]})"));
}
