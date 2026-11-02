// The confirmed project document. A Document is immutable once published; the
// mutation lane builds a new one for every committed transaction.
//
// The canonical text of each document file is the validated JSON as accepted
// (from the client or from disk). The typed model is decoded from it and is
// used for every structural and semantic check.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "schema.hh"

namespace tsugumi {

using CardPath = std::vector<std::string>;
std::string path_key(const CardPath& p);  // "a/b"
CardPath path_from_key(const std::string& key);
std::string path_display(const CardPath& p);

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
};

struct DefinitionDoc {
  schema::CardDefinition card;
  std::string card_json;
  std::optional<schema::CardMeta> meta;
  std::optional<std::string> meta_json;
};

struct Resolution;  // graph.hh

struct Document {
  uint64_t rev = 0;
  uint64_t execution_rev = 0;
  std::string execution_fingerprint;
  schema::Project project;
  std::string project_json;
  schema::Layout layout;
  std::string layout_json;
  std::map<std::string, DefinitionDoc> definitions;
  std::map<std::string, SourceEntry> sources;  // relative path -> entry
  // Hash of the bytes on disk for every document file we wrote or read.
  std::map<std::string, std::string> disk_hashes;
  std::shared_ptr<const Resolution> resolution;

  static bool is_document_file(const std::string& rel);
  static bool is_source_area(const std::string& rel);

  const std::string& pkgs_path() const { return project.pkgs_path->value(); }
  const std::vector<schema::DatabaseConnection>& database_connections() const {
    return project.database_connections->value();
  }
};
using DocumentPtr = std::shared_ptr<const Document>;

bool valid_id(const std::string& id);

class DocumentError : public std::runtime_error {
 public:
  DocumentError(std::string code, const std::string& message,
                std::map<std::string, std::string> args = {})
      : std::runtime_error(message), code(std::move(code)), args(std::move(args)) {}
  std::string code;  // FailureKind name
  std::map<std::string, std::string> args;
};

// Strictly decodes and shape-checks one document part. Throws
// DocumentError(invalid_document) with the byte offset on errors.
schema::Project parse_project_json(const std::string& text);
schema::Layout parse_layout_json(const std::string& text);
schema::CardDefinition parse_card_json(const std::string& id, const std::string& text);
schema::CardMeta parse_meta_json(const std::string& text);
// Decodes the inline graph of a subgraph Card (already shape-checked).
schema::Graph inner_graph(const schema::CardInstance& card);
// Decodes an external graph file (graphs/...). Throws DocumentError.
schema::Graph parse_graph_file(const std::string& rel, const std::string& text);

// Hard structural checks over the whole (final) document: unique ids, known
// definitions, connections between existing cards, subgraph shapes.
void check_structure(const Document& doc);

// Loads the document from disk (startup / Reload). Throws DocumentError.
std::shared_ptr<Document> load_from_disk(const std::filesystem::path& root);

// Fingerprint of everything that influences computation (not layout).
std::string execution_fingerprint(const Document& doc);

constexpr const char* kEmptyLayout = "{}";

// One file system operation produced by a transaction.
struct FileOp {
  std::string path;                    // project relative
  std::optional<std::string> content;  // nullopt = delete
};

}  // namespace tsugumi
