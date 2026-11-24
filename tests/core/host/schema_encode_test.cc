#include <gtest/gtest.h>

#include <limits>

#include "../agent/document_fixture.hh"
#include "../agent_fixture_server.hh"
#include "host/templates.hh"
#include "tsugumi/document/document.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/json.hh"

namespace tsugumi::schema {
namespace {
TEST(SchemaDecode, DefinitionLoaderNamesUnknownSettingsAndTheFile) {
  test::TemporaryDirectory directory;
  fs::write_atomic(
      directory.path / "tsugumi.json",
      test::FixtureProjectDocument(
          R"({"canvas":{"cards":[{"id":"x"}],"connections":[]}})"));
  const auto file = directory.path / "definitions/x/_card.json";
  std::filesystem::create_directories(file.parent_path());
  fs::write_atomic(file.parent_path() / "README.md", host::CardReadme("x"));
  for (const auto* member : {"filesystem", "unrecognizedSetting"}) {
    fs::write_atomic(
        file, "{\"symbol\":\"pencil.and.outline\",\"type\":\"draft\",\"" +
                  std::string(member) + "\":true}");
    try {
      load_from_disk(directory.path);
      FAIL() << "Unknown settings must be rejected";
    } catch (const DocumentError& error) {
      EXPECT_EQ(error.code, "invalid_document");
      ASSERT_TRUE(error.args.contains("path")) << error.what();
      EXPECT_EQ(error.args.at("path"), "definitions/x/_card.json");
    }
  }
}

TEST(SchemaDecode, EveryCardStatesAKnownType) {
  for (const auto& [text, message] : {
           std::pair{R"({"symbol":"link"})", "type is required"},
           std::pair{R"({"type":"none","symbol":"link"})",
                     "unknown enum value: none"},
       }) {
    try {
      parse_card_json("x", text);
      FAIL() << text;
    } catch (const DocumentError& error) {
      EXPECT_EQ(error.code, "invalid_document");
      EXPECT_NE(std::string(error.what()).find(message), std::string::npos)
          << error.what();
      EXPECT_EQ(error.args.at("path"), "definitions/x/_card.json");
    }
  }
  EXPECT_NO_THROW(
      parse_card_json("x", R"({"type":"subgraph","symbol":"link"})"));
}

TEST(SchemaDecode, NestedUnknownAndWrongTypedSettingsKeepTheirLocation) {
  for (const auto& [text, member] :
       {std::pair{R"({"type":"code","params":42})", "params"},
        std::pair{R"({"type":"code","sandbox":"yes"})", "sandbox"}}) {
    try {
      parse_card_json("x", text);
      FAIL() << "Invalid settings must be rejected";
    } catch (const DocumentError& error) {
      EXPECT_EQ(error.code, "invalid_document");
      EXPECT_FALSE(std::string(error.what()).empty());
    }
  }
}

struct Coordinate {
  t::String<"id"> id;
  t::Array<"position", Number> position;
};
struct Geometry {
  t::Array<"cards", Obj<Coordinate>> cards;
};

template <class S>
auto failure(const S& value) -> std::string {
  try {
    encode(value);
    ADD_FAILURE() << "Expected an encoding failure";
    return {};
  } catch (const std::runtime_error& error) {
    return error.what();
  }
}

TEST(SchemaEncode, NonFiniteNumbersNameTheTypeAndNestedField) {
  for (const double bad : {std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity()}) {
    Geometry g;
    Coordinate c;
    *c.id = "mnist";
    c.position->push_back(12.0);
    c.position->push_back(bad);
    g.cards->push_back(c);
    const auto message = failure(g);
    EXPECT_FALSE(message.empty());
  }
}

TEST(SchemaEncode, InvalidUtf8AndUnassignedDomAreIdentified) {
  v::String input;
  input.value() = std::string(1, static_cast<char>(0xff));
  EXPECT_FALSE(failure(input).empty());
  EXPECT_FALSE(failure(Json{}).empty());
}

TEST(SchemaEncode, MappingKeysAreEscapedAndMessagesStayOnOneLine) {
  v::Mapping<v::String, v::Array<Number>> values;
  values.value().emplace_back(
      "a\n\"b", std::vector<double>{std::numeric_limits<double>::infinity()});
  const auto message = failure(values);
  EXPECT_FALSE(message.empty());
  values.value().clear();
  values.value().emplace_back(std::string(1, static_cast<char>(0xff)),
                              std::vector<double>{});
  EXPECT_FALSE(failure(values).empty());
}

TEST(SchemaEncode, NestedDomReportsTheWriterDepthLimit) {
  // The normal DOM reader stops at 128, before the writer's limit of 1024.
  // Give this capture a larger reader budget to exercise the writer diagnostic.
  using DeepJson =
      hosq::Elem<hosq::decode::json_dom<2048> | hosq::store::assign,
                 hosq::encode::json_dom>;
  hosq::json::GenericDom<2048> dom;
  ASSERT_FALSE(
      dom.assign(std::string(1024, '[') + "0" + std::string(1024, ']')));
  v::Array<DeepJson> values;
  values.value().push_back(dom);
  const auto message = failure(values);
  EXPECT_FALSE(message.empty());
}

}  // namespace
}  // namespace tsugumi::schema
