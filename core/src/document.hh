// The confirmed project document. A Document is immutable once published; the
// mutation lane builds a new one for every committed transaction.
//
// The canonical text of each document file is the validated JSON as accepted
// (from the client or from disk). The typed model is decoded from it and is
// used for every structural and semantic check.
#pragma once

#include <cstdint>
#include <functional>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "schema.hh"

namespace tsugumi {

using CardPath = std::vector<std::string>;
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
};

// definitions/<id>/: the settings of one Card (_card.json), or, for a Card in
// Card, the link to its external project (_link.json); never both.
struct DefinitionDoc {
  schema::CardDefinition card;
  std::string card_json;  // empty for a link
  std::optional<std::string> meta_json;
  std::optional<std::string> link;  // externals/<link>/
  std::optional<std::string> link_json;
};

struct Resolution;  // graph.hh

struct Document {
  uint64_t rev = 0;
  uint64_t execution_rev = 0;
  std::string execution_fingerprint;
  schema::Project project;
  std::string project_json;
  std::string layout_json;
  std::map<std::string, DefinitionDoc> definitions;
  std::map<std::string, SourceEntry> sources;  // relative path -> entry
  // Hash of the bytes on disk for every document file we wrote or read.
  std::map<std::string, std::string> disk_hashes;
  std::shared_ptr<const Resolution> resolution;
  // What the resolution follows: the execution fingerprint and every
  // definition's _meta.json (a Draft's planned ports). Equal keys, equal
  // resolutions (a layout-only change keeps the previous one).
  std::string resolution_key;

  static auto is_card_meta_file(const std::string& rel) -> bool;
  static bool is_document_file(const std::string& rel);
  static bool is_source_area(const std::string& rel);
  // Version opt-ins never name internal state or Git metadata, at any depth.
  static auto valid_version_path(const std::string& rel) -> bool;

  const std::string& pkgs_path() const { return project.pkgs_path->value(); }
  bool save_run_history() const { return project.save_run_history->value_or(true); }
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
auto check_opaque_layout(const std::string& text) -> void;
schema::CardDefinition parse_card_json(const std::string& id, const std::string& text);
void check_meta_json(const std::string& text);
schema::CardLink parse_link_json(const std::string& id, const std::string& text);
// The tsugumi.json of an external project (externals/.../tsugumi.json).
schema::Project parse_external_project(const std::string& rel, const std::string& text);

// Hard structural checks over the whole (final) document: unique ids, a
// definition for every card, connections between existing cards.
void check_structure(const Document& doc);

// Loads the document from disk (startup / Reload). Throws DocumentError.
std::shared_ptr<Document> load_from_disk(const std::filesystem::path& root);

// Fingerprint of everything that influences computation (not layout).
std::string execution_fingerprint(const Document& doc, const std::function<std::optional<std::string>(const std::string&)>& read);

constexpr const char* kEmptyLayout = "{}";

// One file system operation produced by a transaction.
struct FileOp {
  std::string path;                    // project relative
  std::optional<std::string> content;  // nullopt = delete
};

}  // namespace tsugumi
