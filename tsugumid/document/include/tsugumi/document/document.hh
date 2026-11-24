// The confirmed project document. A Document is immutable once published; the
// mutation lane builds a new one for every committed transaction.
//
// tsugumi.json, _card.json and _comments.json are JSON text read with the
// FlatBuffers parser against protocol/*_document.fbs; the text kept and saved
// is the canonical text printed from the parsed buffer. layout.bin is a
// FlatBuffers binary the UIs own. Every check reads the parsed buffers.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "tsugumi/document/definition_values.hh"
#include "tsugumi/document/text.hh"

namespace tsugumi {

std::string path_key(const CardPath& p);  // "a/b"
CardPath path_from_key(const std::string& key);

struct Diag {
  bool error = true;
  std::string code;
  std::string message;
  std::optional<CardPath> card;
  std::optional<std::string> path;
  std::optional<uint64_t> offset;
};

struct SourceEntry {
  std::string hash;
  uint64_t size = 0;
  // Bytes of a file carried in snapshots and deltas: a nested project's
  // documents, and the canvas notes (notes/<id>.md).
  std::shared_ptr<const std::string> document = nullptr;
};

inline constexpr size_t kMaxDocumentJsonBytes = 8u << 20;

auto external_document_source(const std::string& path, std::string bytes,
                              bool canonicalize = false)
    -> SourceEntry;
// A canvas note (notes/<id>.md), carried with its text.
auto note_source(const std::string& path, std::string bytes) -> SourceEntry;

inline constexpr uint64_t kDefaultLogLimitBytes = uint64_t{128} << 20;
inline constexpr uint64_t kMaxLogLimitBytes = uint64_t{1} << 30;
inline constexpr uint64_t kDefaultOutputLimitBytes = uint64_t{64} << 30;
inline constexpr uint64_t kDefaultOutputLimitEntries = 1000;

// definitions/<id>/: _card.json, and the optional README.md and
// _comments.json.
struct DefinitionDoc {
  std::string card_buffer;  // _card.json as a FlatBuffer
  std::string card_json;    // its canonical text
  std::optional<std::string> readme;
  std::optional<std::string> comments_json;  // canonical text
  auto card() const -> const Tsugumi::Document::Card& {
    return document_text::root<Tsugumi::Document::Card>(card_buffer);
  }
};

struct Resolution;  // graph.hh

struct Document {
  uint64_t rev = 0;
  uint64_t execution_rev = 0;
  std::string execution_fingerprint;
  std::string project_buffer;  // tsugumi.json as a FlatBuffer
  std::string project_json;    // its canonical text
  std::string layout;          // layout.bin; empty when there is none
  std::map<std::string, DefinitionDoc> definitions;
  std::map<std::string, SourceEntry> sources;  // relative path -> entry
  // Hash of the bytes on disk for every document file we wrote or read.
  std::map<std::string, std::string> disk_hashes;
  std::shared_ptr<const Resolution> resolution;
  // What the resolution follows: the execution fingerprint. Equal keys,
  // equal resolutions (metadata and layout changes keep the previous one).
  std::string resolution_key;

  auto project() const -> const Tsugumi::Document::Project& {
    return document_text::root<Tsugumi::Document::Project>(project_buffer);
  }

  // A Card's README.md or _comments.json: never part of execution.
  static auto is_card_note_file(const std::string& rel) -> bool;
  // notes/<id>.md: a canvas note's Markdown.
  static auto is_generated_input_file(const std::string& path) -> bool;
  static auto is_note_file(const std::string& rel) -> bool;
  static bool is_document_file(const std::string& rel);
  static auto is_external_document_file(std::string_view path) -> bool;
  static auto is_lock_file(std::string_view path) -> bool;
  auto check_document_size() const -> void;
  enum class SourcePath { Invalid, Protected, Source };
  static auto classify_source_path(const std::string& rel) -> SourcePath;
  static bool is_source_area(const std::string& rel);
  // Version opt-ins never name internal state or Git metadata, at any depth.
  static auto valid_version_path(const std::string& rel) -> bool;
};
using DocumentPtr = std::shared_ptr<const Document>;

bool valid_id(const std::string& id);
// The one rule for Card ids, port names (inputs, outputs, boundary ports) and
// Card secret names: [a-z][a-z0-9_]{0,63}. Ids name folders (APFS is case
// insensitive; a leading '.' hides a folder), port names become FlatBuffers
// namespaces and Nix attribute names, and lower-case secret names never collide
// with PATH, HOME, LANG or the TSUGUMI_* variables a worker sets.
bool valid_name(std::string_view name);

// This Tsugumi's version (VERSION): written into every tsugumi.json it creates
// or saves, and required of every tsugumi.json it loads.
extern const char* const kTsugumiVersion;

class DocumentError : public std::runtime_error {
 public:
  DocumentError(std::string code, const std::string& message,
                std::map<std::string, std::string> args = {})
      : std::runtime_error(message),
        code(std::move(code)),
        args(std::move(args)) {}
  std::string code;  // FailureKind name
  std::map<std::string, std::string> args;
};

// The initial tsugumi.json, available before a project daemon exists. It
// writes every value (there are no built-in defaults); agent_exclude is the
// host's project default.
auto initial_project_json(const std::string& name,
                          const std::vector<std::string>& agent_exclude)
    -> std::string;

// A parsed document: its buffer and its canonical text.
struct ParsedDocument {
  std::string buffer;
  std::string text;
};

// Parses and checks one document part. Throws DocumentError(invalid_document)
// naming the file, line and column on errors.
auto parse_project_json(const std::string& text) -> ParsedDocument;
auto parse_project_lock(const std::string& path, const std::string& text)
    -> ParsedDocument;
auto valid_nix_path_name(std::string_view name) -> bool;
auto parse_card_json(const std::string& id, const std::string& text)
    -> ParsedDocument;
auto parse_comments_json(const std::string& rel, const std::string& text)
    -> ParsedDocument;
// Checks a README.md (see readme.hh).
void check_readme(const std::string& rel, const std::string& text);
// Verifies a layout.bin.
void check_layout(const std::string& rel, const std::string& bytes);
// The tsugumi.json of an external project (externals/.../tsugumi.json).
auto parse_external_project(const std::string& rel, const std::string& text)
    -> ParsedDocument;

// Hard structural checks over the whole (final) document: unique ids, a
// definition for every card, connections between existing cards.
void check_structure(const Document& doc);

// Loads the document from disk (startup / Reload). Throws DocumentError.
std::shared_ptr<Document> load_from_disk(const std::filesystem::path& root);

// Fingerprint of everything that influences computation (not layout).
std::string execution_fingerprint(const Document& doc);

// One file system operation produced by a transaction.
// A private streamed file. Failed staging removes it; after a durable journal
// intent it must survive until recovery or the durable commit marker.
struct SourceFile {
  std::filesystem::path path;
  std::string hash;
  uint64_t size = 0;
  bool journaled = false;
  ~SourceFile();
};

struct FileOp {
  std::string path;  // project relative
  std::optional<std::string> content;
  std::shared_ptr<SourceFile> file = {};  // neither content nor file = delete
};

}  // namespace tsugumi
