// Guide policy is exercised through the public Host socket without a provider.
#include "fixture.hh"

#include <hosq/hosq.hh>
#include <hosq/json/dom.hh>

#include <map>
#include <set>

namespace tsugumi::test {
namespace {
using Dom = hosq::json::GenericDom<>;
using Json = hosq::Elem<hosq::decode::json_dom<> | hosq::store::assign,
                        hosq::encode::json_dom>;
namespace v = hosq::types::value;
auto Parse(std::string_view raw) -> Dom {
  Dom d;
  if (auto e = d.assign(raw))
    throw std::runtime_error(std::string(e.detail));
  return d;
}
auto Members(const Dom &d, std::string_view path = "")
    -> std::map<std::string, Dom> {
  v::Mapping<v::String, Json> value;
  if (!d[path].exists())
    return {};
  if (auto e = d[path].decode_into(value))
    throw std::runtime_error(std::string(e.detail));
  std::map<std::string, Dom> out;
  for (const auto &[k, x] : value.value())
    out.emplace(k, x);
  return out;
}
auto Array(const Dom &d, std::string_view path) -> std::vector<Dom> {
  v::Array<Json> value;
  if (!d[path].exists())
    return {};
  if (auto e = d[path].decode_into(value))
    throw std::runtime_error(std::string(e.detail));
  return value.value();
}
auto String(const Dom &d, std::string_view path = "") -> std::string {
  return d[path].exists() ? JsonString(d.source(), path) : "";
}
auto State(const Reply &r) -> H::GuideStateT {
  if (!r.Code().empty())
    throw std::runtime_error(r.Code() + ": " + r.Failure()->message()->str());
  const auto *s = r.HostValue()->value_as_GuideState();
  if (!s)
    throw std::runtime_error("Missing guide state");
  return *std::unique_ptr<H::GuideStateT>(s->UnPack());
}
auto GuideGet(client::Connection &c, std::string session = "desktop",
              std::string language = "ja") -> H::GuideStateT {
  return State(Call(c, H::RequestPayload::GuideGetRequest, [&](Builder &b) {
    return H::CreateGuideGetRequest(b, b.CreateString(session),
                                    b.CreateString(language))
        .Union();
  }));
}
auto Advance(client::Connection &c, H::GuideInputT e,
             std::string session = "desktop", std::string language = "ja")
    -> H::GuideStateT {
  return State(Call(c, H::RequestPayload::GuideAdvanceRequest, [&](Builder &b) {
    const auto event = H::GuideInput::Pack(b, &e);
    return H::CreateGuideAdvanceRequest(b, b.CreateString(session),
                                        b.CreateString(language), event)
        .Union();
  }));
}
auto Event(std::string kind, std::string text = "") -> H::GuideInputT {
  H::GuideInputT e;
  e.kind = std::move(kind);
  e.text = std::move(text);
  return e;
}
const std::filesystem::path root = TSUGUMI_GUIDE_TEST_ROOT;
auto ReadResource(const std::filesystem::path &relative) -> std::string {
  const auto path = root / relative;
  if (!std::filesystem::is_regular_file(path))
    throw std::runtime_error("Missing guide test resource: " + path.string());
  return Read(path);
}
const std::vector<std::string> demos{"report", "exoplanet-transit",
                                     "pharmacokinetics"};

TEST(GuideScript, BothLanguagesHaveCompleteTransitionsAndRealDemoTargets) {
  for (const auto &language : {"ja", "en"}) {
    const auto script = Parse(
        ReadResource(std::filesystem::path("agent/Shared") /
                     (std::string(language) == "en" ? "guide-script.en.json"
                                                    : "guide-script.json")));
    const auto tours = Members(script, "/tours");
    ASSERT_EQ(tours.size(), demos.size());
    std::map<std::string, Dom> steps;
    for (const auto &[demo, tour] : tours) {
      for (const auto &group : {"home", "steps"})
        for (const auto &[id, step] : Members(tour, "/" + std::string(group)))
          steps[demo + "/" + id] = step;
    }
    for (const auto &demo : demos) {
      for (const auto &id :
           {"pick", "about", "start", "resume", "run", "ranOk", "ranFailed",
            "change", "runAgain", "ranAgain", "next", "farewell"})
        EXPECT_TRUE(steps.contains(demo + "/" + id));
      const auto index = Parse(ReadResource(std::filesystem::path("demos") /
                                            language / "index.json"));
      std::string folder;
      for (const auto &entry : Array(index, "/demos"))
        if (String(entry, "/id") == demo)
          folder = String(entry, "/path");
      ASSERT_FALSE(folder.empty());
      const auto dir = std::filesystem::path("demos") / language / folder;
      const auto project = Parse(ReadResource(dir / "tsugumi.json"));
      std::set<std::string> cards;
      for (const auto &card : Array(project, "/canvas/cards"))
        cards.insert(String(card, "/id"));
      for (const auto &[id, step] : steps) {
        if (!id.starts_with(demo + "/"))
          continue;
        SCOPED_TRACE(std::string(language) + ":" + id);
        std::set<std::string> choices, next;
        for (const auto &c : Array(step, "/show/choices"))
          choices.insert(String(c));
        auto resolve = [&](const std::string &target) {
          return target.find('/') == std::string::npos ? demo + "/" + target
                                                       : target;
        };
        for (const auto &[choice, target] : Members(step, "/next")) {
          next.insert(choice);
          EXPECT_TRUE(steps.contains(resolve(String(target))));
        }
        EXPECT_EQ(choices, next);
        for (const auto &[outcome, target] : Members(step, "/ran"))
          EXPECT_TRUE(steps.contains(resolve(String(target))));
        const std::set<std::string> kinds{
            "open_demo", "show_demos",    "run",         "stop",
            "open_card", "open_settings", "new_project", "home"};
        const auto actions = Array(step, "/show/actions");
        EXPECT_LE(actions.size(), 3u);
        EXPECT_LE(choices.size(), 4u);
        for (const auto &action : actions) {
          EXPECT_TRUE(kinds.contains(String(action, "/kind")));
          if (action["/demo"].exists())
            EXPECT_TRUE(tours.contains(String(action, "/demo")));
        }
        const auto h = String(step, "/show/highlight");
        if (h.starts_with("card:"))
          EXPECT_TRUE(cards.contains(h.substr(5, h.find('/', 5) - 5)));
        if (h.starts_with("demo:"))
          EXPECT_EQ(h, "demo:" + demo);
        if (h.starts_with("param:")) {
          const auto opened = String(step, "/show/open");
          ASSERT_TRUE(opened.starts_with("card:"));
          const auto card = Parse(ReadResource(
              dir / "definitions" / opened.substr(5) / "_card.json"));
          bool found = false;
          for (const auto &param : Array(card, "/params"))
            found |= String(param, "/name") == h.substr(6);
          EXPECT_TRUE(found);
        }
      }
      EXPECT_TRUE(
          JsonBool(steps.at(demo + "/farewell").source(), "/show/endTour"));
      EXPECT_NE(String(steps.at(demo + "/farewell"), "/show/say").find("$demo"),
                std::string::npos);
    }
    EXPECT_NE(String(script, "/noConnection/show/say").find("$demo"),
              std::string::npos);
    const auto actions = Array(script, "/noConnection/show/actions");
    ASSERT_EQ(actions.size(), 2u);
    EXPECT_EQ(String(actions[0], "/kind"), "open_settings");
    EXPECT_EQ(String(actions[1], "/kind"), "open_demo");
    std::vector<std::string> offered;
    for (const auto &a : Array(script, "/otherProject/show/actions"))
      offered.push_back(String(a, "/demo"));
    EXPECT_EQ(offered, demos);
  }
}

class GuideFixture : public AgentFixture {
protected:
  auto SetUp() -> void override {
    // agentd resolves $demo against its installed catalog, never a client list.
    for (const auto &language : {"ja", "en"})
      Write(directory.path / "demos" / language / "index.json",
            ReadResource(std::filesystem::path("demos") / language /
                         "index.json"));
    AgentFixture::SetUp();
  }
  auto Choose(const H::GuideStateT &s, const std::string &choice,
              const std::string &lang = "ja", client::Connection *c = nullptr)
      -> H::GuideStateT {
    auto e = Event("message", choice);
    e.expected_step = s.step;
    e.project = s.project;
    return Advance(c ? *c : *host, e, "desktop", lang);
  }
  auto Observe(const std::string &run, const std::string &state, bool active,
               const std::string &lang = "ja") -> H::GuideStateT {
    auto e = Event("observe");
    e.project = "/copy";
    e.connected = true;
    e.run = run;
    e.state = state;
    e.active = active;
    return Advance(*host, e, "desktop", lang);
  }
  auto Start(const std::string &demo, const std::string &lang = "ja")
      -> H::GuideStateT {
    auto e = Event("start");
    e.demo = demo;
    e.title = demo;
    e.project = "/copy";
    Advance(*host, e, "desktop", lang);
    return Observe("-", "none", false, lang);
  }
};
TEST_F(GuideFixture,
       InterestChoicesFallbackAndLanguageShareOneStateWithoutAModel) {
  auto s = Advance(*host, Event("interest", "天文・地震・タンパク質のデータ"));
  EXPECT_EQ(s.step, "exoplanet-transit/pick");
  EXPECT_EQ(s.highlight, "demo:exoplanet-transit");
  s = Choose(s, "もっと詳しく");
  EXPECT_EQ(s.step, "exoplanet-transit/about");
  s = Choose(s, "ほかのデモ");
  EXPECT_EQ(s.step, "pharmacokinetics/pick");
  s = Choose(s, "これは何？");
  EXPECT_EQ(s.choices.size(), 2u);
  EXPECT_EQ(s.highlight, "demo:pharmacokinetics");
  s = Choose(s, "ほかのデモ");
  EXPECT_EQ(s.step, "report/pick");
  const auto english = GuideGet(*host, "desktop", "en");
  EXPECT_EQ(english.step, s.step);
  EXPECT_NE(english.say, s.say);
  const std::vector<std::pair<std::string, std::string>> interests{
      {"Live display", "pharmacokinetics/pick"},
      {"Aggregating data and reports", "report/pick"},
      {"Anything", "report/pick"},
      {"Astronomy, earthquakes and protein data", "exoplanet-transit/pick"}};
  for (const auto &[text, step] : interests)
    EXPECT_EQ(Advance(*host, Event("interest", text), "desktop", "en").step,
              step);
}
TEST_F(GuideFixture,
       EveryTourRunsThroughMoreFailureRetryChangeAndFarewellInBothLanguages) {
  for (const auto &lang : {"ja", "en"})
    for (const auto &demo : demos) {
      SCOPED_TRACE(std::string(lang) + ":" + demo);
      const bool en = std::string(lang) == "en";
      auto s = Start(demo, lang);
      ASSERT_EQ(s.step, demo + "/start");
      s = Choose(s, en ? "Tell me more" : "もっと詳しく", lang);
      EXPECT_EQ(s.step, demo + "/startMore");
      s = Choose(s, "?", lang);
      EXPECT_EQ(s.actions.size(), 1u);
      EXPECT_EQ(s.actions[0]->kind, "open_settings");
      for (int i = 0; i < 25 && s.step != demo + "/run"; ++i)
        s = Choose(s, en ? "Next" : "次へ", lang);
      ASSERT_EQ(s.step, demo + "/run");
      ASSERT_EQ(s.actions.size(), 1u);
      EXPECT_EQ(s.actions[0]->kind, "run");
      s = Choose(s, en ? "I ran it!" : "実行した！", lang);
      Observe("r1", "running", true, lang);
      s = Observe("r1", "failed", false, lang);
      ASSERT_EQ(s.step, demo + "/ranFailed");
      EXPECT_EQ(s.highlight, "logs");
      // Use the server's translated choice, not a client transition table.
      ASSERT_FALSE(s.choices.empty());
      s = Choose(s, s.choices.front(), lang);
      Observe("r2", "running", true, lang);
      s = Observe("r2", "succeeded", false, lang);
      ASSERT_EQ(s.step, demo + "/ranOk");
      s = Choose(s, en ? "Next" : "次へ", lang);
      EXPECT_TRUE(s.highlight.starts_with("param:"));
      ASSERT_FALSE(s.choices.empty());
      s = Choose(s, s.choices.front(), lang);
      Observe("r3", "running", true, lang);
      s = Observe("r3", "succeeded", false, lang);
      ASSERT_EQ(s.step, demo + "/ranAgain");
      s = Choose(s, en ? "Next" : "次へ", lang);
      ASSERT_FALSE(s.choices.empty());
      s = Choose(s, s.choices.back(), lang);
      EXPECT_TRUE(s.end_tour);
      EXPECT_EQ(s.tour, nullptr);
      EXPECT_TRUE(s.present);
      EXPECT_FALSE(Observe("r4", "succeeded", false, lang).present);
    }
}
TEST_F(GuideFixture, LateRunDuplicateRunAndTwoWindowsDoNotAdvanceTwice) {
  auto window = ConnectHost(true);
  auto s = Start("report");
  const auto old = s;
  auto first =
      std::async(std::launch::async, [&] { return Choose(old, "次へ"); });
  auto second = std::async(std::launch::async, [&] {
    return Choose(old, "次へ", "ja", window.get());
  });
  s = first.get();
  const auto stale = second.get();
  EXPECT_EQ(s.step, "report/settings");
  EXPECT_EQ(stale.step, s.step);
  EXPECT_EQ(GuideGet(*window).step, s.step);
  for (int i = 0; i < 25 && s.step != "report/run"; ++i)
    s = Choose(s, "次へ");
  ASSERT_EQ(s.step, "report/run");
  Observe("late", "running", true);
  Observe("current", "running", true);
  const auto late = Observe("late", "failed", false);
  EXPECT_FALSE(late.present);
  EXPECT_EQ(late.step, s.step);
  s = Observe("current", "succeeded", false);
  ASSERT_EQ(s.step, "report/ranOk");
  EXPECT_FALSE(Observe("current", "succeeded", false).present);
  EXPECT_FALSE(Observe("late", "failed", false).present);
  s = Choose(s, "次へ", "ja", window.get());
  EXPECT_EQ(GuideGet(*host).step, s.step);
  // A new tour ignores the old project's delayed terminal observation.
  auto start = Event("start");
  start.demo = "report";
  start.project = "/other-copy";
  Advance(*host, start);
  EXPECT_FALSE(Observe("current", "succeeded", false).present);
}
TEST_F(GuideFixture, ProgressScopePersistsAndLiveStepResumesAfterRestart) {
  Advance(*host, Event("setup"));
  Advance(*host, Event("offered"));
  auto s = Start("report");
  EXPECT_FALSE(GuideGet(*host, "web:one").setup_done);
  EXPECT_FALSE(GuideGet(*host, "web:two").tour);
  Advance(*host, Event("setup"), "web:one");
  EXPECT_FALSE(GuideGet(*host, "web:two").setup_done);
  host.reset();
  human.reset();
  external.reset();
  agent.Stop();
  StartAgent();
  s = GuideGet(*host);
  EXPECT_TRUE(s.setup_done);
  EXPECT_TRUE(s.demo_answered);
  ASSERT_NE(s.tour, nullptr);
  EXPECT_TRUE(s.step.empty());
  auto e = Event("resume");
  e.project = "/copy";
  e.connected = true;
  EXPECT_EQ(Advance(*host, e).step, "report/resume");
  EXPECT_TRUE(GuideGet(*host, "web:one").setup_done);
  EXPECT_FALSE(GuideGet(*host, "web:two").setup_done);
}
TEST_F(GuideFixture, DemoCommandUnknownProjectsAndAbsentVectors) {
  auto e = Event("message", "$demo");
  e.project = "/copy";
  e.title = "My analysis";
  auto s = Advance(*host, e);
  EXPECT_EQ(s.step, "otherProject");
  EXPECT_EQ(s.actions.size(), 3u);
  const auto index = Parse(ReadResource("demos/en/index.json"));
  for (const auto &demo : Array(index, "/demos"))
    if (String(demo, "/id") == "pharmacokinetics")
      e.title = String(demo, "/title");
  s = Advance(*host, e, "desktop", "en");
  ASSERT_EQ(s.step, "pharmacokinetics/start");
  ASSERT_NE(s.tour, nullptr);
  EXPECT_EQ(s.tour->path, "/copy");
  const auto reply =
      Call(*host, H::RequestPayload::GuideGetRequest, [](Builder &b) {
        return H::CreateGuideGetRequest(b, b.CreateString("desktop")).Union();
      });
  const auto *value = reply.HostValue()->value_as_GuideState();
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(value->actions(), nullptr);
  EXPECT_EQ(ListSize(value->actions()), 0u);
  auto finish = Event("action_finished", "run requested");
  finish.project = "/copy";
  EXPECT_EQ(Advance(*host, finish).step, s.step);
}
} // namespace
} // namespace tsugumi::test
