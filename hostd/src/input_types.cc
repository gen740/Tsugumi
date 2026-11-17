#include "host/input_types.hh"

#include "input.hh"
#include "tsugumi/document/types.hh"

namespace tsugumi::host {
namespace H = Tsugumi::Host;
namespace R = Tsugumi::RPC;
auto InputTypes(const R::InputTypesResult& data,
                const std::optional<std::string>& language) -> H::InputTypesT {
  const auto* resolved = &data;
  H::InputTypesT result;
  result.rev = resolved->rev();
  result.language = language               ? *language
                    : resolved->language() ? resolved->language()->str()
                                           : "";
  if (resolved->inputs())
    for (const auto* input : *resolved->inputs()) {
      auto item = std::make_unique<H::InputTypeT>();
      item->port = input->port()->str();
      if (input->source_card())
        item->source_card.reset(input->source_card()->UnPack());
      if (input->source_port()) item->source_port = input->source_port()->str();
      item->state = input->source_card() ? "unresolved" : "no_source";
      if (const auto* port = input->type()) {
        item->kind = port->kind() == Tsugumi::Data::PortKind::File ? "file"
                     : port->kind() == Tsugumi::Data::PortKind::Directory
                         ? "directory"
                         : "value";
        if (port->schema_bfbs() && port->root_type()) {
          types::Type type;
          type.bfbs.assign(
              reinterpret_cast<const char*>(port->schema_bfbs()->data()),
              port->schema_bfbs()->size());
          type.root_type = port->root_type()->str();
          std::string schema;
          // Named inputs get exactly the namespace NixRunner::generate uses.
          // A Draft has no final input name yet, so keep the producer
          // namespace.
          const auto root = types::render_closure(
              type, item->port.empty() ? "" : "inputs." + item->port, schema);
          schema += "root_type " + root + ";\n";
          item->definition = sdk::DescribeInput(schema, result.language);
          item->state = "resolved";
        }
      }
      result.inputs.push_back(std::move(item));
    }
  return result;
}
}  // namespace tsugumi::host
