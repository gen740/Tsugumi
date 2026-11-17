#include <gtest/gtest.h>

#include <limits>

#include "../agent_fixture_server.hh"
#include "document.hh"
#include "util/fs.hh"
#include "util/json.hh"

namespace tsugumi::schema {
namespace {
TEST(SchemaDecode, DefinitionLoaderNamesUnknownSettingsAndTheFile) {
  test::TemporaryDirectory directory;
  fs::write_atomic(
      directory.path / "tsugumi.json",
      R"({"pkgsPath":"tsugumi:default","canvas":{"cards":[],"connections":[]}})");
  const auto file = directory.path / "definitions/x/_card.json";
  std::filesystem::create_directories(file.parent_path());
  for (const auto *member : {"filesystem", "unrecognizedSetting"}) {
    fs::write_atomic(file, "{\"template\":\"draft\",\"" + std::string(member) +
                               "\":true}");
    try {
      load_from_disk(directory.path);
      FAIL() << "Unknown settings must be rejected";
    } catch (const DocumentError &error) {
      EXPECT_EQ(error.code, "invalid_document");
      ASSERT_TRUE(error.args.contains("path")) << error.what();
      EXPECT_EQ(error.args.at("path"), "definitions/x/_card.json");
      const std::string message = error.what();
      EXPECT_NE(message.find("definitions/x/_card.json"), std::string::npos);
      EXPECT_NE(message.find('"' + std::string(member) + '"'),
                std::string::npos);
      EXPECT_NE(message.find("remove"), std::string::npos);
    }
  }
}

TEST(SchemaDecode, NestedUnknownAndWrongTypedSettingsKeepTheirLocation) {
  for (const auto &[text, member] :
       {std::pair{R"({"params":[{"unrecognized":true}]})", "unrecognized"},
        std::pair{R"({"sandbox":"yes"})", "sandbox"}}) {
    try {
      parse_card_json("x", text);
      FAIL() << "Invalid settings must be rejected";
    } catch (const DocumentError &error) {
      const std::string message = error.what();
      EXPECT_NE(message.find("definitions/x/_card.json"), std::string::npos);
      EXPECT_NE(message.find(member), std::string::npos);
      if (std::string(member) == "unrecognized")
        EXPECT_NE(message.find("params[0]"), std::string::npos);
      else {
        EXPECT_EQ(message.find("unknown setting"), std::string::npos);
        EXPECT_NE(message.find("correct"), std::string::npos);
      }
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
    EXPECT_NE(message.find("Geometry"), std::string::npos) << message;
    EXPECT_NE(message.find("$[\"cards\"][0][\"position\"][1]"),
              std::string::npos)
        << message;
    EXPECT_NE(message.find("non-finite"), std::string::npos) << message;
    EXPECT_EQ(message.find('\n'), std::string::npos);
  }
}

TEST(SchemaEncode, InvalidUtf8EmptyOptionalAndUnassignedDomAreIdentified) {
  JournalRecord record;
  JournalFile file;
  *file.path = std::string(1, static_cast<char>(0xff));
  record.files->push_back(file);
  const auto utf8 = failure(record);
  EXPECT_NE(utf8.find("JournalRecord"), std::string::npos);
  EXPECT_NE(utf8.find("$[\"files\"][0][\"p\"]"), std::string::npos) << utf8;
  EXPECT_NE(utf8.find("UTF-8"), std::string::npos) << utf8;
  const auto optional = failure(InvocationDetailJson{});
  EXPECT_NE(optional.find("$[\"stages\"]"), std::string::npos) << optional;
  EXPECT_NE(optional.find("empty optional"), std::string::npos) << optional;
  const auto dom = failure(Json{});
  EXPECT_NE(dom.find("at $:"), std::string::npos) << dom;
  EXPECT_NE(dom.find("no document"), std::string::npos) << dom;
}

TEST(SchemaEncode, MappingKeysAreEscapedAndMessagesStayOnOneLine) {
  v::Mapping<v::String, v::Array<Number>> values;
  values.value().emplace_back(
      "a\n\"b", std::vector<double>{std::numeric_limits<double>::infinity()});
  const auto message = failure(values);
  EXPECT_NE(message.find("$[\"a\\n\\\"b\"][0]"), std::string::npos) << message;
  EXPECT_EQ(message.find('\n'), std::string::npos);
  values.value().clear();
  values.value().emplace_back(std::string(1, static_cast<char>(0xff)),
                              std::vector<double>{});
  EXPECT_NE(failure(values).find("$[key 0]"), std::string::npos);
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
  EXPECT_NE(message.find("writer container depth limit"), std::string::npos)
      << message;
  EXPECT_NE(message.find("at $[0][0]"), std::string::npos) << message;
}

TEST(SchemaEncode, FiniteDoublesAndCapturedDefaultsKeepTheirValues) {
  Number number;
  number.value() = 0.001;
  EXPECT_EQ(encode(number), "0.001");
  Json defaults;
  defaults.value() = capture(
      R"({"params":[{"type":"double","data":1.0},{"type":"double","data":0.001}]})");
  EXPECT_EQ(encode(defaults), text(defaults.value()));
}
}  // namespace
}  // namespace tsugumi::schema
