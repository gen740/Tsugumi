#include "host/templates.hh"

#include <algorithm>
#include <stdexcept>

#include "tsugumi/util/json_object.hh"

extern const unsigned char tsugumi_host_templates[];
extern const unsigned long tsugumi_host_templates_size;

namespace tsugumi::host {
auto WriteTemplate(const std::string &name, const std::string &id)
    -> DocumentTemplate {
  namespace json = ::tsugumi::json;
  static const auto catalog = json::Object(
      std::string(reinterpret_cast<const char *>(tsugumi_host_templates),
                  tsugumi_host_templates_size));
  const auto found = catalog.find(name);
  if (found == catalog.end())
    throw std::runtime_error("Unknown Card template: " + name);
  auto ns = id;
  std::ranges::replace(ns, '-', '_');
  std::ranges::replace(ns, '.', '_');
  json::Members definition;
  DocumentTemplate result;
  for (const auto &[path, raw] : json::Object(found->second)) {
    auto text = json::String(raw);
    for (size_t pos = 0;
         (pos = text.find("@NAMESPACE@", pos)) != std::string::npos;
         pos += ns.size())
      text.replace(pos, 11, ns);
    if (path == "_card.json" || path == "_meta.json") {
      definition[path == "_card.json" ? "card" : "meta"] = text;
    } else {
      result.files.push_back({path, std::move(text)});
    }
  }
  result.definition_json = json::Encode(definition);
  return result;
}
} // namespace tsugumi::host
