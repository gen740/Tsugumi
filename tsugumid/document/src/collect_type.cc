#include "tsugumi/document/collect_type.hh"

#include <stdexcept>

namespace tsugumi {

namespace {

constexpr const char* kDirectorySchema = R"(namespace tsugumi.collect;
table Entry {
  name:string (id: 0);
  path:string (id: 1);
}
table Directory {
  value:string (id: 0);
  items:[Entry] (id: 1);
}
)";

// The message of a collected File / Directory list (compiled once).
auto directory_type() -> types::TypeRef {
  static const types::TypeRef type = [] {
    std::string error;
    auto compiled =
        types::compile(kDirectorySchema, "tsugumi.collect.Directory", &error);
    if (!compiled)
      throw std::logic_error("collect Directory type failed: " + error);
    return *compiled;
  }();
  return type;
}

}  // namespace

auto collect_output_type(const types::PortType& source, std::string& error)
    -> types::TypeRef {
  if (!source.type) {
    error = "the collected input " + source.name + " has no resolved type";
    return nullptr;
  }
  if (source.kind == types::PortKind::FlatBuffer) {
    std::string text;
    const auto member = types::render_closure(
        *source.type, "tsugumi.collect.member", text, /*nested=*/true);
    text += "namespace tsugumi.collect;\ntable Collected {\n  items:[" +
            member + "] (id: 0, required);\n}\n";
    auto type = types::compile(text, "tsugumi.collect.Collected", &error);
    return type ? *type : nullptr;
  }
  if (!types::has_string_value(*source.type)) {
    error = "the collected File/Directory input " + source.name +
            " needs a string `value` field";
    return nullptr;
  }
  try {
    return directory_type();
  } catch (const std::exception& e) {
    error = e.what();
    return nullptr;
  }
}

}  // namespace tsugumi
