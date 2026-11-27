#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <nix/util/logging.hh>
#include <nix/util/terminal.hh>
#include <sstream>

#include "tsugumi/util/text.hh"

namespace tsugumi {
class NixLogSink final : public nix::Logger {
 public:
  explicit NixLogSink(std::function<void(const std::string&)> sink)
      : sink_(std::move(sink)) {}
  NixLogSink() = default;
  auto set(std::function<void(const std::string&)> sink) -> void {
    std::lock_guard lock(mutex_);
    sink_ = std::move(sink);
  }
  // The calling thread's own sink, ahead of the shared one: calls on several
  // threads at once each log to their own Card. The caller keeps it alive.
  auto set_thread(const std::function<void(const std::string&)>* sink) -> void {
    thread_sink_ = sink;
  }
  // The calling thread's receiver of derivation builds starting (true) and
  // ending (false). The caller keeps it alive.
  using BuildActivity = std::function<void(const std::string&, bool)>;
  auto set_thread_builds(const BuildActivity* builds) -> void {
    thread_builds_ = builds;
  }
  auto set_substitutions(
      std::function<void(const std::string&, const std::string&)> receiver)
      -> void {
    substitutions_ = std::move(receiver);
  }
  auto log(nix::Verbosity level, std::string_view message) -> void override {
    if (level <= nix::lvlInfo) emit(message);
  }
  auto logEI(const nix::ErrorInfo& info) -> void override {
    std::ostringstream text;
    nix::showErrorInfo(text, info, false);
    emit(text.str());
  }
  auto startActivity(nix::ActivityId id, nix::Verbosity level,
                     nix::ActivityType type, const std::string& text,
                     const Fields& fields, nix::ActivityId) -> void override {
    if (level <= nix::lvlInfo && !text.empty()) emit(text);
    if (type == nix::actSubstitute && substitutions_ && fields.size() >= 2 &&
        fields[0].type == Field::tString && fields[1].type == Field::tString)
      substitutions_(fields[0].s, fields[1].s);
    if (type == nix::actBuild && thread_builds_ && !fields.empty() &&
        fields[0].type == Field::tString) {
      {
        std::lock_guard lock(mutex_);
        builds_[id] = fields[0].s;
      }
      (*thread_builds_)(fields[0].s, true);
    }
  }
  auto stopActivity(nix::ActivityId id) -> void override {
    std::string drv;
    {
      std::lock_guard lock(mutex_);
      auto found = builds_.find(id);
      if (found == builds_.end()) return;
      drv = std::move(found->second);
      builds_.erase(found);
    }
    if (thread_builds_) (*thread_builds_)(drv, false);
  }
  auto result(nix::ActivityId, nix::ResultType type, const Fields& fields)
      -> void override {
    if (type != nix::resBuildLogLine && type != nix::resPostBuildLogLine)
      return;
    if (fields.empty() || fields[0].type != Field::tString) {
      emit("warning: invalid Nix build log record");
      return;
    }
    emit(fields[0].s);
  }

 private:
  auto emit(std::string_view message) -> void {
    auto forward = [&](const auto& sink) {
      sink(text::replace_invalid_utf8(nix::filterANSIEscapes(message, true)) +
           "\n");
    };
    if (thread_sink_) {
      forward(*thread_sink_);
      return;
    }
    std::lock_guard lock(mutex_);
    if (sink_) forward(sink_);
  }
  static inline thread_local const std::function<void(const std::string&)>*
      thread_sink_ = nullptr;
  static inline thread_local const BuildActivity* thread_builds_ = nullptr;
  std::mutex mutex_;
  std::map<nix::ActivityId, std::string> builds_;
  std::function<void(const std::string&)> sink_;
  std::function<void(const std::string&, const std::string&)> substitutions_;
};
}  // namespace tsugumi
