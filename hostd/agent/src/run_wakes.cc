#include "agent/run_wakes.hh"

#include <algorithm>
#include <chrono>
#include <vector>

#include "agent/store.hh"
#include "agent/turns.hh"
#include "tsugumi/util/json_object.hh"
#include "tsugumi/util/log.hh"

namespace tsugumi::agent {
namespace A = Tsugumi::Agent;
namespace {
// Wake turns carry this prefix: they are not the person's turns.
constexpr std::string_view kTurnPrefix = "run-wake:";
constexpr size_t kFailedCards = 5;

auto ObjectOr(std::string_view raw) -> json::Members {
  try {
    return json::Object(raw);
  } catch (const std::exception&) {
    return {};
  }
}

auto Ended(std::string_view state) -> bool {
  return state == "succeeded" || state == "failed" || state == "cancelled";
}

auto FirstLine(std::string text) -> std::string {
  const auto start = text.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return {};
  text = text.substr(start, text.find('\n', start) - start);
  if (text.size() > 200) text = text.substr(0, 200) + "…";
  return text;
}

auto Failure(const json::Members& failure) -> std::string {
  const auto kind = json::Text(failure, "kind");
  const auto line = FirstLine(json::Text(failure, "diagnostic"));
  if (kind.empty()) return line;
  return line.empty() ? kind : kind + ": " + line;
}

auto CardName(std::string_view raw) -> std::string {
  std::string out;
  try {
    for (const auto& segment :
         json::Array(json::Get(ObjectOr(raw), "segments", "[]"))) {
      if (!out.empty()) out += '/';
      out += json::String(segment);
    }
  } catch (const std::exception&) {
  }
  return out;
}
}  // namespace

RunWakes::RunWakes(Turns& turns, ProjectCall call)
    : turns_(turns),
      call_(std::move(call)),
      delivery_([this](std::stop_token stop) { Loop(stop); }) {}

RunWakes::~RunWakes() {
  std::vector<std::shared_ptr<std::jthread>> watchers;
  {
    std::lock_guard lock(mutex_);
    for (auto& [_, wake] : wakes_)
      if (wake.watcher) watchers.push_back(std::move(wake.watcher));
    watchers.insert(watchers.end(), retired_.begin(), retired_.end());
    retired_.clear();
  }
  for (auto& watcher : watchers) watcher->request_stop();
  watchers.clear();
  delivery_.request_stop();
  if (delivery_.joinable()) delivery_.join();
}

auto RunWakes::ToolResult(const std::string& conversation,
                          const std::string& project, const std::string& name,
                          const std::string& result) -> void {
  const auto envelope = ObjectOr(result);
  if (json::Text(envelope, "reply") == "Failure") return;
  const auto value = ObjectOr(json::Get(envelope, "value", "{}"));
  const auto status =
      name == "run" ? ObjectOr(json::Get(value, "run", "{}")) : value;
  const auto run = json::Text(status, "run_id");
  if (run.empty()) return;
  const Key key{conversation, run};
  if (Ended(json::Text(status, "state"))) {
    // The Agent has read how the run ended: nothing to bring back.
    std::lock_guard lock(mutex_);
    done_.insert(key);
    if (const auto found = wakes_.find(key); found != wakes_.end()) {
      if (found->second.watcher) {
        found->second.watcher->request_stop();
        retired_.push_back(std::move(found->second.watcher));
      }
      wakes_.erase(found);
      changed_.notify_all();
    }
    return;
  }
  if (name != "run") return;
  int64_t turn = 0;
  try {
    std::lock_guard state(turns_.mutex);
    const auto c = turns_.conversations->GetConversation(conversation);
    if (c.role == "guide") return;
    auto latest = turns_.conversations->ConversationDatabase(conversation)
                      .prepare(
                          "SELECT coalesce(max(rowid),0) FROM "
                          "conversation_turns WHERE conversation=?");
    latest.bind(1, conversation);
    latest.step();
    turn = latest.i64(0);
  } catch (const std::exception&) {
    return;  // the conversation is gone
  }
  std::lock_guard lock(mutex_);
  if (wakes_.contains(key) || done_.contains(key)) return;
  auto& wake = wakes_[key];
  wake.conversation = conversation;
  wake.project = project;
  wake.run_id = run;
  wake.turn_rowid = turn;
  wake.watcher = std::make_shared<std::jthread>(
      [this, key, project](std::stop_token stop) { Watch(key, project, stop); });
}

auto RunWakes::Watch(Key key, std::string project, std::stop_token stop)
    -> void {
  const auto& run = key.second;
  const auto pause = [&] {
    std::mutex wait;
    std::unique_lock lock(wait);
    std::condition_variable_any().wait_for(lock, stop, std::chrono::seconds(2),
                                           [] { return false; });
  };
  json::Members status;
  for (int failures = 0;;) {
    if (stop.stop_requested()) return;
    std::string out;
    try {
      out = call_(project, "run_status",
                  json::Encode(json::Members{{"run_id", json::Quote(run)},
                                             {"wait_seconds", "300"}}),
                  stop);
    } catch (const std::exception&) {
      // The project may be restarting; a run it lost ends as interrupted.
      if (stop.stop_requested() || ++failures > 5) return;
      pause();
      continue;
    }
    const auto envelope = ObjectOr(out);
    if (json::Text(envelope, "reply") == "Failure") {
      if (++failures > 5) return;
      pause();
      continue;
    }
    status = ObjectOr(json::Get(envelope, "value", "{}"));
    if (Ended(json::Text(status, "state"))) break;
    failures = 0;
  }
  const auto state = json::Text(status, "state");
  std::string text = "[Tsugumi, not the person] Run " + run +
                     ", which you started in this conversation, ended after "
                     "your reply: " +
                     state;
  if (const auto failure = Failure(ObjectOr(json::Get(status, "failure")));
      !failure.empty())
    text += " (" + failure + ")";
  text += ".";
  if (state == "failed") {
    std::vector<std::string> failed;
    size_t more = 0;
    try {
      const auto latest = ObjectOr(call_(
          project, "run_latest_invocations",
          json::Encode(json::Members{{"run_id", json::Quote(run)}}), stop));
      for (const auto& raw : json::Array(json::Get(
               ObjectOr(json::Get(latest, "value", "{}")), "records", "[]"))) {
        const auto record = ObjectOr(raw);
        if (json::Text(record, "state") != "failed") continue;
        if (failed.size() == kFailedCards) {
          ++more;
          continue;
        }
        auto line = CardName(json::Get(record, "card"));
        if (const auto failure =
                Failure(ObjectOr(json::Get(record, "failure")));
            !failure.empty())
          line += " (" + failure + ")";
        failed.push_back(std::move(line));
      }
    } catch (const std::exception&) {
      // The note still says how the run ended.
    }
    if (!failed.empty()) {
      text += " Failed Cards: ";
      for (size_t i = 0; i < failed.size(); ++i)
        text += (i ? "; " : "") + failed[i];
      if (more) text += "; and " + std::to_string(more) + " more";
      text += ".";
    }
  }
  text += " Continue with what this outcome needs.";
  const auto note = json::Encode(json::Members{{"kind", json::Quote("run_ended")},
                                               {"run_id", json::Quote(run)},
                                               {"state", json::Quote(state)}});
  if (stop.stop_requested()) return;
  std::lock_guard lock(mutex_);
  const auto found = wakes_.find(key);
  if (found == wakes_.end()) return;
  found->second.ended = true;
  found->second.ended_order = ++order_;
  found->second.text = std::move(text);
  found->second.note = note;
  pending_.push_back(key.first);
  changed_.notify_all();
}

auto RunWakes::TurnEnded(const std::string& conversation) -> void {
  std::lock_guard lock(mutex_);
  pending_.push_back(conversation);
  changed_.notify_all();
}

auto RunWakes::Forget(const std::string& conversation) -> void {
  std::lock_guard lock(mutex_);
  for (auto it = wakes_.begin(); it != wakes_.end();) {
    if (it->first.first != conversation) {
      ++it;
      continue;
    }
    if (it->second.watcher) {
      it->second.watcher->request_stop();
      retired_.push_back(std::move(it->second.watcher));
    }
    it = wakes_.erase(it);
  }
  std::erase_if(done_, [&](const Key& key) { return key.first == conversation; });
  changed_.notify_all();
}

auto RunWakes::Loop(std::stop_token stop) -> void {
  while (!stop.stop_requested()) {
    std::string next;
    std::vector<std::shared_ptr<std::jthread>> retired;
    {
      std::unique_lock lock(mutex_);
      changed_.wait(lock, stop,
                    [&] { return !pending_.empty() || !retired_.empty(); });
      if (stop.stop_requested()) return;
      retired.swap(retired_);
      if (!pending_.empty()) {
        next = std::move(pending_.front());
        pending_.pop_front();
      }
    }
    // Joining waits for a stopped watcher, outside the lock it may need.
    retired.clear();
    if (next.empty()) continue;
    try {
      Deliver(next);
    } catch (const std::exception& error) {
      log::warn(std::string("cannot continue a conversation after its run: ") +
                error.what());
    }
  }
}

auto RunWakes::Deliver(const std::string& conversation) -> void {
  std::lock_guard state(turns_.mutex);
  if (turns_.stopping || turns_.running.contains(conversation)) return;
  std::vector<Wake> ready;
  {
    std::lock_guard lock(mutex_);
    for (auto it = wakes_.begin(); it != wakes_.end();) {
      if (it->first.first != conversation || !it->second.ended) {
        ++it;
        continue;
      }
      if (it->second.watcher) retired_.push_back(std::move(it->second.watcher));
      done_.insert(it->first);
      ready.push_back(std::move(it->second));
      it = wakes_.erase(it);
    }
    if (!retired_.empty()) changed_.notify_all();
  }
  if (ready.empty()) return;
  std::ranges::sort(ready, {}, &Wake::ended_order);
  const bool on = turns_.RunWake(conversation);
  auto& store = turns_.conversations->ConversationDatabase(conversation);
  auto latest = store.prepare(
      "SELECT state FROM conversation_turns WHERE conversation=? ORDER BY "
      "rowid DESC LIMIT 1");
  latest.bind(1, conversation);
  // Only after a turn that finished: a stopped, failed or interrupted turn
  // waits for the person.
  const bool finished = latest.step() && latest.text(0) == "finished";
  for (size_t i = 0; i < ready.size(); ++i) {
    auto& wake = ready[i];
    auto spoke = store.prepare(
        "SELECT count(*) FROM conversation_turns WHERE conversation=? AND "
        "rowid>? AND substr(id,1,?)<>?");
    spoke.bind(1, conversation)
        .bind(2, wake.turn_rowid)
        .bind(3, static_cast<int64_t>(kTurnPrefix.size()))
        .bind(4, std::string(kTurnPrefix));
    spoke.step();
    if (!on || !finished || spoke.i64(0) > 0) continue;
    // One run per turn; the others wait for that turn to end.
    {
      std::lock_guard lock(mutex_);
      for (size_t j = i + 1; j < ready.size(); ++j)
        wakes_.emplace(Key{conversation, ready[j].run_id},
                       std::move(ready[j]));
    }
    turns_.Start(false, conversation,
                 std::string(kTurnPrefix) + wake.run_id + ":" +
                     std::to_string(wake.ended_order),
                 wake.text, A::Action::Resume, {}, {}, wake.note);
    return;
  }
}
}  // namespace tsugumi::agent
