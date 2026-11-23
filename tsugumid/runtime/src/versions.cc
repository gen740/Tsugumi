#include "versions.hh"

#include <git2.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "model.hh"
#include "tsugumi/document/graph.hh"
#include "tsugumi/document/text.hh"
#include "tsugumi/protocol/runtime_state_generated.h"
#include "tsugumi/protocol/version_state_generated.h"
#include "tsugumi/util/flat_file.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/log.hh"
#include "tsugumi/util/text.hh"
#include "version_record.hh"

namespace tsugumi {
namespace stdfs = std::filesystem;

namespace {

constexpr const char* kNotesRef = "refs/notes/tsugumi";
constexpr const char* kRecordRef = "refs/notes/tsugumi-record";
constexpr const char* kHistoryFile = ".tsugumi-history";
constexpr const char* kIgnore = ".tsugumi/\n.DS_Store\n";

// ---- libgit2 handles
// ---------------------------------------------------------

struct Init {
  Init() { git_libgit2_init(); }
};
inline auto init_once() -> void { static Init i; }

template <class T, void (*F)(T*)>
struct Ptr {
  T* p = nullptr;
  Ptr() = default;
  Ptr(const Ptr&) = delete;
  Ptr& operator=(const Ptr&) = delete;
  ~Ptr() {
    if (p) F(p);
  }
  auto out() -> T** { return &p; }
  auto get() const -> T* { return p; }
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
  throw Failed(
      FailureKind::internal_error,
      "git: " + what + ": " +
          (e && e->message ? e->message : "error " + std::to_string(rc)));
}

std::string hex(const git_oid* id) {
  char buf[GIT_OID_MAX_HEXSIZE + 1];
  git_oid_tostr(buf, sizeof buf, id);
  return buf;
}

auto resolve_commit(git_repository* repo, const std::string& name, git_oid& oid)
    -> bool {
  git_object* object = nullptr;
  if (git_revparse_single(&object, repo, name.c_str()) != 0) return false;
  const bool commit = git_object_type(object) == GIT_OBJECT_COMMIT;
  if (commit) oid = *git_object_id(object);
  git_object_free(object);
  return commit;
}

bool open(Repo& r, const stdfs::path& root) {
  return git_repository_open_ext(r.out(), root.c_str(),
                                 GIT_REPOSITORY_OPEN_NO_SEARCH, nullptr) == 0;
}

// The user's git identity when there is one (git config), else Tsugumi's.
void signature(Sig& s, git_repository* r,
               std::optional<int64_t> when = std::nullopt) {
  if (!when && git_signature_default(s.out(), r) == 0) return;
  if (when)
    check(git_signature_new(s.out(), "Tsugumi", "tsugumi@localhost", *when, 0),
          "signature");
  else
    check(git_signature_now(s.out(), "Tsugumi", "tsugumi@localhost"),
          "signature");
}

std::optional<git_oid> head_oid(git_repository* r) {
  git_oid id;
  if (git_reference_name_to_id(&id, r, "HEAD") != 0)
    return std::nullopt;  // unborn
  return id;
}

// Use the same document/source areas as Store and ProjectFileList. Other
// files enter a version only through the project's literal opt-in paths.
struct VersionFiles {
  std::vector<std::string> extra;

  explicit VersionFiles(const stdfs::path& root,
                        const Document* accepted = nullptr) {
    std::string buffer;
    if (accepted) {
      buffer = accepted->project_buffer;
    } else if (auto text = fs::read_file(root / "tsugumi.json")) {
      try {
        buffer = document_text::parse(document_text::Schema::Project,
                                      "tsugumi.json", *text);
      } catch (const DocumentError& error) {
        throw Failed(FailureKind::invalid_document, error.what());
      }
    }
    if (buffer.empty()) return;
    if (const auto* paths =
            document_text::root<Tsugumi::Document::Project>(buffer)
                .versionPaths())
      for (const auto* path : *paths) extra.push_back(path->str());
  }

  auto includes(const std::string& path) const -> bool {
    if (!Document::valid_version_path(path)) return false;
    if (Document::is_document_file(path) || Document::is_source_area(path) ||
        Document::is_lock_file(path))
      return true;
    return std::any_of(extra.begin(), extra.end(), [&](const auto& p) {
      return Document::valid_version_path(p) &&
             (path == p || path.starts_with(p + "/"));
    });
  }

  auto descends(const std::string& path) const -> bool {
    if (!Document::valid_version_path(path)) return false;
    if (path == "definitions" || path == "skills" || path == "externals" ||
        path == "templates" ||
        Document::is_document_file(path + "/_card.json") || includes(path))
      return true;
    return std::any_of(extra.begin(), extra.end(), [&](const auto& p) {
      return Document::valid_version_path(p) && p.starts_with(path + "/");
    });
  }
};

// Rebuild the index, including deletions, without touching working files or
// inheriting unrelated files someone staged with Git. Ignore rules do not
// override definitions or an explicit opt-in. Never follow symlinks.
auto stage_project(git_repository* r, const stdfs::path& root,
                   bool write_index = true, const Document* accepted = nullptr)
    -> git_oid {
  const VersionFiles files(root, accepted);
  std::map<std::string, std::string> expected;
  if (accepted) {
    expected = accepted->disk_hashes;
    for (const auto& [path, source] : accepted->sources)
      if (source.document || files.includes(path))
        expected.emplace(path, source.hash);
  }
  const auto changed = [](const std::string& path) {
    throw Failed(FailureKind::external_change,
                 path + " changed outside Tsugumi; Reload before running",
                 {{"path", path}});
  };
  Index idx;
  check(git_repository_index(idx.out(), r), "index");
  check(git_index_clear(idx.get()), "clear index");
  for (auto it = stdfs::recursive_directory_iterator(root);
       it != stdfs::recursive_directory_iterator(); ++it) {
    const auto path = it->path().lexically_relative(root).generic_string();
    const auto status = it->symlink_status();
    if (stdfs::is_symlink(status)) continue;
    if (stdfs::is_directory(status)) {
      if (!files.descends(path)) it.disable_recursion_pending();
    } else if (stdfs::is_regular_file(status) && files.includes(path)) {
      const auto bytes = fs::read_beneath(root, path);
      if (accepted &&
          (Document::is_document_file(path) || Document::is_source_area(path) ||
           Document::is_lock_file(path))) {
        auto hash = expected.find(path);
        if (hash == expected.end() || hash->second != crypto::sha256_hex(bytes))
          changed(path);
        expected.erase(hash);
      }
      git_index_entry entry{};
      entry.path = path.c_str();
      entry.mode = (status.permissions() & stdfs::perms::owner_exec) !=
                           stdfs::perms::none
                       ? GIT_FILEMODE_BLOB_EXECUTABLE
                       : GIT_FILEMODE_BLOB;
      check(
          git_blob_create_from_buffer(&entry.id, r, bytes.data(), bytes.size()),
          "blob " + path);
      check(git_index_add(idx.get(), &entry), "add " + path);
    }
  }
  if (!expected.empty()) changed(expected.begin()->first);
  if (write_index) check(git_index_write(idx.get()), "write index");
  git_oid tree;
  check(git_index_write_tree(&tree, idx.get()), "write tree");
  return tree;
}

std::string commit_tree(git_repository* r, const git_oid& tree_id,
                        const std::vector<git_oid>& parents,
                        const std::string& message, const char* update,
                        std::optional<int64_t> when = std::nullopt) {
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
  check(git_commit_create(&id, r, update, sig.get(), sig.get(), "UTF-8",
                          message.c_str(), tree.get(), ps.size(), ps.data()),
        "commit");
  return hex(&id);
}

// A branch name for a detached HEAD about to get a commit.
std::string new_branch_name(git_repository* r, const std::string& from) {
  std::string base = "try-" + from.substr(0, 7);
  for (int n = 1;; ++n) {
    std::string name = n == 1 ? base : base + "-" + std::to_string(n);
    Ref ref;
    if (git_branch_lookup(ref.out(), r, name.c_str(), GIT_BRANCH_LOCAL) != 0)
      return name;
  }
}

// A file of a commit's tree as text.
std::optional<std::string> file_at(git_repository* r, git_tree* tree,
                                   const char* path) {
  TreeEntry e;
  if (!tree || git_tree_entry_bypath(e.out(), tree, path) != 0)
    return std::nullopt;
  Blob b;
  if (git_blob_lookup(b.out(), r, git_tree_entry_id(e.get())) != 0)
    return std::nullopt;
  return std::string(static_cast<const char*>(git_blob_rawcontent(b.get())),
                     git_blob_rawsize(b.get()));
}

// Version presentation accepts object IDs rather than revspecs.
auto lookup_version(git_repository* repo, const std::string& name, Commit& out)
    -> void {
  if (name.size() < 7 || name.size() > GIT_OID_HEXSZ ||
      name.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
    throw Failed(FailureKind::invalid_request,
                 "Use a Git version object ID or unambiguous abbreviation");
  git_oid oid;
  check(git_oid_fromstrn(&oid, name.c_str(), name.size()), "version id");
  const auto status = git_commit_lookup_prefix(out.out(), repo, &oid, name.size());
  if (status == GIT_ENOTFOUND || status == GIT_EAMBIGUOUS)
    throw Failed(FailureKind::not_found, "no unambiguous version " + name);
  check(status, "version lookup");
}

auto parent_tree(git_commit* commit, Tree& tree) -> void {
  if (git_commit_parentcount(commit) == 0) return;
  Commit parent;
  check(git_commit_parent(parent.out(), commit, 0), "parent");
  check(git_commit_tree(tree.out(), parent.get()), "parent tree");
}

auto comment_files(git_repository* repo, git_tree* tree) -> comments::Files {
  comments::Files files;
  if (!tree) return files;
  struct WalkData {
    git_repository* repo;
    comments::Files& files;
  } data{repo, files};
  check(git_tree_walk(
            tree, GIT_TREEWALK_PRE,
            [](const char* prefix, const git_tree_entry* entry, void* payload) {
              auto& walk = *static_cast<WalkData*>(payload);
              if (git_tree_entry_type(entry) != GIT_OBJECT_BLOB) return 0;
              const std::string path =
                  std::string(prefix) + git_tree_entry_name(entry);
              if (!comments::is_comments(path) &&
                  !(path.ends_with("/_card.json") && comments::card_of(path)))
                return 0;
              Blob blob;
              const auto status =
                  git_blob_lookup(blob.out(), walk.repo, git_tree_entry_id(entry));
              if (status != 0) return status;
              walk.files[path] = std::string(
                  static_cast<const char*>(git_blob_rawcontent(blob.get())),
                  git_blob_rawsize(blob.get()));
              return 0;
            },
            &data),
        "comment trees");
  return files;
}

auto compare_comments(git_repository* repo, git_tree* before, git_tree* after)
    -> std::vector<comments::Change> {
  return comments::changes(comments::of(comment_files(repo, before)),
                           comments::of(comment_files(repo, after)));
}

auto diff_reads(const std::string& path) -> bool {
  if (path == "tsugumi.json" || Document::is_lock_file(path))
    return true;  // layout.bin is where things sit, not what they do
  std::vector<std::string> parts;
  for (const auto& p : stdfs::path(path)) parts.push_back(p.string());
  if (parts.empty()) return false;
  size_t i = 0;
  while (i + 1 < parts.size() && parts[i] == "externals") i += 2;
  if (i == parts.size() - 1) return parts[i] == "tsugumi.json";
  if (i + 2 >= parts.size() || parts[i] != "definitions") return false;
  // Card data directories and CSV data are outside the model-visible diff.
  return std::find(parts.begin() + i + 2, parts.end() - 1, "data") ==
             parts.end() - 1 &&
         stdfs::path(path).extension() != ".csv";
}

auto diff_text_blob(git_repository* repo, const git_diff_file& file) -> bool {
  if (file.mode == 0) return true;  // Added and deleted files have one side.
  if (file.mode != GIT_FILEMODE_BLOB &&
      file.mode != GIT_FILEMODE_BLOB_EXECUTABLE)
    return false;
  Blob blob;
  check(git_blob_lookup(blob.out(), repo, &file.id), "diff blob");
  constexpr size_t kMaxFileSize = 1024 * 1024;
  constexpr size_t kBinaryProbeSize = 8192;
  const auto size = git_blob_rawsize(blob.get());
  if (size == 0) return true;
  if (size > kMaxFileSize) return false;
  const std::string_view prefix(
      static_cast<const char*>(git_blob_rawcontent(blob.get())),
      std::min<size_t>(size, kBinaryProbeSize));
  return prefix.find('\0') == std::string_view::npos;
}

// Every file supplied through a Card's src/ counts as a source input in
// version notes. Only its path is read.
auto source_input(const std::string& path) -> bool {
  std::vector<std::string> parts;
  for (const auto& part : stdfs::path(path)) parts.push_back(part.string());
  size_t i = 0;
  while (i + 1 < parts.size() && parts[i] == "externals") i += 2;
  return i + 3 < parts.size() && parts[i] == "definitions" &&
         parts[i + 2] == "src";
}

namespace D = Tsugumi::Document;

// A version's tsugumi.json; an absent one is an empty project.
auto project_document(const std::optional<std::string>& text,
                      const std::string& path) -> D::ProjectT {
  D::ProjectT out;
  if (!text) return out;
  try {
    document_text::root<D::Project>(
        document_text::parse(document_text::Schema::Project, path, *text))
        .UnPackTo(&out);
  } catch (const DocumentError& error) {
    throw Failed(FailureKind::invalid_document, error.what());
  }
  return out;
}

// What a Card is in this version and, for a Merge, how many ports
// it fans over: the same answers graph.cc gives from the same files.
void read_definition(git_repository* r, git_tree* tree,
                     VersionCanvasData::Card& card) {
  const std::string dir = "definitions/" + card.id + "/";
  const auto text = file_at(r, tree, (dir + "_card.json").c_str());
  // A definition this build cannot read has no kind.
  DefinitionDoc def;
  try {
    if (!text) return;
    def.card_buffer = parse_card_json(card.id, *text).buffer;
  } catch (const DocumentError&) {
    return;
  }
  const CardType kind = card_type_of(def);
  card.kind = card_type_name(kind);
  if (kind == CardType::Merge) {
    if (const auto* inputs = def.card().inputs())
      card.fan = static_cast<uint32_t>(inputs->size());
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

namespace {

auto read_note(git_repository* repo, const char* ref, const git_oid& target)
    -> std::optional<std::string> {
  Note note;
  const auto code = git_note_read(note.out(), repo, ref, &target);
  if (code == GIT_ENOTFOUND) return std::nullopt;
  check(code, "read note");
  Blob blob;
  check(git_blob_lookup(blob.out(), repo, git_note_id(note.get())), "note blob");
  return std::string(static_cast<const char*>(git_blob_rawcontent(blob.get())),
                     git_blob_rawsize(blob.get()));
}

void write_note(git_repository* repo, const char* ref, const git_oid& target,
                std::string_view bytes, std::optional<int64_t> when = {}) {
  // git_note_create takes a NUL-terminated message. Store the FlatBuffer as a
  // blob in the same notes tree so Git can still find it by commit id.
  Index index;
  check(git_index_new(index.out()), "notes index");
  std::vector<git_oid> parents;
  git_oid tip;
  if (resolve_commit(repo, ref, tip)) {
    Commit parent;
    check(git_commit_lookup(parent.out(), repo, &tip), "notes commit");
    Tree tree;
    check(git_commit_tree(tree.out(), parent.get()), "notes tree");
    check(git_index_read_tree(index.get(), tree.get()), "notes index tree");
    parents.push_back(tip);
  }
  const auto path = hex(&target);
  git_index_entry entry{};
  entry.path = path.c_str();
  entry.mode = GIT_FILEMODE_BLOB;
  check(git_blob_create_from_buffer(&entry.id, repo, bytes.data(), bytes.size()),
        "write note blob");
  check(git_index_add(index.get(), &entry), "write note entry");
  git_oid tree_id;
  check(git_index_write_tree_to(&tree_id, index.get(), repo), "write notes tree");
  commit_tree(repo, tree_id, parents, "Record Tsugumi version", ref, when);
}

auto encode_runs(const std::vector<VersionRunNote>& entries) -> std::string {
  Tsugumi::Version::RunsT runs;
  for (const auto& entry : entries) {
    auto run = std::make_unique<Tsugumi::Version::RunT>();
    run->run_id = entry.run_id;
    run->state = entry.state;
    run->ended = entry.ended;
    run->seconds = entry.seconds;
    run->facts.assign(entry.facts.begin(), entry.facts.end());
    runs.runs.push_back(std::move(run));
  }
  return flat::encode<Tsugumi::Version::Runs>(runs);
}

auto decode_runs(const std::string& bytes) -> std::vector<VersionRunNote> {
  const auto runs = flat::decode<Tsugumi::Version::Runs>(bytes);
  if (!runs) throw Failed(FailureKind::invalid_document, "invalid Git run note");
  std::vector<VersionRunNote> out;
  for (const auto& run : runs->runs)
    out.push_back({run->run_id, run->state, run->ended, run->seconds,
                   std::string(run->facts.begin(), run->facts.end())});
  return out;
}

}  // namespace

std::optional<std::string> Versions::card_of(const std::string& path) {
  if (path == "tsugumi.json" || path == "tsugumi.lock" ||
      path == "layout.bin" || path.starts_with("notes/"))
    return std::string();
  std::vector<std::string> parts;
  for (const auto& p : stdfs::path(path)) parts.push_back(p.string());
  std::string prefix;
  size_t i = 0;
  while (i + 1 < parts.size() && parts[i] == "externals") {
    prefix += parts[i + 1] + "/";
    i += 2;
    if (i < parts.size() &&
        (parts[i] == "tsugumi.json" || parts[i] == "tsugumi.lock" ||
         parts[i] == "layout.bin"))
      return prefix.substr(0, prefix.size() - 1);  // the Card in Card itself
  }
  if (i + 2 < parts.size() && parts[i] == "definitions")
    return prefix + parts[i + 1];
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
  if (!stdfs::exists(root_ / ".gitignore"))
    fs::write_atomic(root_ / ".gitignore", kIgnore, 0644);
  auto history = fs::read_file(root_ / kHistoryFile);
  std::error_code ec;
  stdfs::remove(root_ / kHistoryFile, ec);
  auto tip = stage_project(r.get(), root_);
  if (!history) {
    commit_tree(r.get(), tip, {}, version_record::commit("create"), "HEAD");
    auto h = head_oid(r.get());
    return hex(&*h);
  }
  // A demo's history: each commit is the files on disk with its differences,
  // built without touching the working tree, which is the head's tip.
  const VersionFiles version_files(root_);
  // A FlatBuffer of Tsugumi.State.History (protocol/runtime_state.fbs).
  const auto hf = flat::decode<Tsugumi::State::History>(*history);
  if (!hf)
    throw Failed(FailureKind::invalid_document,
                 std::string(kHistoryFile) + " is not a version history");
  Index base;
  check(git_repository_index(base.out(), r.get()), "index");
  std::map<std::string, git_oid> ids;
  for (const auto& c : hf->commits) {
    const std::string& cid = c->id;
    const std::string& message = c->message;
    const auto when = static_cast<int64_t>(c->time);
    Index idx;
    check(git_index_new(idx.out()), "index");
    Tree tip_tree;
    check(git_tree_lookup(tip_tree.out(), r.get(), &tip), "tree");
    check(git_index_read_tree(idx.get(), tip_tree.get()), "read tree");
    for (const auto& path : c->removed)
      git_index_remove_bypath(idx.get(), path.c_str());
    for (const auto& file : c->files) {
      if (!version_files.includes(file->path)) continue;
      git_index_entry e{};
      e.mode = GIT_FILEMODE_BLOB;
      e.path = file->path.c_str();
      check(git_blob_create_from_buffer(&e.id, r.get(), file->bytes.data(),
                                        file->bytes.size()),
            "blob " + file->path);
      check(git_index_add(idx.get(), &e), "add " + file->path);
    }
    git_oid tree;
    check(git_index_write_tree_to(&tree, idx.get(), r.get()), "tree");
    std::vector<git_oid> parents;
    for (const auto& p : c->parents) {
      auto it = ids.find(p);
      if (it == ids.end())
        throw Failed(FailureKind::invalid_document,
                     std::string(kHistoryFile) + ": unknown parent " + p);
      parents.push_back(it->second);
    }
    auto id = commit_tree(r.get(), tree, parents, message, nullptr, when);
    git_oid oid;
    git_oid_fromstr(&oid, id.c_str());
    ids[cid] = oid;
    if (!c->runs.empty()) {
      std::vector<VersionRunNote> runs;
      for (const auto& run : c->runs)
        runs.push_back({run->run_id, run->state, run->ended, run->seconds, {}});
      write_note(r.get(), kNotesRef, oid, encode_runs(runs), when);
    }
    if (!c->tag.empty()) {
      Object target;
      check(git_object_lookup(target.out(), r.get(), &oid, GIT_OBJECT_COMMIT),
            "tag target");
      Sig sig;
      signature(sig, r.get(), when);
      const auto& msg = c->tag_message.empty() ? c->tag : c->tag_message;
      git_oid t;
      check(git_tag_create(&t, r.get(), c->tag.c_str(), target.get(), sig.get(),
                           msg.c_str(), 0),
            "tag");
    }
  }
  for (const auto& branch : hf->branches) {
    const auto& name = branch->name;
    const auto& commit = branch->commit;
    auto it = ids.find(commit);
    if (it == ids.end())
      throw Failed(FailureKind::invalid_document,
                   std::string(kHistoryFile) + ": unknown commit " + commit);
    Commit c;
    check(git_commit_lookup(c.out(), r.get(), &it->second), "branch target");
    Ref ref;
    check(git_branch_create(ref.out(), r.get(), name.c_str(), c.get(), 1),
          "branch " + name);
  }
  check(git_repository_set_head(r.get(), ("refs/heads/" + hf->head).c_str()),
        "head");
  // The index follows the head (the files on disk already are its tip).
  auto h = head_oid(r.get());
  Object head;
  check(git_object_lookup(head.out(), r.get(), &*h, GIT_OBJECT_COMMIT), "head");
  check(git_reset(r.get(), head.get(), GIT_RESET_MIXED, nullptr), "reset");
  return hex(&*h);
}

namespace {
auto commit_worktree(git_repository* repo, const stdfs::path& root,
                     const std::string& message,
                     const Document* accepted = nullptr) -> std::string {
  auto tree = stage_project(repo, root, true, accepted);
  auto head = head_oid(repo);
  if (head) {
    Commit c;
    check(git_commit_lookup(c.out(), repo, &*head), "head");
    if (git_oid_equal(git_commit_tree_id(c.get()), &tree)) return hex(&*head);
    if (git_repository_head_detached(repo) == 1) {
      // Started from an older version: the new commit opens a branch.
      auto name = new_branch_name(repo, hex(&*head));
      Ref ref;
      check(git_branch_create(ref.out(), repo, name.c_str(), c.get(), 0),
            "branch");
      check(git_repository_set_head(repo, ("refs/heads/" + name).c_str()),
            "head");
    }
  }
  std::vector<git_oid> parents;
  if (head) parents.push_back(*head);
  return commit_tree(repo, tree, parents, message, "HEAD");
}
}  // namespace

std::string Versions::commit_all(const std::string& message,
                                 const Document* accepted) {
  std::lock_guard<std::mutex> l(mu_);
  Repo r;
  if (!open(r, root_)) return "";
  return commit_worktree(r.get(), root_, message, accepted);
}

void Versions::note_run(const std::string& commit, const VersionRunNote& run) {
  std::lock_guard<std::mutex> l(mu_);
  Repo r;
  if (!open(r, root_)) return;
  git_oid oid;
  if (!resolve_commit(r.get(), commit, oid)) return;
  std::vector<VersionRunNote> runs;
  if (const auto note = read_note(r.get(), kNotesRef, oid))
    runs = decode_runs(*note);
  runs.push_back(run);
  write_note(r.get(), kNotesRef, oid, encode_runs(runs));
}

std::string Versions::mark(const std::string& name,
                           const std::string& message) {
  {
    std::lock_guard<std::mutex> l(mu_);
    int valid = 0;
    git_reference_name_is_valid(&valid, ("refs/tags/" + name).c_str());
    if (!valid || name.empty())
      throw Failed(
          FailureKind::invalid_request,
          "\"" + name + "\" is not a valid version name (a git tag name)");
  }
  auto id = commit_all(message.empty() ? name : message);
  if (id.empty())
    throw Failed(FailureKind::invalid_request,
                 "the project is not versioned yet");
  std::lock_guard<std::mutex> l(mu_);
  Repo r;
  check(open(r, root_) ? 0 : -1, "open");
  git_oid oid;
  git_oid_fromstr(&oid, id.c_str());
  Object target;
  check(git_object_lookup(target.out(), r.get(), &oid, GIT_OBJECT_COMMIT),
        "tag target");
  Sig sig;
  signature(sig, r.get());
  git_oid t;
  int rc = git_tag_create(&t, r.get(), name.c_str(), target.get(), sig.get(),
                          (message.empty() ? name : message).c_str(), 0);
  if (rc == GIT_EEXISTS)
    throw Failed(FailureKind::operation_conflict,
                 "a version named " + name + " exists already");
  check(rc, "tag");
  return id;
}

void Versions::checkout(
    const std::string& commit,
    const std::function<void(const stdfs::path&)>& validate) {
  std::lock_guard<std::mutex> l(mu_);
  Repo r;
  if (!open(r, root_))
    throw Failed(FailureKind::invalid_request,
                 "the project is not versioned yet");
  git_oid oid;
  if (!resolve_commit(r.get(), commit, oid))
    throw Failed(FailureKind::invalid_request, "not a commit id: " + commit);
  Commit target;
  check(git_commit_lookup(target.out(), r.get(), &oid), "checkout target");
  Tree target_tree;
  check(git_commit_tree(target_tree.out(), target.get()), "checkout tree");

  // Stage the working project in memory, preserving the user's original index.
  Index index;
  check(git_repository_index(index.out(), r.get()), "checkout index");
  const stdfs::path index_path = git_index_path(index.get());
  const auto index_before = fs::read_file(index_path);
  const stdfs::path head_path =
      stdfs::path(git_repository_path(r.get())) / "HEAD";
  const auto head_before = fs::read_file_or_throw(head_path);
  const auto baseline_oid = stage_project(r.get(), root_, false);
  Tree baseline;
  check(git_tree_lookup(baseline.out(), r.get(), &baseline_oid),
        "checkout baseline");

  // Untracked and ignored paths survive an ordinary checkout. Give Git the
  // same input here and install this exact tree after validation.
  std::set<std::string> roots{"tsugumi.json", "tsugumi.lock", "layout.bin",
                              "notes",        "definitions",  "externals",
                              "skills",       "templates"};
  for (auto* tree : {baseline.get(), target_tree.get()})
    for (size_t i = 0; i < git_tree_entrycount(tree); ++i) {
      const std::string name =
          git_tree_entry_name(git_tree_entry_byindex(tree, i));
      if (!Document::valid_version_path(name))
        throw Failed(FailureKind::invalid_path,
                     "invalid checkout path: " + name);
      roots.insert(name);
    }
  auto scratch_path = (root_ / ".tsugumi" / "checkout-XXXXXX").string();
  if (!::mkdtemp(scratch_path.data()))
    throw Failed(FailureKind::persistence_failed,
                 "could not create a checkout directory");
  struct CheckoutScratch {
    stdfs::path path;
    bool keep = false;
    ~CheckoutScratch() {
      if (keep) return;
      std::error_code error;
      stdfs::remove_all(path, error);
      if (error) log::error("remove checkout directory: " + error.message());
    }
  } scratch{scratch_path};
  const auto preview = scratch.path / "target";
  const auto backup = scratch.path / "original";
  stdfs::create_directory(preview);
  stdfs::create_directory(backup);
  for (const auto& name : roots) {
    const auto from = root_ / name;
    if (stdfs::exists(stdfs::symlink_status(from)))
      stdfs::copy(
          from, preview / name,
          stdfs::copy_options::recursive | stdfs::copy_options::copy_symlinks);
  }
  git_checkout_options opts{};
  check(git_checkout_options_init(&opts, GIT_CHECKOUT_OPTIONS_VERSION),
        "checkout options");
  opts.checkout_strategy = GIT_CHECKOUT_FORCE | GIT_CHECKOUT_DONT_UPDATE_INDEX |
                           GIT_CHECKOUT_NO_REFRESH;
  opts.baseline = baseline.get();
  opts.target_directory = preview.c_str();
  Object treeish;
  check(git_object_lookup(treeish.out(), r.get(), &oid, GIT_OBJECT_COMMIT),
        "checkout object");
  check(git_checkout_tree(r.get(), treeish.get(), &opts), "prepare checkout");
  validate(preview);

  // Build the outgoing version without changing the branch or HEAD. Advance
  // that branch only after the worktree, index and new HEAD are installed.
  const auto before = head_oid(r.get());
  Ref head;
  check(git_reference_lookup(head.out(), r.get(), "HEAD"), "checkout HEAD");
  std::string source_branch;
  std::optional<git_oid> saved;
  Commit current;
  if (before)
    check(git_commit_lookup(current.out(), r.get(), &*before),
          "checkout current version");
  if (!before ||
      !git_oid_equal(git_commit_tree_id(current.get()), &baseline_oid)) {
    if (const auto* name = git_reference_symbolic_target(head.get()))
      source_branch = name;
    else
      source_branch = "refs/heads/" + new_branch_name(r.get(), hex(&*before));
    const auto id = commit_tree(
        r.get(), baseline_oid,
        before ? std::vector<git_oid>{*before} : std::vector<git_oid>{},
        version_record::commit("checkout"), nullptr);
    git_oid saved_oid;
    check(git_oid_fromstr(&saved_oid, id.c_str()), "checkout saved version");
    saved = saved_oid;
  }

  // A branch at the target stays attached, unless it is about to advance.
  RefIter it;
  check(git_reference_iterator_glob_new(it.out(), r.get(), "refs/heads/*"),
        "branches");
  std::string branch;
  for (;;) {
    Ref ref;
    const int status = git_reference_next(ref.out(), it.get());
    if (status == GIT_ITEROVER) break;
    check(status, "iterate checkout branches");
    const auto* target_id = git_reference_target(ref.get());
    const std::string name = git_reference_name(ref.get());
    if (target_id && name != source_branch && git_oid_equal(target_id, &oid)) {
      branch = name;
      break;
    }
  }

  struct Move {
    std::string name;
    bool backed_up = false;
    bool installed = false;
  };
  std::vector<Move> moves;
  moves.reserve(roots.size());
  bool index_written = false, head_written = false;
  try {
    for (const auto& name : roots) {
      auto& move = moves.emplace_back(Move{name});
      if (stdfs::exists(stdfs::symlink_status(root_ / name))) {
        stdfs::rename(root_ / name, backup / name);
        move.backed_up = true;
      }
      if (stdfs::exists(stdfs::symlink_status(preview / name))) {
        stdfs::rename(preview / name, root_ / name);
        move.installed = true;
      }
    }
    check(git_index_read_tree(index.get(), target_tree.get()),
          "set checkout index");
    check(git_index_write(index.get()), "write checkout index");
    index_written = true;
    check(branch.empty() ? git_repository_set_head_detached(r.get(), &oid)
                         : git_repository_set_head(r.get(), branch.c_str()),
          "set checkout HEAD");
    head_written = true;
    if (saved) {
      Ref ref;
      const auto* expected = git_reference_symbolic_target(head.get()) && before
                                 ? &*before
                                 : nullptr;
      check(git_reference_create_matching(
                ref.out(), r.get(), source_branch.c_str(), &*saved,
                expected != nullptr, expected, "save project before checkout"),
            "save checkout branch");
    }
  } catch (const std::exception& error) {
    // Keep original files available if a second I/O error prevents rollback.
    std::string failures;
    auto restore = [&](const auto& action) {
      try {
        action();
      } catch (const std::exception& failure) {
        failures += "; " + std::string(failure.what());
      }
    };
    if (head_written)
      restore([&] { fs::write_atomic(head_path, head_before); });
    if (index_written)
      restore([&] {
        if (index_before)
          fs::write_atomic(index_path, *index_before);
        else
          fs::remove_file_durable(index_path);
      });
    for (auto move = moves.rbegin(); move != moves.rend(); ++move) {
      if (move->installed)
        restore(
            [&] { stdfs::rename(root_ / move->name, preview / move->name); });
      if (move->backed_up)
        restore(
            [&] { stdfs::rename(backup / move->name, root_ / move->name); });
    }
    if (!failures.empty()) {
      scratch.keep = true;
      throw Failed(FailureKind::persistence_failed,
                   std::string(error.what()) +
                       "; checkout rollback failed (originals in " +
                       backup.string() + ")" + failures);
    }
    throw Failed(FailureKind::persistence_failed, error.what());
  }
}

VersionGraphData Versions::graph(size_t limit, bool include_comments) {
  std::lock_guard<std::mutex> l(mu_);
  VersionGraphData g;
  Repo r;
  if (!open(r, root_)) return g;
  g.enabled = true;
  auto head = head_oid(r.get());
  if (head) g.head = hex(&*head);
  if (git_repository_head_detached(r.get()) != 1) {
    Ref h;
    if (git_repository_head(h.out(), r.get()) == 0)
      g.branch = git_reference_shorthand(h.get());
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
        if (name.starts_with("refs/heads/"))
          branches[id].push_back(name.substr(11));
        else if (name.starts_with("refs/tags/"))
          tags[id].push_back(name.substr(10));
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
    for (unsigned i = 0; i < git_commit_parentcount(c.get()); ++i)
      vc.parents.push_back(hex(git_commit_parent_id(c.get(), i)));
    if (const char* s = git_commit_summary(c.get())) vc.summary = s;
    if (const char* m = git_commit_message(c.get())) vc.message = m;
    if (const auto* a = git_commit_author(c.get()))
      vc.author = a->name ? a->name : "";
    vc.time_ms = static_cast<int64_t>(git_commit_time(c.get())) * 1000;
    vc.branches = branches[vc.id];
    vc.tags = tags[vc.id];
    if (const auto note = read_note(r.get(), kNotesRef, oid))
      vc.runs = decode_runs(*note);
    // What changed from the first parent, by Card.
    Tree mine;
    Tree theirs;
    check(git_commit_tree(mine.out(), c.get()), "tree");
    parent_tree(c.get(), theirs);
    if (include_comments)
      vc.comments = compare_comments(r.get(), theirs.get(), mine.get());
    Diff d;
    if (git_diff_tree_to_tree(d.out(), r.get(), theirs.get(), mine.get(),
                              nullptr) == 0) {
      std::set<std::string> cards;
      for (size_t i = 0; i < git_diff_num_deltas(d.get()); ++i) {
        const auto* delta = git_diff_get_delta(d.get(), i);
        for (const char* p : {delta->old_file.path, delta->new_file.path})
          if (p)
            if (auto k = card_of(p)) {
              cards.insert(*k);
            }
      }
      vc.changed.assign(cards.begin(), cards.end());
    }
    g.commits.push_back(std::move(vc));
  }
  return g;
}

VersionCanvasData Versions::canvas(const std::string& commit) {
  std::lock_guard<std::mutex> l(mu_);
  VersionCanvasData out;
  Repo r;
  git_oid oid;
  if (!open(r, root_) || !resolve_commit(r.get(), commit, oid))
    throw Failed(FailureKind::not_found, "no version " + commit);
  Commit c;
  Tree tree;
  if (git_commit_lookup(c.out(), r.get(), &oid) != 0 ||
      git_commit_tree(tree.out(), c.get()) != 0)
    throw Failed(FailureKind::not_found, "no version " + commit);
  out.layout = file_at(r.get(), tree.get(), "layout.bin").value_or("");
  auto text = file_at(r.get(), tree.get(), "tsugumi.json");
  if (!text) return out;
  std::string buffer;
  try {
    buffer = document_text::parse(document_text::Schema::Project,
                                  "tsugumi.json", *text);
  } catch (const DocumentError&) {
    return out;
  }
  const auto& graph = *document_text::root<D::Project>(buffer).canvas();
  std::map<std::string, size_t> index;
  for (const auto* card : *graph.cards()) {
    const auto id = card->id()->str();
    index[id] = out.cards.size();
    out.cards.push_back({id});
    read_definition(r.get(), tree.get(), out.cards.back());
  }
  for (const auto* w : *graph.connections()) {
    const auto a = w->from()->card()->str(), b = w->to()->card()->str();
    if (index.count(a) && index.count(b)) out.wires.emplace_back(a, b);
  }
  return out;
}

// Read full trees, independently of the cut-down text sent to a model. Never
// include parameter values, source contents, data files or inferred intent.
auto Versions::note(const std::string& commit) -> VersionNoteData {
  std::lock_guard<std::mutex> l(mu_);
  Repo r;
  if (!open(r, root_))
    throw Failed(FailureKind::not_found, "Project has no version repository");
  Commit c;
  lookup_version(r.get(), commit, c);
  const auto oid = *git_commit_id(c.get());
  Tree mine, theirs;
  check(git_commit_tree(mine.out(), c.get()), "tree");
  parent_tree(c.get(), theirs);
  VersionNoteData out;
  out.commit = hex(&oid);
  if (auto record = read_note(r.get(), kRecordRef, oid)) {
    out.facts = std::move(*record);
    return out;
  }
  Diff d;
  check(git_diff_tree_to_tree(d.out(), r.get(), theirs.get(), mine.get(),
                              nullptr),
        "diff");
  std::set<std::string> added, removed;
  std::map<std::string, std::set<std::string>> changed;
  bool workflow = false;
  const auto project_at = [&](git_tree* tree, const std::string& path) {
    return project_document(file_at(r.get(), tree, path.c_str()), path);
  };
  const auto ids = [](const D::ProjectT& p) {
    std::set<std::string> result;
    if (p.canvas)
      for (const auto& card : p.canvas->cards) result.insert(card->id);
    return result;
  };
  const auto wires = [](const D::ProjectT& p) {
    std::map<std::string, std::set<std::string>> result;
    if (p.canvas)
      for (const auto& wire : p.canvas->connections) {
        const auto endpoint_text = [](const D::EndpointT* endpoint) {
          if (!endpoint) return std::string();
          return endpoint->card + std::string(1, '\x1f') + endpoint->port;
        };
        const auto encoded = endpoint_text(wire->from.get()) +
                             std::string(1, '\x1e') +
                             endpoint_text(wire->to.get());
        for (const auto* endpoint : {wire->from.get(), wire->to.get()})
          if (endpoint) result[endpoint->card].insert(encoded);
      }
    return result;
  };
  // The project's settings without its canvas.
  const auto settings = [](D::ProjectT p) {
    p.canvas = std::make_unique<D::GraphT>();
    return document_text::print(document_text::Schema::Project,
                                document_text::pack<D::Project>(p));
  };
  for (size_t i = 0; i < git_diff_num_deltas(d.get()); ++i) {
    const auto* delta = git_diff_get_delta(d.get(), i);
    const std::string path =
        delta->new_file.path ? delta->new_file.path : delta->old_file.path;
    const bool source = source_input(path) || Document::is_lock_file(path);
    // Other files are opaque to the run definition (including README.md).
    const auto filename = stdfs::path(path).filename().string();
    if (!source && filename != "tsugumi.json" && filename != "_card.json" &&
        filename != "_outputs.fbs" && filename != "default.nix" &&
        filename != "params.nix")
      continue;
    auto card = card_of(path);
    if (!card) continue;
    if (!source && filename == "tsugumi.json") {
      const auto before = project_at(theirs.get(), path),
                 after = project_at(mine.get(), path);
      const auto old_ids = ids(before), new_ids = ids(after);
      // card_of(tsugumi.json) is the containing Workflow; its children use it
      // as a prefix.
      const std::string prefix = card->empty() ? "" : *card + "/";
      for (const auto& id : new_ids)
        if (!old_ids.count(id)) added.insert(prefix + id);
      for (const auto& id : old_ids)
        if (!new_ids.count(id)) removed.insert(prefix + id);
      auto old_wires = wires(before), new_wires = wires(after);
      auto all = old_ids;
      all.insert(new_ids.begin(), new_ids.end());
      for (const auto& id : all)
        if (old_wires[id] != new_wires[id])
          changed[prefix + id].insert("wiring");
      if (settings(std::move(before)) != settings(std::move(after))) {
        if (card->empty())
          workflow = true;
        else
          changed[*card].insert("definition");
      }
    } else if (!source && filename == "_card.json") {
      const auto old_text = file_at(r.get(), theirs.get(), path.c_str());
      const auto new_text = file_at(r.get(), mine.get(), path.c_str());
      if (!old_text || !new_text ||
          document_text::canonical(document_text::Schema::Card, path, *old_text) !=
              document_text::canonical(document_text::Schema::Card, path, *new_text))
        changed[*card].insert("definition");
    } else if (!source && filename == "params.nix") {
      changed[*card].insert("parameters");
    } else {
      changed[*card].insert(source ? "source" : "definition");
    }
  }
  for (const auto& id : added) changed.erase(id);
  for (const auto& id : removed) changed.erase(id);
  Tsugumi::RPC::VersionChangesT facts;
  facts.added.assign(added.begin(), added.end());
  facts.removed.assign(removed.begin(), removed.end());
  facts.workflow = workflow;
  for (const auto& [id, kinds] : changed) {
    auto change = std::make_unique<Tsugumi::RPC::VersionCardChangeT>();
    change->card = id;
    change->kinds.assign(kinds.begin(), kinds.end());
    facts.changed.push_back(std::move(change));
  }
  out.facts = flat::encode<Tsugumi::RPC::VersionChanges>(facts);
  write_note(r.get(), kRecordRef, oid, out.facts);
  return out;
}

auto Versions::diff(const std::string& commit, const std::string& from)
    -> VersionDiffData {
  std::lock_guard lock(mu_);
  Repo repo;
  if (!open(repo, root_))
    throw Failed(FailureKind::not_found, "Project has no version repository");
  Commit target;
  lookup_version(repo.get(), commit, target);
  Tree mine, theirs;
  check(git_commit_tree(mine.out(), target.get()), "tree");
  if (from.empty()) {
    parent_tree(target.get(), theirs);
  } else {
    Commit base;
    lookup_version(repo.get(), from, base);
    check(git_commit_tree(theirs.out(), base.get()), "base tree");
  }
  git_diff_options options;
  check(git_diff_options_init(&options, GIT_DIFF_OPTIONS_VERSION),
        "diff options");
  options.context_lines = 2;
  Diff changes;
  check(git_diff_tree_to_tree(changes.out(), repo.get(), theirs.get(), mine.get(),
                             &options),
        "diff");
  // Bound model input and keep one file from using the full patch budget.
  constexpr size_t kDiffLimit = 12000;
  constexpr size_t kDiffFileLimit = 4000;
  VersionDiffData out;
  for (size_t i = 0; i < git_diff_num_deltas(changes.get()); ++i) {
    const auto* delta = git_diff_get_delta(changes.get(), i);
    const char* path =
        delta->new_file.path ? delta->new_file.path : delta->old_file.path;
    if (!path || !diff_reads(path) ||
        !diff_text_blob(repo.get(), delta->old_file) ||
        !diff_text_blob(repo.get(), delta->new_file))
      continue;
    Patch patch;
    check(git_patch_from_diff(patch.out(), changes.get(), i), "file patch");
    if (!patch) continue;
    struct Buffer {
      git_buf value = GIT_BUF_INIT;
      ~Buffer() { git_buf_dispose(&value); }
    } buf;
    check(git_patch_to_buf(&buf.value, patch.get()), "patch text");
    if (!buf.value.size) continue;
    auto patch_text = text::replace_invalid_utf8(
        std::string(buf.value.ptr, buf.value.size));
    if (patch_text.size() > kDiffFileLimit) {
      auto end = kDiffFileLimit;
      while ((static_cast<unsigned char>(patch_text[end]) & 0xc0) == 0x80) --end;
      patch_text.resize(end);
      out.truncated = true;
    }
    if (patch_text.back() != '\n') patch_text += '\n';
    if (out.text.size() + patch_text.size() > kDiffLimit) {
      out.truncated = true;
      break;
    }
    out.text += patch_text;
  }
  return out;
}

auto Versions::comment_changes(const std::string& commit)
    -> std::vector<comments::Change> {
  std::lock_guard lock(mu_);
  Repo repo;
  if (!open(repo, root_))
    throw Failed(FailureKind::not_found, "Project has no version repository");
  Commit target;
  lookup_version(repo.get(), commit, target);
  Tree mine, theirs;
  check(git_commit_tree(mine.out(), target.get()), "tree");
  parent_tree(target.get(), theirs);
  return compare_comments(repo.get(), theirs.get(), mine.get());
}

}  // namespace tsugumi
