#include <gtest/gtest.h>

#include "execution_service.hh"
#include "service.hh"
#include "store.hh"
#include "util/crypto.hh"
#include "util/fs.hh"

namespace tsugumi {
namespace {
class NativeExecution : public testing::TestWithParam<bool> {};
// A transfer error is not the Card's call failing: a living resident keeps its state.
TEST_P(NativeExecution, OutputCopyFailureKeepsTheResidentWorker) {
  namespace stdfs = std::filesystem;
  const auto root = stdfs::canonical(stdfs::temp_directory_path()) / ("tsugumi-transfer-" + crypto::uuid4());
  stdfs::create_directories(root);
  struct Cleanup {
    stdfs::path root;
    ~Cleanup() { std::error_code error; stdfs::remove_all(root, error); }
  } cleanup{root};
  fs::write_atomic(root / "tsugumi.json", R"({"name":"test","pkgsPath":"tsugumi:default","canvas":{"cards":[],"connections":[]}})");
  fs::write_atomic(root / "layout.json", "{}");
  ::setenv("TSUGUMI_MOCK_CONTROL", (root / "mock").c_str(), 1);
  ServiceOptions options;
  options.root = root;
  options.sdk_dir = TSUGUMI_TEST_SDK;
  options.worker_binary = TSUGUMI_WORKER_BINARY;
  options.scratch_dir = root / "scratch";
  options.run_scratch_dir = root / "runs";
  Service service(options);
  service.open();
  NativeExecutionService execution(service, root / "workers");
  Node node;
  node.path = {"card"};
  node.settings.sandbox = false;
  node.settings.reuse_worker = true;
  std::string error;
  const auto type = types::compile("table Path { value:string; }", "Path", &error);
  ASSERT_TRUE(type) << error;
  node.outputs.push_back({"out", PortKind::File, false, *type});
  CardBuild build;
  // The same build fails its first transfer and produces a real file next.
  // A worker can complete its ABI call even when its declared file is absent.
  build.bash = "printf 'worker log\\n'\nif [ -f '" + (root / "called").string() + "' ]; then touch \"$TSUGUMI_OUTPUT_DIR/result\"; "
               "else touch '" + (root / "called").string() + "'; " +
               (GetParam() ? "ln -s /etc/passwd \"$TSUGUMI_OUTPUT_DIR/result\"; " : "") + "fi\n"
               "printf '{\"out\":{\"value\":\"%s/result\"}}' \"$TSUGUMI_OUTPUT_DIR\" > \"$TSUGUMI_OUTPUT\"\n";
  std::atomic<bool> stop{false};
  const std::vector<Value> inputs;
  const std::vector<InputSourceData> sources;
  InvocationSummaryData summary;
  summary.run_id = crypto::uuid4();
  summary.card = node.path;
  summary.invocation = 1;
  std::vector<std::pair<LogStream, std::string>> logs;
  ExecutionRequest request{node, build, inputs, sources, summary, root / "invocation", stop,
      [] { return false; }, stop, [&](auto stream, const auto& text) { logs.emplace_back(stream, text); }, [](const auto&) {},
      [](auto, auto) {}, [](const auto&) {}, [] {}};
  request.log_limit_bytes = 1;
  EXPECT_THROW(execution.run(request), Failed);
  EXPECT_TRUE(std::ranges::any_of(logs, [](const auto& entry) {
    return entry.first == LogStream::journal && entry.second.find("1-byte limit") != std::string::npos;
  }));
  EXPECT_FALSE(std::ranges::any_of(logs, [](const auto& entry) { return entry.first == LogStream::stdout; }));
  summary.invocation = 2;
  const auto result = execution.run(request);
  ASSERT_FALSE(result.outcome.failure);
  EXPECT_EQ(summary.worker_reused, true);
}
INSTANTIATE_TEST_SUITE_P(InvalidFile, NativeExecution, testing::Values(false, true));
}  // namespace
}  // namespace tsugumi

TEST(GraphLanguage, ArbitraryAndOmittedLanguagesRemainExecutable) {
  namespace ts = tsugumi;
  const auto root = std::filesystem::temp_directory_path() / ("tsugumi-language-" + ts::crypto::uuid4());
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{root};
  ts::fs::write_atomic(root / "tsugumi.json", R"({"name":"test","pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"card"}],"connections":[]}})");
  ts::fs::write_atomic(root / "layout.json", "{}");
  ts::fs::write_atomic(root / "definitions/card/default.nix", "{pkgs,inputs,params}:{run=\"true\";}");
  ts::fs::write_atomic(root / "definitions/card/_outputs.fbs", "table Outputs {} root_type Outputs;");
  for (const auto* definition : {"{}", "{\"language\":\"custom-sdk\"}"}) {
    ts::fs::write_atomic(root / "definitions/card/_card.json", definition);
    ts::Store store({root, ""});
    store.open();
    const auto& node = store.current()->resolution->nodes.at(0);
    EXPECT_TRUE(node.exec_diags.empty());
  }
}
