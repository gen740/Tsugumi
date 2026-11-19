#pragma once

#include <string>
#include <vector>

namespace tsugumi::host {

// Internal transformation results, staged in the caller's atomic operation.
struct DocumentSubgraph {
  std::string project_json;
  std::string external_json;
  std::string link_json;
};
struct DocumentFileCopy {
  std::string from;
  std::string to;
};
struct DocumentCopy {
  std::string edits_json;
  std::vector<DocumentFileCopy> files;
};
struct DocumentConnect {
  std::string state_json;
  std::string inserted_id;
  bool needs_fork = false;
};

// Pure document edits. The caller saves the returned documents, definitions
// and file moves together through project_save; the host never writes them.
auto MakeSubgraph(const std::string& project,
                  const std::vector<std::string>& cards, const std::string& id)
    -> DocumentSubgraph;

auto CopyDocuments(const std::string& state,
                   const std::vector<std::string>& paths,
                   const std::string& from, const std::string& to,
                   const std::string& name) -> DocumentCopy;

auto ConnectDocuments(const std::string& state, const std::string& from_card,
                      const std::string& from_port, const std::string& to_card,
                      const std::string& to_port, const std::string& fork_id)
    -> DocumentConnect;

auto DocumentsEqual(const std::string& a, const std::string& b) -> bool;

}  // namespace tsugumi::host
