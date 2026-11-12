// The JSON face of a Card: port messages and params as JSON text, checked
// against the Card's own FlatBuffers schemas, for languages that have no flatc
// generator and work with plain maps and arrays (Ruby today). Nothing here
// knows the language: an adapter decodes inputs with tsugumi_json_decode and
// sends outputs with tsugumi_json_emit.
//
// No interpreter API is called in this translation unit, and nothing here
// throws across its C boundary: an embedded interpreter whose errors longjmp
// (MRI, R) must never unwind through live C++ objects, so its C shim raises
// only after these functions have returned.
//
// Non-finite doubles are not JSON. FlatBuffers spells them nan, inf and -inf;
// this face spells them NaN, Infinity and -Infinity, which is what the JSON
// libraries with a non-finite extension read and write.
#include <flatbuffers/idl.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "json_contract.hh"
#include "tsugumi/message_json.hh"
#include "tsugumi/sdk/abi.h"

namespace {
thread_local std::string result, error;
std::map<std::string, std::unique_ptr<flatbuffers::Parser>> parsers;

auto parser(const char* direction, const char* name) -> flatbuffers::Parser& {
  std::string root;
  if (std::strcmp(direction, "params") == 0)
    root = "params.Params";
  else {
    const auto* ports = std::strcmp(direction, "inputs") == 0 ? json_inputs : json_outputs;
    for (const auto* p = ports; p->name; ++p)
      if (name == std::string(p->name)) root = p->root;
    if (root.empty()) throw std::runtime_error("unknown " + std::string(direction) + " port " + name);
  }
  const std::string key = std::string(direction) + ":" + root;
  if (auto it = parsers.find(key); it != parsers.end()) return *it->second;
  flatbuffers::IDLOptions options;
  options.strict_json = true;
  options.natural_utf8 = true;
  options.output_default_scalars_in_json = true;
  options.indent_step = -1;
  auto value = std::make_unique<flatbuffers::Parser>(options);
  std::ifstream file(std::string(TSUGUMI_JSON_CONTRACT) + "/_" + direction + ".bfbs", std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(file)), {});
  if (bytes.empty() || !value->Deserialize(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()) ||
      !value->SetRootType(root.c_str()))
    throw std::runtime_error("cannot load port schema " + key);
  return *(parsers[key] = std::move(value));
}

// Rewrites the bare non-finite tokens outside strings from one spelling to
// the other. A bare word in JSON text is otherwise only true, false or null.
auto respell(std::string_view text, bool to_flatbuffers) -> std::string {
  static constexpr std::pair<std::string_view, std::string_view> words[] = {
      {"nan", "NaN"}, {"inf", "Infinity"}, {"infinity", "Infinity"}};
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    const char c = text[i];
    if (c == '"') {
      const size_t start = i++;
      while (i < text.size() && text[i] != '"') i += text[i] == '\\' ? 2 : 1;
      i = std::min(i + 1, text.size());
      out.append(text.substr(start, i - start));
      continue;
    }
    if (!std::isalpha(static_cast<unsigned char>(c))) {
      out += c;
      ++i;
      continue;
    }
    size_t end = i;
    while (end < text.size() && std::isalpha(static_cast<unsigned char>(text[end]))) ++end;
    const std::string_view word = text.substr(i, end - i);
    std::string_view replacement = word;
    for (const auto& [flat, json] : words) {
      if (to_flatbuffers && word == json) replacement = flat == "infinity" ? "inf" : flat;
      if (!to_flatbuffers && word == flat) replacement = json;
    }
    out.append(replacement);
    i = end;
  }
  return out;
}
}  // namespace

extern "C" auto tsugumi_json_error() -> const char* { return error.c_str(); }

// direction is "inputs" or "params" (name is then ignored). The returned text
// is valid until the next call on this thread; NULL means tsugumi_json_error.
extern "C" auto tsugumi_json_decode(const char* direction, const char* name, const uint8_t* bytes,
                                    size_t size) -> const char* {
  try {
    result.clear();
    if (!bytes || !size) return "{}";
    std::string text;
    if (const char* why = tsugumi::message_json::gen_text(parser(direction, name), bytes, &text))
      throw std::runtime_error(why);
    result = respell(text, false);
    return result.c_str();
  } catch (const std::exception& e) {
    error = e.what();
    return nullptr;
  }
}

// Parses one output message against its schema (unknown fields, wrong types
// and out-of-range numbers are errors) and hands it to the host.
extern "C" auto tsugumi_json_emit(const tsugumi_call* call, const char* port, const char* json) -> int {
  try {
    auto& p = parser("outputs", port);
    if (!p.ParseJson(respell(json, true).c_str()))
      throw std::runtime_error("output " + std::string(port) + ": " + p.error_);
    if (call->emit_output(call->host, port, p.builder_.GetBufferPointer(), p.builder_.GetSize()) != 0)
      throw std::runtime_error("output " + std::string(port) + " was rejected");
    return 0;
  } catch (const std::exception& e) {
    error = e.what();
    return 1;
  }
}
