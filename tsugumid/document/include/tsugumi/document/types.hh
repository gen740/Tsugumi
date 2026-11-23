// Types resolved from the project's port declarations and wires.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tsugumi/protocol/tsugumi_enums_generated.h"

namespace reflection {
struct Schema;
struct Object;
}  // namespace reflection

namespace tsugumi::types {

using PortKind = Tsugumi::Data::PortKind;
using ExecutionShape = Tsugumi::Data::ExecutionShape;

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
  PortKind kind = PortKind::FlatBuffer;
  bool required = false;  // output catalog attribute only
  TypeRef type;           // null when unresolved
  ExecutionShape execution_shape = ExecutionShape::unspecified;
  std::vector<std::string> merge_member_types{};  // presentation only

  auto resolved() const -> bool { return kind == PortKind::Path || type; }
};

struct CatalogResult {
  std::vector<PortType> ports;
  std::vector<std::string> errors;  // parse/shape errors, user readable
};

// Parses an _outputs.fbs text. The root table is the port catalog; each field
// is one port carrying a table or an annotated Path string.
CatalogResult parse_catalog(const std::string& fbs_text,
                            const std::string& filename);

// The whole schema of an _outputs.fbs text, rooted at its catalog (`root`).
std::optional<std::string> schema_bfbs(const std::string& fbs_text,
                                       std::string* root, std::string* error);

// The text with one catalog field (port) set to `type_name` (a table of the
// schema, fully qualified or not) or to a Path string. Only that field's
// declaration changes.
std::optional<std::string> set_catalog_port(const std::string& fbs_text,
                                            const std::string& port,
                                            const std::string& type_name,
                                            PortKind kind, std::string* error);

// Compiles a schema text and returns the type rooted at `root`.
std::optional<TypeRef> compile(const std::string& fbs_text,
                               const std::string& root, std::string* error);

// Renders the closure of `t` into fbs declarations under namespace `prefix`
// and returns the new fully qualified root name.
std::string render_closure(const Type& t, const std::string& prefix,
                           std::string& out, bool nested = false);

// A catalog of the message types used to generate a Card's SDK bindings.
auto card_catalog(const std::vector<PortType>& ports, bool inputs)
    -> std::string;

// Builtin message types.
TypeRef empty_type();

// Builds a FlatBuffer of `t` from JSON text. Returns false with error text.
bool json_to_flatbuffer(const Type& t, const std::string& json,
                        std::string* out, std::string* error,
                        bool force_defaults = false);
// Generates a Params schema text (`table Params {...}`) for the given
// name -> fbs type list.
std::string params_schema(
    const std::vector<std::pair<std::string, std::string>>& fields);

}  // namespace tsugumi::types
