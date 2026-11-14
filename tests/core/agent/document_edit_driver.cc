// A socket-free host boundary for client gesture/undo tests.
#include <iostream>
#include <string>
#include <vector>

#include "agent/json.hh"
#include "host/document_edits.hh"
#include "host/templates.hh"

auto main() -> int {
  namespace json = tsugumi::agent::json;
  try {
    std::string raw((std::istreambuf_iterator<char>(std::cin)), {});
    const auto request = json::Object(raw);
    if (request.contains("template")) {
      const auto result = tsugumi::host::WriteTemplate(
          json::Text(request, "template"), json::Text(request, "id"));
      std::vector<std::string> files;
      for (const auto &file : result.files)
        files.push_back(
            json::Encode(json::Members{{"path", json::Quote(file->path)},
                                       {"text", json::Quote(file->text)}}));
      std::cout << json::Encode(
          json::Members{{"definitionJson", json::Quote(result.definition_json)},
                        {"files", json::Encode(files)}});
      return 0;
    }
    if (request.contains("fromCard")) {
      const auto result = tsugumi::host::ConnectDocuments(
          json::Text(request, "stateJson"), json::Text(request, "fromCard"),
          json::Text(request, "fromPort"), json::Text(request, "toCard"),
          json::Text(request, "toPort"), json::Text(request, "forkId"));
      std::cout << json::Encode(
          json::Members{{"stateJson", json::Quote(result.state_json)},
                        {"insertedId", json::Quote(result.inserted_id)},
                        {"needsFork", result.needs_fork ? "true" : "false"}});
      return 0;
    }
    if (request.contains("stateJson")) {
      std::vector<std::string> paths;
      for (const auto &path : json::Array(json::Get(request, "paths", "[]")))
        paths.push_back(json::String(path));
      const auto result = tsugumi::host::CopyDocuments(
          json::Text(request, "stateJson"), paths, json::Text(request, "from"),
          json::Text(request, "to"), json::Text(request, "name"));
      std::vector<std::string> files;
      for (const auto &file : result.files)
        files.push_back(json::Encode(json::Members{
            {"from", json::Quote(file->from)}, {"to", json::Quote(file->to)}}));
      std::cout << json::Encode(
          json::Members{{"editsJson", json::Quote(result.edits_json)},
                        {"files", json::Encode(files)}});
      return 0;
    }
    if (request.contains("baseJson")) {
      const auto result = tsugumi::host::MergeDocuments(
          json::Text(request, "baseJson"), json::Text(request, "oursJson"),
          json::Text(request, "theirsJson"));
      std::vector<std::string> conflicts;
      for (const auto &conflict : result.conflicts)
        conflicts.push_back(json::Quote(conflict));
      std::cout << json::Encode(
          json::Members{{"mergedJson", json::Quote(result.merged_json)},
                        {"conflicts", json::Encode(conflicts)}});
      return 0;
    }
    std::vector<std::string> cards;
    for (const auto &card : json::Array(json::Get(request, "cards", "[]")))
      cards.push_back(json::String(card));
    const auto result = tsugumi::host::MakeSubgraph(
        json::Text(request, "projectJson"), cards, json::Text(request, "id"));
    std::cout << json::Encode(
        json::Members{{"projectJson", json::Quote(result.project_json)},
                      {"externalJson", json::Quote(result.external_json)},
                      {"linkJson", json::Quote(result.link_json)}});
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
