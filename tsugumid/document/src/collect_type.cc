#include "tsugumi/document/collect_type.hh"

#include <stdexcept>

namespace tsugumi {

namespace {

// Opaque envelopes, the Card ABI's own CardPort: the core never reflects or
// rebuilds member types.
constexpr const char* kCollectedSchema = R"(namespace Tsugumi.Data;
enum PortKind : ubyte { unspecified = 0, Path = 1, FlatBuffer = 2 }
namespace Tsugumi.Card;
table CardPort {
  name:string (id: 0, required);
  kind:Tsugumi.Data.PortKind (id: 1);
  bytes:[ubyte] (id: 2);
  path:string (id: 3);
}
namespace tsugumi.builtin;
table Collected {
  items:[Tsugumi.Card.CardPort] (id: 0);
}
)";

auto collected_type() -> types::TypeRef {
  static const types::TypeRef type = [] {
    std::string error;
    auto compiled =
        types::compile(kCollectedSchema, "tsugumi.builtin.Collected", &error);
    if (!compiled)
      throw std::logic_error("Collected type failed: " + error);
    return *compiled;
  }();
  return type;
}

}  // namespace

auto collect_output_type(std::string& error) -> types::TypeRef {
  try {
    return collected_type();
  } catch (const std::exception& e) {
    error = e.what();
    return nullptr;
  }
}

}  // namespace tsugumi
