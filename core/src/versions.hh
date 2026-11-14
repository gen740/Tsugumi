// A project's versions: its folder is a git repository (libgit2). A run
// commits what is saved when it starts (when it changed) and records that
// commit; when it ends, a line goes to the commit's note (refs/notes/tsugumi),
// with what ran, so a commit has its runs' results and git selects versions by them
// (git log --notes=tsugumi --grep=failed). The Agent service keeps what a version
// changed, in words a model wrote from its diff, in a note of its own
// (refs/notes/tsugumi-summary), apart from that record. A version the user
// names is an annotated tag. .tsugumi/ (tsugumid's state) is never part of it.
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
  // What ran, from what the run did ("whole: a, b ×2 / failed: b (worker_failed)").
  // Empty for a run a demo's history file lists.
  std::string summary;
};

struct VersionCommit {
  std::string id;
  std::vector<std::string> parents;
  std::string summary;
  std::string message;
  std::string author;
  int64_t time_ms = 0;
  std::vector<std::string> branches;
  std::vector<std::string> tags;
  std::vector<VersionRunNote> runs;
  // Card keys ("a", "outer/b") whose files changed from the first parent;
  // "" for the project's own documents (tsugumi.json, layout.json).
  std::vector<std::string> changed;
};

// A version's own Cards and how they connect, for a minimap.
struct VersionCanvasData {
  struct Card {
    std::string id;
    std::string kind = "code";
    uint32_t fan = 0;  // a fork's outputs or a merge's inputs
  };
  std::vector<Card> cards;
  std::vector<std::pair<std::string, std::string>> wires;  // from, to
  std::string layout_json = "{}";
};

struct VersionNoteData {
  std::string text;
  std::string agent_summary;
};

struct VersionDiffData {
  std::string text;
  bool truncated = false;
};

struct VersionGraphData {
  bool enabled = false;
  std::string head;
  std::string branch;                  // empty on a detached HEAD
  bool dirty = false;                  // saved changes not committed yet
  std::vector<VersionCommit> commits;  // newest first, parents after children
};

class Versions {
 public:
  explicit Versions(std::filesystem::path root);
  ~Versions();

  bool enabled() const;
  // Makes the folder a repository with a first commit of its definitions and
  // versionPaths opt-ins, always excluding .tsugumi/. A demo's history file
  // (.tsugumi-history.json) becomes its commits, branches, tags and notes
  // instead, ending in what is on disk. Returns HEAD. `english` is the language
  // of the first commit's message (the connection's, as for runs).
  std::string init(bool english);
  // Commits saved definitions and versionPaths opt-ins when they differ from
  // HEAD (on a detached HEAD a branch is made first, so nothing is left
  // unreachable). Returns HEAD; "" when the folder is not a repository.
  std::string commit_all(const std::string& message);
  void note_run(const std::string& commit, const VersionRunNote& run);
  // commit_all(message), then an annotated tag `name` on it.
  std::string mark(const std::string& name, const std::string& message);
  // The folder becomes `commit` (what is saved now is committed first). On a
  // branch's tip HEAD follows that branch, else it is detached and the next
  // commit starts a branch. `english`: the language of that first commit's message.
  void checkout(const std::string& commit, bool english);
  VersionGraphData graph(size_t limit);
  // Raw layout and definition data of `commit`, without presentation logic.
  VersionCanvasData canvas(const std::string& commit);
  // What `commit` changed from from_commit, or its first parent when omitted:
  // the files a summary may read (summary_reads), cut at a limit.
  auto diff(const std::string& commit, const std::string& from_commit = "") -> VersionDiffData;
  // A short factual record, kept per language in refs/notes/tsugumi-record-{ja,en}.
  // No model or data values. Also returns the saved Agent note, if any.
  auto note(const std::string& commit, bool english) -> VersionNoteData;
  void set_summary(const std::string& commit, const std::string& summary);

  static std::string format_note(const VersionRunNote& r);
  static std::string iso_utc(int64_t ms);
  static std::vector<VersionRunNote> parse_note(const std::string& text);
  // "definitions/a/x" -> "a", "externals/p/definitions/b/y" -> "p/b",
  // "tsugumi.json" -> "", anything else -> nullopt.
  static std::optional<std::string> card_of(const std::string& path);
  // Whether a file's changes may go to a model for a summary: a project's and
  // a Card's own documents and its code, never data ("definitions/a/src/x.py"
  // yes, "definitions/a/src/x.csv" and "definitions/a/data/x.txt" no).
  static bool summary_reads(const std::string& path);

 private:
  std::filesystem::path root_;
  mutable std::mutex mu_;
};

}  // namespace tsugumi
