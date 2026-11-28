#include <gtest/gtest.h>

#include <map>

#include "../agent_fixture_server.hh"
#include "document_fixture.hh"
#include "host/projects.hh"
#include "host/templates.hh"
#include "host/vm_process.hh"
#include "tsugumi/document/document.hh"

namespace tsugumi::test {
namespace {
namespace H = Tsugumi::Host;
auto Request(host::Projects& service, H::RequestT request) -> std::string {
  flatbuffers::FlatBufferBuilder builder;
  const auto offset = H::Request::Pack(builder, &request);
  builder.Finish(offset);
  return service.Handle(
      *flatbuffers::GetRoot<H::Request>(builder.GetBufferPointer()), 1);
}

TEST(HostProjectFiles, CreationUsesTheCoreInitialDocumentAndCopiesPreserveIt) {
  TemporaryDirectory dir;
  host::Projects service(dir.path / "state", {});
  H::HostProjectCreateRequestT create;
  create.parent = dir.path.string();
  create.name = "New 日本語";
  H::RequestT request;
  request.value.Set(std::move(create));
  Request(service, std::move(request));
  const auto source = dir.path / "New 日本語";
  EXPECT_EQ(Read(source / "tsugumi.json"),
            initial_project_json("New 日本語", {".git", ".DS_Store"}));
  H::RequestT defaults;
  defaults.value.Set(H::HostProjectDefaultsRequestT{});
  const auto bytes = Request(service, std::move(defaults));
  const auto* pin = H::GetMessage(bytes.data())
                        ->payload_as_Reply()
                        ->value_as_ProjectDefaults();
  ASSERT_NE(pin, nullptr);
  const auto initial = json::Object(Read(source / "tsugumi.json"));
  EXPECT_EQ(initial.at("formatVersion"), std::to_string(kProjectFormatVersion));
  EXPECT_EQ(initial.at("nixPath"), "[]");
  EXPECT_EQ(initial.at("trackedPaths"), "[]");
  EXPECT_TRUE(json::Bool(initial.at("recordLogs")));
  EXPECT_FALSE(json::Bool(initial.at("recordIO"), true));
  EXPECT_EQ(json::UInt(initial.at("logLimitBytes")), 134217728u);
  EXPECT_EQ(json::UInt(initial.at("outputLimitBytes")), 68719476736ULL);
  EXPECT_EQ(json::UInt(initial.at("outputLimitEntries")), 1000u);
  EXPECT_EQ(json::Array(initial.at("agentExclude")).size(),
            pin->agent_exclude()->size());

  EXPECT_TRUE(std::filesystem::exists(source / "layout.bin"));
  const std::string custom = FixtureProjectDocument(
      R"({"name":"custom","nixPath":[{"name":"custom","url":"github:org/project"}],"canvas":{"cards":[],"connections":[]}})");
  Write(source / "tsugumi.json", custom);
  H::HostProjectDuplicateRequestT duplicate;
  duplicate.path = source.string();
  duplicate.parent = dir.path.string();
  duplicate.name = "copy";
  request.value.Set(std::move(duplicate));
  Request(service, std::move(request));
  EXPECT_EQ(Read(dir.path / "copy/tsugumi.json"), custom);
}
TEST(HostProjectFiles, DemoCopyKeepsItsDocumentInBothLanguages) {
  TemporaryDirectory dir;
  for (const auto& language : {"ja", "en"}) {
    const auto demos = dir.path / "demos";
    const std::string document = FixtureProjectDocument(
        R"({"name":"sample","nixPath":[{"name":"demo","url":"github:org/demo"}],"canvas":{"cards":[],"connections":[]}})");

    Write(demos / language / "sample/tsugumi.json", document);
    Write(demos / language / "index.json",
          R"({"demos":[{"id":"sample","path":"sample","title":"Sample"}]})");
    host::Projects service(dir.path / "state", demos);
    H::HostDemoCopyRequestT copy;
    copy.id = "sample";
    copy.parent = (dir.path / language).string();
    copy.language = language;
    H::RequestT request;
    request.value.Set(std::move(copy));
    Request(service, std::move(request));
    EXPECT_EQ(Read(dir.path / language / "sample/tsugumi.json"), document);
  }
}
TEST(VMResetPolicy, HostAllowsDiscardingAllVMFiles) {
  const auto policy = host::VMResetPolicy();
  EXPECT_TRUE(policy.keep_files.empty());
  EXPECT_TRUE(policy.discard_other_files);
}
TEST(HostProjectFiles, ImportAndDuplicateUseTheSameRuleAndPreserveBytes) {
  TemporaryDirectory dir;
  const auto source = dir.path / "source";
  const std::map<std::string, std::string> kept = {
      {"tsugumi.json",
       FixtureProjectDocument(
           R"({"canvas":{"cards":[{"id":"card"},{"id":"sub"}],"connections":[]}})")},
      {"definitions/card/_card.json",
       R"({"type":"draft","symbol":"pencil.and.outline"})"},
      {"definitions/card/README.md", host::CardReadme("card")},
      {"definitions/sub/_card.json", R"({"type":"subgraph","symbol":"link"})"},
      {"definitions/sub/README.md", host::CardReadme("sub")},
      {".project-settings", "hidden"},
      {"definitions/card/src/.config", "config"},
      {"definitions/card/.assets/blob.bin", std::string("\xff\0\x80", 3)},
      {"externals/sub/tsugumi.json",
       FixtureProjectDocument(
           R"({"canvas":{"cards":[{"id":"card"}],"connections":[]}})")},
      {"externals/sub/definitions/card/_card.json",
       R"({"type":"draft","symbol":"pencil.and.outline"})"},
      {"externals/sub/definitions/card/README.md", host::CardReadme("card")},
      {"externals/sub/definitions/card/.config", "nested"},
      {"empty", ""},
      {"large.bin", std::string((1 << 20) + 3, static_cast<char>(0x80))}};
  for (const auto& [path, bytes] : kept) Write(source / path, bytes);
  for (const auto& path :
       {".git/config", ".tsugumi/run", "definitions/card/.git/config",
        "externals/sub/.tsugumi/run"})
    Write(source / path, "state");
  Write(dir.path / "outside/secret", "not a project file");
  std::filesystem::create_directory_symlink(dir.path / "outside",
                                            source / "linked-directory");
  std::filesystem::create_symlink(dir.path / "outside/secret",
                                  source / "linked-file");
  host::Projects service(dir.path / "state", {});
  auto read = [](const std::filesystem::path& path) {
    const auto files = host::ReadProjectFiles(path.string());
    return std::map<std::string, std::string>(files.begin(), files.end());
  };
  EXPECT_EQ(read(source), kept);
  H::HostProjectDuplicateRequestT duplicate;
  duplicate.path = source.string();
  duplicate.parent = dir.path.string();
  duplicate.name = "copy";
  H::RequestT request;
  request.value.Set(std::move(duplicate));
  Request(service, std::move(request));
  EXPECT_EQ(read(dir.path / "copy"), kept);
  EXPECT_FALSE(std::filesystem::exists(dir.path / "copy/.git"));
  EXPECT_FALSE(std::filesystem::exists(dir.path / "copy/linked-file"));
}
TEST(HostProjectFiles, RejectsTooLargeProjectsBeforeReadingContents) {
  TemporaryDirectory dir;
  Write(dir.path / "project/tsugumi.json", FixtureProjectDocument());
  Write(dir.path / "project/large.bin", "");
  std::filesystem::resize_file(dir.path / "project/large.bin", (64 << 20) + 1);
  EXPECT_THROW(host::ReadProjectFiles((dir.path / "project").string()),
               host::ProjectFailure);
}
}  // namespace
}  // namespace tsugumi::test
