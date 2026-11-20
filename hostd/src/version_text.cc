#include "host/version_text.hh"

#include <algorithm>
#include <set>

#include "host/projects.hh"
#include "tsugumi/util/git.hh"

namespace tsugumi::host {
namespace {
auto Names(const std::vector<std::string>& names, bool en) -> std::string {
  std::string text;
  for (size_t i = 0; i < std::min(names.size(), size_t{6}); ++i)
    text += (i ? (en ? ", " : "、") : "") + names[i];
  if (names.size() > 6)
    text += en ? " and " + std::to_string(names.size() - 6) + " more"
               : " ほか " + std::to_string(names.size() - 6);
  return text;
}
}  // namespace

auto CommitText(const Tsugumi::RPC::VersionCommitFactsT& record, bool en)
    -> std::string {
  if (record.kind == "create")
    return en ? "Create the project" : "プロジェクトを作成";
  if (record.kind == "checkout")
    return en ? "Changes in progress" : "作業中の変更";
  if (record.kind == "run")
    return std::string(en ? "Run: " : "実行: ") +
           (record.card.empty() ? "Workflow" : record.card);
  return record.kind;
}
auto RunText(const Tsugumi::RPC::VersionRunFactsT& record, bool en)
    -> std::string {
  std::vector<std::string> ran, failed;
  for (const auto& count : record.ran)
    ran.push_back(count->card + (count->count > 1
                                     ? " ×" + std::to_string(count->count)
                                     : ""));
  for (const auto& failure : record.failures)
    failed.push_back(failure->card + (en ? " (" : "（") + failure->kind +
                     (en ? ")" : "）"));
  std::string out =
      en ? (record.whole ? "whole"
                         : (record.propagate ? "from " : "only ") + record.card)
         : (record.whole
                ? "全体"
                : record.card + (record.propagate ? " から" : " だけ"));
  if (record.saved_inputs)
    out += en ? " (with saved inputs)" : "（保存済みの入力で）";
  if (!ran.empty()) out += ": " + Names(ran, en);
  if (!failed.empty())
    out += (en ? " / failed: " : " / 失敗: ") + Names(failed, en);
  if (!record.never.empty())
    out += (en ? " / not run: " : " / 未実行: ") + Names(record.never, en);
  return out;
}
auto ChangeText(const Tsugumi::RPC::VersionChangesT& record, bool en)
    -> std::string {
  std::vector<std::string> entries;
  for (const auto& id : record.added)
    entries.push_back(std::string(en ? "Added " : "追加: ") + id);
  for (const auto& id : record.removed)
    entries.push_back(std::string(en ? "Removed " : "削除: ") + id);
  for (const auto& change : record.changed) {
    std::string entry = std::string(en ? "Changed " : "変更: ") +
                        (change->card.empty() ? "Workflow" : change->card) +
                        " (";
    for (const auto& kind : change->kinds) {
      std::string label = kind;
      if (!en) {
        if (kind == "definition")
          label = "定義";
        else if (kind == "parameters")
          label = "パラメータ";
        else if (kind == "source")
          label = "ソース";
        else if (kind == "wiring")
          label = "接続";
      }
      entry += (entry.back() == '(' ? "" : ", ") + label;
    }
    entries.push_back(entry + ")");
  }
  if (record.workflow)
    entries.push_back(en ? "Changed Workflow settings"
                         : "Workflow の設定を変更");
  std::string out;
  for (size_t i = 0; i < std::min(entries.size(), size_t{6}); ++i)
    out += (i ? "; " : "") + entries[i];
  if (entries.size() > 6)
    out += en ? "; and " + std::to_string(entries.size() - 6) + " more"
              : " ほか " + std::to_string(entries.size() - 6) + " 件";
  if (out.empty())
    out = en ? "No Card definition, source, parameter or wiring changes"
             : "Card の定義・ソース・パラメータ・接続の変更なし";
  return out;
}
auto PresentGraph(Tsugumi::RPC::VersionGraphT& graph, bool en) -> void {
  for (auto& commit : graph.commits) {
    if (commit->facts)
      commit->message = commit->summary = CommitText(*commit->facts, en);
    for (auto& run : commit->runs)
      run->summary = run->facts ? RunText(*run->facts, en) : "";
  }
}

namespace stdfs = std::filesystem;
namespace {
constexpr size_t kDiffLimit = 12000;
constexpr size_t kDiffFileLimit = 4000;
// ---- libgit2 handles
// ---------------------------------------------------------

using git::init_once;
using git::Ptr;

using Repo = Ptr<git_repository, git_repository_free>;
using Tree = Ptr<git_tree, git_tree_free>;
using Commit = Ptr<git_commit, git_commit_free>;
using Diff = Ptr<git_diff, git_diff_free>;
using Patch = Ptr<git_patch, git_patch_free>;

void check(int rc, const std::string& what) {
  if (rc >= 0) return;
  const git_error* e = git_error_last();
  throw ProjectFailure(
      "internal_error",
      "git: " + what + ": " +
          (e && e->message ? e->message : "error " + std::to_string(rc)));
}

bool open(Repo& r, const stdfs::path& root) {
  return git_repository_open_ext(r.out(), root.c_str(),
                                 GIT_REPOSITORY_OPEN_NO_SEARCH, nullptr) == 0;
}

}  // namespace
auto ResolveVersion(const std::filesystem::path& root, const std::string& name)
    -> std::string {
  init_once();
  Repo repo;
  git_oid oid;
  Commit commit;
  if (!open(repo, root) ||
      git_oid_fromstrn(&oid, name.c_str(), name.size()) != 0 ||
      git_commit_lookup_prefix(commit.out(), repo.get(), &oid, name.size()) !=
          0)
    throw ProjectFailure("not_found", "No unambiguous version " + name);
  char full[GIT_OID_MAX_HEXSIZE + 1];
  git_oid_tostr(full, sizeof full, git_commit_id(commit.get()));
  return full;
}

auto DiffReads(const std::string& path) -> bool {
  if (path == "tsugumi.json")
    return true;  // layout.json is where things sit, not what they do
  std::vector<std::string> parts;
  for (const auto& p : stdfs::path(path)) parts.push_back(p.string());
  size_t i = 0;
  while (i + 1 < parts.size() && parts[i] == "externals") i += 2;
  if (i == parts.size() - 1) return parts[i] == "tsugumi.json";
  if (i + 2 >= parts.size() || parts[i] != "definitions") return false;
  const std::string& file = parts[i + 2];
  if (i + 3 == parts.size())
    return file == "_card.json" || file == "_meta.json" ||
           file == "_outputs.fbs" || file == "default.nix";
  static const std::set<std::string> code = {".py",  ".cc",  ".cpp", ".cxx",
                                             ".c",   ".h",   ".hh",  ".hpp",
                                             ".nix", ".fbs", ".sh"};
  return file == "src" &&
         code.count(stdfs::path(path).extension().string()) > 0;
}

auto VersionDiff(const stdfs::path& root, const std::string& commit,
                 const std::string& from_commit)
    -> Tsugumi::Host::VersionDiffT {
  init_once();
  Tsugumi::Host::VersionDiffT out;
  Repo r;
  git_oid oid;
  if (!open(r, root) || git_oid_fromstr(&oid, commit.c_str()) != 0)
    throw ProjectFailure("not_found", "no version " + commit);
  Commit c;
  if (git_commit_lookup(c.out(), r.get(), &oid) != 0)
    throw ProjectFailure("not_found", "no version " + commit);
  Tree mine;
  Tree theirs;
  check(git_commit_tree(mine.out(), c.get()), "tree");
  if (!from_commit.empty()) {
    git_oid from_oid;
    Commit from;
    if (git_oid_fromstr(&from_oid, from_commit.c_str()) != 0 ||
        git_commit_lookup(from.out(), r.get(), &from_oid) != 0)
      throw ProjectFailure("not_found", "no version " + from_commit);
    check(git_commit_tree(theirs.out(), from.get()), "base tree");
  } else if (git_commit_parentcount(c.get()) > 0) {
    Commit p;
    if (git_commit_parent(p.out(), c.get(), 0) == 0)
      git_commit_tree(theirs.out(), p.get());
  }
  git_diff_options options;
  git_diff_options_init(&options, GIT_DIFF_OPTIONS_VERSION);
  options.context_lines = 2;
  Diff d;
  check(git_diff_tree_to_tree(d.out(), r.get(), theirs.get(), mine.get(),
                              &options),
        "diff");
  for (size_t i = 0; i < git_diff_num_deltas(d.get()); ++i) {
    const auto* delta = git_diff_get_delta(d.get(), i);
    const char* path =
        delta->new_file.path ? delta->new_file.path : delta->old_file.path;
    if (!path || !DiffReads(path)) continue;
    Patch p;
    if (git_patch_from_diff(p.out(), d.get(), i) != 0 || !p) continue;
    git_buf buf = GIT_BUF_INIT;
    if (git_patch_to_buf(&buf, p.get()) != 0) continue;
    std::string text(buf.ptr, buf.size);
    git_buf_dispose(&buf);
    if (text.size() > kDiffFileLimit) {
      text.resize(kDiffFileLimit);
      out.truncated = true;
    }
    if (out.text.size() + text.size() > kDiffLimit) {
      out.truncated = true;
      break;
    }
    out.text += text;
    if (out.text.back() != '\n') out.text += '\n';
  }
  // Cut inside a character at most: what a client sends stays valid text.
  while (!out.text.empty() &&
         (static_cast<unsigned char>(out.text.back()) & 0x80))
    out.text.pop_back();
  return out;
}

}  // namespace tsugumi::host
