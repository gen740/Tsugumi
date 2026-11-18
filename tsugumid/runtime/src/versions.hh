// A project's Git versions: runs commit saved inputs when they change and
// append neutral JSON facts to refs/notes/tsugumi. Mechanical change facts
// live in refs/notes/tsugumi-record. The host presents them in either language
// and owns model summaries. Named versions remain annotated tags; .tsugumi/
// is never committed.
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
  // Neutral JSON facts about what actually ran (version_record::Run).
  // Empty for a run a demo's history file lists.
  std::string record_json;
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
  std::string record_json;
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
  // instead, ending in what is on disk. Returns HEAD.
  std::string init();
  // Commits saved definitions and versionPaths opt-ins when they differ from
  // HEAD (on a detached HEAD a branch is made first, so nothing is left
  // unreachable). Returns HEAD; "" when the folder is not a repository.
  std::string commit_all(const std::string& message);
  void note_run(const std::string& commit, const VersionRunNote& run);
  // commit_all(message), then an annotated tag `name` on it.
  std::string mark(const std::string& name, const std::string& message);
  // The folder becomes `commit` (what is saved now is committed first). On a
  // branch's tip HEAD follows that branch, else it is detached and the next
  // commit starts a branch.
  void checkout(const std::string& commit);
  VersionGraphData graph(size_t limit);
  // Raw layout and definition data of `commit`, without presentation logic.
  VersionCanvasData canvas(const std::string& commit);
  // Neutral factual changes, cached in refs/notes/tsugumi-record.
  auto note(const std::string& commit) -> VersionNoteData;

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
