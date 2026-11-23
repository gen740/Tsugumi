#pragma once

#include <functional>
#include <optional>
#include <string>

namespace flatbuffers {
class Parser;
}

namespace tsugumi::types {
using SchemaSourceReader =
    std::function<std::optional<std::string>(const std::string&)>;

// Read includes from the confirmed definition files and the bundled Artifact
// schema. The parser never opens files outside this scope.
auto parse_schema(flatbuffers::Parser& parser, const std::string& text,
                  const std::string& filename, const SchemaSourceReader& read)
    -> bool;

}  // namespace tsugumi::types
