// Comments on Cards (_meta.json "comments"): what the user and the Agent write
// to each other about a Card. Anyone may add, edit, resolve or delete one; what
// is kept is who did: a save from a model is stamped here with what it did, so
// that the record does not depend on what the model says about itself.
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "agent/json.hh"

namespace tsugumi::host::comments {

struct Comment {
  std::string card;
  std::string id;
  std::string text;
  bool resolved = false;
  std::string resolution;
  std::string
      author; // "agent" or "user"; empty: not recorded (written by hand)
  std::string edited_by;          // who last changed the text
  std::string state_by;           // who last resolved or reopened it
  std::vector<std::string> cards; // what the save that resolved it changed
};

// What one state of a project did to a comment, from an earlier one.
struct Change {
  std::string card;
  std::string text;
  std::string change; // added, edited, resolved, reopened or removed
  std::string by; // "agent", "user" or empty (not recorded: always for removed)
  std::string resolution;
  std::vector<std::string> cards;
};

// The comments of a _meta.json (none when it is not valid).
std::vector<Comment> parse(const std::string &card,
                           const std::string &meta_json);

// Told apart by id, so a comment follows a Card that was renamed or split. A
// comment written and resolved within the two states is resolved.
std::vector<Change> changes(const std::vector<Comment> &before,
                            const std::vector<Comment> &after);

using Files = std::map<std::string, std::string>;
// Definition paths use ids, including nested external project directories.
auto is_meta(const std::string &path) -> bool;
auto card_of(const std::string &path, const Files &files = {})
    -> std::optional<std::string>;
auto of(const Files &files) -> std::vector<Comment>;
auto stamped(const std::string &text, const std::string &card,
             const std::vector<Comment> &before,
             const std::vector<std::string> &changed) -> std::string;
auto record_json(const std::vector<Change> &changes) -> std::string;
auto version(const std::filesystem::path &root, const std::string &commit)
    -> std::vector<Change>;
} // namespace tsugumi::host::comments
