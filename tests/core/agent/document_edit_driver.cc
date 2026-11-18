// A socket-free host boundary for client gesture/undo tests.
#include <algorithm>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "host/project_operations.hh"
#include "tsugumi/util/crypto.hh"
#include "tsugumi/util/json_object.hh"

namespace {
namespace json = tsugumi::json;
namespace crypto = tsugumi::crypto;
using tsugumi::host::OperationReply;
class ProjectFixture {
 public:
  std::map<std::string, std::string> files;
  uint64_t saves = 0;
  auto Call(const std::string& method, const json::Members& args)
      -> OperationReply {
    std::string type;
    json::Members value;
    if (method == "operation_get") {
      type = "OperationResult";
      value["state"] = "\"unknown\"";
    } else if (method == "project_inspect") {
      type = "ProjectSnapshot";
      value = {{"rev", std::to_string(saves)},
               {"project_json", json::Quote(files.at("tsugumi.json"))},
               {"layout_json", json::Quote(files.at("layout.json"))}};
      json::Members definitions;
      std::vector<std::string> sources;
      for (const auto& [path, bytes] : files) {
        if (path == "tsugumi.json" || path == "layout.json") continue;
        if (path.starts_with("definitions/") &&
            std::count(path.begin(), path.end(), '/') == 2) {
          const auto slash = path.find('/', 12);
          const auto name = path.substr(slash + 1);
          const auto key = name == "_card.json"   ? "card_json"
                           : name == "_meta.json" ? "meta_json"
                           : name == "_link.json" ? "link_json"
                                                  : "";
          if (key != std::string("")) {
            const auto id = path.substr(12, slash - 12);
            auto d = json::Object(json::Get(definitions, id, "{}"));
            d["id"] = json::Quote(id);
            d[key] = json::Quote(bytes);
            definitions[id] = json::Encode(d);
            continue;
          }
        }
        sources.push_back(json::Encode(
            json::Members{{"path", json::Quote(path)},
                          {"hash", json::Quote(crypto::sha256_hex(bytes))}}));
      }
      std::vector<std::string> list;
      for (const auto& [id, raw] : definitions) {
        (void)id;
        list.push_back(raw);
      }
      value["definitions"] = json::Encode(list);
      value["sources"] = json::Encode(sources);
    } else if (method == "project_file_read") {
      type = "FileContent";
      value = {{"bytes", json::Encode(json::Members{
                             {"base64", json::Quote(crypto::base64(files.at(
                                            json::Text(args, "path"))))}})},
               {"eof", "true"}};
    } else {
      if (method != "project_save") throw std::runtime_error(method);
      if (json::UInt(args.at("base_rev")) != saves)
        throw std::runtime_error("stale fixture revision");
      for (const auto& raw : json::Array(args.at("changes"))) {
        const auto change = json::Object(raw),
                   v = json::Object(change.at("value"));
        const auto kind = json::Text(change, "value_type");
        if (kind == "SetProject" || kind == "SetLayout")
          files[kind == "SetProject" ? "tsugumi.json" : "layout.json"] =
              json::Text(v, "json");
        else if (kind == "PutDefinition") {
          const auto dir = "definitions/" + json::Text(v, "id") + '/';
          for (const auto& [key, name] : {std::pair{"card_json", "_card.json"},
                                          {"meta_json", "_meta.json"},
                                          {"link_json", "_link.json"}}) {
            const auto text = json::Text(v, key);
            if (text.empty())
              files.erase(dir + name);
            else
              files[dir + name] = text;
          }
        } else if (kind == "RemoveDefinition") {
          const auto dir = "definitions/" + json::Text(v, "id") + '/';
          std::erase_if(files, [&](const auto& entry) {
            return entry.first.starts_with(dir);
          });
        } else if (kind == "RemoveSource")
          files.erase(json::Text(v, "path"));
        else {
          if (kind != "PutSource") throw std::runtime_error(kind);
          const auto data = json::Get(v, "bytes");
          std::string bytes;
          if (data.starts_with('"'))
            bytes = json::String(data);
          else if (!crypto::base64_decode(
                       json::Text(json::Object(data), "base64"), bytes))
            throw std::runtime_error("invalid fixture bytes");
          files[json::Text(v, "path")] = bytes;
        }
      }
      ++saves;
      type = "SaveResult";
      value = {{"rev", std::to_string(saves)},
               {"operation_data", args.at("operation_data")}};
    }
    return {json::Encode(json::Members{{"reply", json::Quote(type)},
                                       {"value", json::Encode(value)}}),
            {}};
  }
};
}  // namespace

auto main() -> int {
  namespace json = tsugumi::json;
  try {
    std::string raw((std::istreambuf_iterator<char>(std::cin)), {});
    const auto request = json::Object(raw);
    if (request.contains("operation")) {
      ProjectFixture project;
      project.saves = json::UInt(json::Get(request, "revision", "0"));
      for (const auto& [path, encoded] :
           json::Object(json::Get(request, "files", "{}"))) {
        if (!tsugumi::crypto::base64_decode(json::String(encoded),
                                            project.files[path]))
          throw std::runtime_error("invalid fixture file");
      }
      std::string bytes;
      if (!tsugumi::crypto::base64_decode(json::Text(request, "operation"),
                                          bytes))
        throw std::runtime_error("invalid fixture operation");
      namespace H = Tsugumi::Host;
      flatbuffers::Verifier verifier(
          reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
      if (!verifier.VerifyBuffer<H::ProjectOperateRequest>(nullptr))
        throw std::runtime_error("invalid operation buffer");
      const auto operation = std::unique_ptr<H::ProjectOperateRequestT>(
          flatbuffers::GetRoot<H::ProjectOperateRequest>(bytes.data())
              ->UnPack());
      const auto result = tsugumi::host::ProjectOperations().Execute(
          *operation, {"ui", {}, {}, {}},
          [&](const auto& method, const auto& args) {
            return project.Call(method, args);
          });
      json::Members files;
      for (const auto& [path, content] : project.files)
        files[path] = json::Quote(tsugumi::crypto::base64(content));
      std::cout << json::Encode(
          json::Members{{"files", json::Encode(files)},
                        {"revision", std::to_string(project.saves)},
                        {"reply", result.json}});
      return 0;
    }
    throw std::runtime_error("Missing project operation");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
