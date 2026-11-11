// Comments on Cards (_meta.json "comments"): what the user and the Agent write
// to each other about a Card. Anyone may add, edit, resolve or delete one; what
// is kept is who did: a save from a model is stamped here with what it did, so
// that the record does not depend on what the model says about itself.
#pragma once

#include <string>
#include <vector>

#include "graph.hh"
#include "store.hh"

namespace tsugumi::comments {

struct Comment {
  std::string card;
  std::string id;
  std::string text;
  bool resolved = false;
  std::string resolution;
  std::string author;     // "agent" or "user"; empty: not recorded (written by hand)
  std::string edited_by;  // who last changed the text
  std::string state_by;   // who last resolved or reopened it
  std::vector<std::string> cards;  // what the save that resolved it changed
};

// What one state of a project did to a comment, from an earlier one.
struct Change {
  std::string card;
  std::string text;
  std::string change;  // added, edited, resolved, reopened or removed
  std::string by;      // "agent", "user" or empty (not recorded: always for removed)
  std::string resolution;
  std::vector<std::string> cards;
};

// The comments of a _meta.json (none when it is not valid).
std::vector<Comment> parse(const std::string& card, const std::string& meta_json);

// The comments of every Card of a document, in node order.
std::vector<Comment> of(const Document& doc, const SourceReader& read);

// Told apart by id, so a comment follows a Card that was renamed or split. A
// comment written and resolved within the two states is resolved.
std::vector<Change> changes(const std::vector<Comment>& before, const std::vector<Comment>& after);

// A model's save, staged as `staged`: stamps each comment it added or changed
// (who wrote it, edited it, marked it; which Cards the save changed), in place
// in `changes`. Whatever the model wrote in those fields does not count. A
// comment without an id gets one. True when `changes` changed: stage them again.
bool stamp_model_save(const Document& before, const SourceReader& read_before, const Store::Staged& staged,
                      const SourceReader& read_after, std::vector<ChangeIn>& changes);

}  // namespace tsugumi::comments
