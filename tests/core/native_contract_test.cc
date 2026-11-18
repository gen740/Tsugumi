// Exercise the common ABI boundary without opening sockets or loading Nix.
#define main tsugumi_worker_main
#include "native_main.cc"
#undef main

#include <gtest/gtest.h>

#include <bit>
#include <filesystem>
#include <sstream>

namespace {
// NixOS and the nixos/nix container have no /bin/bash; use the one on PATH
// there.
std::string host_bash() {
  if (::access("/bin/bash", X_OK) == 0) return "/bin/bash";
  std::istringstream path(std::getenv("PATH") ? std::getenv("PATH") : "");
  for (std::string directory; std::getline(path, directory, ':');)
    if (::access((directory + "/bash").c_str(), X_OK) == 0)
      return directory + "/bash";
  return "/bin/bash";
}

class NativeContract : public testing::Test {
 protected:
  flatbuffers::FlatBufferBuilder message;
  flatbuffers::Parser schema;
  Call call{};
  std::string directory;

  void SetUp() override {
    auto pattern =
        (std::filesystem::temp_directory_path() / "tn-XXXXXX").string();
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    directory = pattern;
    ASSERT_TRUE(
        schema.Parse("namespace sample; table Output { value:string "
                     "(required); } root_type Output;"));
    schema.Serialize();
    W::WorkerCallT request;
    request.run_id = "run";
    request.card = std::make_unique<D::CardPathT>();
    request.card->segments = {"card"};
    request.invocation = 1;
    request.params = std::make_unique<D::FlatBufferDataT>();
    request.output_schema =
        "namespace sample; table Output { value:string (required); } "
        "table Outputs { first:Output; second:Output; } root_type Outputs;";
    request.working_directory = directory;
    request.temporary_directory = directory;
    for (const auto* name : {"first", "second"}) {
      auto port = std::make_unique<D::PortT>();
      port->name = name;
      port->optional = name == std::string("second");
      port->root_type = "sample.Output";
      port->schema_bfbs.assign(
          schema.builder_.GetBufferPointer(),
          schema.builder_.GetBufferPointer() + schema.builder_.GetSize());
      request.outputs.push_back(std::move(port));
    }
    message.Finish(W::WorkerCall::Pack(message, &request));
    call.msg = flatbuffers::GetRoot<W::WorkerCall>(message.GetBufferPointer());
    call.card = "card";
    call.run_id = "run";
    call.call_id = 1;
    g_call = &call;
  }
  void TearDown() override {
    g_call = nullptr;
    std::filesystem::remove_all(directory);
  }
  auto receive(const std::string& bytes) -> void {
    tsugumi_buffer buffer{};
    ASSERT_EQ(tsugumi_copy_outputs(&buffer, bytes.data(), bytes.size()), 0);
    receive_outputs(buffer);
    std::free(buffer.data);
  }
  auto receive_json(const std::string& json) -> void {
    tsugumi_buffer buffer{};
    ASSERT_EQ(outputs_json_impl(nullptr, json.c_str(), &buffer), 0);
    receive_outputs(buffer);
    std::free(buffer.data);
  }
};

TEST_F(NativeContract, InvalidReturnedBufferIsRejected) {
  receive("not a FlatBuffer");
  EXPECT_TRUE(call.failure);
  EXPECT_TRUE(call.outputs.empty());
}
TEST_F(NativeContract, OversizedReturnedBufferIsRejected) {
  receive(std::string(kMaxFrame + 1, 'x'));
  EXPECT_TRUE(call.failure);
  EXPECT_TRUE(call.outputs.empty());
}
TEST_F(NativeContract, CombinedOutputsMustFitOneResultFrame) {
  const std::string value(8u << 20, 'a');
  receive_json("{\"first\":{\"value\":\"" + value +
               "\"},\"second\":{\"value\":\"" + value + "\"}}");
  ASSERT_TRUE(call.failure);
  EXPECT_EQ(call.failure->kind, D::FailureKind::output_invalid);
}
TEST_F(NativeContract, ValidOutputAndOmittedOptionalCompletes) {
  receive_json(R"({"first":{"value":"ok"}})");
  validate_required_outputs(call);
  EXPECT_FALSE(call.failure);
  ASSERT_EQ(call.outputs.size(), 1u);
  EXPECT_EQ(call.outputs[0].first, "first");
}
TEST_F(NativeContract, RequiredOutputIsCheckedOnReturn) {
  receive_json("{}");
  validate_required_outputs(call);
  ASSERT_TRUE(call.failure);
  ASSERT_EQ(call.failure->args.size(), 1u);
  EXPECT_EQ(call.failure->args[0].first, "port");
  EXPECT_EQ(call.failure->args[0].second, "first");
}
TEST_F(NativeContract, SourceFailureSurvivesCompletionAndTheFailureReply) {
  fail_at_impl(nullptr, "worker_failed", "cell traceback", "src/notebook.py", 8,
               "calculation");
  validate_required_outputs(call);
  fail_impl(nullptr, "output_invalid", "must not replace the first failure");

  // Capture the real framed reply in a file, without a worker socket.
  const int saved = ::dup(kData);
  FILE* capture = std::tmpfile();
  ASSERT_NE(capture, nullptr);
  ASSERT_EQ(::dup2(::fileno(capture), kData), kData);
  reply(call);
  if (saved >= 0) {
    EXPECT_EQ(::dup2(saved, kData), kData);
    ::close(saved);
  } else if (::fileno(capture) != kData) {
    ::close(kData);
  }
  std::rewind(capture);
  const auto bytes = read_frame(::fileno(capture));
  std::fclose(capture);
  ASSERT_TRUE(bytes);
  flatbuffers::Verifier verifier(
      reinterpret_cast<const uint8_t*>(bytes->data()), bytes->size());
  ASSERT_TRUE(W::VerifyWorkerMessageBuffer(verifier));
  const auto* message = W::GetWorkerMessage(bytes->data());
  EXPECT_EQ(message->call_id(), call.call_id);
  ASSERT_NE(message->value_as_WorkerFailure(), nullptr);
  const auto* failure = message->value_as_WorkerFailure()->failure();
  EXPECT_EQ(failure->kind(), D::FailureKind::worker_failed);
  EXPECT_EQ(failure->diagnostic()->str(), "cell traceback");
  std::map<std::string, std::string> args;
  for (const auto* entry : *failure->args())
    args.emplace(entry->key()->str(), entry->value()->str());
  const std::map<std::string, std::string> expected = {
      {"card", "card"},
      {"source", "src/notebook.py"},
      {"line", "8"},
      {"cell", "calculation"}};
  EXPECT_EQ(args, expected);
}
TEST_F(NativeContract, ReportRejectionCanBeHandledByTheCard) {
  ASSERT_NE(display_scalar_impl(nullptr, NAN, "value"), 0);
  ASSERT_FALSE(call.failure);
  receive_json(R"({"first":{"value":"ok"}})");
  validate_required_outputs(call);
  EXPECT_FALSE(call.failure);
}
TEST_F(NativeContract, RequiredOutputFailurePrecedesPostRun) {
  const std::string base = directory + "/contract";
  std::filesystem::create_directories(base);
  write_text(base + "/body", ":\n");
  write_text(base + "/post",
             "printf '%s' \"$TSUGUMI_FAILED\" > " + base + "/failed\n");
  Options options;
  options.bash = host_bash();
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
  EXPECT_NE(
      display_image_impl(nullptr, "image/png",
                         reinterpret_cast<const uint8_t*>("invalid"), 7, ""),
      0);
  EXPECT_NE(display_table_impl(nullptr, nullptr, 0, nullptr, 0), 0);
}
}  // namespace

TEST(NumberConversion,
     CanonicalNonFiniteValuesRemainNumbersAndNullRemainsAbsent) {
  flatbuffers::Parser parser;
  ASSERT_TRUE(
      parser.Parse("table Numbers { values:[double]; text:string; "
                   "missing:double = null; } root_type Numbers;"));
  ASSERT_TRUE(tsugumi::message_json::parse_json(
      parser,
      R"({"values":[NaN,Infinity,-Infinity,-0.0,1e-20,5e-324],"text":"nan inf NaN Infinity","missing":null})"));
  std::string json;
  ASSERT_EQ(tsugumi::message_json::gen_language_text(
                parser, parser.builder_.GetBufferPointer(), &json),
            nullptr);
  EXPECT_NE(json.find("NaN"), std::string::npos);
  EXPECT_NE(json.find("Infinity"), std::string::npos);
  EXPECT_NE(json.find("-Infinity"), std::string::npos);
  EXPECT_NE(json.find("-0.0"), std::string::npos);
  EXPECT_NE(json.find("nan inf NaN Infinity"), std::string::npos);
  ASSERT_TRUE(tsugumi::message_json::parse_json(parser, json));
  const auto* table = flatbuffers::GetRoot<flatbuffers::Table>(
      parser.builder_.GetBufferPointer());
  const auto* values = table->GetPointer<const flatbuffers::Vector<double>*>(4);
  ASSERT_NE(values, nullptr);
  ASSERT_EQ(values->size(), 6u);
  EXPECT_TRUE(std::isnan(values->Get(0)));
  const double expected[] = {NAN, INFINITY, -INFINITY, -0.0, 1e-20, 5e-324};
  for (flatbuffers::uoffset_t i = 1; i < values->size(); ++i)
    EXPECT_EQ(std::bit_cast<uint64_t>(values->Get(i)),
              std::bit_cast<uint64_t>(expected[i]))
        << i;
  EXPECT_FALSE((table->GetOptional<double, double>(8)).has_value());
  EXPECT_TRUE(tsugumi::message_json::parse_json(
      parser, R"({"values":["NaN","Inf","-Inf"]})"));
}
