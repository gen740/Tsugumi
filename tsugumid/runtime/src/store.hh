// Owns the confirmed document, its pending file writes and save/reload
// receipts. Every public mutating member must be called from the mutation lane
// only.
#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "model.hh"
#include "tsugumi/document/document.hh"
#include "tsugumi/document/graph.hh"
#include "tsugumi/util/sqlite.hh"

namespace tsugumi {

struct SourcePatchIn {
  uint64_t offset = 0;
  uint64_t remove = 0;
  std::string text;
};

struct ChangeIn {
  enum class Kind {
    SetProject,
    SetLayout,
    PutDefinition,
    RemoveDefinition,
    PutSource,
    PatchSource,
    RemoveSource
  };
  Kind kind;
  std::string id;    // definitions
  std::string json;  // SetProject / card_json; SetLayout's layout.bin bytes
  // PutDefinition: a README.md or _comments.json not given is removed, unless
  // its keep flag keeps the current one.
  std::optional<std::string> readme;
  bool keep_readme = false;
  std::optional<std::string> comments_json;
  bool keep_comments = false;
  std::string path;   // sources
  std::string bytes;  // PutSource
  std::optional<std::string> base_hash;
  std::vector<SourcePatchIn> patches;
  std::shared_ptr<SourceFile> file;
};

// What changed in a committed transaction. Content comes from `doc`.
struct Delta {
  uint64_t base_rev = 0;
  uint64_t rev = 0;
  uint64_t execution_rev = 0;
  std::string op_id;
  std::string origin;
  bool project_changed = false;
  bool layout_changed = false;
  std::vector<std::string> definitions_upsert;
  std::vector<std::string> definitions_removed;
  std::vector<std::string> sources_upsert;
  std::vector<std::string> sources_removed;
  DocumentPtr doc;
};

struct Receipt {
  std::string op_id;
  std::string kind;    // "save", "reload", "run", "cache_clear", "sdk"
  std::string digest;  // canonical request digest
  std::string bytes;   // OperationReceipt FlatBuffer (root)
};

struct StoreOptions {
  std::filesystem::path root;
  // Fault injection for tests ("" disables): after_prepare, partial_apply,
  // before_commit.
  std::string fault;
};

class Store {
 public:
  explicit Store(StoreOptions opts);
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  // Recovers pending writes and loads the document. Throws on failure.
  void open();

  DocumentPtr current() const;
  auto allocate_card_id(const std::string& stem,
                        const std::string& project_path) -> std::string;
  const std::filesystem::path& root() const { return opts_.root; }
  std::string project_id() const { return project_id_; }

  std::optional<Receipt> receipt(const std::string& op_id) const;
  bool halted() const { return halted_; }

  // Builds a new document from `changes` (validation only, nothing written).
  struct Staged {
    std::shared_ptr<Document> doc;
    std::vector<FileOp> ops;
    Delta delta;
    bool effective = false;
  };
  Staged stage(const std::vector<ChangeIn>& changes, const std::string& op_id,
               const std::string& origin) const;
  auto stage_file(const std::filesystem::path& root, const std::string& from,
                  const std::optional<std::string>& expected_hash,
                  const std::function<void()>& check_cancelled) const
      -> std::shared_ptr<SourceFile>;
  Staged stage_reload(const std::string& op_id, const std::string& origin,
                      const std::filesystem::path& checkout = {}) const;
  // The document as it would be after `changes`, resolved (nothing written).
  DocumentPtr preview(const std::vector<ChangeIn>& changes) const;
  // Resolves a staged document now (commit then reuses it).
  void resolve_staged(Staged& staged) const;
  // Resolves and assigns revisions before checking the encoded response.
  void prepare_delta(Staged& staged) const;

  // Persists a staged transaction atomically with its receipt. The
  // `make_receipt` callback receives the delta (with final revision numbers)
  // and returns the OperationReceipt bytes. Publishes the new document.
  void commit(
      Staged& staged, const std::string& kind, const std::string& digest,
      const std::function<std::string(const Delta*)>& make_receipt,
      const std::function<void()>& check_cancelled = [] {});
  // Stores a terminal receipt without changing the document.
  void record_receipt(const Receipt& r);

  // Document sources come from the revision. Other source bytes are verified
  // against it.
  std::string read_source(const Document& doc, const std::string& rel) const;

  // Snapshot + view storage helpers.
  std::filesystem::path state_dir() const { return opts_.root / ".tsugumi"; }

 private:
  auto recover_pending() -> void;
  auto persist_pending(const Document& doc, const Receipt& receipt,
                       const std::vector<FileOp>& ops) -> void;
  auto finish_pending() -> void;  // under mu_ once the Store is open
  void apply_ops(const std::vector<FileOp>& ops, bool inject);
  // Resolves the document. `pending` holds the transaction's file writes,
  // which are read instead of the disk (they are applied after resolution).
  // Resolves the document (fingerprint, graph and types), reusing `base`'s
  // resolution when nothing it follows changed.
  std::shared_ptr<Document> finalize(
      std::shared_ptr<Document> doc,
      const std::vector<FileOp>* pending = nullptr,
      const Document* base = nullptr) const;
  void check_disk_unchanged(const Document& base,
                            const std::vector<FileOp>& ops) const;
  void fault(const char* point) const;

  StoreOptions opts_;
  std::string project_id_;
  mutable std::mutex mu_;
  DocumentPtr doc_;
  std::unique_ptr<sql::Db> db_;  // serialized by mu_
  bool halted_ = false;
};

// Size limits from the protocol.
constexpr size_t kMaxManifestEntries = 10000;
constexpr size_t kMaxFrameBytes = 16u << 20;

}  // namespace tsugumi
