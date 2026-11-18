#include "host/project_edit.hh"

#include <gtest/gtest.h>

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
      R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"knn"},{"id":"compare"}],"connections":[{"from":{"card":"knn","port":""},"to":{"card":"compare","port":""}}]}})";
  base.files["tsugumi.json"] = before;
  base.files["definitions/knn/_card.json"] = R"({"template":"draft"})";
  H::ProjectEditsT edits;
  auto implement = Field({}, R"({"template":"draft"})", "{}");
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
  doc.project = parse_project_json(project);
  for (const auto& [id, raw] :
       {std::pair{"knn", "{}"}, {"compare", "{\"template\":\"draft\"}"}}) {
    DefinitionDoc definition;
    definition.card_json = raw;
    definition.card = parse_card_json(id, raw);
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
          {"layout_json", "\"{}\""},
          {"definitions",
           R"([{"id":"knn","card_json":"{\"template\":\"draft\"}"},{"id":"compare","card_json":"{\"template\":\"draft\"}"}])"}};
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
      ProjectOperations().Execute("p", request, {"ui", {}, {}, {}}, call);
  EXPECT_EQ(json::Text(json::Object(saved.json), "reply"), "SaveResult");
  EXPECT_EQ(saves, 1);

  edits.edits.back()->value.AsProjectFieldEdit()->old_value->json = "\"stale\"";
  request.body.Set(edits);
  // UI conflicts are typed replies; Agent conflicts still throw. Neither
  // origin may send a partially applied wire edit to core.
  const auto conflict =
      ProjectOperations().Execute("p", request, {"ui", {}, {}, {}}, call);
  const auto envelope = json::Object(conflict.json);
  ASSERT_EQ(json::Text(envelope, "reply"), "Failure");
  const auto value = json::Object(envelope.at("value"));
  EXPECT_EQ(json::Text(value, "kind"), "value_conflict");
  EXPECT_EQ(saves, 1);
  try {
    ProjectOperations().Execute("p", request, {"agent", {}, {}, {}}, call);
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
    base.files[definition] = std::string(variant) == "already_code"
                                 ? "{}"
                                 : R"({"template":"draft"})";
    H::ProjectEditsT edits;
    auto implement =
        Field({}, base.files.at(definition),
              std::string(variant) == "multiple_inputs"
                  ? R"({"inputs":[{"name":"source"},{"name":"other"}]})"
                  : R"({"inputs":[{"name":"source"}]})");
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
        R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[{"id":"knn"},{"id":"compare"}],"connections":[{"from":{"card":"knn","port":"out"},"to":{"card":"compare","port":"source"}}]}})";
    doc.project = parse_project_json(doc.project_json);
    const auto add = [&](const std::string& id, const std::string& raw) {
      DefinitionDoc definition;
      definition.card_json = raw;
      definition.card = parse_card_json(id, raw);
      doc.definitions[id] = std::move(definition);
    };
    add("knn", R"({"template":"trigger"})");
    add("compare",
        std::string(scenario) == "draft" ? R"({"template":"draft"})"
        : std::string(scenario) == "unknown_param"
            ? R"({"inputs":[{"name":"source"}],"params":[{"name":"k","type":"number","data":3}]})"
            : R"({"inputs":[]})");
    const auto resolved = resolve(
        doc, [](const auto&) -> std::optional<std::string> { return {}; });
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
TEST(ProjectEdit,
     PreservesEveryUntouchedByteIncludingPinsHashesAndUint64Spelling) {
  const std::string before =
      "{\n  \"name\": \"before\",\n  \"pkgsPath\": \"nixpkgs-stable\",\n"
      "  \"nixSources\": { \"pin\": {\"hash\": \"sha256-ExactBytes=\", "
      "\"rev\": 18446744073709551615} },\n"
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
TEST(ProjectEdit, MetadataCreationUndoHasARedo) {
  const std::string path = "definitions/a/_meta.json";
  const std::string meta = R"({"comments":[{"id":"c","text":"first"}]})";
  SaveBase base;
  base.snapshot["rev"] = "0";
  base.files["definitions/a/_card.json"] = R"({"template":"draft"})";
  const auto read = [](const auto&) -> std::optional<std::string> {
    return {};
  };
  H::ProjectJSONEditT create;
  create.file_path = path;
  create.old_value = std::make_unique<H::ProjectValueT>();
  create.new_value = std::make_unique<H::ProjectValueT>();
  create.new_value->present = true;
  create.new_value->json = meta;
  H::ProjectEditsT edits;
  auto entry = std::make_unique<H::ProjectEditT>();
  entry->value.Set(std::move(create));
  edits.edits.push_back(std::move(entry));
  const auto plan = PrepareProjectEdits(edits, base, read);
  const auto undo = ReverseProjectChanges(plan, base, read);
  ASSERT_EQ(undo.edits.size(), 1u);
  ASSERT_NE(undo.edits.front()->value.AsProjectFileRemove(), nullptr);
  base.files[path] = meta;
  const auto undone = PrepareProjectEdits(undo, base, read);
  const auto redo = ReverseProjectChanges(undone, base, read);
  ASSERT_EQ(redo.edits.size(), 1u);
  const auto* put = redo.edits.front()->value.AsProjectFilePut();
  ASSERT_NE(put, nullptr);
  EXPECT_EQ(put->file_path, path);
  EXPECT_TRUE(put->expect_missing);
  EXPECT_EQ(std::string(put->bytes.begin(), put->bytes.end()), meta);
  base.files.erase(path);
  const auto redone = PrepareProjectEdits(redo, base, read);
  const auto changes = json::Array(redone.at("changes"));
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(
      json::Text(json::Object(json::Get(json::Object(changes[0]), "value")),
                 "meta_json"),
      meta);
}

TEST(ProjectEdit, LastCommentDeletesMetadataWithAFileGuard) {
  const std::string path = "definitions/a/_meta.json";
  const std::string meta = R"({"comments":[{"id":"c","text":"last"}]})";
  SaveBase base;
  base.snapshot["rev"] = "0";
  base.files["definitions/a/_card.json"] = R"({"template":"draft"})";
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
  EXPECT_FALSE(definition.contains("meta_json"));
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
      auto field = Field({"content"}, "\"stale\"", "\"mine\"");
      field.file_path = "definitions/a/_meta.json";
      entry->value.Set(std::move(field));
    } else {
      H::ProjectFileRemoveT remove;
      remove.file_path = "definitions/a/_meta.json";
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
          R"({"reply":"ProjectSnapshot","value":{"rev":0,"project_json":"{}","layout_json":"{}","definitions":[{"id":"a","card_json":"{}","meta_json":"{}"}]}})",
          {}};
    };
    const auto conflict =
        ProjectOperations().Execute("p", request, {"ui", {}, {}, {}}, call);
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
        ProjectOperations().Execute("p", request, {"agent", {}, {}, {}}, call),
        Failure);
  }
}

}  // namespace
}  // namespace tsugumi::host
