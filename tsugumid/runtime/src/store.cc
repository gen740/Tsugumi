#include "store.hh"

#include <unistd.h>

#include <set>
#include <thread>

#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/fs.hh"
#include "tsugumi/util/text.hh"

namespace tsugumi {

namespace stdfs = std::filesystem;

namespace {

auto load_document(const stdfs::path& root) -> std::shared_ptr<Document> {
  try {
    return load_from_disk(root);
  } catch (const DocumentError& e) {
    std::vector<std::pair<std::string, std::string>> args(e.args.begin(),
                                                          e.args.end());
    throw Failed(
        failure_kind_from_name(e.code).value_or(FailureKind::invalid_document),
        e.what(), args);
  }
}

std::string def_dir(const std::string& id) { return "definitions/" + id + "/"; }

// What a save may write: the documents and the source areas, never
// .tsugumi/ or a path leaving the project.
bool writable(const stdfs::path& root, const std::string& rel) {
  return fs::valid_relative_path(rel) &&
         (Document::is_document_file(rel) || Document::is_source_area(rel)) &&
         fs::resolves_inside(root, root / rel);
}

auto put_receipt(sql::Db& db, const Receipt& receipt) -> void {
  db.prepare("INSERT INTO receipt(op_id, kind, digest, bytes) VALUES(?,?,?,?)")
      .bind(1, receipt.op_id)
      .bind(2, receipt.kind)
      .bind(3, receipt.digest)
      .bind_blob(4, receipt.bytes)
      .run();
}

}  // namespace

Store::Store(StoreOptions opts) : opts_(std::move(opts)) {}

auto Store::stage_file(const stdfs::path& root, const std::string& from,
                       const std::optional<std::string>& expected_hash,
                       const std::function<void()>& check_cancelled) const
    -> std::shared_ptr<SourceFile> {
  if (!fs::valid_relative_path(from))
    throw Failed(FailureKind::invalid_path, "invalid transfer path " + from);
  auto file = std::make_shared<SourceFile>();
  const auto directory = state_dir() / "document-files";
  stdfs::create_directories(directory);
  stdfs::permissions(directory, stdfs::perms::owner_all);
  fs::fsync_dir(state_dir());
  file->path = directory / crypto::uuid4();
  crypto::Sha256 hash;
  bool entered = false;
  try {
    file->size =
        fs::copy_beneath(root, from, file->path, [&](std::string_view chunk) {
          check_cancelled();
          if (opts_.fault == "transfer_gate" && !entered) {
            entered = true;
            fs::write_atomic(state_dir() / "transfer.started", "ready");
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(60);
            while (!stdfs::exists(state_dir() / "transfer.release")) {
              check_cancelled();
              if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error(
                    "transfer fault gate was not released");
              std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
          }
          hash.update(chunk);
        });
  } catch (const fs::NotRegularFile& error) {
    throw Failed(
        FailureKind::invalid_path,
        std::string(error.what()) +
            (error.folder
                 ? "; list its files or copy the whole Card with card + id"
                 : "; choose a regular file to copy"));
  } catch (const fs::Error& error) {
    auto message = std::string(error.what());
    const auto full_path = (root / from).string();
    if (const auto at = message.find(full_path); at != std::string::npos)
      message.replace(at, full_path.size(), from);
    if (const auto at = message.find(root.string()); at != std::string::npos)
      message.replace(at, root.string().size(), "source root");
    throw Failed(FailureKind::invalid_path, message);
  }
  file->hash = hash.finish_hex();
  if (expected_hash && *expected_hash != file->hash)
    throw Failed(
        FailureKind::external_change,
        from + " differs from the confirmed document; Reload before retrying");
  return file;
}

auto Store::allocate_card_id(const std::string& stem,
                             const std::string& project_path) -> std::string {
  if (!valid_id(stem) || stem.size() > 123)
    throw Failed(FailureKind::invalid_request,
                 "Card stem must be 1..123 ASCII identifier characters");
  const auto fold = [](std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(), [](unsigned char c) {
          return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
        });
    return value;
  };
  // A scope can still be a local draft, so validate its path rather than
  // requiring it on disk.
  auto rest = project_path;
  while (!rest.empty()) {
    if (!rest.starts_with("externals/"))
      throw Failed(FailureKind::invalid_path, "invalid project_path");
    rest.erase(0, 10);
    auto slash = rest.find('/');
    if (slash == std::string::npos || !valid_id(rest.substr(0, slash)))
      throw Failed(FailureKind::invalid_path, "invalid project_path");
    rest.erase(0, slash + 1);
  }
  if (!fs::resolves_inside(opts_.root, opts_.root / project_path))
    throw Failed(FailureKind::invalid_path, "project_path leaves the project");
  // Reservations survive reconnects and restarts, including abandoned drafts.
  auto reservations = fs::read_file(state_dir() / "card-ids").value_or("");
  std::set<std::string> issued;
  std::istringstream lines(reservations);
  for (std::string line; std::getline(lines, line);) issued.insert(fold(line));
  auto doc = current();
  std::set<std::string> canvas_ids;
  if (project_path.empty()) {
    for (const auto& card : doc->project.canvas->value().cards->value())
      canvas_ids.insert(fold(card.id->value()));
  } else if (doc->sources.count(project_path + "tsugumi.json")) {
    auto project = parse_external_project(
        project_path + "tsugumi.json",
        read_source(*doc, project_path + "tsugumi.json"));
    for (const auto& card : project.canvas->value().cards->value())
      canvas_ids.insert(fold(card.id->value()));
  }
  if (project_path.empty())
    for (const auto& [id, definition] : doc->definitions) {
      (void)definition;
      canvas_ids.insert(fold(id));
    }
  const auto base = fold(stem);
  // The name itself is the first candidate; duplicates compose a small index.
  // One case keeps generated definition folders portable to macOS.
  for (uint64_t number = 1; number != 0; ++number) {
    const auto suffix = number == 1 ? "" : "-" + std::to_string(number);
    auto id = base.substr(0, 128 - suffix.size()) + suffix;
    auto key = project_path + id;
    auto dir = project_path + def_dir(id);
    bool exists = issued.count(fold(key)) || canvas_ids.count(id) ||
                  stdfs::exists(opts_.root / dir) ||
                  stdfs::exists(opts_.root / project_path / "externals" / id);
    auto source = doc->sources.lower_bound(dir);
    exists = exists ||
             (source != doc->sources.end() && source->first.starts_with(dir));
    if (exists) continue;
    fs::write_atomic(state_dir() / "card-ids", reservations + key + "\n", 0600);
    return id;
  }
  throw Failed(FailureKind::resource_limit,
               "cannot allocate a unique Card identifier");
}

void Store::fault(const char* point) const {
  if (opts_.fault == point) {
    // Simulates a crash at a precise point of the commit protocol.
    ::_exit(97);
  }
}

DocumentPtr Store::current() const {
  std::lock_guard<std::mutex> l(mu_);
  return doc_;
}

std::optional<Receipt> Store::receipt(const std::string& op_id) const {
  std::lock_guard<std::mutex> l(mu_);
  auto statement = db_->prepare(
      "SELECT kind, digest, bytes FROM receipt WHERE op_id=? AND NOT EXISTS "
      "(SELECT 1 FROM document_state WHERE pending_op=receipt.op_id)");
  statement.bind(1, op_id);
  if (!statement.step()) return std::nullopt;
  return Receipt{op_id, statement.text(0), statement.text(1),
                 statement.blob(2)};
}

void Store::open() {
  stdfs::create_directories(state_dir());
  // Project identity: a short reference persisted in internal state, never in
  // the root files.
  auto id_path = state_dir() / "project-id";
  if (auto id = fs::read_file(id_path); id && crypto::is_short_id(*id)) {
    project_id_ = *id;
  } else {
    project_id_ = crypto::short_id();
    fs::write_atomic(id_path, project_id_, 0600);
  }
  recover_pending();
  // Remaining private files belong to completed changes or interrupted staging.
  stdfs::remove_all(state_dir() / "document-files");
  auto state = db_->prepare("SELECT rev, execution_rev FROM document_state");
  state.step();
  const auto rev = state.u64(0);
  const auto exec_rev = state.u64(1);
  auto loaded = load_document(opts_.root);
  loaded->rev = rev;
  loaded->execution_rev = exec_rev;
  auto doc = finalize(loaded);
  std::lock_guard<std::mutex> l(mu_);
  doc_ = doc;
}

void Store::resolve_staged(Staged& st) const {
  finalize(st.doc, &st.ops, current().get());
}

DocumentPtr Store::preview(const std::vector<ChangeIn>& changes) const {
  auto st = stage(changes, "preview", "preview");
  return finalize(st.doc, &st.ops, current().get());
}

namespace {
SourceReader source_reader(const stdfs::path& root, const Document& doc,
                           const std::vector<FileOp>* pending = nullptr) {
  return [root, &doc,
          pending](const std::string& rel) -> std::optional<std::string> {
    auto it = doc.sources.find(rel);
    if (it == doc.sources.end()) return std::nullopt;
    if (it->second.document_json) return *it->second.document_json;
    if (pending) {
      for (const auto& op : *pending)
        if (op.path == rel)
          return op.file ? fs::read_file(op.file->path) : op.content;
    }
    auto data = fs::read_file(root / rel);
    if (!data || crypto::sha256_hex(*data) != it->second.hash)
      return std::nullopt;
    return data;
  };
}

}  // namespace

std::shared_ptr<Document> Store::finalize(std::shared_ptr<Document> doc,
                                          const std::vector<FileOp>* pending,
                                          const Document* base) const {
  auto resolution_key = doc->execution_fingerprint;
  if (doc->resolution && doc->resolution_key == resolution_key)
    return doc;  // resolved before commit
  doc->resolution_key = resolution_key;
  if (base && base->resolution && base->resolution_key == doc->resolution_key) {
    doc->resolution = base->resolution;
    return doc;
  }
  doc->resolution = resolve(*doc, source_reader(opts_.root, *doc, pending));
  return doc;
}

auto Store::recover_pending() -> void {
  const auto path = state_dir() / "document.sqlite";
  // Saved bytes and receipts can contain private project data.
  if (!stdfs::exists(path)) fs::write_atomic(path, "", 0600);
  db_ = std::make_unique<sql::Db>(path.string());
  // Sync the parent directory when deleting SQLite's rollback journal too.
  db_->exec("PRAGMA synchronous=EXTRA");
  db_->exec(R"sql(
    CREATE TABLE IF NOT EXISTS document_state(
      id INTEGER PRIMARY KEY CHECK(id=1),
      rev INTEGER NOT NULL,
      execution_rev INTEGER NOT NULL,
      pending_op TEXT
    );
    INSERT OR IGNORE INTO document_state(id, rev, execution_rev) VALUES(1,0,0);
    CREATE TABLE IF NOT EXISTS receipt(
      op_id TEXT PRIMARY KEY,
      kind TEXT NOT NULL,
      digest TEXT NOT NULL,
      bytes BLOB NOT NULL
    );
    CREATE TABLE IF NOT EXISTS pending_file(
      seq INTEGER PRIMARY KEY,
      path TEXT NOT NULL,
      content BLOB,
      staged_file TEXT,
      hash TEXT
    );
  )sql");
  {
    auto pending = db_->prepare(
        "SELECT pending_op FROM document_state WHERE pending_op IS NOT NULL");
    if (!pending.step()) return;
  }

  std::vector<FileOp> ops;
  auto files = db_->prepare(
      "SELECT path, content, staged_file, hash FROM pending_file ORDER BY seq");
  while (files.step()) {
    FileOp op;
    op.path = files.text(0);
    if (!files.is_null(2)) {
      const auto name = files.text(2);
      if (!crypto::is_uuid(name)) throw fs::Error("invalid staged source name");
      op.file = std::make_shared<SourceFile>();
      op.file->journaled = true;
      op.file->path = state_dir() / "document-files" / name;
      op.file->hash = files.text(3);
      if (fs::sha256_file(op.file->path) != op.file->hash)
        throw fs::Error("staged source hash mismatch");
    } else if (!files.is_null(1)) {
      op.content = files.blob(1);
    }
    ops.push_back(std::move(op));
  }
  apply_ops(ops, false);
  finish_pending();
}

auto Store::persist_pending(const Document& doc, const Receipt& receipt,
                            const std::vector<FileOp>& ops) -> void {
  std::lock_guard lock(mu_);
  sql::Transaction transaction(*db_);
  put_receipt(*db_, receipt);
  db_->prepare("UPDATE document_state SET rev=?, execution_rev=?, pending_op=?")
      .bind(1, doc.rev)
      .bind(2, doc.execution_rev)
      .bind(3, receipt.op_id)
      .run();
  auto row = db_->prepare(
      "INSERT INTO pending_file(path, content, staged_file, hash) "
      "VALUES(?,?,?,?)");
  for (const auto& op : ops) {
    row.bind(1, op.path);
    if (op.content) row.bind_blob(2, *op.content);
    if (op.file)
      row.bind(3, op.file->path.filename().string()).bind(4, op.file->hash);
    row.run();
    row.reset();
  }
  transaction.commit();
}

auto Store::finish_pending() -> void {
  sql::Transaction transaction(*db_);
  db_->exec(
      "DELETE FROM pending_file; UPDATE document_state SET pending_op=NULL");
  transaction.commit();
}

void Store::apply_ops(const std::vector<FileOp>& ops, bool inject) {
  bool first = true;
  for (const auto& op : ops) {
    if (!writable(opts_.root, op.path))
      throw Failed(FailureKind::invalid_path,
                   op.path + " is not a document or source path",
                   {{"path", op.path}});
    auto target = opts_.root / op.path;
    if (op.file) {
      crypto::Sha256 hash;
      fs::copy_beneath(op.file->path.parent_path(), op.file->path.filename(),
                       target,
                       [&](std::string_view chunk) { hash.update(chunk); });
      if (hash.finish_hex() != op.file->hash)
        throw fs::Error("staged source hash mismatch");
    } else if (op.content) {
      fs::write_atomic(target, *op.content);
    } else {
      fs::remove_file_durable(target);
      // Remove now-empty definition directories.
      auto dir = target.parent_path();
      std::error_code ec;
      while (dir != opts_.root && stdfs::is_empty(dir, ec) && !ec) {
        stdfs::remove(dir, ec);
        dir = dir.parent_path();
      }
    }
    if (inject && first) fault("partial_apply");
    first = false;
  }
}

void Store::check_disk_unchanged(const Document& base,
                                 const std::vector<FileOp>& ops) const {
  for (const auto& op : ops) {
    auto disk = fs::file_sha256(opts_.root / op.path);
    std::optional<std::string> expected;
    if (Document::is_document_file(op.path)) {
      auto it = base.disk_hashes.find(op.path);
      if (it != base.disk_hashes.end()) expected = it->second;
    } else {
      auto it = base.sources.find(op.path);
      if (it != base.sources.end()) expected = it->second.hash;
    }
    if (disk != expected)
      throw Failed(FailureKind::external_change,
                   op.path + " changed outside Tsugumi; Reload before saving",
                   {{"path", op.path}});
  }
}

std::string Store::read_source(const Document& doc,
                               const std::string& rel) const {
  auto it = doc.sources.find(rel);
  if (it == doc.sources.end())
    throw Failed(FailureKind::not_found, rel + " is not a source",
                 {{"path", rel}});
  if (it->second.document_json) return *it->second.document_json;
  auto data = fs::read_file(opts_.root / rel);
  if (!data || crypto::sha256_hex(*data) != it->second.hash)
    throw Failed(FailureKind::external_change,
                 rel + " differs from the confirmed document", {{"path", rel}});
  return *data;
}

Store::Staged Store::stage(const std::vector<ChangeIn>& changes,
                           const std::string& op_id,
                           const std::string& origin) const {
  using K = ChangeIn::Kind;
  auto base = current();
  if (changes.empty())
    throw Failed(FailureKind::invalid_request, "changes must not be empty");
  std::set<std::string> targets;
  std::set<std::string> removed_defs;
  for (const auto& c : changes) {
    std::vector<std::string> keys;
    switch (c.kind) {
      case K::SetProject:
        keys = {"project"};
        break;
      case K::SetLayout:
        keys = {"layout"};
        break;
      case K::PutDefinition:
      case K::RemoveDefinition:
        keys = {"def:" + c.id};
        break;
      default:
        keys = {"src:" + c.path};
        break;
    }
    for (const auto& key : keys)
      if (!targets.insert(key).second)
        throw Failed(
            FailureKind::invalid_request,
            "multiple changes target " + key.substr(key.find(':') + 1));
    if (c.kind == K::RemoveDefinition) removed_defs.insert(c.id);
  }
  for (const auto& c : changes) {
    if (c.kind == K::PutSource || c.kind == K::PatchSource ||
        c.kind == K::RemoveSource) {
      for (const auto& id : removed_defs)
        if (c.path.starts_with(def_dir(id)))
          throw Failed(
              FailureKind::invalid_request,
              "cannot edit " + c.path + " while removing definition " + id);
    }
  }

  auto doc = std::make_shared<Document>(*base);
  doc->resolution.reset();
  Staged st;
  st.delta.base_rev = base->rev;
  st.delta.op_id = op_id;
  st.delta.origin = origin;
  std::map<std::string, std::optional<std::string>>
      files;  // final content per path
  std::map<std::string, std::shared_ptr<SourceFile>> streamed;

  auto hash_conflict = [](const std::string& path, const std::string& expected,
                          const std::string& actual) {
    return Failed(
        FailureKind::hash_conflict, path + " has a different base hash",
        {{"path", path}, {"expected_hash", expected}, {"actual_hash", actual}});
  };
  auto wrap = [](auto&& fn) {
    try {
      fn();
    } catch (const DocumentError& e) {
      std::vector<std::pair<std::string, std::string>> args(e.args.begin(),
                                                            e.args.end());
      auto kind = failure_kind_from_name(e.code).value_or(
          FailureKind::invalid_document);
      throw Failed(kind, e.what(), args);
    }
  };

  for (const auto& c : changes) {
    switch (c.kind) {
      case K::SetProject:
        wrap([&] {
          doc->project = parse_project_json(c.json);
          doc->project_json = c.json;
        });
        break;
      case K::SetLayout:
        wrap([&] { doc->layout_json = c.json; });
        break;
      case K::PutDefinition: {
        if (!valid_id(c.id))
          throw Failed(FailureKind::invalid_request,
                       "invalid definition id " + c.id);
        DefinitionDoc d;
        wrap([&] {
          d.card = parse_card_json(c.id, c.json);
          d.card_json = c.json;
          if (c.meta_json) {
            check_meta_json(*c.meta_json);
            d.meta_json = *c.meta_json;
          } else if (c.keep_meta) {
            if (auto old = doc->definitions.find(c.id);
                old != doc->definitions.end() && old->second.meta_json) {
              d.meta_json = old->second.meta_json;
            }
          }
        });
        doc->definitions[c.id] = std::move(d);
        break;
      }
      case K::RemoveDefinition: {
        if (!doc->definitions.erase(c.id))
          throw Failed(FailureKind::not_found,
                       "definition " + c.id + " does not exist",
                       {{"definition", c.id}});
        for (auto it = doc->sources.begin(); it != doc->sources.end();) {
          if (it->first.starts_with(def_dir(c.id))) {
            files[it->first] = std::nullopt;
            it = doc->sources.erase(it);
          } else {
            ++it;
          }
        }
        break;
      }
      case K::PutSource:
      case K::PatchSource:
      case K::RemoveSource: {
        const auto area = Document::classify_source_path(c.path);
        if (area == Document::SourcePath::Invalid)
          throw Failed(FailureKind::invalid_path, "invalid path " + c.path,
                       {{"path", c.path}});
        if (area == Document::SourcePath::Protected)
          throw Failed(FailureKind::permission_denied,
                       c.path + " cannot be changed through source operations",
                       {{"path", c.path}});
        if (!fs::resolves_inside(opts_.root, opts_.root / c.path))
          throw Failed(FailureKind::invalid_path,
                       c.path + " leaves the project", {{"path", c.path}});
        auto it = doc->sources.find(c.path);
        if (c.kind == K::PutSource) {
          if (it != doc->sources.end()) {
            if (!c.base_hash)
              throw Failed(FailureKind::hash_conflict,
                           "base_hash is required to replace " + c.path,
                           {{"path", c.path},
                            {"expected_hash", ""},
                            {"actual_hash", it->second.hash}});
            if (*c.base_hash != it->second.hash)
              throw hash_conflict(c.path, *c.base_hash, it->second.hash);
          } else if (c.base_hash) {
            throw hash_conflict(c.path, *c.base_hash, "");
          } else {
            std::error_code ec;
            if (stdfs::exists(opts_.root / c.path, ec))
              throw Failed(
                  FailureKind::external_change,
                  c.path +
                      " exists on disk but not in the document; Reload first",
                  {{"path", c.path}});
          }
          if (c.file) {
            doc->sources[c.path] = SourceEntry{c.file->hash, c.file->size};
            streamed[c.path] = c.file;
          } else {
            doc->sources[c.path] =
                SourceEntry{crypto::sha256_hex(c.bytes), c.bytes.size()};
            files[c.path] = c.bytes;
          }
        } else if (c.kind == K::PatchSource) {
          if (it == doc->sources.end())
            throw Failed(FailureKind::not_found, c.path + " does not exist",
                         {{"path", c.path}});
          if (!c.base_hash || *c.base_hash != it->second.hash)
            throw hash_conflict(c.path, c.base_hash.value_or(""),
                                it->second.hash);
          std::string original = read_source(*base, c.path);
          if (!text::valid_utf8(original))
            throw Failed(FailureKind::invalid_request, c.path + " is not UTF-8",
                         {{"path", c.path}});
          text::PatchApplier patch(original);
          for (const auto& p : c.patches) {
            try {
              patch.append(p.offset, p.remove, p.text);
            } catch (const text::PatchBoundaryError& error) {
              throw Failed(FailureKind::invalid_range, error.what(),
                           {{"offset", std::to_string(p.offset)}});
            } catch (const text::PatchRangeError& error) {
              throw Failed(FailureKind::invalid_range, error.what());
            }
          }
          if (c.patches.empty())
            throw Failed(FailureKind::invalid_request,
                         "patches must not be empty");
          auto out = std::move(patch).finish();
          doc->sources[c.path] =
              SourceEntry{crypto::sha256_hex(out), out.size()};
          files[c.path] = std::move(out);
        } else {
          if (it == doc->sources.end())
            throw Failed(FailureKind::not_found, c.path + " does not exist",
                         {{"path", c.path}});
          if (*c.base_hash != it->second.hash)
            throw hash_conflict(c.path, *c.base_hash, it->second.hash);
          doc->sources.erase(it);
          files[c.path] = std::nullopt;
        }
        break;
      }
    }
  }
  // Sources under a definition need the definition in the final document.
  for (const auto& [path, _] : doc->sources) {
    if (!path.starts_with("definitions/")) continue;
    auto id = path.substr(12, path.find('/', 12) - 12);
    if (!doc->definitions.count(id))
      throw Failed(
          FailureKind::invalid_document,
          path + " belongs to definition " + id + " which does not exist",
          {{"path", path}});
  }
  wrap([&] { check_structure(*doc); });

  for (const auto& [path, content] : files) {
    if (!content || !Document::is_external_document_file(path)) continue;
    wrap([&] {
      doc->sources.at(path) = external_document_source(path, *content);
    });
  }
  for (const auto& [path, file] : streamed) {
    if (!Document::is_external_document_file(path)) continue;
    wrap([&] {
      if (file->size > kMaxDocumentJsonBytes)
        throw DocumentError("resource_limit",
                            path + " exceeds the document JSON limit");
      doc->sources.at(path) =
          external_document_source(path, fs::read_file_or_throw(file->path));
    });
  }
  wrap([&] { doc->check_document_size(); });
  if (doc->sources.size() > kMaxManifestEntries)
    throw Failed(
        FailureKind::resource_limit,
        "source manifest has " + std::to_string(doc->sources.size()) +
            " entries; limit is 10000 (10,000). Keep data in an attached "
            "folder and read it through hostFolders (/host/<name>).");

  // File operations for document files, computed from the final model.
  if (doc->project_json != base->project_json) {
    files["tsugumi.json"] = doc->project_json;
    st.delta.project_changed = true;
  }
  if (doc->layout_json != base->layout_json) {
    files["layout.json"] = doc->layout_json;
    st.delta.layout_changed = true;
  }
  for (const auto& [id, d] : doc->definitions) {
    auto old = base->definitions.find(id);
    bool fresh = old == base->definitions.end();
    bool card_changed = fresh || old->second.card_json != d.card_json;
    bool meta_changed = fresh || old->second.meta_json != d.meta_json;
    if (card_changed) files[def_dir(id) + "_card.json"] = d.card_json;
    if (meta_changed) {
      if (d.meta_json)
        files[def_dir(id) + "_meta.json"] = *d.meta_json;
      else if (old != base->definitions.end() && old->second.meta_json)
        files[def_dir(id) + "_meta.json"] = std::nullopt;
    }
    if (card_changed || meta_changed) st.delta.definitions_upsert.push_back(id);
  }
  for (const auto& [id, d] : base->definitions) {
    if (doc->definitions.count(id)) continue;
    st.delta.definitions_removed.push_back(id);
    files[def_dir(id) + "_card.json"] = std::nullopt;
    if (d.meta_json) files[def_dir(id) + "_meta.json"] = std::nullopt;
  }
  for (const auto& [path, content] : files) {
    if (Document::is_document_file(path)) continue;
    auto old = base->sources.find(path);
    if (content) {
      if (old == base->sources.end() ||
          old->second.hash != doc->sources.at(path).hash)
        st.delta.sources_upsert.push_back(path);
    } else if (old != base->sources.end()) {
      st.delta.sources_removed.push_back(path);
    }
  }
  for (const auto& [path, content] : files) {
    if (content) {
      // Skip writes that would not change the bytes on disk.
      bool same = false;
      if (Document::is_document_file(path)) {
        auto it = base->disk_hashes.find(path);
        same = it != base->disk_hashes.end() &&
               it->second == crypto::sha256_hex(*content);
      } else {
        auto it = base->sources.find(path);
        same = it != base->sources.end() &&
               it->second.hash == crypto::sha256_hex(*content);
      }
      if (same) continue;
    }
    st.ops.push_back({path, content});
  }
  for (const auto& [path, file] : streamed) {
    auto old = base->sources.find(path);
    if (old != base->sources.end() && old->second.hash == file->hash) continue;
    st.delta.sources_upsert.push_back(path);
    st.ops.push_back({path, std::nullopt, file});
  }
  st.effective = st.delta.project_changed || st.delta.layout_changed ||
                 !st.delta.definitions_upsert.empty() ||
                 !st.delta.definitions_removed.empty() ||
                 !st.delta.sources_upsert.empty() ||
                 !st.delta.sources_removed.empty();
  for (const auto& op : st.ops) {
    if (!Document::is_document_file(op.path)) continue;
    if (op.content)
      doc->disk_hashes[op.path] = crypto::sha256_hex(*op.content);
    else
      doc->disk_hashes.erase(op.path);
  }
  wrap([&] { doc->execution_fingerprint = execution_fingerprint(*doc); });
  st.doc = doc;
  return st;
}

namespace {
void check_ports(const Resolution& resolution) {
  // Endpoint validity is a document invariant, including when a Draft becomes
  // implemented. Never infer a port, even if there is only one candidate.
  for (const auto& diagnostic : resolution.all_diagnostics(false))
    if (diagnostic.code == "unknown_port")
      throw Failed(FailureKind::validation_failed, diagnostic.message);
}
}  // namespace

Store::Staged Store::stage_reload(const std::string& op_id,
                                  const std::string& origin,
                                  const stdfs::path& checkout) const {
  auto base = current();
  auto doc = load_document(checkout.empty() ? opts_.root : checkout);
  if (!checkout.empty()) {
    doc->resolution = resolve(*doc, source_reader(checkout, *doc));
    doc->resolution_key = doc->execution_fingerprint;
    for (const auto& diagnostic : doc->resolution->all_diagnostics(false))
      if (diagnostic.code == "invalid_document")
        throw Failed(FailureKind::invalid_document, diagnostic.message);
  }
  Staged st;
  st.delta.base_rev = base->rev;
  st.delta.op_id = op_id;
  st.delta.origin = origin;
  st.delta.project_changed = doc->project_json != base->project_json;
  st.delta.layout_changed = doc->layout_json != base->layout_json;
  for (const auto& [id, d] : doc->definitions) {
    auto old = base->definitions.find(id);
    if (old == base->definitions.end() ||
        old->second.card_json != d.card_json ||
        old->second.meta_json != d.meta_json)
      st.delta.definitions_upsert.push_back(id);
  }
  for (const auto& [id, _] : base->definitions)
    if (!doc->definitions.count(id)) st.delta.definitions_removed.push_back(id);
  for (const auto& [path, s] : doc->sources) {
    auto old = base->sources.find(path);
    if (old == base->sources.end() || old->second.hash != s.hash)
      st.delta.sources_upsert.push_back(path);
  }
  for (const auto& [path, _] : base->sources)
    if (!doc->sources.count(path)) st.delta.sources_removed.push_back(path);
  st.effective = st.delta.project_changed || st.delta.layout_changed ||
                 !st.delta.definitions_upsert.empty() ||
                 !st.delta.definitions_removed.empty() ||
                 !st.delta.sources_upsert.empty() ||
                 !st.delta.sources_removed.empty();
  // Disk hashes always follow what is on disk now, even without effective
  // changes.
  doc->rev = base->rev;
  doc->execution_rev = base->execution_rev;
  st.doc = doc;
  return st;
}

void Store::prepare_delta(Staged& st) const {
  if (!st.effective || st.delta.doc) return;
  const auto base = current();
  auto doc = st.doc;
  doc->rev = base->rev + 1;
  finalize(doc, &st.ops, base.get());
  check_ports(*doc->resolution);
  doc->execution_rev = doc->execution_fingerprint != base->execution_fingerprint
                           ? base->execution_rev + 1
                           : base->execution_rev;
  st.delta.rev = doc->rev;
  st.delta.execution_rev = doc->execution_rev;
  st.delta.doc = doc;
}

void Store::commit(Staged& st, const std::string& kind,
                   const std::string& digest,
                   const std::function<std::string(const Delta*)>& make_receipt,
                   const std::function<void()>& check_cancelled) {
  if (halted_)
    throw Failed(
        FailureKind::persistence_failed,
        "changes are halted after a persistence failure; restart to recover");
  check_cancelled();
  auto base = current();
  if (!st.effective) {
    Receipt r{st.delta.op_id, kind, digest, make_receipt(nullptr)};
    check_cancelled();
    record_receipt(r);
    // Receipt only: no revision change, no notification. A reload may still
    // learn new on-disk bytes (e.g. reformatted JSON with the same content).
    if (st.doc && st.doc->disk_hashes != base->disk_hashes) {
      auto refreshed = std::make_shared<Document>(*base);
      refreshed->disk_hashes = st.doc->disk_hashes;
      std::lock_guard<std::mutex> l(mu_);
      doc_ = refreshed;
    }
    st.doc = nullptr;
    return;
  }
  check_disk_unchanged(*base, st.ops);
  prepare_delta(st);
  auto doc = st.doc;
  Receipt receipt{st.delta.op_id, kind, digest, make_receipt(&st.delta)};
  check_cancelled();
  // A failed commit can leave the intent durable. Recovery owns the staged
  // files from this point until the receipt is published.
  for (const auto& op : st.ops)
    if (op.file) op.file->journaled = true;
  try {
    persist_pending(*doc, receipt, st.ops);
    fault("after_prepare");
    apply_ops(st.ops, true);
    fault("before_commit");
    {
      std::lock_guard lock(mu_);
      finish_pending();
      doc_ = doc;
    }
    for (const auto& op : st.ops)
      if (op.file) op.file->journaled = false;
  } catch (const std::exception& e) {
    halted_ = true;
    throw Failed(FailureKind::persistence_failed, e.what());
  }
}

void Store::record_receipt(const Receipt& r) {
  std::lock_guard lock(mu_);
  try {
    put_receipt(*db_, r);
  } catch (const sql::Error& e) {
    throw Failed(FailureKind::persistence_failed, e.what());
  }
}

}  // namespace tsugumi
