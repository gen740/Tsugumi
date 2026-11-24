// Types resolved from the project's port declarations and wires.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tsugumi/document/schema_sources.hh"
#include "tsugumi/protocol/tsugumi_enums_generated.h"

namespace reflection {
struct Schema;
struct Object;
}  // namespace reflection

namespace tsugumi::types {


struct Type {
  std::string bfbs;       // reflection schema, root_table = root message
  std::string root_type;  // fully qualified name
  std::string
      hash;  // sha256 of bfbs, set where the type is made (computed once)
  const reflection::Schema* schema() const;
  const reflection::Object* root() const;
};
using TypeRef = std::shared_ptr<const Type>;

struct PortType {
  std::string name;
  bool required = false;  // output catalog attribute only
  TypeRef type;           // null when unresolved
  std::vector<std::string> merge_member_types{};  // presentation only

  auto resolved() const -> bool { return type != nullptr; }
};

struct CatalogResult {
  std::vector<PortType> ports;
  std::vector<std::string> errors;  // parse/shape errors, user readable
};

// Parses an _outputs.fbs text. The root table is the port catalog; each field
// is one port carrying a table.
CatalogResult parse_catalog(const std::string& fbs_text,
                            const std::string& filename,
                            const SchemaSourceReader& read = {});

// The whole schema of an _outputs.fbs text, rooted at its catalog (`root`).
std::optional<std::string> schema_bfbs(const std::string& fbs_text,
                                       std::string* root, std::string* error);

// The text with one catalog field (port) set to `type_name` (a table of the
// schema, fully qualified or not). Only that field's
// declaration changes.
std::optional<std::string> set_catalog_port(const std::string& fbs_text,
                                            const std::string& port,
                                            const std::string& type_name,
                                            std::string* error);

// Compiles a schema text and returns the type rooted at `root`.
std::optional<TypeRef> compile(const std::string& fbs_text,
                               const std::string& root, std::string* error);

// Renders the closure of `t` into fbs declarations under namespace `prefix`
// and returns the new fully qualified root name.
std::string render_closure(const Type& t, const std::string& prefix,
                           std::string& out);

// Builtin message types.
TypeRef empty_type();

}  // namespace tsugumi::types
