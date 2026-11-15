#include <gtest/gtest.h>

#include <map>

#include "../agent_fixture_server.hh"
#include "document.hh"
#include "host/projects.hh"
#include "host/vm_process.hh"

namespace tsugumi::test {
namespace {
namespace H = Tsugumi::Host;
auto Request(host::Projects &service, H::RequestT request) -> std::string {
  flatbuffers::FlatBufferBuilder builder;
  const auto offset = H::Request::Pack(builder, &request);
  builder.Finish(offset);
  return service.Handle(
      *flatbuffers::GetRoot<H::Request>(builder.GetBufferPointer()), 1);
}
auto Files(const H::ProjectFiles *files) -> std::map<std::string, std::string> {
  std::map<std::string, std::string> result;
  for (const auto *file : *files->files()) {
    const auto *bytes = file->bytes();
    result[file->path()->str()] =
        bytes ? std::string(reinterpret_cast<const char *>(bytes->data()),
                            bytes->size())
              : std::string();
  }
  return result;
}
TEST(HostProjectFiles, CreationUsesTheCoreInitialDocumentAndCopiesPreserveIt) {
  TemporaryDirectory dir;
  host::Projects service(dir.path / "state", {});
  H::HostProjectCreateRequestT create;
  create.parent = dir.path.string();
  create.name = "New 日本語";
  create.flow = "leftToRight";
  H::RequestT request;
  request.value.Set(std::move(create));
  Request(service, std::move(request));
  const auto source = dir.path / "New 日本語";
  EXPECT_EQ(Read(source / "tsugumi.json"), initial_project_json("New 日本語"));
  H::RequestT defaults;
  defaults.value.Set(H::HostProjectDefaultsRequestT{});
  const auto bytes = Request(service, std::move(defaults));
  const auto* pin = H::GetMessage(bytes.data())->payload_as_Reply()
                        ->value_as_ProjectDefaults();
  ASSERT_NE(pin, nullptr);
  EXPECT_EQ(pin->nixpkgs_pin()->str(),
            parse_project_json(Read(source / "tsugumi.json")).pkgs_path->value());
  EXPECT_NE(Read(source / "layout.json").find("leftToRight"),
            std::string::npos);
  const std::string custom =
      R"({"name":"custom","pkgsPath":"explicit-pin","canvas":{"cards":[],"connections":[]}})";
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
  for (const auto &language : {"ja", "en"}) {
    const auto demos = dir.path / "demos";
    auto document = initial_project_json(language);
    const auto pin = parse_project_json(document).pkgs_path->value();
    document.replace(document.find(pin), pin.size(), "demo-specific-pin");
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
TEST(VMResetPolicy,
     HostChoosesRetentionAndDescribesTheSameFilesInBothLanguages) {
  auto policy = host::VMResetPolicy();
  EXPECT_EQ(policy.keep_files, std::vector<std::string>{"history.img"});
  EXPECT_TRUE(policy.discard_other_files);
  H::HostRuntimeT runtime;
  host::DescribeVMReset(runtime);
  for (const auto &name : policy.keep_files) {
    EXPECT_NE(runtime.reset_confirmation_ja.find(name), std::string::npos);
    EXPECT_NE(runtime.reset_confirmation_en.find(name), std::string::npos);
  }
  EXPECT_NE(runtime.reset_confirmation_en.find("everything else"),
            std::string::npos);
  EXPECT_NE(runtime.reset_confirmation_en.find("control folder"),
            std::string::npos);
  EXPECT_NE(runtime.reset_confirmation_en.find(".tsugumi/runs.sqlite"),
            std::string::npos);
  Tsugumi::VMHost::RequestT request;
  request.value.Set(policy);
  flatbuffers::FlatBufferBuilder builder;
  builder.Finish(Tsugumi::VMHost::Request::Pack(builder, &request));
  const auto *reset =
      flatbuffers::GetRoot<Tsugumi::VMHost::Request>(builder.GetBufferPointer())
          ->value_as_Reset();
  ASSERT_NE(reset, nullptr);
  EXPECT_EQ(reset->keep_files()->Get(0)->str(), "history.img");
  EXPECT_TRUE(reset->discard_other_files());
}
TEST(HostProjectFiles, ImportAndDuplicateUseTheSameRuleAndPreserveBytes) {
  TemporaryDirectory dir;
  const auto source = dir.path / "source";
  const std::map<std::string, std::string> kept = {
      {"tsugumi.json", "{}"},
      {".project-settings", "hidden"},
      {"definitions/card/src/.config", "config"},
      {"definitions/card/.assets/blob.bin", std::string("\xff\0\x80", 3)},
      {"externals/sub/tsugumi.json", "{}"},
      {"externals/sub/definitions/card/.config", "nested"},
      {"empty", ""},
      {"large.bin", std::string((1 << 20) + 3, static_cast<char>(0x80))}};
  for (const auto &[path, bytes] : kept)
    Write(source / path, bytes);
  for (const auto &path :
       {".git/config", ".tsugumi/run", "definitions/card/.git/config",
        "externals/sub/.tsugumi/run"})
    Write(source / path, "state");
  Write(dir.path / "outside/secret", "not a project file");
  std::filesystem::create_directory_symlink(dir.path / "outside",
                                            source / "linked-directory");
  std::filesystem::create_symlink(dir.path / "outside/secret",
                                  source / "linked-file");
  host::Projects service(dir.path / "state", {});
  auto read = [&](const std::filesystem::path &path) {
    std::map<std::string, std::string> result;
    uint32_t index = 0;
    uint64_t offset = 0;
    size_t pages = 0;
    for (;;) {
      if (++pages > 100)
        throw std::runtime_error("Import cursor did not finish");
      H::HostProjectReadRequestT value;
      value.path = path.string();
      value.file_index = index;
      value.offset = offset;
      H::RequestT request;
      request.value.Set(std::move(value));
      const auto bytes = Request(service, std::move(request));
      EXPECT_LT(bytes.size(), 16u << 20);
      const auto *reply = H::GetMessage(bytes.data())
                              ->payload_as_Reply()
                              ->value_as_ProjectFiles();
      EXPECT_NE(reply, nullptr);
      for (const auto &[name, chunk] : Files(reply))
        result[name] += chunk;
      if (reply->complete())
        break;
      if (reply->next_file() == index && reply->next_offset() <= offset)
        throw std::runtime_error("Import cursor did not advance");
      index = reply->next_file();
      offset = reply->next_offset();
    }
    return result;
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
  Write(dir.path / "project/tsugumi.json", "{}");
  Write(dir.path / "project/large.bin", "");
  std::filesystem::resize_file(dir.path / "project/large.bin", (64 << 20) + 1);
  host::Projects service(dir.path / "state", {});
  H::HostProjectReadRequestT read;
  read.path = (dir.path / "project").string();
  H::RequestT request;
  request.value.Set(std::move(read));
  EXPECT_THROW(Request(service, std::move(request)), host::ProjectFailure);
}
} // namespace
} // namespace tsugumi::test
