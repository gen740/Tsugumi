#pragma once

#include <functional>
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
  auto log(nix::Verbosity level, std::string_view message) -> void override {
    if (level <= nix::lvlInfo) emit(message);
  }
  auto logEI(const nix::ErrorInfo& info) -> void override {
    std::ostringstream text;
    nix::showErrorInfo(text, info, false);
    emit(text.str());
  }
  auto startActivity(nix::ActivityId, nix::Verbosity level, nix::ActivityType,
                     const std::string& text, const Fields&, nix::ActivityId)
      -> void override {
    if (level <= nix::lvlInfo && !text.empty()) emit(text);
  }
  auto result(nix::ActivityId, nix::ResultType type, const Fields& fields)
      -> void override {
    if ((type == nix::resBuildLogLine || type == nix::resPostBuildLogLine) &&
        !fields.empty() && fields[0].type == Field::tString)
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
  std::mutex mutex_;
  std::function<void(const std::string&)> sink_;
};
}  // namespace tsugumi
