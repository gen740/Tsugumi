// One open project: hostd starts its environment (EnvironmentRequest Start),
// the UI connects to the project's tsugumid with the endpoint and token it
// gives, opens the project (ProjectOpenRequest) and follows its
// notifications. All state is read and changed on the main thread.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "canvas_scene.hh"
#include "host_link.hh"
#include "log_window.hh"
#include "tsugumi/client/client.hh"
#include "tsugumi/protocol/tsugumi_generated.h"

namespace tsugumi::linux_ui {

// A LogAvailable notification for a top-level Card.
struct LogNotice {
  std::string run_id;
  std::string card;
  Tsugumi::Data::LogStream stream = Tsugumi::Data::LogStream::unspecified;
  std::optional<uint64_t> end_offset;
};

class ProjectSession : public std::enable_shared_from_this<ProjectSession> {
 public:
  enum class Phase { kStarting, kOpen, kFailed };

  ProjectSession(std::shared_ptr<HostLink> host, std::string path);
  ~ProjectSession();
  ProjectSession(const ProjectSession&) = delete;
  auto operator=(const ProjectSession&) -> ProjectSession& = delete;

  auto Open() -> void;

  [[nodiscard]] auto phase() const -> Phase { return phase_; }
  // The project's folder as hostd names it (HostRecentRememberRequest).
  [[nodiscard]] auto path() const -> const std::string& { return path_; }
  [[nodiscard]] auto title() const -> const std::string& { return title_; }
  // While starting: what is being done; when failed: what went wrong.
  [[nodiscard]] auto message() const -> const std::string& { return message_; }
  [[nodiscard]] auto detail() const -> const std::string& { return detail_; }
  [[nodiscard]] auto scene() const -> const Scene& { return scene_; }
  [[nodiscard]] auto run_id() const -> const std::string& { return run_id_; }
  [[nodiscard]] auto run_state() const -> Tsugumi::Data::RunState {
    return run_state_;
  }
  [[nodiscard]] auto running() const -> bool;
  [[nodiscard]] auto CardState(const std::string& card) const
      -> Tsugumi::Data::CardState;

  // Runs from card through what is downstream of it (RunRequest, propagate).
  auto Run(const std::string& card) -> void;
  auto Cancel() -> void;
  // Reads part of a top-level Card's log stream in the current run.
  auto ReadLog(
      const std::string& card, Tsugumi::Data::LogStream stream,
      const LogRequest& request,
      std::function<void(std::optional<LogPiece>, std::string error)> done)
      -> void;

  // Anything shown changed.
  std::function<void()> on_changed;
  std::function<void(const LogNotice&)> on_log;

 private:
  auto Fail(std::string message, std::string detail = {}) -> void;
  auto Changed() -> void;
  auto StartEnvironment() -> void;
  auto Connect(const std::string& endpoint, const std::string& token) -> void;
  auto Reopen() -> void;
  auto Opened(const Tsugumi::RPC::OpenResult& result) -> void;
  auto CompleteLayout(std::string project_json, std::string layout_json,
                      std::vector<Definition> definitions) -> void;
  auto ReadInvocations() -> void;
  auto Notified(const Tsugumi::RPC::Notification& notification) -> void;
  auto RunEvent(const Tsugumi::RPC::RunEvent& event) -> void;
  auto SetRun(const Tsugumi::RPC::RunStatus& status) -> void;

  template <class T, class Value>
  auto ProjectCall(Value value, std::function<void(Reply<T>)> done) -> void;

  std::shared_ptr<HostLink> host_;
  int host_listener_ = 0;
  std::string path_;
  std::string title_;
  Phase phase_ = Phase::kStarting;
  std::string message_;
  std::string detail_;
  std::shared_ptr<client::Connection> connection_;
  std::string subscription_;
  uint64_t seq_ = 0;
  bool reopening_ = false;
  bool reopen_again_ = false;
  uint64_t rev_ = 0;
  Scene scene_;
  std::string run_id_;
  Tsugumi::Data::RunState run_state_ = Tsugumi::Data::RunState::idle;
  std::map<std::string, Tsugumi::Data::CardState> states_;
};

}  // namespace tsugumi::linux_ui
