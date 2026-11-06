// Owns the confirmed document, its journal and save/reload receipts.
// Every public mutating member must be called from the mutation lane only.
#pragma once

#include <filesystem>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "document.hh"
#include "graph.hh"
#include "model.hh"

namespace tsugumi {

struct SourcePatchIn {
  uint64_t offset = 0;
  uint64_t remove = 0;
  std::string text;
};

struct ChangeIn {
  enum class Kind { SetProject, SetLayout, PutDefinition, RemoveDefinition, RenameDefinition, PutSource, PatchSource, RemoveSource };
  Kind kind;
  std::string id;    // definitions
  std::string to;    // RenameDefinition
  std::optional<std::string> link_json;
  std::string json;  // SetProject / SetLayout / card_json
  std::optional<std::string> meta_json;
  bool keep_meta = false;  // PutDefinition without meta_json keeps the current _meta.json
  // With keep_meta: what fills the _meta.json's fields that are empty.
  std::optional<std::string> fill_description;
  std::optional<std::string> fill_context;
  std::string path;   // sources
  std::string bytes;  // PutSource
  std::optional<std::string> base_hash;
  std::vector<SourcePatchIn> patches;
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
  std::vector<std::pair<std::string, std::string>> renamed;  // definition id: from -> to
  DocumentPtr doc;
};

struct Receipt {
  std::string op_id;
  std::string kind;    // "save", "reload", "import", "run", "cache_clear", "sdk"
  std::string digest;  // canonical request digest
  std::string bytes;   // OperationReceipt FlatBuffer (root)
};

struct StoreOptions {
  std::filesystem::path root;
  // Fault injection for tests ("" disables): after_journal, partial_apply, before_commit.
  std::string fault;
};

class Store {
 public:
  explicit Store(StoreOptions opts);

  // Recovers the journal and loads the document. Throws on failure.
  void open();

  DocumentPtr current() const;
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
  Staged stage_reload(const std::string& op_id, const std::string& origin) const;
  // The document as it would be after `changes`, resolved (nothing written).
  DocumentPtr preview(const std::vector<ChangeIn>& changes) const;
  // Resolves a staged document now (commit then reuses it).
  void resolve_staged(Staged& staged) const;
  // Reads the files of `doc`, with the writes of a staged save still `pending`;
  // nullopt for a file it does not have or whose bytes on disk are not its own.
  // Keep `doc` and `pending` alive while using it.
  SourceReader reader(const Document& doc, const std::vector<FileOp>* pending = nullptr) const;

  // Persists a staged transaction atomically with its receipt. The
  // `make_receipt` callback receives the delta (with final revision numbers)
  // and returns the OperationReceipt bytes. Publishes the new document.
  void commit(Staged& staged, const std::string& kind, const std::string& digest,
              const std::function<std::string(const Delta*)>& make_receipt);
  // Stores a terminal receipt without changing the document.
  void record_receipt(const Receipt& r);

  // Reads a confirmed source; throws Failed(external_change) on mismatch.
  std::string read_source(const Document& doc, const std::string& rel) const;

  // Snapshot + view storage helpers.
  std::filesystem::path state_dir() const { return opts_.root / ".tsugumi"; }

 private:
  void recover_journal();
  void append_record(const std::string& payload);
  void remember_receipt(Receipt r);  // under mu_
  // Rewrites the journal as the revisions and the newest receipts once it is
  // large (committed transactions are in the files already).
  void compact_journal();
  void apply_ops(const std::vector<FileOp>& ops, bool inject);
  // Resolves the document. `pending` holds the transaction's file writes,
  // which are read instead of the disk (they are applied after resolution).
  // Resolves the document (fingerprint, graph and types), reusing `base`'s
  // resolution when nothing it follows changed.
  std::shared_ptr<Document> finalize(std::shared_ptr<Document> doc, const std::vector<FileOp>* pending = nullptr,
                                     const Document* base = nullptr) const;
  void check_disk_unchanged(const Document& base, const std::vector<FileOp>& ops) const;
  void fault(const char* point) const;

  StoreOptions opts_;
  std::string project_id_;
  mutable std::mutex mu_;
  DocumentPtr doc_;
  std::map<std::string, Receipt> receipts_;
  std::deque<std::string> receipt_order_;  // op ids, oldest first (the compaction keeps the newest)
  int journal_fd_ = -1;
  bool halted_ = false;
};

// Size limits from the protocol.
constexpr size_t kMaxDocumentJsonBytes = 8u << 20;
constexpr size_t kMaxManifestEntries = 10000;
constexpr size_t kMaxFrameBytes = 16u << 20;

}  // namespace tsugumi
