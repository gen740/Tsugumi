// These policies moved from the project's comment/Draft RPC suites to agentd.
#include "../agent_fixture_server.hh"
#include "agent/catalog.hh"
#include "host/save.hh"
#include "util/crypto.hh"
#include <git2.h>
#include <gtest/gtest.h>

namespace tsugumi::host {
namespace {
namespace json = agent::json;
class AgentSavePolicy : public ::testing::Test {
protected:
  comments::Files files{
      {"definitions/plan/_card.json", R"({"template":"draft"})"},
      {"definitions/plan/_meta.json",
       R"({"description":"Counts words","content":"Split and count.","comments":[{"id":"c1","text":"Use a log scale","author":"user"},{"id":"c2","text":"Name the axis","author":"user"}]})"}};
  auto Read(const std::string &method, const json::Members &args)
      -> json::Members {
    if (method == "project_file_read") {
      EXPECT_EQ(json::String(json::Get(args, "at_rev")), "7");
      return {{"bytes", json::Quote(files.at(json::Text(args, "path")))}};
    }
    EXPECT_EQ(method, "project_inspect");
    std::map<std::string, json::Members> definitions;
    std::vector<std::string> sources;
    for (const auto &[path, text] : files) {
      if (path.starts_with("definitions/") &&
          (path.ends_with("/_card.json") || path.ends_with("/_meta.json") ||
           path.ends_with("/_link.json"))) {
        const auto slash = path.find('/', 12);
        const auto id = path.substr(12, slash - 12),
                   file = path.substr(slash + 1);
        auto &d = definitions[id];
        d["id"] = json::Quote(id);
        d[file == "_card.json"   ? "card_json"
          : file == "_meta.json" ? "meta_json"
                                 : "link_json"] = json::Quote(text);
      } else
        sources.push_back(json::Encode(
            json::Members{{"path", json::Quote(path)},
                          {"hash", json::Quote(crypto::sha256_hex(text))}}));
    }
    std::vector<std::string> defs;
    for (const auto &[id, d] : definitions)
      defs.push_back(json::Encode(d));
    return {{"rev", json::Quote("7")},
            {"definitions", json::Encode(defs)},
            {"sources", json::Encode(sources)}};
  }
  auto Put(const std::string &meta,
           const std::string &body = R"({"template":"sink"})",
           const std::string &id = "plan") -> std::string {
    return json::Encode(json::Members{
        {"value_type", json::Quote("PutDefinition")},
        {"value",
         json::Encode(json::Members{{"id", json::Quote(id)},
                                    {"card_json", json::Quote(body)},
                                    {"meta_json", json::Quote(meta)}})}});
  }
  auto Prepare(const std::vector<std::string> &changes) -> AgentSave {
    return PrepareAgentSave(
        {{"base_rev", json::Quote("7")}, {"changes", json::Encode(changes)}},
        [&](const auto &method, const auto &args) {
          return Read(method, args);
        });
  }
  auto Meta(const AgentSave &saved, size_t i = 0) -> json::Members {
    const auto c = json::Object(json::Get(
        json::Object(json::Array(saved.arguments.at("changes"))[i]), "value"));
    return json::Object(json::Text(c, "meta_json"));
  }
  auto Comments(const AgentSave &saved, size_t i = 0)
      -> std::vector<json::Members> {
    std::vector<json::Members> out;
    for (const auto &raw :
         json::Array(json::Get(Meta(saved, i), "comments", "[]")))
      out.push_back(json::Object(raw));
    return out;
  }
  auto Implement(const json::Members &extra = {}) -> std::string {
    json::Members value{{"id", json::Quote("plan")},
                        {"card_json", json::Quote(R"({"template":"sink"})")},
                        {"keep_meta", "true"}};
    for (const auto &[k, v] : extra)
      value[k] = v;
    return json::Encode(
        json::Members{{"value_type", json::Quote("PutDefinition")},
                      {"value", json::Encode(value)}});
  }
  auto Sink() -> void {
    files["definitions/plan/_card.json"] = R"({"template":"sink"})";
  }
};
TEST_F(AgentSavePolicy, DecimalStringRevisionsAreCheckedBeforePolicy) {
  EXPECT_THROW(
      PrepareAgentSave({{"base_rev", json::Quote("8")}, {"changes", "[]"}},
                       [&](const auto &method, const auto &args) {
                         return Read(method, args);
                       }),
      SaveRevisionConflict);
}
TEST_F(AgentSavePolicy, DraftIntentIsKeptAndOnlyEmptyFieldsAreFilled) {
  EXPECT_THROW(Prepare({Implement()}), std::runtime_error);
  const auto saved =
      Prepare({Implement({{"fill_description", json::Quote("Ignored")},
                          {"fill_context", json::Quote("For the report.")}})});
  EXPECT_EQ(json::Text(Meta(saved), "description"), "Counts words");
  EXPECT_EQ(json::Text(Meta(saved), "content"), "Split and count.");
  EXPECT_EQ(json::Text(Meta(saved), "context"), "For the report.");
  EXPECT_EQ(Comments(saved).size(), 2u);
  const auto value = json::Object(json::Get(
      json::Object(json::Array(saved.arguments.at("changes"))[0]), "value"));
  EXPECT_FALSE(value.contains("fill_context"));
  EXPECT_FALSE(value.contains("fill_description"));
}
TEST_F(AgentSavePolicy, ImplementingDraftAsCardInCardAlsoKeepsItsIntent) {
  json::Members link{{"id", json::Quote("plan")},
                     {"card_json", json::Quote("")},
                     {"link_json", json::Quote(R"({"external":"library"})")},
                     {"keep_meta", "true"}};
  auto change = [&] {
    return json::Encode(
        json::Members{{"value_type", json::Quote("PutDefinition")},
                      {"value", json::Encode(link)}});
  };
  EXPECT_THROW(Prepare({change()}), std::runtime_error);
  link["fill_context"] = json::Quote("User requested a nested workflow");
  EXPECT_NO_THROW(Prepare({change()}));
}
TEST_F(AgentSavePolicy, NonblankIntentCannotBeChangedOrDropped) {
  for (const auto *field : {"description", "content", "context"}) {
    files["definitions/plan/_meta.json"] =
        R"({"description":"Summary","content":"Plan","context":"Reason"})";
    auto meta = json::Object(files.at("definitions/plan/_meta.json"));
    meta[field] = json::Quote("Different");
    EXPECT_THROW(Prepare({Put(json::Encode(meta))}), std::runtime_error);
    meta.erase(field);
    EXPECT_THROW(Prepare({Put(json::Encode(meta))}), std::runtime_error);
  }
}
TEST_F(AgentSavePolicy,
       EmptyDraftNeedsDescriptionAndContextDependsOnItsExistingIntent) {
  files.erase("definitions/plan/_meta.json");
  EXPECT_THROW(Prepare({Implement()}), std::runtime_error);
  EXPECT_NO_THROW(
      Prepare({Implement({{"fill_description", json::Quote("Logs input")}})}));
  files["definitions/plan/_meta.json"] = R"({"content":"Plan"})";
  EXPECT_THROW(
      Prepare({Implement({{"fill_description", json::Quote("Logs input")}})}),
      std::runtime_error);
  EXPECT_NO_THROW(
      Prepare({Implement({{"fill_description", json::Quote("Logs input")},
                          {"fill_context", json::Quote("Reason")}})}));
}
TEST_F(AgentSavePolicy,
       DraftThatStaysDraftAndOrdinaryCardsNeedNoIntentCompletion) {
  EXPECT_NO_THROW(Prepare({Put("{}", R"({"template":"draft"})")}));
  Sink();
  EXPECT_NO_THROW(Prepare({Put("{}")}));
}
TEST_F(AgentSavePolicy, ResolutionRecordsWhoDidItAndTheChangedCardIds) {
  Sink();
  auto saved = Prepare({Put(
      R"({"comments":[{"id":"c1","text":"Use a log scale","author":"agent","resolved":true,"resolution":"Log y","stateBy":"user","cards":["fake"]},{"id":"c2","text":"Name the axis","author":"user"}]})",
      R"({"template":"sink","maxRuns":2})")});
  auto comments = Comments(saved);
  ASSERT_EQ(comments.size(), 2u);
  EXPECT_EQ(json::Text(comments[0], "author"), "user");
  EXPECT_EQ(json::Text(comments[0], "stateBy"), "agent");
  EXPECT_EQ(json::Array(json::Get(comments[0], "cards")),
            (std::vector<std::string>{json::Quote("plan")}));
  const auto record = json::Object(json::Array(saved.comments).front());
  EXPECT_EQ(json::Text(record, "card"), "plan");
  EXPECT_EQ(json::Text(record, "change"), "resolved");
  EXPECT_EQ(json::Text(record, "resolution"), "Log y");
}
TEST_F(AgentSavePolicy, MetadataOnlyResolutionNamesNoChangedCards) {
  Sink();
  auto saved = Prepare({Put(
      R"({"comments":[{"id":"c1","text":"Use a log scale","resolved":true}]})")});
  EXPECT_FALSE(Comments(saved).front().contains("cards"));
}
TEST_F(AgentSavePolicy, AddEditRemoveAndDuplicateIdsAreStamped) {
  Sink();
  auto saved = Prepare({Put(
      R"({"comments":[{"id":"c1","text":"Reworded","author":"agent"},{"id":"c1","text":"Twin","author":"user"},{"text":"New","author":"user","resolved":true,"stateBy":"user"}]})")});
  auto comments = Comments(saved);
  ASSERT_EQ(comments.size(), 3u);
  EXPECT_EQ(json::Text(comments[0], "editedBy"), "agent");
  EXPECT_EQ(json::Text(comments[0], "author"), "user");
  EXPECT_NE(json::Text(comments[0], "id"), json::Text(comments[1], "id"));
  EXPECT_FALSE(json::Text(comments[2], "id").empty());
  EXPECT_EQ(json::Text(comments[2], "author"), "agent");
  EXPECT_EQ(json::Text(comments[2], "stateBy"), "agent");
  std::set<std::string> changes;
  for (const auto &raw : json::Array(saved.comments))
    changes.insert(json::Text(json::Object(raw), "change"));
  EXPECT_EQ(changes,
            (std::set<std::string>{"added", "edited", "resolved", "removed"}));
}
TEST_F(AgentSavePolicy, ReopeningClearsResolutionCardsAndMovingKeepsIdentity) {
  Sink();
  files["definitions/plan/_meta.json"] =
      R"({"comments":[{"id":"c1","text":"Ask","author":"user","resolved":true,"stateBy":"user","cards":["plan"]}]})";
  auto saved = Prepare(
      {Put(R"({"comments":[{"id":"c1","text":"Ask","author":"user"}]})")});
  EXPECT_EQ(json::Text(Comments(saved)[0], "stateBy"), "agent");
  EXPECT_FALSE(Comments(saved)[0].contains("cards"));
  saved = Prepare({Put("{}"), Put(files.at("definitions/plan/_meta.json"),
                                  R"({"template":"sink"})", "split")});
  EXPECT_TRUE(json::Array(saved.comments).empty());
}
TEST_F(AgentSavePolicy,
       KeepingMetadataAndDeletingCardsPreserveTheExpectedRecord) {
  Sink();
  auto saved = Prepare({Implement()});
  EXPECT_TRUE(json::Array(saved.comments).empty());
  saved =
      Prepare({R"({"value_type":"RemoveDefinition","value":{"id":"plan"}})"});
  auto record = json::Array(saved.comments);
  ASSERT_EQ(record.size(), 2u);
  for (const auto &raw : record)
    EXPECT_EQ(json::Text(json::Object(raw), "change"), "removed");
}
TEST_F(AgentSavePolicy, PresentationFieldsDoNotDisableCommentStamping) {
  Sink();
  auto saved = Prepare({Put(
      R"({"name":false,"future":1,"comments":[{"text":"New","author":"user"}]})")});
  EXPECT_EQ(json::Get(Meta(saved), "name"), "false");
  EXPECT_EQ(json::Get(Meta(saved), "future"), "1");
  EXPECT_EQ(json::Text(Comments(saved)[0], "author"), "agent");
}
TEST_F(AgentSavePolicy, NestedMetadataPatchesAreStampedAndDraftIntentChecked) {
  files.clear();
  const std::string path = "externals/outer/definitions/plan/";
  files[path + "_card.json"] = R"({"template":"draft"})";
  files[path + "_meta.json"] = R"({"content":"Plan","description":"Summary"})";
  auto source = [&](const std::string &file, const std::string &bytes) {
    return json::Encode(json::Members{
        {"value_type", json::Quote("PutSource")},
        {"value", json::Encode(json::Members{
                      {"path", json::Quote(path + file)},
                      {"base_hash",
                       json::Quote(crypto::sha256_hex(files.at(path + file)))},
                      {"bytes", json::Quote(bytes)}})}});
  };
  EXPECT_THROW(Prepare({source("_card.json", R"({"template":"sink"})")}),
               std::runtime_error);
  EXPECT_NO_THROW(Prepare(
      {source("_card.json", R"({"template":"sink"})"),
       source(
           "_meta.json",
           R"({"content":"Plan","description":"Summary","context":"Why"})")}));
  files[path + "_meta.json"] =
      R"({"comments":[{"id":"c1","text":"Ask","author":"user"}]})";
  auto meta = files.at(path + "_meta.json");
  const auto offset = meta.find(",\"author\"");
  auto patch = json::Encode(json::Members{
      {"value_type", json::Quote("PatchSource")},
      {"value",
       json::Encode(json::Members{
           {"path", json::Quote(path + "_meta.json")},
           {"base_hash", json::Quote(crypto::sha256_hex(meta))},
           {"patches",
            json::Encode(std::vector<std::string>{json::Encode(json::Members{
                {"offset", json::Quote(std::to_string(offset))},
                {"remove", json::Quote("0")},
                {"text", json::Quote(",\"resolved\":true")}})})}})}});
  auto saved = Prepare({patch});
  const auto wrapper =
      json::Object(json::Array(saved.arguments.at("changes"))[0]);
  EXPECT_EQ(json::Text(wrapper, "value_type"), "PutSource");
  EXPECT_EQ(json::Text(json::Object(json::Array(saved.comments)[0]), "card"),
            "outer/plan");
  EXPECT_NE(json::Text(json::Object(json::Get(wrapper, "value")), "bytes")
                .find("stateBy"),
            std::string::npos);
}
TEST(AgentPresentation, StemDerivationIsOwnedByTheCaller) {
  EXPECT_EQ(CardStem("Aggregate Results!"), "aggregate-results");
  EXPECT_EQ(CardStem("集計"), "card");
  EXPECT_EQ(CardStem("---A___B---"), "a-b");
  EXPECT_LE(CardStem(std::string(500, 'a')).size(), 123u);
  const agent::Catalog catalog;
  EXPECT_NE(catalog.Find("card_id")->spec.input_schema.find("\"name\""),
            std::string::npos);
  EXPECT_EQ(catalog.Find("card_id")->spec.input_schema.find("\"stem\""),
            std::string::npos);
  EXPECT_NE(
      catalog.Find("project_save")->spec.input_schema.find("fill_description"),
      std::string::npos);
}
} // namespace
} // namespace tsugumi::host

#include "../agent_fixture_server.hh"
#include <git2.h>

namespace tsugumi::host {
namespace {
TEST(HostVersionComments,
     RecordsFirstParentChangesAndUsesCardIdsAcrossExternalDirectoryAliases) {
  test::TemporaryDirectory directory;
  git_libgit2_init();
  git_repository *repo = nullptr;
  ASSERT_EQ(git_repository_init(&repo, directory.path.c_str(), false), 0);
  struct Cleanup {
    git_repository *repo;
    ~Cleanup() {
      git_repository_free(repo);
      git_libgit2_shutdown();
    }
  } cleanup{repo};
  auto commit = [&](const char *message, const git_oid *parent) {
    git_index *index = nullptr;
    EXPECT_EQ(git_repository_index(&index, repo), 0);
    EXPECT_EQ(git_index_add_all(index, nullptr, 0, nullptr, nullptr), 0);
    EXPECT_EQ(git_index_write(index), 0);
    git_oid tree_id;
    EXPECT_EQ(git_index_write_tree(&tree_id, index), 0);
    git_index_free(index);
    git_tree *tree = nullptr;
    EXPECT_EQ(git_tree_lookup(&tree, repo, &tree_id), 0);
    git_signature *signature = nullptr;
    EXPECT_EQ(git_signature_now(&signature, "Test", "test@example.com"), 0);
    git_commit *previous = nullptr;
    if (parent)
      EXPECT_EQ(git_commit_lookup(&previous, repo, parent), 0);
    git_oid id;
    EXPECT_EQ(git_commit_create(&id, repo, "HEAD", signature, signature,
                                nullptr, message, tree, parent ? 1 : 0,
                                parent
                                    ? const_cast<const git_commit **>(&previous)
                                    : nullptr),
              0);
    git_commit_free(previous);
    git_signature_free(signature);
    git_tree_free(tree);
    return id;
  };
  test::Write(directory.path / "definitions/outer-id/_link.json",
              R"({"external":"library"})");
  const auto meta =
      directory.path / "externals/library/definitions/inner-id/_meta.json";
  test::Write(
      meta,
      R"({"future":1,"comments":[{"id":"c1","text":"Ask","author":"user"}]})");
  auto first = commit("First", nullptr);
  char id[GIT_OID_HEXSZ + 1];
  git_oid_tostr(id, sizeof id, &first);
  auto changes = comments::version(directory.path, id);
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].card, "outer-id/inner-id");
  EXPECT_EQ(changes[0].change, "added");
  EXPECT_EQ(changes[0].by, "user");
  test::Write(
      meta,
      R"({"name":false,"comments":[{"id":"c1","text":"Ask","author":"user","resolved":true,"stateBy":"agent","resolution":"Done","cards":["outer-id/inner-id"]}]})");
  auto next = commit("Resolve", &first);
  git_oid_tostr(id, sizeof id, &next);
  changes = comments::version(directory.path, id);
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].change, "resolved");
  EXPECT_EQ(changes[0].by, "agent");
  EXPECT_EQ(changes[0].resolution, "Done");
  EXPECT_EQ(changes[0].cards, (std::vector<std::string>{"outer-id/inner-id"}));
  std::filesystem::remove(meta);
  next = commit("Remove", &next);
  git_oid_tostr(id, sizeof id, &next);
  changes = comments::version(directory.path, id);
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].change, "removed");
  EXPECT_TRUE(changes[0].by.empty());
}
} // namespace
} // namespace tsugumi::host
