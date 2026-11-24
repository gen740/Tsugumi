#pragma once

#include <string>

namespace tsugumi {
namespace types {
struct Type;
}

// SHA-256 (lowercase hex) of the canonical reflection schema rooted at the
// port's declared type. Both CardPort.schema and _inputs.fbs use this ID.
auto schema_id(const types::Type& type) -> std::string;

}  // namespace tsugumi
