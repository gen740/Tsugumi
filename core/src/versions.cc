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
#include <tuple>

#include "comments.hh"
#include "graph.hh"
#include "model.hh"
#include "schema.hh"
#include "util/fs.hh"
#include "util/log.hh"

namespace tsugumi {
namespace stdfs = std::filesystem;

namespace {

constexpr const char* kNotesRef = "refs/notes/tsugumi";
constexpr const char* kSummaryRef = "refs/notes/tsugumi-summary";
constexpr const char* kRecordRefEn = "refs/notes/tsugumi-record-en";
constexpr const char* kRecordRefJa = "refs/notes/tsugumi-record-ja";
constexpr size_t kDiffLimit = 12000;     // what a summary reads of a version
constexpr size_t kDiffFileLimit = 4000;  // of one file, so one file cannot fill it
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
using Patch = Ptr<git_patch, git_patch_free>;
using TreeEntry = Ptr<git_tree_entry, git_tree_entry_free>;
using Blob = Ptr<git_blob, git_blob_free>;

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

// Use the same document/source areas as Store and ProjectFileList. Other
// files enter a version only through the project's literal opt-in paths.
struct VersionFiles {
  std::vector<std::string> extra;

  explicit VersionFiles(const stdfs::path& root) {
    if (auto text = fs::read_file(root / "tsugumi.json")) {
      schema::Project project;
      if (auto error = schema::decode(*text, project))
        throw Failed(FailureKind::invalid_document, "tsugumi.json: " + error->message);
      if (const auto* paths = schema::get(project.version_paths)) extra = *paths;
    }
  }

  auto includes(const std::string& path) const -> bool {
    if (!Document::valid_version_path(path)) return false;
    if (Document::is_document_file(path) || Document::is_source_area(path)) return true;
    return std::any_of(extra.begin(), extra.end(), [&](const auto& p) {
      return Document::valid_version_path(p) && (path == p || path.starts_with(p + "/"));
    });
  }

  auto descends(const std::string& path) const -> bool {
    if (!Document::valid_version_path(path)) return false;
    if (path == "definitions" || path == "skills" || path == "externals" ||
        Document::is_document_file(path + "/_card.json") || includes(path)) return true;
    return std::any_of(extra.begin(), extra.end(), [&](const auto& p) {
      return Document::valid_version_path(p) && p.starts_with(path + "/");
    });
  }
};

// Rebuild the index, including deletions, without touching working files or
// inheriting unrelated files someone staged with Git. Ignore rules do not
// override definitions or an explicit opt-in. Never follow symlinks.
auto stage_project(git_repository* r, const stdfs::path& root, bool write_index = true) -> git_oid {
  const VersionFiles files(root);
  Index idx;
  check(git_repository_index(idx.out(), r), "index");
  check(git_index_clear(idx.get()), "clear index");
  for (auto it = stdfs::recursive_directory_iterator(root); it != stdfs::recursive_directory_iterator(); ++it) {
    const auto path = it->path().lexically_relative(root).generic_string();
    const auto status = it->symlink_status();
    if (stdfs::is_symlink(status)) continue;
    if (stdfs::is_directory(status)) {
      if (!files.descends(path)) it.disable_recursion_pending();
    } else if (stdfs::is_regular_file(status) && files.includes(path)) {
      const auto bytes = fs::read_beneath(root, path);
      git_index_entry entry{};
      entry.path = path.c_str();
      entry.mode = (status.permissions() & stdfs::perms::owner_exec) != stdfs::perms::none
                       ? GIT_FILEMODE_BLOB_EXECUTABLE : GIT_FILEMODE_BLOB;
      check(git_blob_create_from_buffer(&entry.id, r, bytes.data(), bytes.size()), "blob " + path);
      check(git_index_add(idx.get(), &entry), "add " + path);
    }
  }
  if (write_index) check(git_index_write(idx.get()), "write index");
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

// A file of a commit's tree as text.
std::optional<std::string> file_at(git_repository* r, git_tree* tree, const char* path) {
  TreeEntry e;
  if (!tree || git_tree_entry_bypath(e.out(), tree, path) != 0) return std::nullopt;
  Blob b;
  if (git_blob_lookup(b.out(), r, git_tree_entry_id(e.get())) != 0) return std::nullopt;
  return std::string(static_cast<const char*>(git_blob_rawcontent(b.get())), git_blob_rawsize(b.get()));
}

// Every file supplied through a Card's src/ is a source input, including
// assets and languages outside the model-diff allowlist. Only its path is read.
auto source_input(const std::string& path) -> bool {
  std::vector<std::string> parts;
  for (const auto& part : stdfs::path(path)) parts.push_back(part.string());
  size_t i = 0;
  while (i + 1 < parts.size() && parts[i] == "externals") i += 2;
  return i + 3 < parts.size() && parts[i] == "definitions" && parts[i + 2] == "src";
}

// Compare JSON values without whitespace or object member ordering. Document
// schemas have optional decode-only fields, so they are not serialization types.
auto canonical_value(const hosq::json::GenericDom<>& value) -> std::string {
  const auto raw = schema::text(value);
  const auto first = raw.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return "";
  if (raw[first] == '{') {
    schema::v::Mapping<schema::v::String, schema::Json> fields;
    if (auto error = schema::decode(value, fields)) throw std::runtime_error(error->message);
    std::map<std::string, std::string> sorted;
    for (const auto& [key, item] : fields.value()) sorted[key] = canonical_value(item);
    std::string out = "{";
    for (const auto& [key, item] : sorted) {
      schema::v::String name; name.value() = key;
      out += (out.size() > 1 ? "," : "") + schema::encode(name) + ":" + item;
    }
    return out + "}";
  }
  if (raw[first] == '[') {
    schema::v::Array<schema::Json> items;
    if (auto error = schema::decode(value, items)) throw std::runtime_error(error->message);
    std::string out = "[";
    for (const auto& item : items.value()) out += (out.size() > 1 ? "," : "") + canonical_value(item);
    return out + "]";
  }
  if (raw[first] == '"') {
    schema::v::String text;
    if (auto error = schema::decode(value, text)) throw std::runtime_error(error->message);
    return schema::encode(text);
  }
  return raw.substr(first, raw.find_last_not_of(" \t\r\n") - first + 1);
}

auto document_fields(const std::optional<std::string>& text) -> std::map<std::string, std::string> {
  std::map<std::string, std::string> out;
  if (!text) return out;
  schema::v::Mapping<schema::v::String, schema::Json> fields;
  if (auto error = schema::decode(*text, fields)) throw std::runtime_error(error->message);
  for (const auto& [name, value] : fields.value()) out[name] = canonical_value(value);
  return out;
}

// What a Card is in this version and, for a fork or merge, how many ports it
// fans over: the same answers graph.cc gives from the same files.
void read_definition(git_repository* r, git_tree* tree, VersionCanvasData::Card& card) {
  const std::string dir = "definitions/" + card.id + "/";
  const auto link = file_at(r, tree, (dir + "_link.json").c_str());
  const auto text = file_at(r, tree, (dir + "_card.json").c_str());
  DefinitionDoc def;
  if (link) def.link = *link;
  if (text && schema::decode(*text, def.card)) def.card = {};
  const Template kind = template_of(link || text ? &def : nullptr);
  card.kind = kind == Template::Code ? "code" : template_name(kind);
  if (kind == Template::Fork) {
    card.fan = 2;
    if (const auto* params = schema::get(def.card.params))
      for (const auto& p : *params) {
        const auto* name = schema::get(p.name);
        const auto* data = schema::get(p.data);
        if (name && *name == "branches" && data) card.fan = static_cast<uint32_t>(string_array_param(*data).size());
      }
  } else if (kind == Template::Merge) {
    if (const auto* inputs = schema::get(def.card.inputs)) card.fan = static_cast<uint32_t>(inputs->size());
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
  std::string line = "run " + r.run_id + " " + r.state + " " + r.ended + " " + secs;
  if (!r.summary.empty()) line += " " + r.summary;
  return line;
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
    std::getline(w >> std::ws, r.summary);
    out.push_back(std::move(r));
  }
  return out;
}

namespace {

// The comments of these _meta.json files (path -> its Card) in a tree.
std::vector<comments::Comment> comments_at(git_repository* r, git_tree* tree, const std::map<std::string, std::string>& metas) {
  std::vector<comments::Comment> out;
  if (!tree) return out;
  for (const auto& [path, card] : metas)
    if (auto text = file_at(r, tree, path.c_str())) {
      auto list = comments::parse(card, *text);
      out.insert(out.end(), list.begin(), list.end());
    }
  return out;
}

}  // namespace

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

std::string Versions::init(bool english) {
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
  auto tip = stage_project(r.get(), root_);
  if (!history) {
    commit_tree(r.get(), tip, {}, english ? "Create the project" : "プロジェクトを作成", "HEAD");
    auto h = head_oid(r.get());
    return hex(&*h);
  }
  // A demo's history: each commit is the files on disk with its differences,
  // built without touching the working tree, which is the head's tip.
  const VersionFiles version_files(root_);
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
        if (!version_files.includes(path)) continue;
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
                         need(schema::get(run.ended), "ended"), secs ? *secs : 0, {}};
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
  auto tree = stage_project(r.get(), root_);
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

void Versions::checkout(const std::string& commit, bool english) {
  commit_all(english ? "Changes in progress" : "作業中の変更");
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
  const auto saved = stage_project(r.get(), root_, false);
  if (head) {
    Commit c;
    check(git_commit_lookup(c.out(), r.get(), &*head), "head");
    g.dirty = !git_oid_equal(git_commit_tree_id(c.get()), &saved);
  } else {
    Tree tree;
    check(git_tree_lookup(tree.out(), r.get(), &saved), "tree");
    g.dirty = git_tree_entrycount(tree.get()) > 0;
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
      std::map<std::string, std::string> metas;  // _meta.json -> its Card
      for (size_t i = 0; i < git_diff_num_deltas(d.get()); ++i) {
        const auto* delta = git_diff_get_delta(d.get(), i);
        for (const char* p : {delta->old_file.path, delta->new_file.path})
          if (p)
            if (auto k = card_of(p)) {
              cards.insert(*k);
              if (stdfs::path(p).filename() == "_meta.json") metas[p] = *k;
            }
      }
      vc.changed.assign(cards.begin(), cards.end());
      vc.comments = comments::changes(comments_at(r.get(), theirs.get(), metas), comments_at(r.get(), mine.get(), metas));
    }
    g.commits.push_back(std::move(vc));
  }
  return g;
}

bool Versions::summary_reads(const std::string& path) {
  if (path == "tsugumi.json") return true;  // layout.json is where things sit, not what they do
  std::vector<std::string> parts;
  for (const auto& p : stdfs::path(path)) parts.push_back(p.string());
  size_t i = 0;
  while (i + 1 < parts.size() && parts[i] == "externals") i += 2;
  if (i == parts.size() - 1) return parts[i] == "tsugumi.json";
  if (i + 2 >= parts.size() || parts[i] != "definitions") return false;
  const std::string& file = parts[i + 2];
  if (i + 3 == parts.size())
    return file == "_card.json" || file == "_meta.json" || file == "_link.json" || file == "_outputs.fbs" || file == "default.nix";
  static const std::set<std::string> code = {".py", ".cc", ".cpp", ".cxx", ".c", ".h", ".hh", ".hpp", ".nix", ".fbs", ".sh"};
  return file == "src" && code.count(stdfs::path(path).extension().string()) > 0;
}

VersionCanvasData Versions::canvas(const std::string& commit) {
  std::lock_guard<std::mutex> l(mu_);
  VersionCanvasData out;
  Repo r;
  git_oid oid;
  if (!open(r, root_) || git_oid_fromstr(&oid, commit.c_str()) != 0) throw Failed(FailureKind::not_found, "no version " + commit);
  Commit c;
  Tree tree;
  if (git_commit_lookup(c.out(), r.get(), &oid) != 0 || git_commit_tree(tree.out(), c.get()) != 0)
    throw Failed(FailureKind::not_found, "no version " + commit);
  auto text = file_at(r.get(), tree.get(), "tsugumi.json");
  schema::Project project;
  if (!text || schema::decode(*text, project)) return out;
  const auto* graph = schema::get(project.canvas);
  const auto* cards = graph ? schema::get(graph->cards) : nullptr;
  if (!cards) return out;
  std::map<std::string, size_t> index;
  for (const auto& card : *cards)
    if (const auto* id = schema::get(card.id)) {
      index[*id] = out.cards.size();
      out.cards.push_back({*id});
      read_definition(r.get(), tree.get(), out.cards.back());
    }
  if (const auto* connections = schema::get(graph->connections))
    for (const auto& w : *connections) {
      const auto* from = schema::get(w.from);
      const auto* to = schema::get(w.to);
      const auto* a = from ? schema::get(from->card) : nullptr;
      const auto* b = to ? schema::get(to->card) : nullptr;
      if (a && b && index.count(*a) && index.count(*b)) out.wires.emplace_back(*a, *b);
    }
  // Saved positions, when every Card has one.
  std::map<std::string, schema::Position> saved;
  schema::Layout layout;
  auto layout_text = file_at(r.get(), tree.get(), "layout.json");
  if (layout_text && !schema::decode(*layout_text, layout))
    if (const auto* positions = schema::get(layout.cards))
      for (const auto& [key, position] : positions->entries())
        if (schema::get(position.x) && schema::get(position.y)) saved[key] = position;
  bool placed = !out.cards.empty();
  for (const auto& card : out.cards) placed = placed && saved.count(card.id);
  if (placed) {
    for (auto& card : out.cards) {
      const auto& position = saved[card.id];
      card.x = *schema::get(position.x);
      card.y = *schema::get(position.y);
      const auto* collapsed = schema::get(position.collapsed);
      card.collapsed = collapsed && *collapsed;
    }
    // The saved direction; without one, read from where the wired Cards sit,
    // as the clients do for the canvas.
    const auto* flow = schema::get(layout.flow);
    if (flow && (*flow == "topToBottom" || *flow == "leftToRight")) {
      out.flow = *flow;
    } else {
      double across = 0;
      double down = 0;
      for (const auto& [a, b] : out.wires) {
        across += std::abs(out.cards[index[b]].x - out.cards[index[a]].x);
        down += std::abs(out.cards[index[b]].y - out.cards[index[a]].y);
      }
      if (down > 0 && down >= across) out.flow = "topToBottom";
    }
    return out;
  }
  // Columns by depth (a wire back in a loop does not push a Card further).
  std::vector<size_t> depth(out.cards.size(), 0);
  for (size_t round = 0; round < out.cards.size(); ++round)
    for (const auto& [a, b] : out.wires)
      depth[index[b]] = std::max(depth[index[b]], std::min(depth[index[a]] + 1, out.cards.size() - 1));
  std::map<size_t, int> rows;
  for (size_t i = 0; i < out.cards.size(); ++i) {
    out.cards[i].x = static_cast<double>(depth[i]) * 300;
    out.cards[i].y = rows[depth[i]]++ * 120.0;
  }
  return out;
}

auto Versions::diff(const std::string& commit, const std::string& from_commit) -> VersionDiffData {
  std::lock_guard<std::mutex> l(mu_);
  VersionDiffData out;
  Repo r;
  git_oid oid;
  if (!open(r, root_) || git_oid_fromstr(&oid, commit.c_str()) != 0) throw Failed(FailureKind::not_found, "no version " + commit);
  Commit c;
  if (git_commit_lookup(c.out(), r.get(), &oid) != 0) throw Failed(FailureKind::not_found, "no version " + commit);
  Tree mine;
  Tree theirs;
  check(git_commit_tree(mine.out(), c.get()), "tree");
  if (!from_commit.empty()) {
    git_oid from_oid;
    Commit from;
    if (git_oid_fromstr(&from_oid, from_commit.c_str()) != 0 || git_commit_lookup(from.out(), r.get(), &from_oid) != 0)
      throw Failed(FailureKind::not_found, "no version " + from_commit);
    check(git_commit_tree(theirs.out(), from.get()), "base tree");
  } else if (git_commit_parentcount(c.get()) > 0) {
    Commit p;
    if (git_commit_parent(p.out(), c.get(), 0) == 0) git_commit_tree(theirs.out(), p.get());
  }
  git_diff_options options;
  git_diff_options_init(&options, GIT_DIFF_OPTIONS_VERSION);
  options.context_lines = 2;
  Diff d;
  check(git_diff_tree_to_tree(d.out(), r.get(), theirs.get(), mine.get(), &options), "diff");
  for (size_t i = 0; i < git_diff_num_deltas(d.get()); ++i) {
    const auto* delta = git_diff_get_delta(d.get(), i);
    const char* path = delta->new_file.path ? delta->new_file.path : delta->old_file.path;
    if (!path || !summary_reads(path)) continue;
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
  while (!out.text.empty() && (static_cast<unsigned char>(out.text.back()) & 0x80)) out.text.pop_back();
  return out;
}

// Read full trees, independently of the cut-down text sent to a model. Never
// include parameter values, source contents, data files or inferred intent.
auto Versions::note(const std::string& commit, bool english) -> VersionNoteData {
  std::lock_guard<std::mutex> l(mu_);
  Repo r;
  git_oid oid;
  if (!open(r, root_) || git_oid_fromstr(&oid, commit.c_str()) != 0)
    throw Failed(FailureKind::not_found, "no version " + commit);
  Commit c;
  if (git_commit_lookup(c.out(), r.get(), &oid) != 0)
    throw Failed(FailureKind::not_found, "no version " + commit);
  Tree mine, theirs;
  check(git_commit_tree(mine.out(), c.get()), "tree");
  if (git_commit_parentcount(c.get()) > 0) {
    Commit parent;
    check(git_commit_parent(parent.out(), c.get(), 0), "parent");
    check(git_commit_tree(theirs.out(), parent.get()), "parent tree");
  }
  VersionNoteData out;
  Note saved;
  if (git_note_read(saved.out(), r.get(), kSummaryRef, &oid) == 0)
    out.agent_summary = git_note_message(saved.get());
  const auto* record_ref = english ? kRecordRefEn : kRecordRefJa;
  Note record;
  if (git_note_read(record.out(), r.get(), record_ref, &oid) == 0) {
    out.text = git_note_message(record.get());
    return out;
  }
  Diff d;
  check(git_diff_tree_to_tree(d.out(), r.get(), theirs.get(), mine.get(), nullptr), "diff");
  std::set<std::string> added, removed;
  std::map<std::string, std::set<std::string>> changed;
  bool workflow = false;
  const auto label = [english](const char* en, const char* ja) { return std::string(english ? en : ja); };
  const auto project_at = [&](git_tree* tree, const std::string& path) {
    schema::Project p;
    if (auto text = file_at(r.get(), tree, path.c_str()))
      if (auto error = schema::decode(*text, p)) throw Failed(FailureKind::invalid_document, path + ": " + error->message);
    return p;
  };
  const auto ids = [](const schema::Project& p) {
    std::set<std::string> result;
    if (auto g = schema::get(p.canvas))
      if (auto cards = schema::get(g->cards))
        for (const auto& card : *cards)
          if (auto id = schema::get(card.id)) result.insert(*id);
    return result;
  };
  const auto wires = [](const schema::Project& p) {
    std::map<std::string, std::set<std::string>> result;
    if (auto g = schema::get(p.canvas))
      if (auto connections = schema::get(g->connections))
        for (const auto& wire : *connections) {
          const auto endpoint_text = [](const auto* endpoint) {
            if (!endpoint) return std::string();
            const auto* card = schema::get(endpoint->card);
            const auto* port = schema::get(endpoint->port);
            return (card ? *card : "") + std::string(1, '\x1f') + (port ? *port : "");
          };
          const auto encoded = endpoint_text(schema::get(wire.from)) + std::string(1, '\x1e') + endpoint_text(schema::get(wire.to));
          for (auto endpoint : {schema::get(wire.from), schema::get(wire.to)})
            if (endpoint)
              if (auto id = schema::get(endpoint->card)) result[*id].insert(encoded);
        }
    return result;
  };
  for (size_t i = 0; i < git_diff_num_deltas(d.get()); ++i) {
    const auto* delta = git_diff_get_delta(d.get(), i);
    const std::string path = delta->new_file.path ? delta->new_file.path : delta->old_file.path;
    const bool source = source_input(path);
    if (!summary_reads(path) && !source) continue;
    auto card = card_of(path);
    if (!card) continue;
    const auto filename = stdfs::path(path).filename().string();
    if (!source && filename == "tsugumi.json") {
      const auto before = project_at(theirs.get(), path), after = project_at(mine.get(), path);
      const auto old_ids = ids(before), new_ids = ids(after);
      // card_of(tsugumi.json) is the containing Workflow; its children use it as a prefix.
      const std::string prefix = card->empty() ? "" : *card + "/";
      for (const auto& id : new_ids) if (!old_ids.count(id)) added.insert(prefix + id);
      for (const auto& id : old_ids) if (!new_ids.count(id)) removed.insert(prefix + id);
      auto old_wires = wires(before), new_wires = wires(after);
      auto all = old_ids;
      all.insert(new_ids.begin(), new_ids.end());
      for (const auto& id : all)
        if (old_wires[id] != new_wires[id]) changed[prefix + id].insert(label("wiring", "接続"));
      auto a = document_fields(file_at(r.get(), theirs.get(), path.c_str()));
      auto b = document_fields(file_at(r.get(), mine.get(), path.c_str()));
      a.erase("canvas"); b.erase("canvas");
      if (a != b) {
        if (card->empty()) workflow = true;
        else changed[*card].insert(label("definition", "定義"));
      }
    } else if (!source && filename == "_card.json") {
      const auto old_text = file_at(r.get(), theirs.get(), path.c_str());
      const auto new_text = file_at(r.get(), mine.get(), path.c_str());
      auto before = document_fields(old_text), after = document_fields(new_text);
      if (before["params"] != after["params"])
        changed[*card].insert(label("parameters", "パラメータ"));
      before.erase("params"); after.erase("params");
      if (before != after || !old_text || !new_text)
        changed[*card].insert(label("definition", "定義"));
    } else {
      changed[*card].insert(source ? label("source", "ソース") : label("definition", "定義"));
    }
  }
  for (const auto& id : added) changed.erase(id);
  for (const auto& id : removed) changed.erase(id);
  std::vector<std::string> entries;
  for (const auto& id : added) entries.push_back(label("Added ", "追加: ") + id);
  for (const auto& id : removed) entries.push_back(label("Removed ", "削除: ") + id);
  for (const auto& [id, kinds] : changed) {
    std::string entry = label("Changed ", "変更: ") + (id.empty() ? label("Workflow", "Workflow") : id) + " (";
    for (const auto& kind : kinds) entry += (entry.back() == '(' ? "" : ", ") + kind;
    entries.push_back(entry + ")");
  }
  if (workflow) entries.push_back(label("Changed Workflow settings", "Workflow の設定を変更"));
  for (size_t i = 0; i < std::min(entries.size(), size_t{6}); ++i)
    out.text += (i ? "; " : "") + entries[i];
  if (entries.size() > 6)
    out.text += english ? "; and " + std::to_string(entries.size() - 6) + " more" : " ほか " + std::to_string(entries.size() - 6) + " 件";
  if (out.text.empty()) out.text = label("No Card definition, source, parameter or wiring changes", "Card の定義・ソース・パラメータ・接続の変更なし");
  Sig sig;
  signature(sig, r.get());
  git_oid note_id;
  check(git_note_create(&note_id, r.get(), record_ref, sig.get(), sig.get(), &oid, out.text.c_str(), 0), "record note");
  return out;
}

void Versions::set_summary(const std::string& commit, const std::string& summary) {
  std::lock_guard<std::mutex> l(mu_);
  Repo r;
  git_oid oid;
  if (!open(r, root_) || git_oid_fromstr(&oid, commit.c_str()) != 0) throw Failed(FailureKind::not_found, "no version " + commit);
  Commit c;
  if (git_commit_lookup(c.out(), r.get(), &oid) != 0) throw Failed(FailureKind::not_found, "no version " + commit);
  Sig sig;
  signature(sig, r.get());
  git_oid out;
  check(git_note_create(&out, r.get(), kSummaryRef, sig.get(), sig.get(), &oid, summary.c_str(), 1), "note");
}

}  // namespace tsugumi
