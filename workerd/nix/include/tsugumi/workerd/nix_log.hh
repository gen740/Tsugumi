#pragma once

#include <functional>
#include <mutex>
#include <nix/util/logging.hh>
#include <nix/util/terminal.hh>
#include <sstream>

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
  auto log(nix::Verbosity level, std::string_view text) -> void override {
    if (level <= nix::lvlInfo) emit(std::string(text));
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
  auto emit(const std::string& text) -> void {
    std::lock_guard lock(mutex_);
    if (sink_) sink_(nix::filterANSIEscapes(text, true) + "\n");
  }
  std::mutex mutex_;
  std::function<void(const std::string&)> sink_;
};
}  // namespace tsugumi
