// Brings a conversation back when a run its Agent started ends after the turn
// that started it.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace tsugumi::agent {
struct Turns;

class RunWakes {
 public:
  // A project RPC by tool name with JSON arguments; returns the tool's JSON
  // result envelope. run_status accepts wait_seconds.
  using ProjectCall = std::function<std::string(
      const std::string& project, const std::string& name,
      const std::string& arguments, std::stop_token stop)>;
  RunWakes(Turns& turns, ProjectCall call);
  ~RunWakes();
  RunWakes(const RunWakes&) = delete;
  auto operator=(const RunWakes&) -> RunWakes& = delete;

  // A model turn's run or run_status result. A run that is still active after
  // run is watched; a result showing it ended means the Agent saw the end.
  auto ToolResult(const std::string& conversation, const std::string& project,
                  const std::string& name, const std::string& result) -> void;
  // Called with the state mutex held, after the turn no longer runs.
  auto TurnEnded(const std::string& conversation) -> void;
  auto Forget(const std::string& conversation) -> void;

 private:
  struct Wake {
    std::string conversation;
    std::string project;
    std::string run_id;
    // The turn that started the run; a later turn from the person drops it.
    int64_t turn_rowid = 0;
    bool ended = false;
    int64_t ended_order = 0;
    std::string text;
    std::string note;
    std::shared_ptr<std::jthread> watcher;
  };
  using Key = std::pair<std::string, std::string>;  // conversation, run
  auto Watch(Key key, std::string project, std::stop_token stop) -> void;
  auto Deliver(const std::string& conversation) -> void;
  auto Loop(std::stop_token stop) -> void;

  Turns& turns_;
  ProjectCall call_;
  std::mutex mutex_;
  std::condition_variable_any changed_;
  std::map<Key, Wake> wakes_;
  // Runs already brought back or dropped: each run at most once.
  std::set<Key> done_;
  std::vector<std::shared_ptr<std::jthread>> retired_;
  std::deque<std::string> pending_;
  int64_t order_ = 0;
  std::jthread delivery_;
};
}  // namespace tsugumi::agent
