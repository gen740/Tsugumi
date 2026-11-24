// These policies moved from the project's comment/Draft RPC suites to hostd.
#include "agent/save_policy.hh"

#include <gtest/gtest.h>

#include "../agent_fixture_server.hh"
#include "agent/catalog.hh"
#include "document_fixture.hh"
#include "host/failure.hh"
#include "host/save.hh"
#include "tsugumi/util/crypto.hh"

namespace tsugumi::host {
using agent::AgentSave;
using agent::PrepareAgentSave;
namespace {
namespace json = ::tsugumi::json;
class AgentSavePolicy : public ::testing::Test {
 protected:
  comments::Files files{
      {"definitions/plan/_card.json",
       R"({"type":"draft","symbol":"pencil.and.outline"})"},
      {"definitions/plan/README.md",
       "---\ntitle: Plan\ndescription: Counts words\n---\n\nSplit and count."},
      {"definitions/plan/_comments.json",
       R"({"comments":[{"id":"c1","text":"Use a log scale","author":"user"},{"id":"c2","text":"Name the axis","author":"user"}]})"}};
  auto Base() -> SaveBase {
    std::vector<std::string> sources;
    for (const auto& [path, text] : files)
      if (path.starts_with("externals/"))
        sources.push_back(json::Encode(
            json::Members{{"path", json::Quote(path)},
                          {"hash", json::Quote(crypto::sha256_hex(text))}}));
    return {{{"rev", json::Quote("7")}, {"sources", json::Encode(sources)}},
            files};
  }
  auto Put(const std::string& meta,
           const std::string& body = ::tsugumi::test::FixtureCodeCard(),
           const std::string& id = "plan") -> std::string {
    return json::Encode(json::Members{
        {"value_type", json::Quote("PutDefinition")},
        {"value",
         json::Encode(json::Members{{"id", json::Quote(id)},
                                    {"card_json", json::Quote(body)},
                                    {"comments_json", json::Quote(meta)}})}});
  }
  auto Prepare(const std::vector<std::string>& changes) -> AgentSave {
    return PrepareAgentSave(
        {{"base_rev", json::Quote("7")}, {"changes", json::Encode(changes)}},
        Base(), [&](const auto&, const auto& args) -> json::Members {
          EXPECT_EQ(json::Text(args, "at_rev"), "7");
          return {{"bytes", json::Quote(files.at(json::Text(args, "path")))}};
        });
  }
  auto Meta(const AgentSave& saved, size_t i = 0) -> json::Members {
    const auto c = json::Object(json::Get(
        json::Object(json::Array(saved.arguments.at("changes"))[i]), "value"));
    return json::Object(json::Text(c, "comments_json"));
  }
  auto Comments(const AgentSave& saved, size_t i = 0)
      -> std::vector<json::Members> {
    std::vector<json::Members> out;
    for (const auto& raw :
         json::Array(json::Get(Meta(saved, i), "comments", "[]")))
      out.push_back(json::Object(raw));
    return out;
  }
  auto Implement(const json::Members& extra = {}) -> std::string {
    json::Members value{
        {"id", json::Quote("plan")},
        {"card_json", json::Quote(::tsugumi::test::FixtureCodeCard())},
        {"keep_readme", "true"},
        {"keep_comments", "true"}};
    for (const auto& [k, v] : extra) value[k] = v;
    return json::Encode(
        json::Members{{"value_type", json::Quote("PutDefinition")},
                      {"value", json::Encode(value)}});
  }
  auto Sink() -> void {
    files["definitions/plan/_card.json"] = ::tsugumi::test::FixtureCodeCard();
  }
};
TEST_F(AgentSavePolicy, DraftIntentIsKeptAndOnlyEmptyDescriptionIsFilled) {
  EXPECT_NO_THROW(Prepare({Implement()}));
  EXPECT_NO_THROW(
      Prepare({Implement({{"fill_description", json::Quote("Ignored")}})}));
  files["definitions/plan/README.md"] =
      "---\ntitle: Plan\ndescription:\n---\n\nSplit and count.";
  EXPECT_THROW(Prepare({Implement()}), Failure);
  const auto saved = Prepare({
      Implement({{"fill_description", json::Quote("Counts words")}})});
  const auto value = json::Object(json::Get(
      json::Object(json::Array(saved.arguments.at("changes"))[0]), "value"));
  EXPECT_EQ(json::Text(value, "readme"),
            "---\ntitle: Plan\ndescription: Counts words\n---\n\nSplit and count.");
  EXPECT_FALSE(value.contains("fill_description"));
}

TEST_F(AgentSavePolicy, NonblankIntentCannotBeChangedOrDropped) {
  for (const auto* readme : {"---\ntitle: Plan\ndescription: Different\n---\n\nSplit and count.",
                             "---\ntitle: Plan\ndescription: Counts words\n---\n\nDifferent.",
                             "---\ntitle: Plan\ndescription: Counts words\n---\n"}) {
    EXPECT_THROW(Prepare({Implement({{"readme", json::Quote(readme)}})}),
                 Failure);
  }
}

TEST_F(AgentSavePolicy,
       DraftThatStaysDraftAndOrdinaryCardsNeedNoIntentCompletion) {
  EXPECT_NO_THROW(
      Prepare({Put(R"({"comments":[]})",
                   R"({"type":"draft","symbol":"pencil.and.outline"})")}));
  Sink();
  EXPECT_NO_THROW(Prepare({Put(R"({"comments":[]})")}));
}
TEST_F(AgentSavePolicy, ResolutionRecordsWhoDidItAndTheChangedCardIds) {
  Sink();
  auto saved = Prepare({Put(
      R"({"comments":[{"id":"c1","text":"Use a log scale","author":"agent","resolved":true,"resolution":"Log y","state_by":"user","cards":["fake"]},{"id":"c2","text":"Name the axis","author":"user"}]})",
      ::tsugumi::test::FixtureCodeCard(R"({"maxRuns":2})"))});
  auto comments = Comments(saved);
  ASSERT_EQ(comments.size(), 2u);
  EXPECT_EQ(json::Text(comments[0], "author"), "user");
  EXPECT_EQ(json::Text(comments[0], "state_by"), "agent");
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
      R"({"comments":[{"id":"c1","text":"Reworded","author":"agent"},{"id":"c1","text":"Twin","author":"user"},{"text":"New","author":"user","resolved":true,"state_by":"user"}]})")});
  auto comments = Comments(saved);
  ASSERT_EQ(comments.size(), 3u);
  EXPECT_EQ(json::Text(comments[0], "edited_by"), "agent");
  EXPECT_EQ(json::Text(comments[0], "author"), "user");
  EXPECT_NE(json::Text(comments[0], "id"), json::Text(comments[1], "id"));
  EXPECT_FALSE(json::Text(comments[2], "id").empty());
  EXPECT_EQ(json::Text(comments[2], "author"), "agent");
  EXPECT_EQ(json::Text(comments[2], "state_by"), "agent");
  std::set<std::string> changes;
  for (const auto& raw : json::Array(saved.comments))
    changes.insert(json::Text(json::Object(raw), "change"));
  EXPECT_EQ(changes,
            (std::set<std::string>{"added", "edited", "resolved", "removed"}));
}
TEST_F(AgentSavePolicy, ReopeningClearsResolutionCardsAndMovingKeepsIdentity) {
  Sink();
  files["definitions/plan/_comments.json"] =
      R"({"comments":[{"id":"c1","text":"Ask","author":"user","resolved":true,"state_by":"user","cards":["plan"]}]})";
  auto saved = Prepare(
      {Put(R"({"comments":[{"id":"c1","text":"Ask","author":"user"}]})")});
  EXPECT_EQ(json::Text(Comments(saved)[0], "state_by"), "agent");
  EXPECT_FALSE(Comments(saved)[0].contains("cards"));
  saved = Prepare({Put(R"({"comments":[]})"),
                   Put(files.at("definitions/plan/_comments.json"),
                       ::tsugumi::test::FixtureCodeCard(), "split")});
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
  for (const auto& raw : record)
    EXPECT_EQ(json::Text(json::Object(raw), "change"), "removed");
}
TEST_F(AgentSavePolicy, NestedCommentsPatchesAreStampedAndDraftIntentChecked) {
  files.clear();
  const std::string path = "externals/outer/definitions/plan/";
  files[path + "_card.json"] =
      R"({"type":"draft","symbol":"pencil.and.outline"})";
  files[path + "README.md"] = "---\ntitle: Plan\ndescription:\n---\n\nPlan";
  auto source = [&](const std::string& file, const std::string& bytes) {
    return json::Encode(json::Members{
        {"value_type", json::Quote("PutSource")},
        {"value", json::Encode(json::Members{
                      {"path", json::Quote(path + file)},
                      {"base_hash",
                       json::Quote(crypto::sha256_hex(files.at(path + file)))},
                      {"bytes", json::Quote(bytes)}})}});
  };
  EXPECT_THROW(
      Prepare({source("_card.json", ::tsugumi::test::FixtureCodeCard())}),
      std::runtime_error);
  EXPECT_NO_THROW(
      Prepare({source("_card.json", ::tsugumi::test::FixtureCodeCard()),
               source("README.md", "---\ntitle: Plan\ndescription: Summary\n---\n\nPlan")}));
  files[path + "_comments.json"] =
      R"({"comments":[{"id":"c1","text":"Ask","author":"user"}]})";
  auto meta = files.at(path + "_comments.json");
  const auto offset = meta.find(",\"author\"");
  auto patch = json::Encode(json::Members{
      {"value_type", json::Quote("PatchSource")},
      {"value",
       json::Encode(json::Members{
           {"path", json::Quote(path + "_comments.json")},
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
                .find("state_by"),
            std::string::npos);
}
TEST(AgentPresentation, StemDerivationIsOwnedByTheCaller) {
  EXPECT_EQ(CardStem("Aggregate Results!"), "aggregate_results");
  EXPECT_EQ(CardStem("集計"), "card");
  EXPECT_EQ(CardStem("---A___B---"), "a_b");
  EXPECT_LE(CardStem(std::string(500, 'a')).size(), 64u);
}
}  // namespace
}  // namespace tsugumi::host
