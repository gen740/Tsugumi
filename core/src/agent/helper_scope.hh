#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>

#include "agent/json.hh"

namespace tsugumi::agent {

// An exact set of permanent Card paths. A container never grants its children.
// The reader is private to hostd: resolving a nested path must not disclose
// the enclosing project's document to the helper.
class HelperScope {
public:
  // Missing files return an empty string; transport/revision errors throw.
  using Reader = std::function<std::string(const std::string &)>;
  explicit HelperScope(const json::Members &task,
                       std::string authorization = {}, std::string scratch_identity = {});
  auto authorization() const -> const std::string & { return authorization_; }
  auto RequireRun(const std::string &card) const -> void;
  auto Ran(const std::string &run_id, const std::string &card) -> void;
  static auto Validate(const json::Members &task, bool allow_unscoped) -> void;
  auto Check(const std::string &tool, const json::Members &args,
             const Reader &read) const -> void;
  auto Inspect(const std::string &result, const Reader &read) const
      -> std::string;
  auto Completed(const std::string &tool, const json::Members &args,
                 const std::string &result, const Reader &read) -> void;
  auto Restore(const std::string &receipt, const Reader &read) -> void;
  auto CheckResult(const std::string &result, const Reader &read) const -> void;
  auto Created(const std::string &id, const std::string &project_path,
               const Reader &read) -> void;
  auto CheckPlan(const std::string &result, bool executing) const -> void;
  auto scoped() const -> bool { return scoped_; }
  auto scratch_identity() const -> const std::string& { return scratch_identity_; }

private:
  auto Require(const std::string &card, bool writing) const -> void;
  auto File(const std::string &path, bool writing, const Reader &read) const
      -> void;
  auto Root(const std::string &card, const Reader &read,
            bool writing = false) const -> std::string;
  bool scoped_;
  std::set<std::string> reads_, writes_, runs_;
  std::string authorization_;
  std::string scratch_identity_;
  std::map<std::string, std::string> runs_started_;
  std::map<std::string, std::string> created_;
};
} // namespace tsugumi::agent
