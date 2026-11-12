// No Ruby API calls in this translation unit: Ruby's exception longjmp must
// never cross live C++ objects. The C shim raises only after these return.
#include <flatbuffers/idl.h>

#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>

#include "ruby_contract.hh"
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
    const auto* ports = std::strcmp(direction, "inputs") == 0 ? ruby_inputs : ruby_outputs;
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
  std::ifstream file(std::string(TSUGUMI_RUBY_CONTRACT) + "/_" + direction + ".bfbs", std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(file)), {});
  if (bytes.empty() || !value->Deserialize(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()) ||
      !value->SetRootType(root.c_str()))
    throw std::runtime_error("cannot load Ruby port schema " + key);
  return *(parsers[key] = std::move(value));
}
}  // namespace

extern "C" auto tsugumi_ruby_json_error() -> const char* { return error.c_str(); }

extern "C" auto tsugumi_ruby_decode(const char* direction, const char* name, const uint8_t* bytes,
                                    size_t size) -> const char* {
  try {
    result.clear();
    if (!bytes || !size) return "{}";
    if (const char* why = tsugumi::message_json::gen_text(parser(direction, name), bytes, &result))
      throw std::runtime_error(why);
    return result.c_str();
  } catch (const std::exception& e) {
    error = e.what();
    return nullptr;
  }
}

extern "C" auto tsugumi_ruby_emit(const tsugumi_call* call, const char* port, const char* json) -> int {
  try {
    auto& p = parser("outputs", port);
    if (!p.ParseJson(json)) throw std::runtime_error("output " + std::string(port) + ": " + p.error_);
    if (call->emit_output(call->host, port, p.builder_.GetBufferPointer(), p.builder_.GetSize()) != 0)
      throw std::runtime_error("output " + std::string(port) + " was rejected");
    return 0;
  } catch (const std::exception& e) {
    error = e.what();
    return 1;
  }
}
