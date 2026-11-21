// What the Linux UI shows, without GTK or a host: strings, the Canvas scene
// and the log window.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "canvas_scene.hh"
#include "log_window.hh"
#include "strings.hh"

namespace {

int failures = 0;

auto Check(bool ok, const char* what, int line) -> void {
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "model_test.cc:%d: failed: %s\n", line, what);
}

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): the condition's text
#define CHECK(condition) Check((condition), #condition, __LINE__)

namespace ui = tsugumi::linux_ui;

auto StringsTest() -> void {
  const ui::Strings ja(ui::Language::kJapanese, ui::EnglishDictionary());
  const ui::Strings en(ui::Language::kEnglish, ui::EnglishDictionary());
  CHECK(ja.Text("最近のプロジェクト") == "最近のプロジェクト");
  CHECK(en.Text("最近のプロジェクト") == "Recent Projects");
  // Entries with values ({text, mac}) and values put in.
  CHECK(en.Text("失敗: {0}", {"x"}) == "Failed: x");
  CHECK(ja.Text("失敗: {0}", {"x"}) == "失敗: x");
  CHECK(en.Text("Run {0} · 実行 #{1}", {"a", "2"}) == "Run a · invocation #2");
  // Text without an entry is shown as written; a missing value stays.
  CHECK(en.Text("未登録の文 {0} {1}", {"a"}) == "未登録の文 a {1}");
  CHECK(ui::LanguageNamed("en") == ui::Language::kEnglish);
  CHECK(ui::LanguageNamed("system") == ui::Language::kJapanese);
}

auto SceneTest() -> void {
  const std::string project = R"({
    "name": "Collatz",
    "canvas": {
      "cards": [{"id": "start"}, {"id": "square"}, {"id": "gone"}],
      "connections": [
        {"from": {"card": "start", "port": "out"}, "to": {"card": "square", "port": "x"}},
        {"from": {"card": "square", "port": "y"}, "to": {"card": "elsewhere", "port": "z"}}
      ]
    }
  })";
  const std::string layout =
      R"({"flow": "leftToRight", "cards": {"start": {"x": 10, "y": -20.5}, "square": {"x": 400, "y": 0}}})";
  const std::vector<ui::Definition> definitions = {
      {"start", R"({"type": "trigger"})", R"({"name": "Start"})", ""},
      {"square", R"({"type": "code"})", "", ""},
  };
  const auto scene = ui::BuildScene(project, layout, definitions);
  CHECK(!scene.top_to_bottom);
  CHECK(scene.cards.size() == 3);
  CHECK(scene.cards[0].name == "Start");
  CHECK(scene.cards[1].name == "square");
  CHECK(scene.cards[0].x == 10 && scene.cards[0].y == -20.5);
  // Only wires between drawn Cards.
  CHECK(scene.wires.size() == 1);
  const auto [from, to] = scene.WireEnds(scene.wires[0]);
  const auto start = ui::Scene::CardBox(scene.cards[0]);
  CHECK(from.x == start.x + start.width && to.x == 400);
  CHECK(scene.CardAt(15, -10) == &scene.cards[0]);
  CHECK(scene.CardAt(-100, -100) == nullptr);
  CHECK(ui::Scene::CardBox(scene.cards[1]).Contains(401, 1));
  CHECK(ui::ProjectTitle(project, "folder") == "Collatz");
  CHECK(ui::ProjectTitle("{}", "folder") == "folder");
  CHECK(ui::BuildScene("not json", "", {}).cards.empty());
  CHECK(ui::BuildScene(project, "{}", {}).top_to_bottom);
  CHECK(!scene.cards[0].definition_error && !scene.cards[1].definition_error);
  CHECK(scene.cards[2].definition_error);  // "gone" has no definition

  // definitions_json carries each _card.json as text.
  CHECK(
      ui::CompletionDefinitions(definitions) ==
      R"({"square":"{\"type\": \"code\"}","start":"{\"type\": \"trigger\"}"})");
}

// A Card without a known type is a definition error, not a Draft: drawn with
// the core's diagnostic and never run. The Web UI and the Mac app check the
// same (ui/web/tests/definitionErrors.tsx, UILogicTests).
auto DefinitionErrorTest() -> void {
  CHECK(ui::StatesKnownType(R"({"type": "draft"})"));
  CHECK(ui::StatesKnownType(R"({"type": "subgraph", "external": "lib"})"));
  CHECK(!ui::StatesKnownType(R"({})"));
  CHECK(!ui::StatesKnownType(R"({"type": "something new"})"));
  CHECK(!ui::StatesKnownType(R"({"type": "code", "external": "lib"})"));
  CHECK(!ui::StatesKnownType(R"({"type": "subgraph"})"));
  CHECK(!ui::StatesKnownType("not json"));

  const std::string project =
      R"({"canvas": {"cards": [{"id": "old"}, {"id": "future"}, {"id": "plan"}], "connections": []}})";
  const std::string missing =
      R"(definitions/old/_card.json: "type" is required (one of code, draft, trigger, input, branch, fork, merge, collect, subgraph))";
  const auto scene =
      ui::BuildScene(project, "{}",
                     {{"old", R"({})", "", missing},
                      {"future", R"({"type": "something new"})", "", ""},
                      {"plan", R"({"type": "draft"})", "", ""}});
  CHECK(scene.cards[0].definition_error &&
        scene.cards[0].diagnostic == missing);
  CHECK(scene.cards[1].definition_error && scene.cards[1].diagnostic.empty());
  CHECK(!scene.cards[2].definition_error);  // a Draft stays a Draft
}

auto LogWindowTest() -> void {
  ui::LogWindow window;
  CHECK(window.Next().read == ui::LogRead::kTail);
  const auto tail = ui::LogWindow::Tail();
  CHECK(tail.tail && tail.limit_bytes == ui::LogWindow::kReadBytes);
  window.Apply(ui::LogRead::kTail, {"c\nd\n", 100, 104, 104, true});
  CHECK(window.text() == "c\nd\n" && window.start() == 100 &&
        window.end() == 104);
  CHECK(!window.behind());

  const auto earlier = window.Earlier();
  CHECK(earlier.read == ui::LogRead::kEarlier && earlier.before == 100 &&
        !earlier.tail);
  window.Apply(ui::LogRead::kEarlier, {"a\nb\n", 96, 100, 104, true});
  CHECK(window.text() == "a\nb\nc\nd\n" && window.start() == 96);
  // A piece that does not meet the window is dropped.
  window.Apply(ui::LogRead::kEarlier, {"x", 10, 20, 104, true});
  CHECK(window.start() == 96);

  window.Grew(110);
  CHECK(window.behind());
  const auto next = window.Next();
  CHECK(next.read == ui::LogRead::kAppend && next.offset == 104);
  window.Apply(ui::LogRead::kAppend, {"e\n", 104, 106, 110, true});
  CHECK(window.text() == "a\nb\nc\nd\ne\n" && window.end() == 106 &&
        window.behind());
  window.Apply(ui::LogRead::kAppend, {"stale", 90, 95, 110, true});
  CHECK(window.end() == 106);

  // Far behind: read the end again.
  window.Grew(106 + (2 * ui::LogWindow::kReadBytes));
  CHECK(window.Next().read == ui::LogRead::kTail);

  // At most kWindowBytes, cut at a line.
  ui::LogWindow big;
  const std::string line(1023, 'x');
  std::string text;
  for (int i = 0; i < 3000; ++i) text += line + "\n";
  big.Apply(ui::LogRead::kTail, {text, 0, text.size(), text.size(), true});
  big.Apply(ui::LogRead::kAppend, {line + "\n", text.size(), text.size() + 1024,
                                   text.size() + 1024, true});
  CHECK(big.text().size() <= ui::LogWindow::kWindowBytes);
  CHECK(big.text().front() == 'x' && big.start() % 1024 == 0);
  CHECK(big.end() - big.start() == big.text().size());
}

}  // namespace

auto main() -> int {
  StringsTest();
  SceneTest();
  DefinitionErrorTest();
  LogWindowTest();
  if (failures != 0) return EXIT_FAILURE;
  std::puts("tsugumi_linux_ui_tests: passed");
  return EXIT_SUCCESS;
}
