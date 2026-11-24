#pragma once

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>

#include "host/authorization.hh"
#include "tsugumi/util/json_object.hh"

namespace tsugumi::agent {

// An exact set of permanent Card paths. A container never grants its children.
// The reader is private to hostd: resolving a nested path must not disclose
// the enclosing project's document to the helper.
class HelperScope {
 public:
  // Missing files return an empty string; transport/revision errors throw.
  using Reader = std::function<std::string(const std::string&)>;
  explicit HelperScope(
      const json::Members& task,
      std::optional<host::TurnAuthorization> authorization = {},
      std::string scratch_identity = {});
  auto authorization() const -> const std::optional<host::TurnAuthorization>& {
    return authorization_;
  }
  auto Require(const std::string& card, bool writing) const -> void;
  auto RequireRun(const std::string& card) const -> void;
  auto Ran(const std::string& run_id, const std::string& card) -> void;
  static auto Validate(const json::Members& task, bool allow_unscoped) -> void;
  auto Check(const std::string& tool, const json::Members& args,
             const Reader& read) const -> void;
  // Check an executor's full current-state plan without rewriting its bytes.
  // Layout entries here have already been computed exclusively by the host;
  // model-authored layout reads/edits must be authorized separately.
  auto CheckCurrentSave(const json::Members& args, const Reader& read) const
      -> void;
  auto ExpandSave(json::Members args, const Reader& read) const
      -> json::Members;
  auto ProjectPrefix(const std::string& path, const Reader& read) const
      -> std::string;
  auto FailureResult(const std::string& result, const Reader& read) const
      -> std::string;
  auto CheckBindings(const Reader& before, const Reader& now) const -> void;
  auto Graph(const std::string& text, const std::string& prefix) const
      -> std::string;
  auto SaveResult(const std::string& result, const Reader& read) const
      -> std::string;
  auto Inspect(const std::string& result, const Reader& read) const
      -> std::string;
  auto Completed(const std::string& tool, const json::Members& args,
                 const std::string& result, const Reader& read) -> void;
  auto Restore(const std::string& receipt, const Reader& read) -> void;
  auto CheckResult(const std::string& result, const Reader& read) const -> void;
  auto Created(const std::string& id, const std::string& project_path,
               const Reader& read) -> void;
  auto CheckPlan(const std::string& result, bool executing) const -> void;
  auto scoped() const -> bool { return scoped_; }
  auto depth() const -> uint64_t { return depth_; }
  auto count() const -> uint64_t { return count_; }
  auto CheckChild(const json::Members& task) const -> void;
  std::string identity;
  auto scratch_identity() const -> const std::string& {
    return scratch_identity_;
  }

 private:
  auto File(const std::string& path, bool writing, const Reader& read) const
      -> void;
  auto Root(const std::string& card, const Reader& read,
            bool writing = false) const -> std::string;
  uint64_t depth_, count_;
  bool scoped_;
  std::set<std::string> reads_, writes_, runs_;
  std::optional<host::TurnAuthorization> authorization_;
  std::string scratch_identity_;
  std::map<std::string, std::string> runs_started_;
  std::map<std::string, std::string> created_;
};
}  // namespace tsugumi::agent
