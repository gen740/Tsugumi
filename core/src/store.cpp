#include "store.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <set>

#include "util/crypto.hpp"
#include "util/fs.hpp"
#include "util/text.hpp"

namespace tsugumi {

namespace stdfs = std::filesystem;

namespace {

constexpr char kMagic[4] = {'T', 'S', 'J', '1'};

bool utf8_boundary(const std::string& s, uint64_t off) {
  if (off == 0 || off == s.size()) return true;
  if (off > s.size()) return false;
  return (static_cast<unsigned char>(s[off]) & 0xC0) != 0x80;
}

std::string def_dir(const std::string& id) { return "definitions/" + id + "/"; }

std::string journal_payload(const char* type, uint64_t rev, uint64_t exec_rev, const std::string& op,
                            const std::string& kind, const std::string& digest, const std::string& receipt,
                            const std::vector<FileOp>& ops) {
  schema::JournalRecord r;
  *r.type = type;
  *r.rev = rev;
  *r.execution_rev = exec_rev;
  *r.op = op;
  *r.kind = kind;
  *r.digest = digest;
  *r.receipt_b64 = crypto::base64(receipt);
  for (const auto& o : ops) {
    schema::JournalFile f;
    *f.path = o.path;
    *f.remove = !o.content.has_value();
    *f.content_b64 = o.content ? crypto::base64(*o.content) : "";
    r.files->push_back(std::move(f));
  }
  return schema::encode(r);
}

}  // namespace

Store::Store(StoreOptions opts) : opts_(std::move(opts)) {}

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
  auto it = receipts_.find(op_id);
  if (it == receipts_.end()) return std::nullopt;
  return it->second;
}

void Store::open() {
  stdfs::create_directories(state_dir());
  // Project identity: a UUID persisted in internal state, never in the root files.
  auto id_path = state_dir() / "project-id";
  if (auto id = fs::read_file(id_path); id && crypto::is_uuid(*id)) {
    project_id_ = *id;
  } else {
    project_id_ = crypto::uuid4();
    fs::write_atomic(id_path, project_id_, 0600);
  }
  recover_journal();
  uint64_t rev = 0, exec_rev = 0;
  {
    std::lock_guard<std::mutex> l(mu_);
    if (doc_) {
      rev = doc_->rev;
      exec_rev = doc_->execution_rev;
    }
  }
  auto loaded = load_from_disk(opts_.root);
  loaded->rev = rev;
  loaded->execution_rev = exec_rev;
  auto doc = finalize(loaded);
  std::lock_guard<std::mutex> l(mu_);
  doc_ = doc;
}

std::shared_ptr<Document> Store::finalize(std::shared_ptr<Document> doc) const {
  doc->execution_fingerprint = execution_fingerprint(*doc);
  auto root = opts_.root;
  Document* raw = doc.get();
  doc->resolution = resolve(*doc, [root, raw](const std::string& rel) -> std::optional<std::string> {
    auto it = raw->sources.find(rel);
    if (it == raw->sources.end()) return std::nullopt;
    auto data = fs::read_file(root / rel);
    if (!data || crypto::sha256_hex(*data) != it->second.hash) return std::nullopt;
    return data;
  });
  return doc;
}

// Journal record: magic(4) | len(u32 LE) | crc32(u32 LE) | payload (compact JSON).
void Store::recover_journal() {
  auto path = state_dir() / "document.journal";
  journal_fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_APPEND, 0600);
  if (journal_fd_ < 0) throw fs::Error("open journal: " + std::string(std::strerror(errno)));
  auto data = fs::read_file_or_throw(path);
  size_t pos = 0, good = 0;
  struct Txn {
    schema::JournalRecord rec;
    bool committed = false;
  };
  std::vector<Txn> txns;
  std::map<std::string, Receipt> receipts;
  uint64_t rev = 0, exec_rev = 0;
  auto to_receipt = [](const schema::JournalRecord& r) {
    Receipt rc;
    rc.op_id = *r.op;
    rc.kind = *r.kind;
    rc.digest = *r.digest;
    crypto::base64_decode(*r.receipt_b64, rc.bytes);
    return rc;
  };
  while (pos + 12 <= data.size()) {
    if (std::memcmp(data.data() + pos, kMagic, 4) != 0) break;
    uint32_t len, crc;
    std::memcpy(&len, data.data() + pos + 4, 4);
    std::memcpy(&crc, data.data() + pos + 8, 4);
    if (pos + 12 + len > data.size()) break;
    std::string_view payload(data.data() + pos + 12, len);
    if (crypto::crc32(payload.data(), payload.size()) != crc) break;
    schema::JournalRecord rec;
    if (schema::decode(payload, rec)) break;
    pos += 12 + len;
    good = pos;
    if (*rec.type == "txn") {
      txns.push_back({rec, false});
    } else if (*rec.type == "commit") {
      for (auto& tx : txns)
        if (*tx.rec.rev == *rec.rev) tx.committed = true;
    } else if (*rec.type == "receipt") {
      receipts[*rec.op] = to_receipt(rec);
    }
  }
  if (good != data.size()) {
    // Torn tail from a crash during append: drop it.
    if (::ftruncate(journal_fd_, static_cast<off_t>(good)) != 0)
      throw fs::Error("truncate journal: " + std::string(std::strerror(errno)));
    ::fsync(journal_fd_);
  }
  for (auto& tx : txns) {
    const auto& rec = tx.rec;
    if (!tx.committed) {
      // Redo: the txn record holds every file's final content.
      std::vector<FileOp> ops;
      for (const auto& f : *rec.files) {
        FileOp op;
        op.path = *f.path;
        if (!*f.remove) {
          std::string bytes;
          crypto::base64_decode(*f.content_b64, bytes);
          op.content = std::move(bytes);
        }
        ops.push_back(std::move(op));
      }
      apply_ops(ops, false);
      append_record(journal_payload("commit", *rec.rev, *rec.execution_rev, *rec.op, "", "", "", {}));
    }
    rev = *rec.rev;
    exec_rev = *rec.execution_rev;
    receipts[*rec.op] = to_receipt(rec);
  }
  auto doc = std::make_shared<Document>();
  doc->rev = rev;
  doc->execution_rev = exec_rev;
  std::lock_guard<std::mutex> l(mu_);
  doc_ = doc;  // placeholder carrying only the revision numbers
  receipts_ = std::move(receipts);
}

void Store::append_record(const std::string& payload) {
  std::string rec(kMagic, 4);
  uint32_t len = static_cast<uint32_t>(payload.size());
  uint32_t crc = crypto::crc32(payload.data(), payload.size());
  rec.append(reinterpret_cast<const char*>(&len), 4);
  rec.append(reinterpret_cast<const char*>(&crc), 4);
  rec += payload;
  const char* p = rec.data();
  size_t n = rec.size();
  while (n > 0) {
    ssize_t w = ::write(journal_fd_, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      halted_ = true;
      throw Failed(FailureKind::persistence_failed, "journal write failed: " + std::string(std::strerror(errno)));
    }
    p += w;
    n -= static_cast<size_t>(w);
  }
  if (::fsync(journal_fd_) != 0) {
    halted_ = true;
    throw Failed(FailureKind::persistence_failed, "journal fsync failed");
  }
}

void Store::apply_ops(const std::vector<FileOp>& ops, bool inject) {
  bool first = true;
  for (const auto& op : ops) {
    auto target = opts_.root / op.path;
    if (op.content) {
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

void Store::check_disk_unchanged(const Document& base, const std::vector<FileOp>& ops) const {
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

std::string Store::read_source(const Document& doc, const std::string& rel) const {
  auto it = doc.sources.find(rel);
  if (it == doc.sources.end()) throw Failed(FailureKind::not_found, rel + " is not a source", {{"path", rel}});
  auto data = fs::read_file(opts_.root / rel);
  if (!data || crypto::sha256_hex(*data) != it->second.hash)
    throw Failed(FailureKind::external_change, rel + " differs from the confirmed document",
                 {{"path", rel}});
  return *data;
}

Store::Staged Store::stage(const std::vector<ChangeIn>& changes, const std::string& op_id,
                           const std::string& origin) const {
  using K = ChangeIn::Kind;
  auto base = current();
  if (changes.empty()) throw Failed(FailureKind::invalid_request, "changes must not be empty");
  std::set<std::string> targets;
  std::set<std::string> removed_defs;
  for (const auto& c : changes) {
    std::string key;
    switch (c.kind) {
      case K::SetProject: key = "project"; break;
      case K::SetLayout: key = "layout"; break;
      case K::PutDefinition:
      case K::RemoveDefinition: key = "def:" + c.id; break;
      default: key = "src:" + c.path; break;
    }
    if (!targets.insert(key).second)
      throw Failed(FailureKind::invalid_request, "multiple changes target " + key.substr(key.find(':') + 1));
    if (c.kind == K::RemoveDefinition) removed_defs.insert(c.id);
  }
  for (const auto& c : changes) {
    if (c.kind == K::PutSource || c.kind == K::PatchSource || c.kind == K::RemoveSource) {
      for (const auto& id : removed_defs)
        if (c.path.starts_with(def_dir(id)))
          throw Failed(FailureKind::invalid_request,
                       "cannot edit " + c.path + " while removing definition " + id);
    }
  }

  auto doc = std::make_shared<Document>(*base);
  doc->resolution.reset();
  Staged st;
  st.delta.base_rev = base->rev;
  st.delta.op_id = op_id;
  st.delta.origin = origin;
  std::map<std::string, std::optional<std::string>> files;  // final content per path

  auto hash_conflict = [](const std::string& path, const std::string& expected,
                          const std::string& actual) {
    return Failed(FailureKind::hash_conflict, path + " has a different base hash",
                  {{"path", path}, {"expected_hash", expected}, {"actual_hash", actual}});
  };
  auto wrap = [](auto&& fn) {
    try {
      fn();
    } catch (const DocumentError& e) {
      std::vector<std::pair<std::string, std::string>> args(e.args.begin(), e.args.end());
      auto kind = failure_kind_from_name(e.code).value_or(FailureKind::invalid_document);
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
        wrap([&] {
          doc->layout = parse_layout_json(c.json);
          doc->layout_json = c.json;
        });
        break;
      case K::PutDefinition: {
        if (!valid_id(c.id)) throw Failed(FailureKind::invalid_request, "invalid definition id " + c.id);
        DefinitionDoc d;
        wrap([&] {
          d.card = parse_card_json(c.id, c.json);
          d.card_json = c.json;
          if (c.meta_json) {
            d.meta = parse_meta_json(*c.meta_json);
            d.meta_json = *c.meta_json;
          }
        });
        doc->definitions[c.id] = std::move(d);
        break;
      }
      case K::RemoveDefinition: {
        if (!doc->definitions.erase(c.id))
          throw Failed(FailureKind::not_found, "definition " + c.id + " does not exist", {{"definition", c.id}});
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
        if (!fs::valid_relative_path(c.path))
          throw Failed(FailureKind::invalid_path, "invalid path " + c.path, {{"path", c.path}});
        if (!Document::is_source_area(c.path))
          throw Failed(FailureKind::permission_denied,
                       c.path + " cannot be changed through source operations", {{"path", c.path}});
        if (!fs::resolves_inside(opts_.root, opts_.root / c.path))
          throw Failed(FailureKind::invalid_path, c.path + " leaves the project", {{"path", c.path}});
        auto it = doc->sources.find(c.path);
        if (c.kind == K::PutSource) {
          if (it != doc->sources.end()) {
            if (!c.base_hash)
              throw Failed(FailureKind::hash_conflict, "base_hash is required to replace " + c.path,
                           {{"path", c.path}, {"expected_hash", ""}, {"actual_hash", it->second.hash}});
            if (*c.base_hash != it->second.hash) throw hash_conflict(c.path, *c.base_hash, it->second.hash);
          } else if (c.base_hash) {
            throw hash_conflict(c.path, *c.base_hash, "");
          } else {
            std::error_code ec;
            if (stdfs::exists(opts_.root / c.path, ec))
              throw Failed(FailureKind::external_change,
                           c.path + " exists on disk but not in the document; Reload first",
                           {{"path", c.path}});
          }
          doc->sources[c.path] = SourceEntry{crypto::sha256_hex(c.bytes), c.bytes.size()};
          files[c.path] = c.bytes;
        } else if (c.kind == K::PatchSource) {
          if (it == doc->sources.end())
            throw Failed(FailureKind::not_found, c.path + " does not exist", {{"path", c.path}});
          if (!c.base_hash || *c.base_hash != it->second.hash)
            throw hash_conflict(c.path, c.base_hash.value_or(""), it->second.hash);
          std::string original = read_source(*base, c.path);
          if (!text::valid_utf8(original))
            throw Failed(FailureKind::invalid_request, c.path + " is not UTF-8", {{"path", c.path}});
          std::string out;
          uint64_t cursor = 0;
          for (const auto& p : c.patches) {
            if (p.offset < cursor)
              throw Failed(FailureKind::invalid_range, "patches must be ascending and non-overlapping");
            if (p.offset + p.remove > original.size() || p.offset + p.remove < p.offset)
              throw Failed(FailureKind::invalid_range, "patch exceeds the source size");
            if (!utf8_boundary(original, p.offset) || !utf8_boundary(original, p.offset + p.remove))
              throw Failed(FailureKind::invalid_range, "patch is not on a UTF-8 boundary",
                           {{"offset", std::to_string(p.offset)}});
            out.append(original, cursor, p.offset - cursor);
            out += p.text;
            cursor = p.offset + p.remove;
          }
          if (c.patches.empty()) throw Failed(FailureKind::invalid_request, "patches must not be empty");
          out.append(original, cursor, std::string::npos);
          doc->sources[c.path] = SourceEntry{crypto::sha256_hex(out), out.size()};
          files[c.path] = std::move(out);
        } else {
          if (it == doc->sources.end())
            throw Failed(FailureKind::not_found, c.path + " does not exist", {{"path", c.path}});
          if (*c.base_hash != it->second.hash) throw hash_conflict(c.path, *c.base_hash, it->second.hash);
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
      throw Failed(FailureKind::invalid_document,
                   path + " belongs to definition " + id + " which does not exist", {{"path", path}});
  }
  wrap([&] { check_structure(*doc); });

  size_t json_bytes = doc->project_json.size() + doc->layout_json.size();
  for (const auto& [_, d] : doc->definitions)
    json_bytes += d.card_json.size() + (d.meta_json ? d.meta_json->size() : 0);
  if (json_bytes > kMaxDocumentJsonBytes)
    throw Failed(FailureKind::resource_limit, "document JSON exceeds 8 MiB");
  if (doc->sources.size() > kMaxManifestEntries)
    throw Failed(FailureKind::resource_limit, "source manifest exceeds 10,000 entries");

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
    bool card_changed = old == base->definitions.end() || old->second.card_json != d.card_json;
    bool meta_changed = old == base->definitions.end() || old->second.meta_json != d.meta_json;
    if (card_changed) files[def_dir(id) + "_card.json"] = d.card_json;
    if (meta_changed) {
      if (d.meta_json) files[def_dir(id) + "_meta.json"] = *d.meta_json;
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
      if (old == base->sources.end() || old->second.hash != doc->sources.at(path).hash)
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
        same = it != base->disk_hashes.end() && it->second == crypto::sha256_hex(*content);
      } else {
        auto it = base->sources.find(path);
        same = it != base->sources.end() && it->second.hash == crypto::sha256_hex(*content);
      }
      if (same) continue;
    }
    st.ops.push_back({path, content});
  }
  st.effective = st.delta.project_changed || st.delta.layout_changed ||
                 !st.delta.definitions_upsert.empty() || !st.delta.definitions_removed.empty() ||
                 !st.delta.sources_upsert.empty() || !st.delta.sources_removed.empty();
  for (const auto& op : st.ops) {
    if (!Document::is_document_file(op.path)) continue;
    if (op.content) doc->disk_hashes[op.path] = crypto::sha256_hex(*op.content);
    else doc->disk_hashes.erase(op.path);
  }
  st.doc = doc;
  return st;
}

Store::Staged Store::stage_reload(const std::string& op_id, const std::string& origin) const {
  auto base = current();
  std::shared_ptr<Document> doc;
  try {
    doc = load_from_disk(opts_.root);
  } catch (const DocumentError& e) {
    std::vector<std::pair<std::string, std::string>> args(e.args.begin(), e.args.end());
    throw Failed(failure_kind_from_name(e.code).value_or(FailureKind::invalid_document), e.what(), args);
  }
  Staged st;
  st.delta.base_rev = base->rev;
  st.delta.op_id = op_id;
  st.delta.origin = origin;
  st.delta.project_changed = doc->project_json != base->project_json;
  st.delta.layout_changed = doc->layout_json != base->layout_json;
  for (const auto& [id, d] : doc->definitions) {
    auto old = base->definitions.find(id);
    if (old == base->definitions.end() || old->second.card_json != d.card_json ||
        old->second.meta_json != d.meta_json)
      st.delta.definitions_upsert.push_back(id);
  }
  for (const auto& [id, _] : base->definitions)
    if (!doc->definitions.count(id)) st.delta.definitions_removed.push_back(id);
  for (const auto& [path, s] : doc->sources) {
    auto old = base->sources.find(path);
    if (old == base->sources.end() || old->second.hash != s.hash) st.delta.sources_upsert.push_back(path);
  }
  for (const auto& [path, _] : base->sources)
    if (!doc->sources.count(path)) st.delta.sources_removed.push_back(path);
  st.effective = st.delta.project_changed || st.delta.layout_changed ||
                 !st.delta.definitions_upsert.empty() || !st.delta.definitions_removed.empty() ||
                 !st.delta.sources_upsert.empty() || !st.delta.sources_removed.empty();
  // Disk hashes always follow what is on disk now, even without effective changes.
  doc->rev = base->rev;
  doc->execution_rev = base->execution_rev;
  st.doc = doc;
  return st;
}

void Store::commit(Staged& st, const std::string& kind, const std::string& digest,
                   const std::function<std::string(const Delta*)>& make_receipt) {
  if (halted_)
    throw Failed(FailureKind::persistence_failed,
                 "changes are halted after a persistence failure; restart to recover");
  auto base = current();
  if (!st.effective) {
    // Receipt only: no revision change, no notification. A reload may still
    // learn new on-disk bytes (e.g. reformatted JSON with the same content).
    if (st.doc && st.doc->disk_hashes != base->disk_hashes) {
      auto refreshed = std::make_shared<Document>(*base);
      refreshed->disk_hashes = st.doc->disk_hashes;
      std::lock_guard<std::mutex> l(mu_);
      doc_ = refreshed;
    }
    st.doc = nullptr;
    Receipt r{st.delta.op_id, kind, digest, make_receipt(nullptr)};
    record_receipt(r);
    return;
  }
  check_disk_unchanged(*base, st.ops);
  auto doc = st.doc;
  doc->rev = base->rev + 1;
  doc->execution_fingerprint = execution_fingerprint(*doc);
  doc->execution_rev =
      doc->execution_fingerprint != base->execution_fingerprint ? base->execution_rev + 1 : base->execution_rev;
  finalize(doc);
  st.delta.rev = doc->rev;
  st.delta.execution_rev = doc->execution_rev;
  st.delta.doc = doc;
  std::string receipt = make_receipt(&st.delta);

  append_record(journal_payload("txn", doc->rev, doc->execution_rev, st.delta.op_id, kind, digest, receipt, st.ops));
  fault("after_journal");
  try {
    apply_ops(st.ops, true);
    fault("before_commit");
    append_record(journal_payload("commit", doc->rev, doc->execution_rev, st.delta.op_id, "", "", "", {}));
  } catch (const Failed&) {
    throw;
  } catch (const std::exception& e) {
    // The journal holds the transaction; recovery on restart completes it.
    halted_ = true;
    throw Failed(FailureKind::persistence_failed, std::string("applying the transaction failed: ") + e.what());
  }
  std::lock_guard<std::mutex> l(mu_);
  doc_ = doc;
  receipts_[st.delta.op_id] = Receipt{st.delta.op_id, kind, digest, receipt};
}

void Store::record_receipt(const Receipt& r) {
  auto base = current();
  append_record(journal_payload("receipt", base ? base->rev : 0, base ? base->execution_rev : 0, r.op_id, r.kind,
                                r.digest, r.bytes, {}));
  std::lock_guard<std::mutex> l(mu_);
  receipts_[r.op_id] = r;
}

}  // namespace tsugumi
