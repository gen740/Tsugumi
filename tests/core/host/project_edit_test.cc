#include "host/project_edit.hh"

#include <gtest/gtest.h>

#include "../agent/document_fixture.hh"
#include "host/failure.hh"
#include "host/project_operations.hh"
#include "tsugumi/document/graph.hh"
#include "tsugumi/util/crypto.hh"

namespace tsugumi::host {
namespace {
namespace H = Tsugumi::Host;
namespace json = ::tsugumi::json;
auto Field(std::vector<std::string> fields, std::string old_value,
           std::string new_value) -> H::ProjectFieldEditT {
  H::ProjectFieldEditT edit;
  edit.file_path = "tsugumi.json";
  for (const auto& key : fields) {
    auto step = std::make_unique<H::ProjectSelectorT>();
    step->field = key;
    edit.selector.push_back(std::move(step));
  }
  edit.old_value = std::make_unique<H::ProjectValueT>();
  edit.old_value->present = true;
  edit.old_value->json = std::move(old_value);
  edit.new_value = std::make_unique<H::ProjectValueT>();
  edit.new_value->present = true;
  edit.new_value->json = std::move(new_value);
  return edit;
}
TEST(NixPathDocument, ValidatesNamesAndUrlsAndSortsLockLines) {
  const auto project = [](std::string entries) {
    return test::FixtureProjectDocument("{\"nixPath\":" + entries + "}");
  };
  EXPECT_NO_THROW(parse_project_json(
      project(R"([{"name":"9.compile_x-1","url":"github:org/repo"}])")));
  for (
      const auto* entries : {
          R"([{"name":"_bad","url":"github:org/repo"}])",
          R"([{"name":"ok","url":"github:org/repo"},{"name":"ok","url":"gitlab:org/repo"}])",
          R"([{"name":"ok"}])",
          R"([{"name":"ok","url":"invalid url"}])",
      })
    EXPECT_THROW(parse_project_json(project(entries)), DocumentError);
  const auto lock = parse_project_lock("tsugumi.lock", R"({"entries":[
    {"name":"z","url":"git+https://example.com/z","rev":"0123456789012345678901234567890123456789","narHash":"sha256-z"},
    {"name":"a","url":"https://example.com/a.tar.gz","narHash":"sha256-a"}
  ]})");
  EXPECT_LT(lock.text.find("\"a\""), lock.text.find("\"z\""));
  EXPECT_THROW(
      parse_project_lock("tsugumi.lock", R"({"entries":[{"name":"a","url":"github:org/a"}]})"),
      DocumentError);
  EXPECT_THROW(parse_project_lock("tsugumi.lock", R"({"entries":[true]})"),
               DocumentError);
  EXPECT_EQ(parse_project_lock("tsugumi.lock", lock.text).text, lock.text);
}

struct FakeNixPathCore {
  json::Members snapshot;
  json::Members save;
  std::vector<std::string> calls;
  bool stale_param_hash = false;
  bool fail_param = false;
  bool fail_lock = false;

  auto Call(const std::string& method, const json::Members& args)
      -> OperationReply {
    json::Members value;
    std::string reply;
    if (method == "operation_get") {
      reply = "OperationResult";
      value = {{"state", "\"unknown\""}};
    } else if (method == "project_inspect") {
      reply = "ProjectSnapshot";
      value = snapshot;
    } else if (method == "nix_path_lock_source") {
      const auto url = json::Text(args, "url");
      calls.push_back(url);
      if (fail_lock) {
        reply = "Failure";
        value = {{"kind", "\"nix_failed\""}, {"diagnostic", "\"lock failed\""}};
      } else {
        reply = "NixPathLockedSource";
        value = {{"rev", json::Quote("0123456789012345678901234567890123456789")},
                 {"narHash", json::Quote("sha256-new")}};
      }
    } else if (method == "rewrite_param_literal") {
      EXPECT_EQ(json::Text(args, "name"), "scale");
      EXPECT_EQ(json::Text(args, "literal"), "10.0");
      EXPECT_EQ(json::Text(args, "at_rev"), "7");
      if (fail_param) {
        reply = "Failure";
        value = {{"kind", json::Quote("validation_failed")},
                 {"diagnostic", json::Quote("parameter scale expects float")}};
      } else {
        reply = "RewrittenParamLiteral";
        value = {
            {"path", json::Quote("definitions/a/params.nix")},
            {"base_hash",
             json::Quote(stale_param_hash
                             ? "other"
                             : crypto::sha256_hex("{ scale = 2.0; # keep\n}"))},
            {"text", json::Quote("{ scale = 10.0; # keep\n}")}};
      }
    } else if (method == "project_file_read") {
      EXPECT_EQ(json::Text(args, "path"), "definitions/a/params.nix");
      reply = "FilePage";
      value = {{"bytes", json::Quote("{ scale = 2.0; # keep\n}")},
               {"eof", "true"}};
    } else {
      EXPECT_EQ(method, "project_save");
      reply = "SaveResult";
      value = {{"rev", "\"8\""}};
      save = args;
    }
    return {json::Encode(json::Members{{"reply", json::Quote(reply)},
                                       {"value", json::Encode(value)}}),
            {}};
  }

  auto Execute(const H::ProjectOperateRequestT& operation) -> OperationReply {
    save.clear();
    calls.clear();
    return ProjectOperations().Execute(
        operation, {"ui", {}, {}, {}},
        [this](const auto& method, const auto& args) {
          return Call(method, args);
        });
  }

  auto ReadChange(const std::string& kind) const -> json::Members {
    for (const auto& raw : json::Array(json::Get(save, "changes", "[]"))) {
      const auto change = json::Object(raw);
      if (json::Text(change, "value_type") == kind)
        return json::Object(change.at("value"));
    }
    return {};
  }
};

auto SourceDoc(const std::string& path, const std::string& bytes)
    -> std::string {
  return json::Encode(
      json::Members{{"path", json::Quote(path)},
                    {"hash", json::Quote(crypto::sha256_hex(bytes))},
                    {"document", json::Quote(bytes)}});
}

TEST(NixPathOperation, RootAndExternalLocksUpdateOneAndAllAtomically) {
  const std::string project =
      R"({"nixPath":[{"name":"used","url":"github:org/repo"}],"canvas":{"cards":[],"connections":[]}})";
  const std::string old =
      R"({"entries":[{"name":"stale","url":"github:org/stale","narHash":"sha256-old"}]})";
  FakeNixPathCore core;
  core.snapshot = {
      {"rev", "\"7\""},
      {"project_json", json::Quote(project)},
      {"sources", json::Encode(std::vector<std::string>{
                      SourceDoc("tsugumi.lock", old),
                      SourceDoc("externals/sub/tsugumi.json", project),
                      SourceDoc("externals/sub/tsugumi.lock", old)})}};
  H::ProjectOperateRequestT operation;
  operation.project_id = "p";
  operation.op_id = "update";
  H::ProjectUpdateNixPathLockT update;
  update.name = "used";
  operation.body.Set(update);
  core.Execute(operation);
  auto change = core.ReadChange("SetProjectLock");
  EXPECT_EQ(json::Text(change, "path"), "tsugumi.lock");
  EXPECT_EQ(json::Text(change, "base_hash"), crypto::sha256_hex(old));
  EXPECT_EQ(core.calls, (std::vector<std::string>{"github:org/repo"}));
  EXPECT_EQ(json::Text(change, "json").find("stale"), std::string::npos);
  update.project_path = "externals/sub";
  update.name.clear();
  operation.body.Set(update);
  core.Execute(operation);
  change = core.ReadChange("SetProjectLock");
  EXPECT_EQ(json::Text(change, "path"), "externals/sub/tsugumi.lock");
  EXPECT_EQ(json::Text(change, "json").find("stale"), std::string::npos);
  EXPECT_NE(json::Text(change, "json").find("sha256-new"), std::string::npos);
  core.fail_lock = true;
  EXPECT_THROW(core.Execute(operation), Failure);
  EXPECT_TRUE(core.save.empty());
}

TEST(NixPathOperation, KeepsMatchingEntriesAndResolvesChangedURLs) {
  const std::string project =
      R"({"nixPath":[{"name":"fixed","url":"https://example.com/a.tgz"},{"name":"changed","url":"github:org/repo/new-ref"},{"name":"missing","url":"git+https://example.com/repo"}]})";
  const std::string old =
      R"({"entries":[{"name":"fixed","url":"https://example.com/a.tgz","narHash":"sha256-keep"},{"name":"changed","url":"github:org/repo/old-ref","narHash":"sha256-outdated"}]})";
  FakeNixPathCore core;
  core.snapshot = {
      {"rev", "\"7\""},
      {"project_json", json::Quote(project)},
      {"sources", json::Encode(std::vector<std::string>{
                      SourceDoc("tsugumi.lock", old)})}};
  H::ProjectOperateRequestT operation;
  operation.project_id = "p";
  operation.op_id = "update";
  operation.body.Set(H::ProjectUpdateNixPathLockT{});
  core.Execute(operation);
  EXPECT_EQ(core.calls, (std::vector<std::string>{
                            "github:org/repo/new-ref",
                            "git+https://example.com/repo"}));
  const auto text = json::Text(core.ReadChange("SetProjectLock"), "json");
  EXPECT_NE(text.find("sha256-keep"), std::string::npos);
  EXPECT_EQ(text.find("sha256-outdated"), std::string::npos);
  EXPECT_NE(text.find("github:org/repo/new-ref"), std::string::npos);
}

TEST(NixPathOperation, ParamRewriteUsesTheConfirmedSourceAndRevision) {
  FakeNixPathCore core;
  const std::string source = "{ scale = 2.0; # keep\n}";
  core.snapshot = {
      {"rev", "\"7\""},
      {"project_json",
       R"("{\"canvas\":{\"cards\":[{\"id\":\"a\"}],\"connections\":[]}}")"},
      {"definitions",
       R"([{"id":"a","card_json":"{\"type\":\"code\",\"symbol\":\"chevron.left.forwardslash.chevron.right\",\"inputs\":[],\"maxRuns\":1,\"reuseWorker\":false,\"timeoutSeconds\":0,\"buildTimeoutSeconds\":0,\"recordLogs\":\"project\",\"recordIO\":\"project\",\"sandbox\":{\"filesystem\":true,\"pid\":true,\"ipc\":true,\"identity\":true,\"limits\":{\"cpu\":0,\"memoryMiB\":0}},\"grants\":{\"network\":false,\"gui\":false},\"system\":\"aarch64-linux\",\"workerCapabilities\":[]}"}])"},
      {"sources",
       json::Encode(std::vector<std::string>{json::Encode(json::Members{
           {"path", json::Quote("definitions/a/params.nix")},
           {"hash", json::Quote(crypto::sha256_hex(source))}})})}};
  H::ProjectOperateRequestT operation;
  operation.project_id = "p";
  operation.op_id = "edit";
  H::ProjectRewriteParamLiteralT set;
  set.card = std::make_unique<Tsugumi::Data::CardPathT>();
  set.card->segments = {"a"};
  set.name = "scale";
  set.expected_rev = 7;
  set.literal = "10.0";
  operation.body.Set(set);
  core.Execute(operation);
  const auto change = core.ReadChange("PutSource");
  EXPECT_EQ(json::Text(change, "path"), "definitions/a/params.nix");
  EXPECT_EQ(json::Text(change, "base_hash"), crypto::sha256_hex(source));
  EXPECT_TRUE(core.ReadChange("PutDefinition").empty());

  core.stale_param_hash = true;
  const auto conflict = json::Object(core.Execute(operation).json);
  EXPECT_EQ(json::Text(json::Object(conflict.at("value")), "kind"),
            "source_changed");
  EXPECT_TRUE(core.save.empty());
  core.stale_param_hash = false;

  core.fail_param = true;
  EXPECT_THROW(core.Execute(operation), Failure);
  EXPECT_TRUE(core.save.empty());
  core.fail_param = false;

  set.expected_rev = 6;
  operation.body.Set(set);
  EXPECT_THROW(core.Execute(operation), Failure);
  EXPECT_TRUE(core.save.empty());
}

TEST(NixPathOperation, CardCopyDoesNotCarryPinsOrBlockOnNames) {
  const std::string source =
      R"({"nixPath":[{"name":"used","url":"github:org/repo"}],"canvas":{"cards":[{"id":"source"}],"connections":[]}})";
  const std::string pin =
      R"({"entries":[{"name":"used","url":"github:org/repo","rev":"0123456789012345678901234567890123456789","narHash":"sha256-old"}]})";
  const std::string destination =
      R"({"nixPath":[{"name":"used","url":"github:other/repo"}],"canvas":{"cards":[],"connections":[]}})";
  FakeNixPathCore core;
  core.snapshot = {
      {"rev", "\"7\""},
      {"project_json", json::Quote(source)},
      {"definitions",
       R"([{"id":"source","card_json":"{\"type\":\"draft\",\"symbol\":\"pencil.and.outline\"}"}])"},
      {"sources", json::Encode(std::vector<std::string>{
                      SourceDoc("tsugumi.lock", pin),
                      SourceDoc("externals/sub/tsugumi.json", destination)})}};
  H::ProjectOperateRequestT operation;
  operation.project_id = "p";
  operation.op_id = "copy";
  H::ProjectCopyT copy;
  copy.project_path = "externals/sub";
  auto item = std::make_unique<H::ProjectCopyItemT>();
  item->from = "definitions/source/";
  item->id = "copy";
  copy.items.push_back(std::move(item));
  operation.body.Set(copy);

  EXPECT_EQ(json::Text(json::Object(core.Execute(operation).json), "reply"),
            "SaveResult");
  EXPECT_TRUE(core.ReadChange("SetProjectLock").empty());
  EXPECT_TRUE(core.calls.empty());
}

auto WirePort(const std::string& from, const std::string& to,
              const std::string& from_port, const std::string& to_port,
              const std::string& side, const std::string& value)
    -> H::ProjectFieldEditT {
  auto edit = Field({"canvas", "connections"},
                    json::Quote(side == "from" ? from_port : to_port),
                    json::Quote(value));
  auto match = std::make_unique<H::ProjectSelectorT>();
  match->match_json = json::Encode(json::Members{
      {"from", json::Encode(json::Members{{"card", json::Quote(from)},
                                          {"port", json::Quote(from_port)}})},
      {"to", json::Encode(json::Members{{"card", json::Quote(to)},
                                        {"port", json::Quote(to_port)}})}});
  edit.selector.push_back(std::move(match));
  for (const auto& key : {side, std::string("port")}) {
    auto step = std::make_unique<H::ProjectSelectorT>();
    step->field = key;
    edit.selector.push_back(std::move(step));
  }
  return edit;
}
auto Add(H::ProjectEditsT& edits, H::ProjectFieldEditT field) -> void {
  auto entry = std::make_unique<H::ProjectEditT>();
  entry->value.Set(std::move(field));
  edits.edits.push_back(std::move(entry));
}
TEST(ProjectEdit, SeparateWireEndsBecomeOneAtomicProjectChange) {
  SaveBase base;
  base.snapshot["rev"] = "7";
  const std::string before =
      test::FixtureProjectDocument(
          R"({"canvas":{"cards":[{"id":"knn"},{"id":"compare"}],"connections":[{"from":{"card":"knn","port":""},"to":{"card":"compare","port":""}}]}})");
  base.files["tsugumi.json"] = before;
  base.files["definitions/knn/_card.json"] =
      R"({"type":"draft","symbol":"pencil.and.outline"})";
  H::ProjectEditsT edits;
  auto implement =
      Field({}, R"({"type":"draft","symbol":"pencil.and.outline"})",
            ::tsugumi::test::FixtureCodeCard());
  implement.file_path = "definitions/knn/_card.json";
  Add(edits, std::move(implement));
  Add(edits, WirePort("knn", "compare", "", "", "from", "result"));
  Add(edits, WirePort("knn", "compare", "result", "", "to", ""));
  const auto read = [](const auto&) -> std::optional<std::string> {
    return {};
  };
  const auto changes =
      json::Array(PrepareProjectEdits(edits, base, read).at("changes"));
  ASSERT_EQ(changes.size(), 2u);
  const auto change = json::Object(changes.front());
  EXPECT_EQ(json::Text(change, "value_type"), "SetProject");
  const auto project =
      json::Text(json::Object(json::Get(change, "value")), "json");
  const auto canvas = json::Object(json::Get(json::Object(project), "canvas"));
  const auto wire = json::Object(json::Array(canvas.at("connections")).front());
  EXPECT_EQ(json::Text(json::Object(wire.at("from")), "port"), "result");
  EXPECT_EQ(json::Text(json::Object(wire.at("to")), "port"), "");
  EXPECT_EQ(base.files.at("tsugumi.json"), before);
  // Validate the final endpoints: the code end is bound while compare stays
  // a Draft. No intermediate one-end document is sent to core.
  Document doc;
  doc.project_json = project;
  doc.project_buffer = parse_project_json(project).buffer;
  for (const auto& [id, raw] :
       {std::pair{
            "knn",
            "{\"type\":\"code\",\"symbol\":\"chevron.left.forwardslash.chevron."
            "right\",\"inputs\":[],\"maxRuns\":1,\"reuseWorker\":false,"
            "\"timeoutSeconds\":0,\"buildTimeoutSeconds\":0,\"recordLogs\":"
            "\"project\",\"recordIO\":\"project\",\"sandbox\":{\"filesystem\":"
            "true,\"pid\":true,\"ipc\":true,\"identity\":true,\"limits\":{"
            "\"cpu\":0,\"memoryMiB\":0}},\"grants\":{\"network\":false,\"gui\":"
            "false},\"system\":\"aarch64-linux\",\"workerCapabilities\":[]}"},
        {"compare",
         "{\"type\":\"draft\",\"symbol\":\"pencil.and.outline\"}"}}) {
    DefinitionDoc definition;
    definition.card_json = raw;
    definition.card_buffer = parse_card_json(id, raw).buffer;
    doc.definitions[id] = std::move(definition);
  }
  doc.sources["definitions/knn/_outputs.fbs"] = {"test-outputs", 0};
  const auto resolved =
      resolve(doc, [](const auto& path) -> std::optional<std::string> {
        if (path == "definitions/knn/_outputs.fbs")
          return "table Result { value:string; } table Outputs { "
                 "result:Result; } root_type Outputs;";
        return {};
      });
  for (const auto& diagnostic : resolved->all_diagnostics(false))
    EXPECT_NE(diagnostic.code, "unknown_port") << diagnostic.message;
  ASSERT_EQ(resolved->edges.size(), 1u);

  H::ProjectOperateRequestT request;
  request.project_id = "p";
  request.op_id = "d7f52370-d5f6-4c4f-8a1f-546eb93090f1";
  request.body.Set(edits);
  int saves = 0;
  const auto call = [&](const std::string& method, const json::Members& args) {
    json::Members value;
    std::string reply;
    if (method == "operation_get") {
      reply = "OperationResult";
      value = {{"state", "\"unknown\""}};
    } else if (method == "project_inspect") {
      reply = "ProjectSnapshot";
      value = {
          {"rev", "\"7\""},
          {"project_json", json::Quote(before)},
          {"layout", "\"{}\""},
          {"definitions",
           R"([{"id":"knn","card_json":"{\"type\":\"draft\",\"symbol\":\"pencil.and.outline\"}"},{"id":"compare","card_json":"{\"type\":\"draft\",\"symbol\":\"pencil.and.outline\"}"}])"}};
    } else {
      EXPECT_EQ(method, "project_save");
      ++saves;
      for (const auto& raw : json::Array(args.at("changes"))) {
        const auto change = json::Object(raw);
        if (json::Text(change, "value_type") == "SetProject")
          EXPECT_EQ(json::Text(json::Object(change.at("value")), "json"),
                    project);
      }
      reply = "SaveResult";
      value = {{"rev", "\"8\""}};
    }
    return OperationReply{
        json::Encode(json::Members{{"reply", json::Quote(reply)},
                                   {"value", json::Encode(value)}}),
        {}};
  };
  const auto saved =
      ProjectOperations().Execute(request, {"ui", {}, {}, {}}, call);
  EXPECT_EQ(json::Text(json::Object(saved.json), "reply"), "SaveResult");
  EXPECT_EQ(saves, 1);

  edits.edits.back()->value.AsProjectFieldEdit()->old_value->json = "\"stale\"";
  request.body.Set(edits);
  // UI conflicts are typed replies; Agent conflicts still throw. Neither
  // origin may send a partially applied wire edit to core.
  const auto conflict =
      ProjectOperations().Execute(request, {"ui", {}, {}, {}}, call);
  const auto envelope = json::Object(conflict.json);
  ASSERT_EQ(json::Text(envelope, "reply"), "Failure");
  const auto value = json::Object(envelope.at("value"));
  EXPECT_EQ(json::Text(value, "kind"), "value_conflict");
  EXPECT_EQ(saves, 1);
  try {
    ProjectOperations().Execute(request, {"agent", {}, {}, {}}, call);
    FAIL() << "stale second end must refuse the entire plan";
  } catch (const Failure& failure) {
    EXPECT_EQ(failure.code, "value_conflict");
  }
  EXPECT_EQ(saves, 1);
  EXPECT_EQ(base.files.at("tsugumi.json"), before);
}
TEST(ProjectEdit, BindsOnlyAnUnambiguousUnmentionedInputOnDraftImplementation) {
  for (const auto& variant : {"single", "explicit", "multiple_inputs",
                              "multiple_wires", "already_code", "nested"}) {
    SCOPED_TRACE(variant);
    const std::string prefix =
        std::string(variant) == "nested" ? "externals/work/" : "";
    SaveBase base;
    base.snapshot["rev"] = "7";
    base.files[prefix + "tsugumi.json"] =
        R"({ "canvas": {"cards":[{"id":"knn"},{"id":"compare"}],"connections":[{"from":{"card":"knn","port":"out"},"to":{"card":"compare","port":""}}]} })";
    const auto definition = prefix + "definitions/compare/_card.json";
    base.files[definition] =
        std::string(variant) == "already_code"
            ? ::tsugumi::test::FixtureCodeCard()
            : R"({"type":"draft","symbol":"pencil.and.outline"})";
    H::ProjectEditsT edits;
    auto implement =
        Field({}, base.files.at(definition),
              std::string(variant) == "multiple_inputs"
                  ? ::tsugumi::test::FixtureCodeCard(
                        R"({"inputs":[{"name":"source"},{"name":"other"}]})")
                  : ::tsugumi::test::FixtureCodeCard(
                        R"({"inputs":[{"name":"source"}]})"));
    implement.file_path = definition;
    Add(edits, std::move(implement));
    if (std::string(variant) == "explicit")
      Add(edits, WirePort("knn", "compare", "out", "", "to", ""));
    if (std::string(variant) == "multiple_wires") {
      auto project = json::Object(base.files.at("tsugumi.json"));
      auto canvas = json::Object(project.at("canvas"));
      auto wires = json::Array(canvas.at("connections"));
      wires.push_back(wires.front());
      canvas["connections"] = json::Encode(wires);
      project["canvas"] = json::Encode(canvas);
      base.files["tsugumi.json"] = json::Encode(project);
    }
    const auto plan = PrepareProjectEdits(
        edits, base,
        [](const auto&) -> std::optional<std::string> { return {}; });
    bool bound = false;
    for (const auto& raw : json::Array(plan.at("changes"))) {
      const auto change = json::Object(raw);
      const auto value = json::Object(change.at("value"));
      if (json::Text(change, "value_type") == "SetProject" ||
          json::Text(value, "path") == prefix + "tsugumi.json") {
        std::string project = json::Text(value, "json");
        if (project.empty()) {
          ASSERT_TRUE(crypto::base64_decode(
              json::Text(json::Object(value.at("bytes")), "base64"), project));
        }
        const auto canvas =
            json::Object(json::Get(json::Object(project), "canvas"));
        const auto wire =
            json::Object(json::Array(canvas.at("connections")).front());
        bound = json::Text(json::Object(wire.at("to")), "port") == "source";
      }
    }
    EXPECT_EQ(bound, std::string(variant) == "single" ||
                         std::string(variant) == "nested");
  }
}
TEST(ProjectEditDiagnostics, NamesTheWireEndAndTheDefinitionFieldsToWrite) {
  for (const auto& scenario : {"draft", "missing_input", "unknown_param"}) {
    SCOPED_TRACE(scenario);
    Document doc;
    doc.project_json =
        test::FixtureProjectDocument(
            R"({"canvas":{"cards":[{"id":"knn"},{"id":"compare"}],"connections":[{"from":{"card":"knn","port":"out"},"to":{"card":"compare","port":"source"}}]}})");
    doc.project_buffer = parse_project_json(doc.project_json).buffer;
    const auto add = [&](const std::string& id, const std::string& raw) {
      DefinitionDoc definition;
      definition.card_json = raw;
      definition.card_buffer = parse_card_json(id, raw).buffer;
      doc.definitions[id] = std::move(definition);
    };
    add("knn", R"({"type":"trigger","symbol":"play.circle"})");
    add("compare", std::string(scenario) == "draft"
                       ? R"({"type":"draft","symbol":"pencil.and.outline"})"
                   : std::string(scenario) == "unknown_param"
                       ? ::tsugumi::test::FixtureCodeCard(
                             R"({"inputs":[{"name":"source"}]})")
                       : ::tsugumi::test::FixtureCodeCard());
    if (std::string_view(scenario) == "unknown_param")
      doc.sources["definitions/compare/params.nix"] = {};
    const auto resolved = resolve(
        doc, [](const auto&) -> std::optional<std::string> { return "{ k = []; }"; },
        [](const auto&, auto) -> std::string {
          throw std::runtime_error("empty lists are not allowed");
        });
    bool found = false;
    for (const auto& diagnostic : resolved->all_diagnostics(false)) {
      if (diagnostic.code != (std::string(scenario) == "unknown_param"
                                  ? "invalid_param"
                                  : "unknown_port"))
        continue;
      ASSERT_TRUE(diagnostic.card);
      EXPECT_EQ(*diagnostic.card, CardPath{"compare"});
      EXPECT_FALSE(diagnostic.message.empty());
      found = true;
    }
    EXPECT_TRUE(found);
  }
}
TEST(ProjectEdit, PreservesEveryUntouchedByteIncludingNixPath) {
  const std::string before =
      "{\n  \"name\": \"before\",\n"
      "  \"nixPath\": [ {\"name\": \"science\", \"url\": "
      "\"github:org/science\"} ],\n"
      "  \"canvas\": {\"cards\":[],\"connections\":[]}\n}\n";

  auto edit = Field({"name"}, "\"before\"", "\"after\"");
  auto expected = before;
  expected.replace(expected.find("before"), 6, "after");
  EXPECT_EQ(EditProjectField(before, edit), expected);
}
TEST(ProjectEdit, PolicyNormalizationKeepsUntouchedArrayElementsAndFields) {
  const std::string before =
      R"({ "comments": [ { "id": "a", "text": "keep" }, { "id": "b", "resolved": false } ], "hash": "sha256-exact=" })";
  const std::string normalized =
      R"({"comments":[{"id":"a","text":"keep"},{"id":"b","resolved":true}],"hash":"sha256-exact="})";
  auto expected = before;
  expected.replace(expected.find("false"), 5, "true");
  EXPECT_EQ(PreserveProjectBytes(before, normalized), expected);
}
TEST(ProjectEdit, KeyedSelectorsIgnoreArrayPositionsAndRefuseAmbiguity) {
  auto edit = Field({"cards"}, "1", "2");
  auto key = std::make_unique<H::ProjectSelectorT>();
  key->match_json = R"({"id":"b"})";
  edit.selector.push_back(std::move(key));
  auto field = std::make_unique<H::ProjectSelectorT>();
  field->field = "value";
  edit.selector.push_back(std::move(field));
  EXPECT_EQ(
      EditProjectField(
          R"({"cards":[{"id":"a","value":1},{"id":"b","value":1}]})", edit),
      R"({"cards":[{"id":"a","value":1},{"id":"b","value":2}]})");
  EXPECT_THROW(
      EditProjectField(
          R"({"cards":[{"id":"b","value":1},{"id":"b","value":1}]})", edit),
      Failure);
}
TEST(ProjectEdit, MissingNullEmptyAndGuardedRemovalAreDistinct) {
  auto edit = Field({"value"}, "null", "\"\"");
  EXPECT_THROW(EditProjectField("{}", edit), Failure);
  EXPECT_EQ(EditProjectField("{\"value\": null, \"other\":1}", edit),
            "{\"value\": \"\", \"other\":1}");
  edit.new_value->present = false;
  EXPECT_EQ(EditProjectField("{\"value\": null, \"other\":1}", edit),
            "{\"other\":1}");
  EXPECT_EQ(EditProjectField("{\"other\":1, \"value\": null}", edit),
            "{\"other\":1}");
  edit.old_value->present = false;
  edit.new_value->present = true;
  EXPECT_EQ(EditProjectField("{\"other\":1}", edit),
            "{\"other\":1,\"value\":\"\"}");
}
TEST(ProjectEdit, SourceReplacementRequiresTheExactMatchCountAndIsOnePlan) {
  H::ProjectEditsT edits;
  H::ProjectTextEditT text;
  text.file_path = "definitions/a/main.py";
  text.old_string = "old";
  text.new_string = "new";
  auto entry = std::make_unique<H::ProjectEditT>();
  entry->value.Set(text);
  edits.edits.push_back(std::move(entry));
  SaveBase base;
  base.snapshot["rev"] = "\"7\"";
  auto read = [](const auto&) -> std::optional<std::string> {
    return "old old";
  };
  EXPECT_THROW(PrepareProjectEdits(edits, base, read), Failure);
  edits.edits.front()->value.AsProjectTextEdit()->expected_matches = 2;
  auto plan = PrepareProjectEdits(edits, base, read);
  auto changes = json::Array(plan.at("changes"));
  ASSERT_EQ(changes.size(), 1u);
  auto value = json::Object(json::Get(json::Object(changes.front()), "value"));
  EXPECT_EQ(json::Text(value, "base_hash"), crypto::sha256_hex("old old"));
  EXPECT_EQ(json::Text(value, "bytes"), "");  // binary-safe base64 transport
  EXPECT_TRUE(json::Object(json::Get(value, "bytes")).contains("base64"));
}
TEST(ProjectEdit, FileMoveGuardsBothEndsAndPreservesBinaryBytes) {
  H::ProjectEditsT edits;
  H::ProjectFileMoveT move;
  move.file_path = "definitions/a/from.bin";
  move.to = "definitions/a/to.bin";
  const std::string bytes("\0\xff\n", 3);
  move.expected_hash = crypto::sha256_hex(bytes);
  move.destination_missing = true;
  auto entry = std::make_unique<H::ProjectEditT>();
  entry->value.Set(move);
  edits.edits.push_back(std::move(entry));
  SaveBase base;
  base.snapshot["rev"] = "\"0\"";
  auto read = [&](const std::string& path) -> std::optional<std::string> {
    return path == move.file_path ? std::optional(bytes) : std::nullopt;
  };
  EXPECT_EQ(
      json::Array(PrepareProjectEdits(edits, base, read).at("changes")).size(),
      2u);
  EXPECT_THROW(
      PrepareProjectEdits(edits, base,
                          [](const auto&) -> std::optional<std::string> {
                            return "concurrently replaced";
                          }),
      Failure);
}
TEST(ProjectEdit, AuthorizesEveryReadBeforeMaterializingIt) {
  H::ProjectEditsT edits;
  H::ProjectFileRemoveT remove;
  remove.file_path = "definitions/private/data";
  remove.expected_hash = "hash";
  auto entry = std::make_unique<H::ProjectEditT>();
  entry->value.Set(remove);
  edits.edits.push_back(std::move(entry));
  bool read = false;
  EXPECT_THROW(PrepareProjectEdits(
                   edits, {},
                   [&](const auto&) -> std::optional<std::string> {
                     read = true;
                     return "secret";
                   },
                   [](const auto&) {
                     throw Failure("permission_denied", "outside read set");
                   }),
               Failure);
  EXPECT_FALSE(read);
}
TEST(ProjectEdit, FirstCommentUndoAndRedoAddressItsId) {
  const std::string path = "definitions/a/_comments.json";
  const std::string first = R"({"comments":[{"id":"c","text":"first"}]})";
  const auto saved_comments = [](const json::Members& plan) {
    const auto change = json::Object(json::Array(plan.at("changes")).at(0));
    const auto value = json::Object(change.at("value"));
    return json::Text(value, "comments_json");
  };
  const auto comments = [](const std::string& text) {
    return json::Array(json::Object(text).at("comments"));
  };
  SaveBase base;
  base.snapshot["rev"] = "0";
  base.files["definitions/a/_card.json"] =
      R"({"type":"draft","symbol":"pencil.and.outline"})";
  const auto read = [](const auto&) -> std::optional<std::string> {
    return {};
  };
  H::ProjectJSONEditT create;
  create.file_path = path;
  create.old_value = std::make_unique<H::ProjectValueT>();
  create.new_value = std::make_unique<H::ProjectValueT>();
  create.new_value->present = true;
  create.new_value->json = first;
  H::ProjectEditsT edits;
  auto entry = std::make_unique<H::ProjectEditT>();
  entry->value.Set(std::move(create));
  edits.edits.push_back(std::move(entry));
  const auto plan = PrepareProjectEdits(edits, base, read);
  const auto undo = ReverseProjectChanges(plan, base, read);
  ASSERT_EQ(undo.edits.size(), 1u);
  const auto* remove = undo.edits.front()->value.AsProjectFieldEdit();
  ASSERT_NE(remove, nullptr);
  ASSERT_EQ(remove->selector.size(), 2u);
  EXPECT_EQ(remove->selector[0]->field, "comments");
  EXPECT_EQ(json::Text(json::Object(remove->selector[1]->match_json), "id"), "c");
  base.files[path] = first;
  const auto undone = PrepareProjectEdits(undo, base, read);
  const auto empty = saved_comments(undone);
  EXPECT_TRUE(comments(empty).empty());
  const auto redo = ReverseProjectChanges(undone, base, read);
  ASSERT_EQ(redo.edits.size(), 1u);
  base.files[path] = empty;
  const auto restored =
      comments(saved_comments(PrepareProjectEdits(redo, base, read)));
  ASSERT_EQ(restored.size(), 1u);
  const auto comment = json::Object(restored.front());
  EXPECT_EQ(json::Text(comment, "id"), "c");
  EXPECT_EQ(json::Text(comment, "text"), "first");

  base.files[path] =
      R"({"comments":[{"id":"c","text":"first"},{"id":"later","text":"another"}]})";
  const auto remaining =
      comments(saved_comments(PrepareProjectEdits(undo, base, read)));
  ASSERT_EQ(remaining.size(), 1u);
  EXPECT_EQ(json::Text(json::Object(remaining.front()), "id"), "later");
}

TEST(ProjectEdit, LastCommentDeletesMetadataWithAFileGuard) {
  const std::string path = "definitions/a/_comments.json";
  const std::string meta = R"({"comments":[{"id":"c","text":"last"}]})";
  SaveBase base;
  base.snapshot["rev"] = "0";
  base.files["definitions/a/_card.json"] =
      R"({"type":"draft","symbol":"pencil.and.outline"})";
  base.files[path] = "\n  " + meta + "\n";
  H::ProjectJSONEditT remove;
  remove.file_path = path;
  remove.old_value = std::make_unique<H::ProjectValueT>();
  remove.old_value->present = true;
  remove.old_value->json = meta;
  remove.new_value = std::make_unique<H::ProjectValueT>();
  H::ProjectEditsT edits;
  auto entry = std::make_unique<H::ProjectEditT>();
  entry->value.Set(std::move(remove));
  edits.edits.push_back(std::move(entry));
  const auto read = [](const auto&) -> std::optional<std::string> {
    return {};
  };
  const auto plan = PrepareProjectEdits(edits, base, read);
  const auto changes = json::Array(plan.at("changes"));
  ASSERT_EQ(changes.size(), 1u);
  const auto definition =
      json::Object(json::Get(json::Object(changes[0]), "value"));
  EXPECT_FALSE(definition.contains("comments_json"));
  EXPECT_EQ(json::Text(definition, "card_json"),
            base.files.at("definitions/a/_card.json"));
  base.files[path] = R"({"comments":[{"id":"c","text":"changed"}]})";
  try {
    PrepareProjectEdits(edits, base, read);
    FAIL() << "Stale document deletion was accepted";
  } catch (const Failure& failure) {
    EXPECT_EQ(failure.code, "value_conflict");
  }
}

TEST(ProjectEdit, HostConflictsUseTheTypedOperationOutcome) {
  for (const auto kind : {Tsugumi::Data::FailureKind::value_conflict,
                          Tsugumi::Data::FailureKind::source_changed}) {
    H::ProjectEditsT edits;
    auto entry = std::make_unique<H::ProjectEditT>();
    if (kind == Tsugumi::Data::FailureKind::value_conflict) {
      auto field = Field({"comments"}, "[]", "[]");
      field.file_path = "definitions/a/_comments.json";
      entry->value.Set(std::move(field));
    } else {
      H::ProjectFileRemoveT remove;
      remove.file_path = "definitions/a/_comments.json";
      remove.expected_hash = crypto::sha256_hex("stale");
      entry->value.Set(std::move(remove));
    }
    edits.edits.push_back(std::move(entry));
    H::ProjectOperateRequestT request;
    request.project_id = "p";
    request.op_id = "d7f52370-d5f6-4c4f-8a1f-546eb93090f1";
    request.body.Set(std::move(edits));
    const auto call = [](const std::string& method, const json::Members&) {
      if (method == "operation_get")
        return OperationReply{
            R"({"reply":"OperationResult","value":{"state":"unknown"}})", {}};
      EXPECT_EQ(method, "project_inspect");
      return OperationReply{
          R"({"reply":"ProjectSnapshot","value":{"rev":0,"project_json":"{}","layout":[],"definitions":[{"id":"a","card_json":"{\"type\":\"code\",\"symbol\":\"chevron.left.forwardslash.chevron.right\",\"inputs\":[],\"maxRuns\":1,\"reuseWorker\":false,\"timeoutSeconds\":0,\"buildTimeoutSeconds\":0,\"recordLogs\":\"project\",\"recordIO\":\"project\",\"sandbox\":{\"filesystem\":true,\"pid\":true,\"ipc\":true,\"identity\":true,\"limits\":{\"cpu\":0,\"memoryMiB\":0}},\"grants\":{\"network\":false,\"gui\":false},\"system\":\"aarch64-linux\",\"workerCapabilities\":[]}","comments_json":"{\"comments\":[{\"id\":\"current\",\"text\":\"current\"}]}"}]}})",
          {}};
    };
    const auto conflict =
        ProjectOperations().Execute(request, {"ui", {}, {}, {}}, call);
    flatbuffers::Verifier verifier(
        reinterpret_cast<const uint8_t*>(conflict.wire.data()),
        conflict.wire.size());
    ASSERT_TRUE(Tsugumi::RPC::VerifyMessageBuffer(verifier));
    const auto* failure = Tsugumi::RPC::GetMessage(conflict.wire.data())
                              ->payload_as_Reply()
                              ->value_as_Failure();
    ASSERT_NE(failure, nullptr);
    EXPECT_EQ(failure->kind(), kind);
    EXPECT_THROW(
        ProjectOperations().Execute(request, {"agent", {}, {}, {}}, call),
        Failure);
  }
}

}  // namespace
}  // namespace tsugumi::host
