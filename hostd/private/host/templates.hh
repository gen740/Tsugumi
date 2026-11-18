#pragma once

#include <string>

#include <vector>

namespace tsugumi::host {
struct DocumentTemplateFile {
  std::string path;
  std::string text;
};
struct DocumentTemplate {
  std::string definition_json;
  std::vector<DocumentTemplateFile> files;
};

// Returns a new definition and its text files; never writes the project.
auto WriteTemplate(const std::string &name, const std::string &id)
    -> DocumentTemplate;
} // namespace tsugumi::host
