#include "versions.hh"

#include <git2.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "model.hh"
#include "schema.hh"
#include "util/fs.hh"
#include "util/log.hh"

namespace tsugumi {
namespace stdfs = std::filesystem;

namespace {

constexpr const char* kNotesRef = "refs/notes/tsugumi";
constexpr const char* kHistoryFile = ".tsugumi-history.json";
constexpr const char* kIgnore = ".tsugumi/\n.DS_Store\n";

// ---- libgit2 handles ---------------------------------------------------------

struct Init {
  Init() { git_libgit2_init(); }
};
void init_once() { static Init i; }

template <class T, void (*F)(T*)>
struct Ptr {
  T* p = nullptr;
  Ptr() = default;
  Ptr(const Ptr&) = delete;
  Ptr& operator=(const Ptr&) = delete;
  ~Ptr() {
    if (p) F(p);
  }
  T** out() { return &p; }
  T* get() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};
using Repo = Ptr<git_repository, git_repository_free>;
using Index = Ptr<git_index, git_index_free>;
using Tree = Ptr<git_tree, git_tree_free>;
using Commit = Ptr<git_commit, git_commit_free>;
using Sig = Ptr<git_signature, git_signature_free>;
using Ref = Ptr<git_reference, git_reference_free>;
using Note = Ptr<git_note, git_note_free>;
using Walk = Ptr<git_revwalk, git_revwalk_free>;
using Diff = Ptr<git_diff, git_diff_free>;
using Object = Ptr<git_object, git_object_free>;
using RefIter = Ptr<git_reference_iterator, git_reference_iterator_free>;
using Status = Ptr<git_status_list, git_status_list_free>;

void check(int rc, const std::string& what) {
  if (rc >= 0) return;
  const git_error* e = git_error_last();
  throw Failed(FailureKind::internal_error, "git: " + what + ": " + (e && e->message ? e->message : "error " + std::to_string(rc)));
}

std::string hex(const git_oid* id) {
  char buf[GIT_OID_MAX_HEXSIZE + 1];
  git_oid_tostr(buf, sizeof buf, id);
  return buf;
}

bool open(Repo& r, const stdfs::path& root) {
  return git_repository_open_ext(r.out(), root.c_str(), GIT_REPOSITORY_OPEN_NO_SEARCH, nullptr) == 0;
}

// The user's git identity when there is one (git config), else Tsugumi's.
void signature(Sig& s, git_repository* r, std::optional<int64_t> when = std::nullopt) {
  if (!when && git_signature_default(s.out(), r) == 0) return;
  if (when) check(git_signature_new(s.out(), "Tsugumi", "tsugumi@localhost", *when, 0), "signature");
  else check(git_signature_now(s.out(), "Tsugumi", "tsugumi@localhost"), "signature");
}

std::optional<git_oid> head_oid(git_repository* r) {
  git_oid id;
  if (git_reference_name_to_id(&id, r, "HEAD") != 0) return std::nullopt;  // unborn
  return id;
}

// The working tree as a tree (every file not ignored), written to the index.
git_oid stage_all(git_repository* r) {
  Index idx;
  check(git_repository_index(idx.out(), r), "index");
  char star[] = "*";
  char* pats[] = {star};
  git_strarray spec = {pats, 1};
  check(git_index_add_all(idx.get(), &spec, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr), "add");
  check(git_index_update_all(idx.get(), &spec, nullptr, nullptr), "update");
  check(git_index_write(idx.get()), "write index");
  git_oid tree;
  check(git_index_write_tree(&tree, idx.get()), "write tree");
  return tree;
}

std::string commit_tree(git_repository* r, const git_oid& tree_id, const std::vector<git_oid>& parents,
                        const std::string& message, const char* update, std::optional<int64_t> when = std::nullopt) {
  Tree tree;
  check(git_tree_lookup(tree.out(), r, &tree_id), "tree");
  std::vector<Commit> owned(parents.size());
  std::vector<const git_commit*> ps;
  for (size_t i = 0; i < parents.size(); ++i) {
    check(git_commit_lookup(owned[i].out(), r, &parents[i]), "parent");
    ps.push_back(owned[i].get());
  }
  Sig sig;
  signature(sig, r, when);
  git_oid id;
  check(git_commit_create(&id, r, update, sig.get(), sig.get(), "UTF-8", message.c_str(), tree.get(), ps.size(),
                          ps.data()),
        "commit");
  return hex(&id);
}

// A branch name for a detached HEAD about to get a commit.
std::string new_branch_name(git_repository* r, const std::string& from) {
  std::string base = "try-" + from.substr(0, 7);
  for (int n = 1;; ++n) {
    std::string name = n == 1 ? base : base + "-" + std::to_string(n);
    Ref ref;
    if (git_branch_lookup(ref.out(), r, name.c_str(), GIT_BRANCH_LOCAL) != 0) return name;
  }
}

std::string iso(int64_t seconds) {
  std::time_t t = static_cast<std::time_t>(seconds);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buf;
}

}  // namespace

Versions::Versions(stdfs::path root) : root_(std::move(root)) { init_once(); }
Versions::~Versions() = default;

bool Versions::enabled() const {
  std::lock_guard<std::mutex> l(mu_);
  Repo r;
  return open(r, root_);
}

std::string Versions::iso_utc(int64_t ms) { return iso(ms / 1000); }

std::string Versions::format_note(const VersionRunNote& r) {
  char secs[32];
  std::snprintf(secs, sizeof secs, "%.1fs", r.seconds);
  return "run " + r.run_id + " " + r.state + " " + r.ended + " " + secs;
}

std::vector<VersionRunNote> Versions::parse_note(const std::string& text) {
  std::vector<VersionRunNote> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream w(line);
    std::string word;
    std::string secs;
    VersionRunNote r;
    if (!(w >> word) || word != "run" || !(w >> r.run_id >> r.state >> r.ended)) continue;
    if (w >> secs) r.seconds = std::atof(secs.c_str());
    out.push_back(std::move(r));
  }
  return out;
}

std::optional<std::string> Versions::card_of(const std::string& path) {
  if (path == "tsugumi.json" || path == "layout.json") return std::string();
  std::vector<std::string> parts;
  for (const auto& p : stdfs::path(path)) parts.push_back(p.string());
  std::string prefix;
  size_t i = 0;
  while (i + 1 < parts.size() && parts[i] == "externals") {
    prefix += parts[i + 1] + "/";
    i += 2;
    if (i < parts.size() && (parts[i] == "tsugumi.json" || parts[i] == "layout.json"))
      return prefix.substr(0, prefix.size() - 1);  // the Card in Card itself
  }
  if (i + 2 < parts.size() && parts[i] == "definitions") return prefix + parts[i + 1];
  return std::nullopt;
}

std::string Versions::init() {
  std::lock_guard<std::mutex> l(mu_);
  Repo r;
  if (open(r, root_)) {
    auto h = head_oid(r.get());
    return h ? hex(&*h) : "";
  }
  check(git_repository_init(r.out(), root_.c_str(), false), "init");
  if (!stdfs::exists(root_ / ".gitignore")) fs::write_atomic(root_ / ".gitignore", kIgnore, 0644);
  auto history = fs::read_file(root_ / kHistoryFile);
  std::error_code ec;
  stdfs::remove(root_ / kHistoryFile, ec);
  auto tip = stage_all(r.get());
  if (!history) {
    commit_tree(r.get(), tip, {}, "プロジェクトを作成", "HEAD");
    auto h = head_oid(r.get());
    return hex(&*h);
  }
  // A demo's history: each commit is the files on disk with its differences,
  // built without touching the working tree, which is the head's tip.
  schema::HistoryFile hf;
  if (auto e = schema::decode(*history, hf)) throw Failed(FailureKind::invalid_document, std::string(kHistoryFile) + ": " + e->message);
  Index base;
  check(git_repository_index(base.out(), r.get()), "index");
  std::map<std::string, git_oid> ids;
  auto need = [](const auto* v, const char* what) -> const auto& {
    if (!v) throw Failed(FailureKind::invalid_document, std::string(kHistoryFile) + ": a commit has no " + what);
    return *v;
  };
  for (const auto& c : *hf.commits) {
    const std::string& cid = need(schema::get(c.id), "id");
    const std::string& message = need(schema::get(c.message), "message");
    int64_t when = static_cast<int64_t>(need(schema::get(c.time), "time"));
    Index idx;
    check(git_index_new(idx.out()), "index");
    Tree tip_tree;
    check(git_tree_lookup(tip_tree.out(), r.get(), &tip), "tree");
    check(git_index_read_tree(idx.get(), tip_tree.get()), "read tree");
    if (const auto* removed = schema::get(c.removed))
      for (const auto& path : *removed) git_index_remove_bypath(idx.get(), path.c_str());
    if (const auto* files = schema::get(c.files)) {
      for (const auto& [path, text] : files->entries()) {
        git_index_entry e{};
        e.mode = GIT_FILEMODE_BLOB;
        e.path = path.c_str();
        check(git_blob_create_from_buffer(&e.id, r.get(), text.data(), text.size()), "blob " + path);
        check(git_index_add(idx.get(), &e), "add " + path);
      }
    }
    git_oid tree;
    check(git_index_write_tree_to(&tree, idx.get(), r.get()), "tree");
    std::vector<git_oid> parents;
    if (const auto* ps = schema::get(c.parents))
      for (const auto& p : *ps) {
        auto it = ids.find(p);
        if (it == ids.end()) throw Failed(FailureKind::invalid_document, std::string(kHistoryFile) + ": unknown parent " + p);
        parents.push_back(it->second);
      }
    auto id = commit_tree(r.get(), tree, parents, message, nullptr, when);
    git_oid oid;
    git_oid_fromstr(&oid, id.c_str());
    ids[cid] = oid;
    if (const auto* runs = schema::get(c.runs)) {
      std::string text;
      for (const auto& run : *runs) {
        const auto* secs = schema::get(run.seconds);
        VersionRunNote n{need(schema::get(run.run_id), "run_id"), need(schema::get(run.state), "state"),
                         need(schema::get(run.ended), "ended"), secs ? *secs : 0};
        text += format_note(n) + "\n";
      }
      Sig sig;
      signature(sig, r.get(), when);
      git_oid note;
      check(git_note_create(&note, r.get(), kNotesRef, sig.get(), sig.get(), &oid, text.c_str(), 1), "note");
    }
    if (const auto* tag = schema::get(c.tag)) {
      Object target;
      check(git_object_lookup(target.out(), r.get(), &oid, GIT_OBJECT_COMMIT), "tag target");
      Sig sig;
      signature(sig, r.get(), when);
      const auto* msg = schema::get(c.tag_message);
      git_oid t;
      check(git_tag_create(&t, r.get(), tag->c_str(), target.get(), sig.get(), msg ? msg->c_str() : tag->c_str(), 0), "tag");
    }
  }
  for (const auto& [name, commit] : hf.branches->entries()) {
    auto it = ids.find(commit);
    if (it == ids.end()) throw Failed(FailureKind::invalid_document, std::string(kHistoryFile) + ": unknown commit " + commit);
    Commit c;
    check(git_commit_lookup(c.out(), r.get(), &it->second), "branch target");
    Ref ref;
    check(git_branch_create(ref.out(), r.get(), name.c_str(), c.get(), 1), "branch " + name);
  }
  check(git_repository_set_head(r.get(), ("refs/heads/" + *hf.head).c_str()), "head");
  // The index follows the head (the files on disk already are its tip).
  auto h = head_oid(r.get());
  Object head;
  check(git_object_lookup(head.out(), r.get(), &*h, GIT_OBJECT_COMMIT), "head");
  check(git_reset(r.get(), head.get(), GIT_RESET_MIXED, nullptr), "reset");
  return hex(&*h);
}

std::string Versions::commit_all(const std::string& message) {
  std::lock_guard<std::mutex> l(mu_);
  Repo r;
  if (!open(r, root_)) return "";
  auto tree = stage_all(r.get());
  auto head = head_oid(r.get());
  if (head) {
    Commit c;
    check(git_commit_lookup(c.out(), r.get(), &*head), "head");
    if (git_oid_equal(git_commit_tree_id(c.get()), &tree)) return hex(&*head);
    if (git_repository_head_detached(r.get()) == 1) {
      // Started from an older version: the new commit opens a branch.
      auto name = new_branch_name(r.get(), hex(&*head));
      Ref ref;
      check(git_branch_create(ref.out(), r.get(), name.c_str(), c.get(), 0), "branch");
      check(git_repository_set_head(r.get(), ("refs/heads/" + name).c_str()), "head");
    }
  }
  std::vector<git_oid> parents;
  if (head) parents.push_back(*head);
  return commit_tree(r.get(), tree, parents, message, "HEAD");
}

void Versions::note_run(const std::string& commit, const VersionRunNote& run) {
  std::lock_guard<std::mutex> l(mu_);
  Repo r;
  if (!open(r, root_)) return;
  git_oid oid;
  if (git_oid_fromstr(&oid, commit.c_str()) != 0) return;
  std::string text;
  Note n;
  if (git_note_read(n.out(), r.get(), kNotesRef, &oid) == 0) text = git_note_message(n.get());
  if (!text.empty() && text.back() != '\n') text += "\n";
  text += format_note(run) + "\n";
  Sig sig;
  signature(sig, r.get());
  git_oid out;
  check(git_note_create(&out, r.get(), kNotesRef, sig.get(), sig.get(), &oid, text.c_str(), 1), "note");
}

std::string Versions::mark(const std::string& name, const std::string& message) {
  {
    std::lock_guard<std::mutex> l(mu_);
    int valid = 0;
    git_reference_name_is_valid(&valid, ("refs/tags/" + name).c_str());
    if (!valid || name.empty())
      throw Failed(FailureKind::invalid_request, "\"" + name + "\" is not a valid version name (a git tag name)");
  }
  auto id = commit_all(message.empty() ? name : message);
  if (id.empty()) throw Failed(FailureKind::invalid_request, "the project is not versioned yet");
  std::lock_guard<std::mutex> l(mu_);
  Repo r;
  check(open(r, root_) ? 0 : -1, "open");
  git_oid oid;
  git_oid_fromstr(&oid, id.c_str());
  Object target;
  check(git_object_lookup(target.out(), r.get(), &oid, GIT_OBJECT_COMMIT), "tag target");
  Sig sig;
  signature(sig, r.get());
  git_oid t;
  int rc = git_tag_create(&t, r.get(), name.c_str(), target.get(), sig.get(), (message.empty() ? name : message).c_str(), 0);
  if (rc == GIT_EEXISTS) throw Failed(FailureKind::operation_conflict, "a version named " + name + " exists already");
  check(rc, "tag");
  return id;
}

void Versions::checkout(const std::string& commit) {
  commit_all("作業中の変更");
  std::lock_guard<std::mutex> l(mu_);
  Repo r;
  if (!open(r, root_)) throw Failed(FailureKind::invalid_request, "the project is not versioned yet");
  git_oid oid;
  if (git_oid_fromstr(&oid, commit.c_str()) != 0) throw Failed(FailureKind::invalid_request, "not a commit id: " + commit);
  Object target;
  if (git_object_lookup(target.out(), r.get(), &oid, GIT_OBJECT_COMMIT) != 0)
    throw Failed(FailureKind::not_found, "no version " + commit);
  git_checkout_options opts;
  git_checkout_options_init(&opts, GIT_CHECKOUT_OPTIONS_VERSION);
  opts.checkout_strategy = GIT_CHECKOUT_FORCE;
  check(git_checkout_tree(r.get(), target.get(), &opts), "checkout");
  // On a branch's tip, continue that branch.
  RefIter it;
  check(git_reference_iterator_glob_new(it.out(), r.get(), "refs/heads/*"), "branches");
  git_reference* ref = nullptr;
  std::string branch;
  while (git_reference_next(&ref, it.get()) == 0) {
    const git_oid* t = git_reference_target(ref);
    if (branch.empty() && t && git_oid_equal(t, &oid)) branch = git_reference_name(ref);
    git_reference_free(ref);
  }
  if (!branch.empty()) check(git_repository_set_head(r.get(), branch.c_str()), "head");
  else check(git_repository_set_head_detached(r.get(), &oid), "head");
}

VersionGraphData Versions::graph(size_t limit) {
  std::lock_guard<std::mutex> l(mu_);
  VersionGraphData g;
  Repo r;
  if (!open(r, root_)) return g;
  g.enabled = true;
  auto head = head_oid(r.get());
  if (head) g.head = hex(&*head);
  if (git_repository_head_detached(r.get()) != 1) {
    Ref h;
    if (git_repository_head(h.out(), r.get()) == 0) g.branch = git_reference_shorthand(h.get());
  }
  {
    git_status_options so;
    git_status_options_init(&so, GIT_STATUS_OPTIONS_VERSION);
    so.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
    Status st;
    if (git_status_list_new(st.out(), r.get(), &so) == 0) g.dirty = git_status_list_entrycount(st.get()) > 0;
  }
  if (!head) return g;
  // Branch and tag names by the commit they point at.
  std::map<std::string, std::vector<std::string>> branches;
  std::map<std::string, std::vector<std::string>> tags;
  {
    RefIter it;
    check(git_reference_iterator_new(it.out(), r.get()), "refs");
    git_reference* ref = nullptr;
    while (git_reference_next(&ref, it.get()) == 0) {
      std::string name = git_reference_name(ref);
      Object peeled;
      if (git_reference_peel(peeled.out(), ref, GIT_OBJECT_COMMIT) == 0) {
        auto id = hex(git_object_id(peeled.get()));
        if (name.starts_with("refs/heads/")) branches[id].push_back(name.substr(11));
        else if (name.starts_with("refs/tags/")) tags[id].push_back(name.substr(10));
      }
      git_reference_free(ref);
    }
  }
  Walk w;
  check(git_revwalk_new(w.out(), r.get()), "walk");
  git_revwalk_sorting(w.get(), GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);
  git_revwalk_push_glob(w.get(), "refs/heads/*");
  git_revwalk_push_head(w.get());
  git_oid oid;
  while (g.commits.size() < limit && git_revwalk_next(&oid, w.get()) == 0) {
    Commit c;
    if (git_commit_lookup(c.out(), r.get(), &oid) != 0) continue;
    VersionCommit vc;
    vc.id = hex(&oid);
    for (unsigned i = 0; i < git_commit_parentcount(c.get()); ++i) vc.parents.push_back(hex(git_commit_parent_id(c.get(), i)));
    if (const char* s = git_commit_summary(c.get())) vc.summary = s;
    if (const char* m = git_commit_message(c.get())) vc.message = m;
    if (const auto* a = git_commit_author(c.get())) vc.author = a->name ? a->name : "";
    vc.time_ms = static_cast<int64_t>(git_commit_time(c.get())) * 1000;
    vc.branches = branches[vc.id];
    vc.tags = tags[vc.id];
    Note n;
    if (git_note_read(n.out(), r.get(), kNotesRef, &oid) == 0) vc.runs = parse_note(git_note_message(n.get()));
    // What changed from the first parent, by Card.
    Tree mine;
    Tree theirs;
    git_commit_tree(mine.out(), c.get());
    if (git_commit_parentcount(c.get()) > 0) {
      Commit p;
      if (git_commit_parent(p.out(), c.get(), 0) == 0) git_commit_tree(theirs.out(), p.get());
    }
    Diff d;
    if (git_diff_tree_to_tree(d.out(), r.get(), theirs.get(), mine.get(), nullptr) == 0) {
      std::set<std::string> cards;
      for (size_t i = 0; i < git_diff_num_deltas(d.get()); ++i) {
        const auto* delta = git_diff_get_delta(d.get(), i);
        for (const char* p : {delta->old_file.path, delta->new_file.path})
          if (p)
            if (auto k = card_of(p)) cards.insert(*k);
      }
      vc.changed.assign(cards.begin(), cards.end());
    }
    g.commits.push_back(std::move(vc));
  }
  return g;
}

}  // namespace tsugumi
