#include "host/guide.hh"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>

#include "agent/json.hh"
#include "agent/wire.hh"
#include "util/crypto.hh"
#include "util/fs.hh"

extern const unsigned char tsugumi_guide_script_ja[];
extern const unsigned long tsugumi_guide_script_ja_size;
extern const unsigned char tsugumi_guide_script_en[];
extern const unsigned long tsugumi_guide_script_en_size;

namespace tsugumi::host {
namespace {
namespace H = Tsugumi::Host;
namespace json = agent::json;
auto Text(const flatbuffers::String *s) -> std::string {
  return s ? s->str() : "";
}
auto Lower(std::string s) -> std::string {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}
auto Path(const std::string &s) -> std::string {
  if (s.empty())
    return {};
  return std::filesystem::weakly_canonical(s).string();
}
struct Step {
  H::GuideStateT show;
  std::map<std::string, std::string> next;
  std::string ok, otherwise;
  bool home = false;
};
struct Script {
  std::map<std::string, Step> steps;
  std::vector<std::pair<std::string, std::vector<std::string>>> tours;
  explicit Script(std::string raw) {
    const auto root = json::Object(raw);
    const auto all = json::Object(json::Get(root, "tours"));
    std::vector<std::string> order;
    for (const auto &[demo, tour] : all)
      order.push_back(demo);
    std::sort(order.begin(), order.end(), [&](const auto &a, const auto &b) {
      return raw.find(json::Quote(a) + ":") < raw.find(json::Quote(b) + ":");
    });
    auto add = [&](const std::string &id, const std::string &value,
                   const std::string &demo, bool home) {
      const auto r = json::Object(value);
      const auto show = json::Object(json::Get(r, "show", "{}"));
      auto resolve = [&](std::string target) {
        return target.empty() || demo.empty() ||
                       target.find('/') != std::string::npos
                   ? target
                   : demo + "/" + target;
      };
      Step s;
      s.home = home;
      s.show.step = id;
      s.show.say = json::Text(show, "say");
      s.show.text = json::Text(r, "text", s.show.say);
      s.show.highlight = json::Text(show, "highlight");
      s.show.open = json::Text(show, "open");
      s.show.end_tour = json::Bool(json::Get(show, "endTour"));
      for (const auto &c : json::Array(json::Get(show, "choices", "[]")))
        s.show.choices.push_back(json::String(c));
      for (const auto &a : json::Array(json::Get(show, "actions", "[]"))) {
        const auto v = json::Object(a);
        auto action = std::make_unique<H::GuideActionT>();
        action->kind = json::Text(v, "kind");
        action->label = json::Text(v, "label", action->kind);
        action->demo = json::Text(v, "demo");
        action->card = json::Text(v, "card");
        s.show.actions.push_back(std::move(action));
      }
      for (const auto &[choice, target] :
           json::Object(json::Get(r, "next", "{}")))
        s.next[choice] = resolve(json::String(target));
      const auto ran = json::Object(json::Get(r, "ran", "{}"));
      s.ok = resolve(json::Text(ran, "ok"));
      s.otherwise = resolve(json::Text(ran, "else"));
      steps.emplace(id, std::move(s));
    };
    for (const auto &demo : order) {
      const auto t = json::Object(all.at(demo));
      std::vector<std::string> keywords;
      for (const auto &k : json::Array(json::Get(t, "keywords")))
        keywords.push_back(Lower(json::String(k)));
      tours.emplace_back(demo, std::move(keywords));
      for (const auto &group : {"home", "steps"})
        for (const auto &[id, step] : json::Object(json::Get(t, group)))
          add(demo + "/" + id, step, demo, std::string(group) == "home");
    }
    add("noConnection", json::Get(root, "noConnection"), "", false);
    add("otherProject", json::Get(root, "otherProject"), "", false);
  }
  auto Start(const std::string &demo, const std::string &kind) const
      -> std::string {
    return steps.contains(demo + "/" + kind) ? demo + "/" + kind
                                             : "otherProject";
  }
  auto Demo(const std::string &interest) const -> std::string {
    const auto text = Lower(interest);
    for (const auto &[demo, keywords] : tours)
      for (const auto &k : keywords)
        if (text.find(k) != std::string::npos)
          return demo;
    return tours.front().first;
  }
};
struct Progress {
  bool setup = false, offered = false;
  std::unique_ptr<H::GuideTourT> tour;
  std::string step, shown = "noConnection", seen, active_run, project;
  bool active = false, fallback = false, fallback_here = false, in_chat = false;
  std::set<std::string> finished;
  uint64_t revision = 0, answer_revision = 0;
};
} // namespace

struct Guide::State {
  std::filesystem::path directory, demos;
  Script ja{std::string(reinterpret_cast<const char *>(tsugumi_guide_script_ja),
                        tsugumi_guide_script_ja_size)};
  Script en{std::string(reinterpret_cast<const char *>(tsugumi_guide_script_en),
                        tsugumi_guide_script_en_size)};
  std::map<std::string, Progress> sessions;
  auto File(const std::string &session) const -> std::filesystem::path {
    return directory / "guides" / (crypto::sha256_hex(session) + ".json");
  }
  auto Get(const std::string &session) -> Progress & {
    if (session.empty())
      throw std::runtime_error("A guide session is required");
    auto [it, added] = sessions.try_emplace(session);
    if (added)
      if (auto raw = fs::read_file(File(session))) {
        const auto v = json::Object(*raw);
        it->second.setup = json::Bool(json::Get(v, "setup"));
        it->second.offered = json::Bool(json::Get(v, "offered"));
        if (json::Get(v, "tour") != "null") {
          const auto t = json::Object(json::Get(v, "tour"));
          auto tour = std::make_unique<H::GuideTourT>();
          tour->demo = json::Text(t, "demo");
          tour->title = json::Text(t, "title");
          tour->path = json::Text(t, "path");
          tour->started = json::Bool(json::Get(t, "started"));
          it->second.tour = std::move(tour);
        }
      }
    return it->second;
  }
  auto Save(const std::string &session, const Progress &p) -> void {
    std::filesystem::create_directories(File(session).parent_path());
    fs::write_atomic(File(session), Persistent(p), 0600);
  }
  static auto Persistent(const Progress &p) -> std::string {
    const auto tour =
        p.tour ? json::Encode(json::Members{
                     {"demo", json::Quote(p.tour->demo)},
                     {"title", json::Quote(p.tour->title)},
                     {"path", json::Quote(p.tour->path)},
                     {"started", p.tour->started ? "true" : "false"}})
               : "null";
    return json::Encode(json::Members{{"setup", p.setup ? "true" : "false"},
                                      {"offered", p.offered ? "true" : "false"},
                                      {"tour", tour}});
  }
  auto Show(const std::string &session, const Progress &p,
            const Script &script) const -> H::GuideStateT {
    auto out = script.steps.at(p.shown).show;
    out.step = p.step;
    if (p.fallback) {
      const auto current = script.steps.find(p.step);
      out.highlight = p.fallback_here && current != script.steps.end()
                          ? current->second.show.highlight
                          : "";
      out.choices = p.fallback_here && current != script.steps.end()
                        ? current->second.show.choices
                        : std::vector<std::string>{};
      if (p.in_chat && p.fallback_here && current != script.steps.end())
        std::erase_if(out.actions,
                      [](const auto &a) { return a->kind == "open_demo"; });
    }
    out.project = p.project;
    out.session_id = session;
    out.revision = p.revision;
    out.answer_revision = p.answer_revision;
    out.setup_done = p.setup;
    out.demo_answered = p.offered;
    if (p.tour)
      out.tour = std::make_unique<H::GuideTourT>(*p.tour);
    return out;
  }
  auto Advance(const std::string &session, const std::string &language,
               const H::GuideInputT &e) -> H::GuideStateT {
    auto &p = Get(session);
    const auto before_persistent = Persistent(p);
    const auto &script = language == "en" ? en : ja;
    if (e.kind == "action_finished")
      return Show(session, p, script);
    bool present = false;
    std::string note, asked;
    auto play = [&](const std::string &id) {
      if (!script.steps.contains(id))
        return;
      p.step = p.shown = id;
      p.fallback = false;
      p.project = e.project;
      present = true;
      if (script.steps.at(id).show.end_tour)
        p.tour.reset();
    };
    auto baseline = [&] {
      p.seen = e.connected ? e.run + ":" + e.state : "offline";
      p.active = e.active;
      p.active_run = e.active ? e.run : "";
    };
    auto begin = [&](const std::string &demo, bool started) {
      p.offered = true;
      p.tour = std::make_unique<H::GuideTourT>();
      p.tour->demo = demo;
      p.tour->title = e.title;
      p.tour->path = Path(e.project);
      p.tour->started = started;
      p.seen.clear();
      p.active_run.clear();
      p.active = false;
      p.finished.clear();
    };
    if (e.kind == "connection") {
      if (e.connected)
        p.setup = true;
    } else if (e.kind == "setup")
      p.setup = true;
    else if (e.kind == "introduction") {
      p.setup = false;
      p.offered = false;
    } else if (e.kind == "offered")
      p.offered = true;
    else if (e.kind == "start")
      begin(e.demo, false);
    else if (e.kind == "end") {
      p.tour.reset();
      p.seen.clear();
    } else if (e.kind == "interest") {
      p.offered = true;
      play(script.Demo(e.text) + "/pick");
    } else if (e.kind == "resume" && p.tour) {
      baseline();
      note = "resume";
      if (e.scripted)
        play(p.step.starts_with(p.tour->demo + "/")
                 ? p.step
                 : script.Start(p.tour->demo, "resume"));
    } else if (e.kind == "message") {
      std::istringstream words(e.text);
      std::string word;
      bool demo_command = false;
      while (words >> word)
        if (word == "$demo")
          demo_command = true;
      if (!e.project.empty() && demo_command) {
        // Titles are presentation facts; which bundled demos have tours is core
        // policy.
        std::string demo;
        const auto index =
            demos / (language == "en" ? "en" : "ja") / "index.json";
        if (auto raw = fs::read_file(index))
          for (const auto &entry :
               json::Array(json::Get(json::Object(*raw), "demos"))) {
            const auto v = json::Object(entry);
            const auto id = json::Text(v, "id");
            if (json::Text(v, "title") == e.title &&
                script.Start(id, "start") != "otherProject") {
              demo = id;
              break;
            }
          }
        if (demo.empty())
          play("otherProject");
        else {
          begin(demo, true);
          baseline();
          play(script.Start(demo, "start"));
        }
      } else {
        const auto current = script.steps.find(p.step);
        const bool here =
            !e.project.empty()
                ? p.tour && p.tour->path == Path(e.project)
                : current != script.steps.end() && current->second.home;
        if (here && e.expected_step != p.step) {
          auto out = Show(session, p, script);
          out.present = true;
          return out;
        }
        if (here && current != script.steps.end() &&
            current->second.next.contains(e.text))
          play(current->second.next.at(e.text));
        else {
          p.shown = "noConnection";
          p.fallback = true;
          p.in_chat = !e.project.empty();
          p.fallback_here = here;
          p.project = e.project;
          present = true;
        }
      }
    } else if (e.kind == "observe" && p.tour &&
               p.tour->path == Path(e.project)) {
      if (!p.tour->started && e.connected) {
        p.tour->started = true;
        baseline();
        note = "start";
        if (e.scripted)
          play(script.Start(p.tour->demo, "start"));
      } else {
        const auto now = e.connected ? e.run + ":" + e.state : "offline";
        const auto before = p.seen;
        if (before.empty())
          baseline();
        else if (before != now) {
          if (!e.connected) {
            if (p.active)
              note = "disconnected";
            p.seen = now;
          } else if (before == "offline") {
            if (p.active)
              note = "reconnected";
            baseline();
          } else if (e.active) {
            if (!p.finished.contains(e.run)) {
              if (!p.active_run.empty() && p.active_run != e.run)
                p.finished.insert(p.active_run);
              baseline();
            }
          } else if (!p.active_run.empty() && p.active_run != e.run)
            p.finished.insert(e.run);
          else if (!e.state.empty() && e.state != "none" &&
                   !p.finished.contains(e.run) &&
                   (p.active_run.empty() || p.active_run == e.run) &&
                   (p.active || !before.starts_with(e.run + ":"))) {
            p.finished.insert(e.run);
            baseline();
            note = "run";
            const auto current = script.steps.find(p.step);
            if (e.scripted && current != script.steps.end())
              play(e.state == "succeeded" ? current->second.ok
                                          : current->second.otherwise);
          }
        }
      }
    } else if (e.kind != "action_finished" && e.kind != "observe" &&
               e.kind != "resume") {
      throw std::runtime_error("Unknown guide event: " + e.kind);
    }
    ++p.revision;
    if (present && e.scripted)
      p.answer_revision = p.revision;
    if (Persistent(p) != before_persistent)
      Save(session, p);
    auto out = Show(session, p, script);
    out.present = present && e.scripted;
    out.note = note;
    // Display strings match the clients' run labels in both languages.
    const bool english = language == "en";
    if (note == "start")
      asked = english ? "Starting the tour of the demo “" + e.title + "”"
                      : "デモ「" + e.title + "」の案内を始めます";
    if (note == "resume")
      asked = english ? "Continuing the demo tour" : "デモの案内を続けます";
    if (note == "disconnected")
      asked = english ? "Lost the connection to the environment"
                      : "実行環境との接続が切れました";
    if (note == "reconnected")
      asked = english ? "Reconnected to the environment"
                      : "実行環境に再接続しました";
    if (note == "run") {
      const std::map<std::string, std::pair<std::string, std::string>> labels{
          {"succeeded", {"成功", "Succeeded"}},
          {"failed", {"失敗", "Failed"}},
          {"cancelled", {"停止済み", "Stopped"}}};
      const auto label =
          labels.contains(e.state)
              ? (english ? labels.at(e.state).second : labels.at(e.state).first)
              : e.state;
      asked = english ? "The run ended (" + label + ")"
                      : "実行が終わりました（" + label + "）";
    }
    out.asked = asked;
    return out;
  }
};
Guide::Guide(std::filesystem::path directory, std::filesystem::path demos)
    : state_(std::make_unique<State>()) {
  state_->directory = std::move(directory);
  state_->demos = std::move(demos);
}
Guide::~Guide() = default;
auto Guide::Handles(H::RequestPayload type) -> bool {
  return type == H::RequestPayload::GuideGetRequest ||
         type == H::RequestPayload::GuideAdvanceRequest;
}
auto Guide::Handle(const H::Request &request, uint64_t id) -> std::string {
  if (const auto *r = request.value_as_GuideGetRequest()) {
    const auto session = Text(r->session_id());
    return agent::wire::HostReply(
        id,
        state_->Show(session, state_->Get(session),
                     Text(r->language()) == "en" ? state_->en : state_->ja));
  }
  const auto *r = request.value_as_GuideAdvanceRequest();
  const auto event = std::unique_ptr<H::GuideInputT>(r->event()->UnPack());
  return agent::wire::HostReply(
      id, state_->Advance(Text(r->session_id()), Text(r->language()), *event));
}
} // namespace tsugumi::host
