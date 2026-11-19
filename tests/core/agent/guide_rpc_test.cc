// Guide policy is exercised through the public Host socket without a provider.
#include <algorithm>
#include <hosq/hosq.hh>
#include <hosq/json/dom.hh>
#include <map>
#include <set>

#include "fixture.hh"

namespace tsugumi::test {
namespace {
using Dom = hosq::json::GenericDom<>;
using Json = hosq::Elem<hosq::decode::json_dom<> | hosq::store::assign,
                        hosq::encode::json_dom>;
namespace v = hosq::types::value;
auto Parse(std::string_view raw) -> Dom {
  Dom d;
  if (auto e = d.assign(raw)) throw std::runtime_error(std::string(e.detail));
  return d;
}
auto Members(const Dom& d, std::string_view path = "")
    -> std::map<std::string, Dom> {
  v::Mapping<v::String, Json> value;
  if (!d[path].exists()) return {};
  if (auto e = d[path].decode_into(value))
    throw std::runtime_error(std::string(e.detail));
  std::map<std::string, Dom> out;
  for (const auto& [k, x] : value.value()) out.emplace(k, x);
  return out;
}
auto Array(const Dom& d, std::string_view path) -> std::vector<Dom> {
  v::Array<Json> value;
  if (!d[path].exists()) return {};
  if (auto e = d[path].decode_into(value))
    throw std::runtime_error(std::string(e.detail));
  return value.value();
}
auto String(const Dom& d, std::string_view path = "") -> std::string {
  return d[path].exists() ? JsonString(d.source(), path) : "";
}
auto State(const Reply& r) -> H::GuideStateT {
  if (!r.Code().empty())
    throw std::runtime_error(r.Code() + ": " + r.Failure()->message()->str());
  const auto* s = r.HostValue()->value_as_GuideState();
  if (!s) throw std::runtime_error("Missing guide state");
  return *std::unique_ptr<H::GuideStateT>(s->UnPack());
}
auto GuideGet(client::Connection& c, std::string session = "desktop",
              std::string language = "ja") -> H::GuideStateT {
  return State(Call(c, H::RequestPayload::GuideGetRequest, [&](Builder& b) {
    return H::CreateGuideGetRequest(b, b.CreateString(session),
                                    b.CreateString(language))
        .Union();
  }));
}
auto Advance(client::Connection& c, H::GuideInputT e,
             std::string session = "desktop", std::string language = "ja")
    -> H::GuideStateT {
  return State(Call(c, H::RequestPayload::GuideAdvanceRequest, [&](Builder& b) {
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
auto ReadResource(const std::filesystem::path& relative) -> std::string {
  const auto path = root / relative;
  if (!std::filesystem::is_regular_file(path))
    throw std::runtime_error("Missing guide test resource: " + path.string());
  return Read(path);
}

TEST(GuideScript, BothLanguagesHaveCompleteTransitionsAndRealDemoTargets) {
  for (const auto& language : {"ja", "en"}) {
    const auto script = Parse(
        ReadResource(std::filesystem::path("hostd/agent/Shared") /
                     (std::string(language) == "en" ? "guide-script.en.json"
                                                    : "guide-script.json")));
    const auto tours = Members(script, "/tours");
    std::map<std::string, Dom> steps;
    for (const auto& [demo, tour] : tours) {
      for (const auto& group : {"home", "steps"})
        for (const auto& [id, step] : Members(tour, "/" + std::string(group)))
          steps[demo + "/" + id] = step;
    }
    const auto index = Parse(
        ReadResource(std::filesystem::path("demos") / language / "index.json"));
    for (const auto& [demo, tour] : tours) {
      (void)tour;
      std::string folder;
      for (const auto& entry : Array(index, "/demos"))
        if (String(entry, "/id") == demo) folder = String(entry, "/path");
      ASSERT_FALSE(folder.empty());
      const auto dir = std::filesystem::path("demos") / language / folder;
      const auto project = Parse(ReadResource(dir / "tsugumi.json"));
      std::set<std::string> cards;
      for (const auto& card : Array(project, "/canvas/cards"))
        cards.insert(String(card, "/id"));
      for (const auto& [id, step] : steps) {
        if (!id.starts_with(demo + "/")) continue;
        SCOPED_TRACE(std::string(language) + ":" + id);
        std::set<std::string> choices, next;
        for (const auto& c : Array(step, "/show/choices"))
          choices.insert(String(c));
        auto resolve = [&](const std::string& target) {
          return target.find('/') == std::string::npos ? demo + "/" + target
                                                       : target;
        };
        for (const auto& [choice, target] : Members(step, "/next")) {
          next.insert(choice);
          EXPECT_TRUE(steps.contains(resolve(String(target))));
        }
        EXPECT_EQ(choices, next);
        if (step["/finished"].exists()) {
          EXPECT_TRUE(steps.contains(resolve(String(step, "/finished"))));
          EXPECT_TRUE(steps.contains(demo + "/failed"));
          const auto actions = Array(step, "/show/actions");
          EXPECT_TRUE(std::ranges::any_of(actions, [](const auto& action) {
            return String(action, "/kind") == "run";
          }));
        }
        for (const auto& action : Array(step, "/show/actions")) {
          if (action["/demo"].exists())
            EXPECT_TRUE(tours.contains(String(action, "/demo")));
        }
        const auto h = String(step, "/show/highlight");
        if (h.starts_with("card:"))
          EXPECT_TRUE(cards.contains(h.substr(5, h.find('/', 5) - 5)));
        if (h.starts_with("demo:")) EXPECT_EQ(h, "demo:" + demo);
        if (h.starts_with("param:")) {
          const auto opened = String(step, "/show/open");
          ASSERT_TRUE(opened.starts_with("card:"));
          const auto card = Parse(ReadResource(
              dir / "definitions" / opened.substr(5) / "_card.json"));
          bool found = false;
          for (const auto& param : Array(card, "/params"))
            found |= String(param, "/name") == h.substr(6);
          EXPECT_TRUE(found);
        }
      }
      EXPECT_TRUE(
          JsonBool(steps.at(demo + "/farewell").source(), "/show/endTour"));
    }
    const auto actions = Array(script, "/noConnection/show/actions");
    for (const std::string_view kind : {"open_settings", "open_demo"})
      EXPECT_TRUE(std::ranges::any_of(actions, [&](const auto& action) {
        return String(action, "/kind") == kind;
      }));
    for (const auto& action : Array(script, "/otherProject/show/actions"))
      EXPECT_TRUE(tours.contains(String(action, "/demo")));
  }
}

class GuideFixture : public AgentFixture {
 protected:
  auto SetUp() -> void override {
    // hostd resolves $demo against its installed catalog, never a client list.
    for (const auto& language : {"ja", "en"})
      Write(directory.path / "demos" / language / "index.json",
            ReadResource(std::filesystem::path("demos") / language /
                         "index.json"));
    AgentFixture::SetUp();
  }
  auto Choose(const H::GuideStateT& s, const std::string& choice,
              const std::string& lang = "ja", client::Connection* c = nullptr)
      -> H::GuideStateT {
    auto e = Event("message", choice);
    e.expected_step = s.step;
    e.project = s.project;
    return Advance(c ? *c : *host, e, "desktop", lang);
  }
  auto Observe(const std::string& run, const std::string& state, bool active,
               const std::string& lang = "ja") -> H::GuideStateT {
    auto e = Event("observe");
    e.project = "/copy";
    e.connected = true;
    e.run = run;
    e.state = state;
    e.active = active;
    return Advance(*host, e, "desktop", lang);
  }
  auto Start(const std::string& demo, const std::string& lang = "ja")
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
  for (const auto& [text, step] : interests)
    EXPECT_EQ(Advance(*host, Event("interest", text), "desktop", "en").step,
              step);
}
TEST_F(GuideFixture,
       TourRunsWithoutConfirmationsThroughFailureRetryChangeAndFarewell) {
  const std::string lang = "ja";
  const std::string demo = "report";
  const auto script =
      Parse(ReadResource("hostd/agent/Shared/guide-script.json"));
  const auto next = Members(script, "/tours/report/steps/result/next");
  const auto choose_destination = [&](const H::GuideStateT& state,
                                      std::string_view destination) {
    for (const auto& [label, target] : next)
      if (String(target) == destination) return Choose(state, label, lang);
    throw std::runtime_error("Missing tour transition");
  };
  auto s = Start(demo, lang);
  ASSERT_EQ(s.step, demo + "/start");
  EXPECT_EQ(s.highlight, "run");
  EXPECT_TRUE(s.choices.empty());
  EXPECT_TRUE(std::ranges::any_of(
      s.actions, [](const auto& action) { return action->kind == "run"; }));
  // A completed client action is not a completed run or an acknowledgement
  // the person has to send. Only the observed terminal state advances.
  auto action = Event("action_finished");
  action.project = "/copy";
  EXPECT_EQ(Advance(*host, action, "desktop", lang).step, s.step);
  EXPECT_FALSE(Observe("r1", "running", true, lang).present);
  s = Observe("r1", "failed", false, lang);
  ASSERT_EQ(s.step, demo + "/failed");
  EXPECT_EQ(s.highlight, "logs");
  EXPECT_TRUE(std::ranges::any_of(
      s.actions, [](const auto& action) { return action->kind == "run"; }));
  Observe("r2", "running", true, lang);
  s = Observe("r2", "succeeded", false, lang);
  ASSERT_EQ(s.step, demo + "/result");
  // An unchanged rerun stays on the results step.
  Observe("same-settings", "running", true, lang);
  s = Observe("same-settings", "succeeded", false, lang);
  EXPECT_EQ(s.step, demo + "/result");
  s = choose_destination(s, "change");
  ASSERT_EQ(s.step, demo + "/change");
  EXPECT_TRUE(s.highlight.starts_with("param:"));
  EXPECT_TRUE(s.choices.empty());
  EXPECT_TRUE(std::ranges::any_of(
      s.actions, [](const auto& action) { return action->kind == "run"; }));
  Observe("r3", "running", true, lang);
  s = Observe("r3", "cancelled", false, lang);
  EXPECT_EQ(s.step, demo + "/failed");
  Observe("r4", "running", true, lang);
  s = Observe("r4", "succeeded", false, lang);
  ASSERT_EQ(s.step, demo + "/result");
  s = choose_destination(s, "farewell");
  EXPECT_TRUE(s.end_tour);
  EXPECT_EQ(s.tour, nullptr);
  EXPECT_TRUE(s.present);
  EXPECT_FALSE(Observe("r5", "succeeded", false, lang).present);
}
TEST_F(GuideFixture, LateRunDuplicateRunAndTwoWindowsDoNotAdvanceTwice) {
  auto window = ConnectHost(true);
  auto s = Start("report");
  Observe("late", "running", true);
  Observe("current", "running", true);
  const auto late = Observe("late", "failed", false);
  EXPECT_FALSE(late.present);
  EXPECT_EQ(late.step, s.step);
  s = Observe("current", "succeeded", false);
  ASSERT_EQ(s.step, "report/result");
  EXPECT_FALSE(Observe("current", "succeeded", false).present);
  EXPECT_FALSE(Observe("late", "failed", false).present);
  const auto old = s;
  const auto choice = s.choices.front();
  auto first =
      std::async(std::launch::async, [&] { return Choose(old, choice); });
  auto second = std::async(std::launch::async, [&] {
    return Choose(old, choice, "ja", window.get());
  });
  s = first.get();
  const auto stale = second.get();
  EXPECT_EQ(s.step, "report/change");
  EXPECT_EQ(stale.step, s.step);
  EXPECT_EQ(GuideGet(*window).step, s.step);
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
  Observe("before-resume", "running", true);
  s = Observe("before-resume", "succeeded", false);
  s = Choose(s, s.choices.front());
  ASSERT_EQ(s.step, "report/change");
  auto e = Event("resume");
  e.project = "/copy";
  e.connected = true;
  e.run = "before-resume";
  e.state = "succeeded";
  EXPECT_EQ(Advance(*host, e).step, s.step);
  EXPECT_FALSE(GuideGet(*host, "web:one").setup_done);
  EXPECT_FALSE(GuideGet(*host, "web:two").tour);
  Advance(*host, Event("setup"), "web:one");
  EXPECT_FALSE(GuideGet(*host, "web:two").setup_done);
  host.reset();
  human.reset();
  external.reset();
  service.Stop();
  StartHost();
  s = GuideGet(*host);
  EXPECT_TRUE(s.setup_done);
  EXPECT_TRUE(s.demo_answered);
  ASSERT_NE(s.tour, nullptr);
  EXPECT_TRUE(s.step.empty());
  EXPECT_EQ(Advance(*host, e).step, "report/start");
  EXPECT_TRUE(GuideGet(*host, "web:one").setup_done);
  EXPECT_FALSE(GuideGet(*host, "web:two").setup_done);
}
TEST_F(GuideFixture, DemoCommandDistinguishesUnknownProjectsAndBundledCopies) {
  auto e = Event("message", "$demo");
  e.project = "/copy";
  e.title = "My analysis";
  auto s = Advance(*host, e);
  EXPECT_EQ(s.step, "otherProject");
  const auto index = Parse(ReadResource("demos/en/index.json"));
  for (const auto& demo : Array(index, "/demos"))
    if (String(demo, "/id") == "pharmacokinetics")
      e.title = String(demo, "/title");
  s = Advance(*host, e, "desktop", "en");
  ASSERT_EQ(s.step, "pharmacokinetics/start");
  ASSERT_NE(s.tour, nullptr);
  EXPECT_EQ(s.tour->path, "/copy");
}
}  // namespace
}  // namespace tsugumi::test
