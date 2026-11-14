// A socket-free host boundary for client gesture/undo tests.
#include <iostream>
#include <string>
#include <vector>

#include "agent/json.hh"
#include "host/document_edits.hh"

auto main() -> int {
  namespace json = tsugumi::agent::json;
  try {
    std::string raw((std::istreambuf_iterator<char>(std::cin)), {});
    const auto request = json::Object(raw);
    std::vector<std::string> cards;
    for (const auto& card : json::Array(json::Get(request, "cards", "[]")))
      cards.push_back(json::String(card));
    const auto result = tsugumi::host::MakeSubgraph(
        json::Text(request, "projectJson"), cards, json::Text(request, "id"));
    std::cout << json::Encode(
        json::Members{{"projectJson", json::Quote(result.project_json)},
                      {"externalJson", json::Quote(result.external_json)},
                      {"linkJson", json::Quote(result.link_json)}});
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
