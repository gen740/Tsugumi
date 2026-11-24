#include "tsugumi/document/collect_type.hh"

#include <stdexcept>

extern const unsigned char tsugumi_card_fbs[];
extern const unsigned long tsugumi_card_fbs_size;

namespace tsugumi {

auto collect_schema_declarations() -> std::string_view {
  constexpr std::string_view kNamespace = "namespace tsugumi.builtin;";
  const std::string_view card(reinterpret_cast<const char*>(tsugumi_card_fbs),
                              tsugumi_card_fbs_size);
  return card.substr(card.find(kNamespace));
}

auto collect_output_type(std::string& error) -> types::TypeRef {
  try {
    static const auto type = [] {
      std::string text = "include \"tsugumi/artifact.fbs\";\n";
      text += collect_schema_declarations();
      std::string error;
      auto compiled = types::compile(text, "tsugumi.builtin.Collected", &error);
      if (!compiled) throw std::logic_error("Collected type failed: " + error);
      return *compiled;
    }();
    return type;
  } catch (const std::exception& e) {
    error = e.what();
    return nullptr;
  }
}

}  // namespace tsugumi
