// Port types. Every resolved port carries a self contained BFBS whose root is
// the message carried on that port. Compatibility is structural (field names,
// ids, types, presence rules and defaults compared recursively), never by
// comparing hashes or raw BFBS bytes.
#pragma once

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

struct Type {
  std::string bfbs;       // reflection schema, root_table = root message
  std::string root_type;  // fully qualified name
  std::string hash;       // sha256 of bfbs, set where the type is made (computed once)
  const reflection::Schema* schema() const;
  const reflection::Object* root() const;
};
using TypeRef = std::shared_ptr<const Type>;

struct PortType {
  std::string name;
  PortKind kind = PortKind::FlatBuffer;
  bool optional = false;
  TypeRef type;  // null when unresolved
};

struct CatalogResult {
  std::vector<PortType> ports;
  std::vector<std::string> errors;  // parse/shape errors, user readable
};

// Parses an _outputs.fbs text. The root table is the port catalog; each field
// is one port carrying the field's table type. `tsugumi:"file"` /
// `tsugumi:"directory"` on a catalog field marks File/Directory ports, whose
// message must have a string `value` field.
CatalogResult parse_catalog(const std::string& fbs_text, const std::string& filename);

// Compiles a schema text and returns the type rooted at `root`.
std::optional<TypeRef> compile(const std::string& fbs_text, const std::string& root,
                               std::string* error);

bool compatible(const Type& a, const Type& b);

// Renders the closure of `t` into fbs declarations under namespace `prefix`
// and returns the new fully qualified root name.
std::string render_closure(const Type& t, const std::string& prefix, std::string& out, bool nested = false);

// Builtin message types.
TypeRef empty_type();
TypeRef bool_condition_type();  // table Condition { value:bool; }

// Union wrapper used by merge: `table Merged { value:In; }` with one union
// member per input port name. Returns nullopt with an error on failure.
std::optional<TypeRef> merged_type(const std::vector<std::pair<std::string, TypeRef>>& inputs,
                                   std::string* error);

// True when the message has a `value:bool` field (branch condition).
bool is_condition(const Type& t);
// True when the message has a string `value` field (File/Directory payload).
bool has_string_value(const Type& t);

// Builds a FlatBuffer of `t` from JSON text. Returns false with error text.
bool json_to_flatbuffer(const Type& t, const std::string& json, std::string* out,
                        std::string* error);
// Renders a FlatBuffer of `t` as JSON (strict, verified first).
bool flatbuffer_to_json(const Type& t, const std::string& bytes, std::string* out,
                        std::string* error);
// Builds a buffer of `merged` (a table whose `value` is a union with one
// member per input port) holding the table `bytes` of `member` as the member
// `port`, by copying the table directly (no JSON round trip). `bytes` must
// already be verified against `member` (values are verified when produced).
std::optional<std::string> wrap_in_union(const Type& merged, const std::string& port, const Type& member,
                                         const std::string& bytes, std::string* error);
// Verifies `bytes` as a buffer of `t`.
bool verify(const Type& t, const std::string& bytes);

// Generates a Params schema text (`table Params {...}`) for the given
// name -> fbs type list.
std::string params_schema(const std::vector<std::pair<std::string, std::string>>& fields);

}  // namespace tsugumi::types
