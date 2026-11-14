#include "store.hh"

#include <thread>

#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <set>
#include <random>
#include <sstream>

#include "util/crypto.hh"
#include "util/fs.hh"
#include "util/log.hh"
#include "util/text.hh"

namespace tsugumi {

namespace stdfs = std::filesystem;

namespace {

constexpr char kMagic[4] = {'T', 'S', 'J', '1'};
// The journal is compacted past this size, keeping the latest receipts (a
// client retrying an operation within them gets its answer again): at most
// this many, and at most this many bytes of them, the newest always, so a
// compacted journal is well under the size and the next compaction waits
// for megabytes of new records (receipts of large saves are large).
constexpr uintmax_t kCompactBytes = 8u << 20;
constexpr size_t kKeepReceipts = 2000;
constexpr size_t kKeepReceiptBytes = kCompactBytes / 4;

bool utf8_boundary(const std::string& s, uint64_t off) {
  if (off == 0 || off == s.size()) return true;
  if (off > s.size()) return false;
  return (static_cast<unsigned char>(s[off]) & 0xC0) != 0x80;
}

std::string def_dir(const std::string& id) { return "definitions/" + id + "/"; }

// What a save may write: the documents and the source areas, never
// .tsugumi/ or a path leaving the project.
bool writable(const stdfs::path& root, const std::string& rel) {
  return fs::valid_relative_path(rel) && (Document::is_document_file(rel) || Document::is_source_area(rel)) &&
         fs::resolves_inside(root, root / rel);
}

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

auto Store::allocate_card_id(const std::string& stem, const std::string& project_path) -> std::string {
  if (!valid_id(stem) || stem.size() > 123)
    throw Failed(FailureKind::invalid_request, "Card stem must be 1..123 ASCII identifier characters");
  // A scope can still be a local draft, so validate its path rather than requiring it on disk.
  auto rest = project_path;
  while (!rest.empty()) {
    if (!rest.starts_with("externals/")) throw Failed(FailureKind::invalid_path, "invalid project_path");
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
  for (std::string line; std::getline(lines, line);) issued.insert(line);
  auto doc = current();
  std::set<std::string> canvas_ids;
  if (project_path.empty()) {
    for (const auto& card : doc->project.canvas->value().cards->value()) canvas_ids.insert(card.id->value());
  } else if (doc->sources.count(project_path + "tsugumi.json")) {
    auto project = parse_external_project(project_path + "tsugumi.json",
                                          read_source(*doc, project_path + "tsugumi.json"));
    for (const auto& card : project.canvas->value().cards->value()) canvas_ids.insert(card.id->value());
  }
  constexpr std::string_view alphabet = "abcdefghjkmnpqrstuvwxyz23456789";
  std::random_device random;
  std::uniform_int_distribution<size_t> pick(0, alphabet.size() - 1);
  for (size_t attempt = 0; attempt < 10000; ++attempt) {
    std::string id = stem + '-';
    for (int i = 0; i < 4; ++i) id += alphabet[pick(random)];
    auto key = project_path + id;
    auto dir = project_path + def_dir(id);
    bool exists = issued.count(key) || canvas_ids.count(id) || (project_path.empty() && doc->definitions.count(id)) ||
                  stdfs::exists(opts_.root / dir) || stdfs::exists(opts_.root / project_path / "externals" / id);
    auto source = doc->sources.lower_bound(dir);
    exists = exists || (source != doc->sources.end() && source->first.starts_with(dir));
    if (exists) continue;
    fs::write_atomic(state_dir() / "card-ids", reservations + key + "\n", 0600);
    return id;
  }
  throw Failed(FailureKind::resource_limit, "cannot allocate a unique Card identifier");
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
  uint64_t rev = 0;
  uint64_t exec_rev = 0;
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

void Store::resolve_staged(Staged& st) const { finalize(st.doc, &st.ops, current().get()); }

DocumentPtr Store::preview(const std::vector<ChangeIn>& changes) const {
  auto st = stage(changes, "preview", "preview");
  return finalize(st.doc, &st.ops, current().get());
}

std::shared_ptr<Document> Store::finalize(std::shared_ptr<Document> doc, const std::vector<FileOp>* pending,
                                          const Document* base) const {
  // Staged documents carry their fingerprint already; a loaded one does not.
  if (doc->execution_fingerprint.empty()) doc->execution_fingerprint = execution_fingerprint(*doc, reader(*doc, pending));
  crypto::Sha256 key;
  key.update(doc->execution_fingerprint);
  for (const auto& [id, def] : doc->definitions) {
    key.update("\0meta\0", 6);
    key.update(id);
    if (def.meta_json) key.update(*def.meta_json);
  }
  for (const auto& [path, src] : doc->sources)
    if (Document::is_card_meta_file(path)) { key.update(path); key.update(src.hash); }
  auto resolution_key = key.finish_hex();
  if (doc->resolution && doc->resolution_key == resolution_key) return doc;  // resolved before commit
  doc->resolution_key = resolution_key;
  if (base && base->resolution && base->resolution_key == doc->resolution_key) {
    doc->resolution = base->resolution;
    return doc;
  }
  doc->resolution = resolve(*doc, reader(*doc, pending));
  return doc;
}

SourceReader Store::reader(const Document& doc, const std::vector<FileOp>* pending) const {
  return [root = opts_.root, &doc, pending](const std::string& rel) -> std::optional<std::string> {
    auto it = doc.sources.find(rel);
    if (it == doc.sources.end()) return std::nullopt;
    if (pending) {
      for (const auto& op : *pending)
        if (op.path == rel) return op.content;
    }
    auto data = fs::read_file(root / rel);
    if (!data || crypto::sha256_hex(*data) != it->second.hash) return std::nullopt;
    return data;
  };
}

// Journal record: magic(4) | len(u32 LE) | crc32(u32 LE) | payload (compact JSON).
void Store::recover_journal() {
  auto path = state_dir() / "document.journal";
  journal_fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_APPEND, 0600);
  if (journal_fd_ < 0) throw fs::Error("open journal: " + std::string(std::strerror(errno)));
  auto data = fs::read_file_or_throw(path);
  size_t pos = 0;
  size_t good = 0;
  struct Txn {
    schema::JournalRecord rec;
    bool committed = false;
  };
  std::vector<Txn> txns;
  std::map<std::string, Receipt> receipts;
  std::deque<std::string> order;
  uint64_t rev = 0;
  uint64_t exec_rev = 0;
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
    uint32_t len;
    uint32_t crc;
    std::memcpy(&len, data.data() + pos + 4, 4);
    std::memcpy(&crc, data.data() + pos + 8, 4);
    if (pos + 12 + len > data.size()) break;
    std::string_view payload(data.data() + pos + 12, len);
    if (crypto::crc32(payload.data(), payload.size()) != crc) break;
    schema::JournalRecord rec;
    if (schema::decode(payload, rec)) break;
    pos += 12 + len;
    good = pos;
    if (*rec.type == "base") {
      // A compacted journal starts from these revisions.
      rev = *rec.rev;
      exec_rev = *rec.execution_rev;
    } else if (*rec.type == "txn") {
      txns.push_back({rec, false});
    } else if (*rec.type == "commit") {
      for (auto& tx : txns)
        if (*tx.rec.rev == *rec.rev) tx.committed = true;
    } else if (*rec.type == "receipt") {
      if (!receipts.count(*rec.op)) order.push_back(*rec.op);
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
    // The journal is a file in the project: a record that writes outside
    // what a save may write was not made here, and is not applied.
    bool foreign = false;
    for (const auto& f : *rec.files) foreign |= !writable(opts_.root, *f.path);
    if (foreign) {
      log::warn("journal: a record of revision " + std::to_string(*rec.rev) +
                " writes outside the project's documents and sources; ignored");
      continue;
    }
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
    if (!receipts.count(*rec.op)) order.push_back(*rec.op);
    receipts[*rec.op] = to_receipt(rec);
  }
  auto doc = std::make_shared<Document>();
  doc->rev = rev;
  doc->execution_rev = exec_rev;
  std::lock_guard<std::mutex> l(mu_);
  doc_ = doc;  // placeholder carrying only the revision numbers
  receipts_ = std::move(receipts);
  receipt_order_ = std::move(order);
}

void Store::remember_receipt(Receipt r) {
  if (!receipts_.count(r.op_id)) receipt_order_.push_back(r.op_id);
  receipts_[r.op_id] = std::move(r);
}

void Store::compact_journal() {
  std::error_code ec;
  auto path = state_dir() / "document.journal";
  if (stdfs::file_size(path, ec) < kCompactBytes || ec) return;
  std::vector<Receipt> keep;
  uint64_t rev = 0;
  uint64_t exec_rev = 0;
  {
    std::lock_guard<std::mutex> l(mu_);
    size_t kept = 0;
    size_t bytes = 0;
    for (auto it = receipt_order_.rbegin(); it != receipt_order_.rend(); ++it, ++kept) {
      const auto& r = receipts_.at(*it);
      size_t size = r.op_id.size() + r.kind.size() + r.digest.size() + r.bytes.size();
      if (kept > 0 && (kept == kKeepReceipts || bytes + size > kKeepReceiptBytes)) break;
      bytes += size;
    }
    while (receipt_order_.size() > kept) {
      receipts_.erase(receipt_order_.front());
      receipt_order_.pop_front();
    }
    for (const auto& id : receipt_order_) keep.push_back(receipts_.at(id));
    if (doc_) {
      rev = doc_->rev;
      exec_rev = doc_->execution_rev;
    }
  }
  auto record = [](const std::string& payload) {
    std::string r(kMagic, 4);
    uint32_t len = static_cast<uint32_t>(payload.size());
    uint32_t crc = crypto::crc32(payload.data(), payload.size());
    r.append(reinterpret_cast<const char*>(&len), 4);
    r.append(reinterpret_cast<const char*>(&crc), 4);
    return r + payload;
  };
  std::string data = record(journal_payload("base", rev, exec_rev, "", "", "", "", {}));
  for (const auto& r : keep) data += record(journal_payload("receipt", rev, exec_rev, r.op_id, r.kind, r.digest, r.bytes, {}));
  // One rename: a crash leaves the old journal or the new one, both complete.
  fs::write_atomic(path, data, 0600);
  int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_APPEND);
  if (fd < 0) {
    halted_ = true;
    throw Failed(FailureKind::persistence_failed, "reopening the compacted journal failed");
  }
  ::close(journal_fd_);
  journal_fd_ = fd;
  log::info("journal: compacted to " + std::to_string(data.size()) + " bytes (" + std::to_string(keep.size()) + " receipts)");
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
    if (!writable(opts_.root, op.path))
      throw Failed(FailureKind::invalid_path, op.path + " is not a document or source path", {{"path", op.path}});
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
    std::vector<std::string> keys;
    switch (c.kind) {
      case K::SetProject: keys = {"project"}; break;
      case K::SetLayout: keys = {"layout"}; break;
      case K::PutDefinition:
      case K::RemoveDefinition: keys = {"def:" + c.id}; break;
      default: keys = {"src:" + c.path}; break;
    }
    for (const auto& key : keys)
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
          if (c.link_json) {
            if (!c.json.empty()) throw DocumentError("invalid_document", "a definition has card_json or link_json, not both");
            d.link = parse_link_json(c.id, *c.link_json).external->value();
            d.link_json = *c.link_json;
          } else {
            d.card = parse_card_json(c.id, c.json);
            d.card_json = c.json;
          }
          if (c.meta_json) {
            check_meta_json(*c.meta_json);
            d.meta_json = *c.meta_json;
          } else if (c.keep_meta) {
            if (auto old = doc->definitions.find(c.id); old != doc->definitions.end() && old->second.meta_json) {
              d.meta_json = old->second.meta_json;
            }
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

  // Removing a Card also removes its layout entries.
  {
    std::set<std::string> on_canvas;
    for (const auto& card : doc->project.canvas->value().cards->value()) on_canvas.insert(card.id->value());
    std::set<std::string> gone;
    for (const auto& card : base->project.canvas->value().cards->value()) {
      const auto& id = card.id->value();
      if (!on_canvas.count(id)) gone.insert(id);
    }
    if (!gone.empty())
      wrap([&] {
        auto followed = layout_without_cards(doc->layout_json, gone);
        if (followed != doc->layout_json) {
          doc->layout = parse_layout_json(followed);
          doc->layout_json = std::move(followed);
        }
      });
  }

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
    bool fresh = old == base->definitions.end();
    bool card_changed = fresh || old->second.card_json != d.card_json || old->second.link_json != d.link_json;
    bool meta_changed = fresh || old->second.meta_json != d.meta_json;
    if (card_changed) {
      if (d.link_json) files[def_dir(id) + "_link.json"] = *d.link_json;
      else files[def_dir(id) + "_card.json"] = d.card_json;
      if (!fresh && d.link_json && !old->second.link_json) files[def_dir(id) + "_card.json"] = std::nullopt;
      if (!fresh && !d.link_json && old->second.link_json) files[def_dir(id) + "_link.json"] = std::nullopt;
    }
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
    files[def_dir(id) + (d.link_json ? "_link.json" : "_card.json")] = std::nullopt;
    if (d.meta_json) files[def_dir(id) + "_meta.json"] = std::nullopt;
  }
  for (const auto& [path, content] : files) {
    if (content && Document::is_card_meta_file(path)) wrap([&] { check_meta_json(*content); });
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
  doc->execution_fingerprint = execution_fingerprint(*doc, reader(*doc, &st.ops));
  st.doc = doc;
  return st;
}

Store::Staged Store::stage_reload(const std::string& op_id, const std::string& origin) const {
  auto base = current();
  std::shared_ptr<Document> doc;
  for (int attempt = 0;; ++attempt) {
    try {
      doc = load_from_disk(opts_.root);
      break;
    } catch (const DocumentError& e) {
      // A file rewritten in place may be read half-written (or with a stale
      // size through a VM's shared folder): read once more before failing.
      if (attempt == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        continue;
      }
      std::vector<std::pair<std::string, std::string>> args(e.args.begin(), e.args.end());
      throw Failed(failure_kind_from_name(e.code).value_or(FailureKind::invalid_document), e.what(), args);
    }
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
  doc->execution_fingerprint = execution_fingerprint(*doc, reader(*doc, &st.ops));
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
  finalize(doc, &st.ops, base.get());
  doc->execution_rev =
      doc->execution_fingerprint != base->execution_fingerprint ? base->execution_rev + 1 : base->execution_rev;
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
  {
    std::lock_guard<std::mutex> l(mu_);
    doc_ = doc;
    remember_receipt(Receipt{st.delta.op_id, kind, digest, receipt});
  }
  compact_journal();
}

void Store::record_receipt(const Receipt& r) {
  auto base = current();
  append_record(journal_payload("receipt", base ? base->rev : 0, base ? base->execution_rev : 0, r.op_id, r.kind,
                                r.digest, r.bytes, {}));
  {
    std::lock_guard<std::mutex> l(mu_);
    remember_receipt(r);
  }
  compact_journal();
}

}  // namespace tsugumi
