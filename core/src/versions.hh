// A project's versions: its folder is a git repository (libgit2). A run
// commits what is saved when it starts (when it changed) and records that
// commit; when it ends, a line goes to the commit's note (refs/notes/tsugumi),
// so a commit has its runs' results and git selects versions by them
// (git log --notes=tsugumi --grep=failed). A version the user names is an
// annotated tag. .tsugumi/ (tsugumid's state) is never part of it.
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace tsugumi {

struct VersionRunNote {
  std::string run_id;
  std::string state;  // succeeded, failed, cancelled
  std::string ended;  // ISO 8601 (UTC)
  double seconds = 0;
};

struct VersionCommit {
  std::string id;
  std::vector<std::string> parents;
  std::string summary, message, author;
  int64_t time_ms = 0;
  std::vector<std::string> branches, tags;
  std::vector<VersionRunNote> runs;
  // Card keys ("a", "outer/b") whose files changed from the first parent;
  // "" for the project's own documents (tsugumi.json, layout.json).
  std::vector<std::string> changed;
};

struct VersionGraphData {
  bool enabled = false;
  std::string head, branch;  // branch: empty on a detached HEAD
  bool dirty = false;        // saved changes not committed yet
  std::vector<VersionCommit> commits;  // newest first, parents after children
};

class Versions {
 public:
  explicit Versions(std::filesystem::path root);
  ~Versions();

  bool enabled() const;
  // Makes the folder a repository with a first commit of what is in it
  // (.gitignore keeps .tsugumi/ out). A demo's history file
  // (.tsugumi-history.json) becomes its commits, branches, tags and notes
  // instead, ending in what is on disk. Returns HEAD.
  std::string init();
  // Commits what is on disk when it differs from HEAD (on a detached HEAD a
  // branch is made first, so nothing is left unreachable). Returns HEAD; ""
  // when the folder is not a repository.
  std::string commit_all(const std::string& message);
  void note_run(const std::string& commit, const VersionRunNote& run);
  // commit_all(message), then an annotated tag `name` on it.
  std::string mark(const std::string& name, const std::string& message);
  // The folder becomes `commit` (what is saved now is committed first). On a
  // branch's tip HEAD follows that branch, else it is detached and the next
  // commit starts a branch.
  void checkout(const std::string& commit);
  VersionGraphData graph(size_t limit);

  static std::string format_note(const VersionRunNote& r);
  static std::string iso_utc(int64_t ms);
  static std::vector<VersionRunNote> parse_note(const std::string& text);
  // "definitions/a/x" -> "a", "externals/p/definitions/b/y" -> "p/b",
  // "tsugumi.json" -> "", anything else -> nullopt.
  static std::optional<std::string> card_of(const std::string& path);

 private:
  std::filesystem::path root_;
  mutable std::mutex mu_;
};

}  // namespace tsugumi
